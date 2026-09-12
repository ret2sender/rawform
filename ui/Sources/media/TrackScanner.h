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

#pragma once

#include "media/TrackData.h"

#include <QFutureWatcher>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>

namespace rawform {

/**
 * @brief Off-thread file ingestion: dialog/drop URLs to per-file readTrack to
 *        batched results marshaled back to the GUI thread. Nothing it does on
 *        the GUI thread touches the disk, so adding a huge folder never freezes
 *        the UI.
 *
 * Model-free on purpose: it only emits results, and PlaylistTabs decides which
 * tab's model receives them. That keeps the "which playlist" decision a single
 * wiring seam, correct across tab switches.
 *
 * Two phases, both off the GUI thread:
 *   1. ENUMERATE (QtConcurrent::run): walk folders recursively, apply the
 *      audio-extension allowlist, expand .m3u/.m3u8 (merged with the walk,
 *      deduped by canonical path), natural-sort. Yields an ordered QStringList.
 *   2. SCAN (QtConcurrent::mapped): fan the per-file readTrack (TagLib, then
 *      the engine's FFmpeg probe, plus QFileInfo) across the global pool.
 *      mapped indexes by INPUT position, so output order matches the
 *      enumerated order regardless of which worker finishes first.
 *
 * Marshal: a QFutureWatcher delivers results on the GUI thread; we drain them in
 * order via a "next to emit" cursor (never blocking on one not ready yet) and a
 * coalescing QTimer flushes the buffer as one contiguous tracksReady() batch, so
 * thousands of files become a handful of model insertions.
 *
 * Re-entrancy: a scan requested while one is running is queued and started on
 * finish (one at a time; nothing is dropped).
 */
class TrackScanner : public QObject {
    Q_OBJECT

    /// True from the moment a scan request starts enumerating until its scan
    /// finishes (and stays true across a queued follow-on). Bound by QML to a
    /// minimal status indicator.
    Q_PROPERTY(bool scanning READ isScanning NOTIFY scanningChanged)

public:
    explicit TrackScanner(QObject* parent = nullptr);
    ~TrackScanner() override = default;

    [[nodiscard]] bool isScanning() const { return m_scanning; }

    /// Ingest the given URLs (local files, folders, and/or .m3u playlists).
    /// @p at is the row results are inserted before; pass rowCount() or -1 to
    /// append. The scanner only echoes @p at back via scanStarted(); the caller
    /// owns its meaning against the model. Folders are walked recursively; .m3u
    /// contents are merged in (deduped).
    Q_INVOKABLE void scan(const QList<QUrl>& urls, int at = -1);

signals:
    /// A request actually began (after any queue wait), so the receiver can
    /// resolve @p requestedAt against the model's current rowCount.
    void scanStarted(int requestedAt);

    /// A contiguous, in-order batch ready to insert, on the GUI thread. Consumed
    /// in C++ (carries TrackData by value); never crosses into QML.
    void tracksReady(QList<rawform::TrackData> batch);

    /// A request's scan completed: @p added valid tracks emitted, @p skipped
    /// unreadable/corrupt files dropped.
    void scanFinished(int added, int skipped);

    /// Progress for the RUNNING request, on the GUI thread. @p total is
    /// the enumerated FILE count (known once phase 1 completes; a single
    /// (0, total) is emitted right then), @p done the in-order drain cursor,
    /// re-emitted whenever it advances and finalized at (total, total) before
    /// scanFinished. Files, not valid tracks: a skipped/unreadable file still
    /// advances @p done, so a consumer's bar always reaches the end. During
    /// the enumeration walk no total exists yet and nothing is emitted.
    void scanProgress(int done, int total);

    void scanningChanged();

private slots:
    void onEnumerated();
    void onResultsReady(int begin, int end);
    void onScanFinished();
    void flush();

private:
    void startRequest(QStringList inputs, int at);
    void setScanning(bool on);

    struct PendingRequest {
        QStringList inputs;
        int         at = -1;
    };

    QFutureWatcher<QStringList> m_enumWatcher; ///< phase 1 (folder/m3u expansion)
    QFutureWatcher<TrackData>   m_scanWatcher; ///< phase 2 (per-file readTrack)
    QTimer                      m_flushTimer;  ///< coalesces results into batches

    QList<TrackData>      m_buffer;         ///< drained-but-not-yet-emitted results
    QList<PendingRequest> m_queue;          ///< requests waiting behind the active one
    int                   m_nextEmit = 0;   ///< next result index to drain (in-order)
    int                   m_total = 0;      ///< enumerated file count (0 until phase 1 ends)
    int                   m_requestedAt = -1; ///< echoed via scanStarted
    int                   m_added = 0;      ///< valid tracks emitted this request
    int                   m_skipped = 0;    ///< files dropped this request
    bool m_scanning = false;
};

} // namespace rawform
