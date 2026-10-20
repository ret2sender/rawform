/*
 * This file is part of rawform.
 * Copyright (C) 2026 Etienne Fleurant
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// SpectrumBars.cpp
//
// Implementation of the one-item bar plot. See the header for why it exists;
// this file is the property plumbing (every setter marks the item for a
// repaint) and the geometry build in updatePaintNode.

#include "playback/SpectrumBars.h"

#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <cmath>

namespace rawform {

namespace {

// Every bar is drawn as seven axis-aligned quads of six vertices each (two
// triangles, no index buffer, one DrawTriangles call for the plot): the top
// pixel row in three pieces (left corner, middle, right corner), the body,
// and the bottom pixel row in three pieces. The pieces exist only to give the
// corner pixels and the partial top row their own alpha.
constexpr int kQuadsPerBar     = 7;
constexpr int kVerticesPerQuad = 6;
constexpr int kVerticesPerBar  = kQuadsPerBar * kVerticesPerQuad;

// The coverage a 1 px corner radius leaves its corner pixel: the pixel's area
// minus the quarter-circle cutout, 1 - (1 - pi/4). The Rectangles drew their
// corners at this alpha through antialiasing; the bars keep it.
constexpr float kCornerCoverage = 0.7854f;

// One quad, premultiplied color (the pipeline's convention), alpha scaled by
// `coverage`. Returns the next free vertex.
QSGGeometry::ColoredPoint2D* emitQuad(QSGGeometry::ColoredPoint2D* v,
                                      float x0, float y0, float x1, float y1,
                                      const QColor& color, float coverage) {
    const float a = std::clamp(coverage, 0.0f, 1.0f) * static_cast<float>(color.alphaF());
    const auto r  = static_cast<unsigned char>(std::lround(color.redF() * a * 255.0));
    const auto g  = static_cast<unsigned char>(std::lround(color.greenF() * a * 255.0));
    const auto b  = static_cast<unsigned char>(std::lround(color.blueF() * a * 255.0));
    const auto al = static_cast<unsigned char>(std::lround(a * 255.0));
    v[0].set(x0, y0, r, g, b, al);
    v[1].set(x1, y0, r, g, b, al);
    v[2].set(x0, y1, r, g, b, al);
    v[3].set(x0, y1, r, g, b, al);
    v[4].set(x1, y0, r, g, b, al);
    v[5].set(x1, y1, r, g, b, al);
    return v + kVerticesPerQuad;
}

}  // namespace

SpectrumBars::SpectrumBars(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
}

// ---------------------------------------------------------------------------
void SpectrumBars::setValues(const QList<qreal>& values) {
    // No equality short-cut: the provider republishes a changed list on every
    // tick, and comparing twenty doubles to save a repaint that is needed
    // anyway would be the one wasted step.
    m_values = values;
    emit valuesChanged();
    update();
}

void SpectrumBars::setCount(int count) {
    const int clamped = std::max(0, count);
    if (clamped == m_count) {
        return;
    }
    m_count = clamped;
    emit countChanged();
    update();
}

void SpectrumBars::setColor(const QColor& color) {
    if (color == m_color) {
        return;
    }
    m_color = color;
    emit colorChanged();
    update();
}

void SpectrumBars::setGap(qreal gap) {
    if (qFuzzyCompare(gap, m_gap)) {
        return;
    }
    m_gap = gap;
    emit gapChanged();
    update();
}

void SpectrumBars::setMinBarHeight(qreal height) {
    if (qFuzzyCompare(height, m_minBarHeight)) {
        return;
    }
    m_minBarHeight = height;
    emit minBarHeightChanged();
    update();
}

// ---------------------------------------------------------------------------
// Render thread (the GUI thread is blocked for the duration, so the members
// are safe to read). One geometry node for the whole plot; its vertex buffer
// is resized when the bar count changes and rewritten every time.
QSGNode* SpectrumBars::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* /*data*/) {
    auto* node = static_cast<QSGGeometryNode*>(oldNode);
    const int vertexCount = m_count * kVerticesPerBar;

    if (m_count <= 0 || width() <= 0.0 || height() <= 0.0) {
        delete node;
        return nullptr;
    }

    if (node == nullptr) {
        node = new QSGGeometryNode;
        auto* geometry =
            new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexCount);
        geometry->setDrawingMode(QSGGeometry::DrawTriangles);
        node->setGeometry(geometry);
        node->setFlag(QSGNode::OwnsGeometry);
        node->setMaterial(new QSGVertexColorMaterial);
        node->setFlag(QSGNode::OwnsMaterial);
    }

    QSGGeometry* geometry = node->geometry();
    if (geometry->vertexCount() != vertexCount) {
        geometry->allocate(vertexCount);
    }

    // Layout on whole logical pixels (see the header): one bar width and one
    // gap for the whole row, the row centered, the split's remainder left to
    // the margins. A plot too narrow for a pixel per bar still draws them, at
    // one pixel each, overlapping the gaps.
    const float plotW  = static_cast<float>(width());
    const float plotH  = static_cast<float>(height());
    const int   gapPx  = std::max(0, static_cast<int>(std::lround(m_gap)));
    const int   barPx  = std::max(1, static_cast<int>(std::floor(
                             (plotW - static_cast<float>((m_count - 1) * gapPx))
                             / static_cast<float>(m_count))));
    const int   rowPx  = m_count * barPx + (m_count - 1) * gapPx;
    const float left   = std::floor((plotW - static_cast<float>(rowPx)) / 2.0f);
    const float minH   = std::min(static_cast<float>(m_minBarHeight), plotH);
    const float travel = plotH - minH;

    QSGGeometry::ColoredPoint2D* v = geometry->vertexDataAsColoredPoint2D();
    for (int i = 0; i < m_count; ++i) {
        const float level = (i < m_values.size())
                                ? static_cast<float>(std::clamp(m_values.at(i), 0.0, 1.0))
                                : 0.0f;
        const float x0 = left + static_cast<float>(i * (barPx + gapPx));
        const float x1 = x0 + static_cast<float>(barPx);
        const float y1 = plotH;                              // the bottom, on the grid
        const float top = plotH - (minH + travel * level);   // continuous

        // The top pixel row: from the row's grid line to the next one, drawn
        // at the coverage the continuous top edge leaves it. A top exactly on
        // the grid covers its row fully. The body starts on the row below.
        const float rowTop   = std::floor(top);
        const float coverage = rowTop + 1.0f - top;
        const float bodyTop  = std::min(rowTop + 1.0f, y1);
        // The bottom pixel row, for its corners; the body ends above it.
        const float bottomRowTop = std::max(bodyTop, y1 - 1.0f);

        // Corner pieces are one pixel wide; a bar narrower than three pixels
        // has no middle, and a one-pixel bar is all corner.
        const float cl = std::min(x0 + 1.0f, x1);
        const float cr = std::max(x1 - 1.0f, cl);

        v = emitQuad(v, x0, rowTop, cl, bodyTop, m_color, coverage * kCornerCoverage);
        v = emitQuad(v, cl, rowTop, cr, bodyTop, m_color, coverage);
        v = emitQuad(v, cr, rowTop, x1, bodyTop, m_color, coverage * kCornerCoverage);
        v = emitQuad(v, x0, bodyTop, x1, bottomRowTop, m_color, 1.0f);
        v = emitQuad(v, x0, bottomRowTop, cl, y1, m_color, kCornerCoverage);
        v = emitQuad(v, cl, bottomRowTop, cr, y1, m_color, 1.0f);
        v = emitQuad(v, cr, bottomRowTop, x1, y1, m_color, kCornerCoverage);
    }
    node->markDirty(QSGNode::DirtyGeometry);
    return node;
}

}  // namespace rawform
