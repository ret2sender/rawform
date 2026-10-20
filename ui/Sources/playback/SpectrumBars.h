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

// SpectrumBars.h
//
// The spectrum display's bar plot as one scene graph item. SpectrumView.qml
// hands it the bar values and the look (color, gap, minimum height) and it
// draws every bar from a single geometry node: a handful of quads per bar in
// one vertex buffer under one vertex-color material, rebuilt in
// updatePaintNode on the render thread whenever the values change.
//
// WHY ONE ITEM. The bars update at the spectrum's refresh rate for as long as
// a track plays. As a Repeater of Rectangles, every tick was one binding per
// bar to compute its level, one height write per bar, and one scene graph
// node per bar for the renderer to re-geometry, plus the QList-to-JS-array
// conversion the per-bar reads cost; here a tick is one QList copy into
// `values`, one update() and one vertex upload. The item knows nothing about
// the provider: it draws whatever list it is given, like the Rectangles did.
//
// THE LOOK is the Rectangles' look, reproduced by hand because a geometry node
// has no antialiasing and no radius of its own: every bar is the same whole
// number of logical pixels wide, with the same whole gap, the row centered in
// the plot (the remainder of an uneven split goes to the margins, never to a
// bar or a gap, which is what makes the bars read as uniform); the top edge
// moves continuously, its partial pixel row drawn at the alpha of its coverage,
// the way the antialiased Rectangle edge was; and the four corner pixels carry
// the coverage a 1 px radius leaves them, so the bars keep their softened ends.

#pragma once

#include <QColor>
#include <QList>
#include <QQmlEngine>  // QML_NAMED_ELEMENT
#include <QQuickItem>

namespace rawform {

class SpectrumBars : public QQuickItem {
    Q_OBJECT
    QML_NAMED_ELEMENT(SpectrumBars)

    /// The bar magnitudes, each 0..1, low frequency first. A list shorter than
    /// `count` leaves the trailing bars at the minimum height; a longer one is
    /// truncated to `count`.
    Q_PROPERTY(QList<qreal> values READ values WRITE setValues NOTIFY valuesChanged)

    /// The number of bars drawn. The plot width minus the gaps is split evenly
    /// between them.
    Q_PROPERTY(int count READ count WRITE setCount NOTIFY countChanged)

    /// The bar fill.
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)

    /// The gap between neighboring bars, in logical pixels.
    Q_PROPERTY(qreal gap READ gap WRITE setGap NOTIFY gapChanged)

    /// The smallest height a bar ever shows, so a silent band keeps a visible
    /// nub rather than vanishing.
    Q_PROPERTY(qreal minBarHeight READ minBarHeight WRITE setMinBarHeight
               NOTIFY minBarHeightChanged)

public:
    explicit SpectrumBars(QQuickItem* parent = nullptr);

    [[nodiscard]] QList<qreal> values() const { return m_values; }
    [[nodiscard]] int          count() const { return m_count; }
    [[nodiscard]] QColor       color() const { return m_color; }
    [[nodiscard]] qreal        gap() const { return m_gap; }
    [[nodiscard]] qreal        minBarHeight() const { return m_minBarHeight; }

    void setValues(const QList<qreal>& values);
    void setCount(int count);
    void setColor(const QColor& color);
    void setGap(qreal gap);
    void setMinBarHeight(qreal height);

signals:
    void valuesChanged();
    void countChanged();
    void colorChanged();
    void gapChanged();
    void minBarHeightChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;

private:
    QList<qreal> m_values;
    int          m_count        = 20;
    QColor       m_color        = QColor(0xBD, 0xB2, 0xFF);  // Theme.accentSoft; the view binds it
    qreal        m_gap          = 2.0;
    qreal        m_minBarHeight = 2.0;
};

}  // namespace rawform
