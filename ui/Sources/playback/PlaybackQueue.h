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

// PlaybackQueue.h
//
// The playback queue: the ordered list of tracks that play BEFORE the playing
// playlist's organic next track. AudioController projects it as the prefix of
// the engine's pending queue (queue paths ++ organic tail), so a queued track
// always wins over "the next row down".
//
// TWO HALVES, ONE ROW ORDER. The entries themselves live in an ordinary
// PlaylistModel (built by PlaylistTabs, shown as the "Playback Queue" tab), so
// the queue is viewed, reordered, filtered, and edited with every tool a
// playlist has. This object adds the one thing a playlist row cannot carry:
// where each entry CAME FROM. The origin (the source playlist model and a
// persistent index into it) is what makes the queue cross-playlist aware:
// when a queued track starts, the controller moves its cursor to the origin
// row, so once the queue drains playback continues from there, after the
// queued track, in the playlist it belongs to.
//
// The origins are a sidecar list kept parallel to the model's rows by
// following the model's own structural signals: rowsInserted splices in the
// origins staged by enqueue (null origins for an insert that did not come
// through enqueue, such as a file drop into the queue tab), rowsRemoved
// erases, rowsMoved permutes exactly as the model's moveTracks permutes its
// rows, and a reset degrades every origin to null. A null origin plays fine;
// it only has no continuation (the controller tries a path lookup first).
//
// CULL ON START. An entry leaves the queue the moment it becomes the current
// track (the controller calls takeFront), never on finish. So nothing in the
// queue is ever "playing": the queue is purely what comes next, the
// now-playing marker lives in the origin playlist, and a Stop/Play restart
// of the current track cannot replay the queue head a second time.
//
// Session-transient: nothing here is persisted. The tab's visibility and
// column layout are PlaylistTabs' and PlaylistStore's concern.
//
// Created in main.cpp and exposed as the `playbackQueue` context property.

#pragma once

#include "playlist/PlaylistModel.h" // complete type: a Q_PROPERTY pointer

#include <QList>
#include <QObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QQmlEngine> // QML_ELEMENT / QML_UNCREATABLE
#include <QString>

#include <string>
#include <vector>

namespace rawform {

struct TrackData;

class PlaybackQueue : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("PlaybackQueue is created in C++ and exposed as a context property")

    /// The number of queued entries (the queue model's row count). Notifies on
    /// every structural change of the model, so a tab-bar count or a menu
    /// enablement can bind to it.
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /// The queue's PlaylistModel, for identity checks in QML ("is this view
    /// showing the queue?"). Fixed for the object's lifetime.
    Q_PROPERTY(rawform::PlaylistModel* model READ model CONSTANT)

public:
    /// Where a queued entry came from. `model` is the source playlist (null
    /// once that tab is closed, or for an entry that never had a source);
    /// `index` is the row in it, self-healing across edits and invalid once
    /// the row is deleted.
    struct Origin {
        QPointer<PlaylistModel> model;
        QPersistentModelIndex   index;
    };

    /// @p model is the queue's PlaylistModel, owned elsewhere (PlaylistTabs
    /// builds it so it carries the schema, the registry, and a tab's worth of
    /// proxy/selection/reloader) and outliving this object.
    explicit PlaybackQueue(PlaylistModel* model, QObject* parent = nullptr);
    ~PlaybackQueue() override = default;

    [[nodiscard]] PlaylistModel* model() const { return m_model; }
    [[nodiscard]] int            count() const;

    // --- QML surface -------------------------------------------------------

    /// Append copies of @p rows of @p source (SOURCE rows, any order; sorted
    /// ascending, duplicates and out-of-range rows dropped) to the END of the
    /// queue, each remembering its origin. Returns the number added.
    Q_INVOKABLE int enqueue(rawform::PlaylistModel* source, const QList<int>& rows);

    /// Same as enqueue, but the rows go to the FRONT of the queue, in playlist
    /// order (the "Play Next" command): the first selected row plays next,
    /// the rest follow it, and everything already queued comes after.
    Q_INVOKABLE int enqueueNext(rawform::PlaylistModel* source, const QList<int>& rows);

    /// Drop every entry. The explicit play gesture on a normal playlist
    /// (double-click / Enter) calls this; so does Playback > Clear.
    Q_INVOKABLE void clear();

    // --- Controller surface (C++ only) -------------------------------------

    /// The file path of entry @p index, empty when out of range.
    [[nodiscard]] QString pathAt(int index) const;

    /// The TrackData of entry @p index, nullptr when out of range. The
    /// controller snapshots the now-playing facts from it when an entry
    /// starts without an origin to resolve them from.
    [[nodiscard]] const TrackData* trackAt(int index) const;

    /// The origin of entry @p index; a default (null) Origin when out of range.
    [[nodiscard]] Origin originAt(int index) const;

    /// The lowest entry index whose path equals @p path, -1 for none. The
    /// controller's test for "did the engine just advance into the queue?".
    [[nodiscard]] int firstIndexOfPath(const QString& path) const;

    /// Remove the first @p n entries (clamped to the count). The cull: the
    /// entry that just started, plus any unopenable entries the engine skipped
    /// on its way to it.
    void takeFront(int n);

    /// Every entry's path, in queue order, as the engine's string type. The
    /// prefix of the controller's lookahead projection. Empty paths are
    /// skipped (there are none in practice; TrackData always carries one).
    [[nodiscard]] std::vector<std::string> paths() const;

signals:
    void countChanged();

private:
    /// Build the TrackData copies and their origins for @p rows of @p source,
    /// stage the origins for the rowsInserted hook, and insert at @p at. The
    /// shared body of enqueue / enqueueNext. Returns the number inserted.
    int insertFrom(PlaylistModel* source, const QList<int>& rows, int at);

    // Sidecar maintenance, driven by the model's structural signals.
    void onRowsInserted(const QModelIndex& parent, int first, int last);
    void onRowsRemoved(const QModelIndex& parent, int first, int last);
    void onRowsMoved(const QModelIndex& parent, int start, int end,
                     const QModelIndex& destination, int row);
    void onModelReset();

    PlaylistModel* m_model = nullptr; ///< non-owning; see the ctor
    QList<Origin>  m_origins;         ///< parallel to the model's rows
    /// Origins for the insert in flight: staged by insertFrom immediately
    /// before the model insert, consumed by onRowsInserted (synchronous, so
    /// the two can never interleave with another insert).
    QList<Origin>  m_staged;
};

} // namespace rawform
