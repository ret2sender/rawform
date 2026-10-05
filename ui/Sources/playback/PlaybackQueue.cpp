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

// PlaybackQueue.cpp
//
// Implementation of the playback queue's origin sidecar and its two entry
// points (enqueue at the end, enqueue at the front). The row data itself is
// the queue PlaylistModel's; this file only keeps the parallel origin list
// aligned with it and answers the controller's queries over the pair.

#include "playback/PlaybackQueue.h"

#include "media/TrackData.h"

#include <QAbstractItemModel>
#include <QVariantList>

#include <algorithm> // std::ranges::sort / unique
#include <utility>   // std::move

namespace rawform {

PlaybackQueue::PlaybackQueue(PlaylistModel* model, QObject* parent)
    : QObject(parent), m_model(model) {
    // The model is the authority on row order; the sidecar follows its
    // structural signals, so every path that edits the queue (the controller's
    // cull, the tab's Delete key and drag-reorder, a scan into the tab, a
    // column-layout reset) keeps the origins aligned without knowing they
    // exist. The one path that KNOWS origins, insertFrom, stages them for the
    // rowsInserted hook rather than writing the list directly, so there is a
    // single writer per signal.
    if (m_model == nullptr) {
        return;
    }
    connect(m_model, &QAbstractItemModel::rowsInserted,
            this, &PlaybackQueue::onRowsInserted);
    connect(m_model, &QAbstractItemModel::rowsRemoved,
            this, &PlaybackQueue::onRowsRemoved);
    connect(m_model, &QAbstractItemModel::rowsMoved,
            this, &PlaybackQueue::onRowsMoved);
    connect(m_model, &QAbstractItemModel::modelReset,
            this, &PlaybackQueue::onModelReset);
    // A model handed over with rows already in it (none today; defensive).
    m_origins.resize(m_model->rowCount());
}

int PlaybackQueue::count() const {
    return m_model ? m_model->rowCount() : 0;
}

// ---------------------------------------------------------------------------
// QML surface
// ---------------------------------------------------------------------------

int PlaybackQueue::enqueue(PlaylistModel* source, const QList<int>& rows) {
    return insertFrom(source, rows, count());
}

int PlaybackQueue::enqueueNext(PlaylistModel* source, const QList<int>& rows) {
    return insertFrom(source, rows, 0);
}

void PlaybackQueue::clear() {
    if (!m_model || m_model->rowCount() == 0) {
        return;
    }
    // setTracks resets the model; onModelReset re-sizes the sidecar to the
    // (now zero) row count. One reset rather than a removeTracks over every
    // row: the queue tab's view rebuilds either way, and a reset is the
    // cheaper of the two for a large queue.
    m_model->setTracks({});
}

// ---------------------------------------------------------------------------
// Controller surface
// ---------------------------------------------------------------------------

QString PlaybackQueue::pathAt(int index) const {
    if (!m_model) {
        return {};
    }
    return m_model->trackFilePath(index);
}

const TrackData* PlaybackQueue::trackAt(int index) const {
    return m_model ? m_model->trackAt(index) : nullptr;
}

PlaybackQueue::Origin PlaybackQueue::originAt(int index) const {
    if (index < 0 || index >= m_origins.size()) {
        return {};
    }
    return m_origins.at(index);
}

int PlaybackQueue::firstIndexOfPath(const QString& path) const {
    if (!m_model || path.isEmpty()) {
        return -1;
    }
    const int n = m_model->rowCount();
    for (int i = 0; i < n; ++i) {
        if (m_model->trackFilePath(i) == path) {
            return i;
        }
    }
    return -1;
}

void PlaybackQueue::takeFront(int n) {
    if (!m_model) {
        return;
    }
    const int take = std::min(n, m_model->rowCount());
    if (take <= 0) {
        return;
    }
    QVariantList rows;
    rows.reserve(take);
    for (int i = 0; i < take; ++i) {
        rows << i;
    }
    // One contiguous run: removeTracks emits a single rowsRemoved, which the
    // sidecar hook erases in one splice.
    m_model->removeTracks(rows);
}

std::vector<std::string> PlaybackQueue::paths() const {
    std::vector<std::string> out;
    if (!m_model) {
        return out;
    }
    const int n = m_model->rowCount();
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const QString p = m_model->trackFilePath(i);
        if (!p.isEmpty()) {
            out.push_back(p.toStdString());
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Insert body
// ---------------------------------------------------------------------------

int PlaybackQueue::insertFrom(PlaylistModel* source, const QList<int>& rows, int at) {
    if (!m_model || !source || rows.isEmpty()) {
        return 0;
    }
    // Playlist order, once each: a selection arrives in click order and a
    // Shift range can list a row twice through the proxy; the queue should
    // read like the playlist does.
    QList<int> sorted = rows;
    std::ranges::sort(sorted);
    const auto dup = std::ranges::unique(sorted);
    sorted.erase(dup.begin(), dup.end());

    QList<TrackData> tracks;
    QList<Origin>    origins;
    tracks.reserve(sorted.size());
    origins.reserve(sorted.size());
    for (const int row : sorted) {
        const TrackData* td = source->trackAt(row);
        if (td == nullptr) {
            continue; // out of range: dropped, like removeTracks drops them
        }
        tracks.push_back(*td);
        origins.push_back(Origin{ QPointer<PlaylistModel>(source),
                                  QPersistentModelIndex(source->index(row, 0)) });
    }
    if (tracks.isEmpty()) {
        return 0;
    }
    const int added = static_cast<int>(tracks.size());
    // Stage, then insert: insertTracks emits rowsInserted synchronously, and
    // onRowsInserted consumes the staged list inside that emission.
    m_staged = std::move(origins);
    m_model->insertTracks(at, std::move(tracks));
    m_staged.clear(); // consumed; cleared again here for the no-signal case
    return added;
}

// ---------------------------------------------------------------------------
// Sidecar maintenance
// ---------------------------------------------------------------------------

void PlaybackQueue::onRowsInserted(const QModelIndex& parent, int first, int last) {
    if (parent.isValid()) {
        return;
    }
    const int n = last - first + 1;
    // The staged origins belong to THIS insert iff their count matches it;
    // anything else (a scanner batch into the queue tab, a .rwfpl concat, a
    // mismatch that should not happen) gets null origins, which play but do
    // not continue.
    QList<Origin> incoming;
    if (m_staged.size() == n) {
        incoming = std::move(m_staged);
        m_staged.clear();
    } else {
        incoming.resize(n);
    }
    const int at = std::clamp(first, 0, static_cast<int>(m_origins.size()));
    for (int i = 0; i < n; ++i) {
        m_origins.insert(at + i, incoming.at(i));
    }
    emit countChanged();
}

void PlaybackQueue::onRowsRemoved(const QModelIndex& parent, int first, int last) {
    if (parent.isValid()) {
        return;
    }
    const int hi = std::min(last, static_cast<int>(m_origins.size()) - 1);
    for (int i = hi; i >= first && i >= 0; --i) {
        m_origins.removeAt(i);
    }
    emit countChanged();
}

void PlaybackQueue::onRowsMoved(const QModelIndex& parent, int start, int end,
                                const QModelIndex& destination, int row) {
    if (parent.isValid() || destination.isValid()) {
        return;
    }
    // The same mechanics as PlaylistModel::moveTracks, so the two lists
    // permute identically: lift the block, then re-insert at the destination
    // boundary, which shifts left by the block size for a downward move.
    const int count = end - start + 1;
    if (start < 0 || count <= 0 || end >= m_origins.size()) {
        return;
    }
    const QList<Origin> block = m_origins.mid(start, count);
    m_origins.remove(start, count);
    const int insertAt = (row <= start) ? row : row - count;
    for (int k = 0; k < count; ++k) {
        m_origins.insert(insertAt + k, block.at(k));
    }
    // No countChanged: the count is unchanged, and the controller rebuilds
    // its lookahead from the model's own rowsMoved.
}

void PlaybackQueue::onModelReset() {
    // A reset (clear, or a load-time applyColumnLayout) rebuilds the rows
    // without telling us where they came from; the origins are gone.
    m_origins.clear();
    m_origins.resize(m_model ? m_model->rowCount() : 0);
    emit countChanged();
}

} // namespace rawform
