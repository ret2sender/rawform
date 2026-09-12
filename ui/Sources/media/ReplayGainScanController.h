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

// =============================================================================
// ReplayGainScanController.h
//
// The thin Qt driver that runs the zero-Qt ReplayGainScanner off the GUI thread
// and reports progress back to QML. It is the scanning sibling of
// ReplayGainEditor: registered into the com.rawform.app module via QML_ELEMENT so
// the Properties window can instantiate it directly, and built on the same
// QtConcurrent + QFutureWatcher pattern.
//
// One important divergence from ReplayGainEditor's shape. The editor maps N
// independent file writes with QtConcurrent::mapped, where order does not matter.
// Scanning an album does: every track's loudness state must be measured by ONE
// scanner instance, in order, so they can be combined into a single album value.
// So this controller runs the whole batch as a single QtConcurrent::run task that
// walks the tracks sequentially, rather than a parallel map. Track mode uses the
// same sequential runner (a full decode per file is CPU-bound, so parallel decode
// mostly thrashes and would scramble the per-track progress readout).
//
// Responsibility boundary. This controller MEASURES and reports; it does not
// write. On completion it emits the measured values as an edit list (the same
// shape ReplayGainEditor::apply consumes), which the Properties pane stages into
// its cells; the user then commits through the pane's Apply / OK path, which is
// what actually calls ReplayGainEditor, MetadataReloader, and the cache flush.
// Keeping the write out of here means there is exactly one write path in the app.
//
// Threading. scan() takes plain { row, path, name } item maps, already
// snapshotted by the Properties pane from its own on-open copy (never resolved
// against the live playlist: rows are positions, and the playlist can mutate
// while the non-modal Properties window is open). It builds a plain-data Batch
// on the GUI thread (deduping subsongs by path) and hands it to the worker; the
// worker never touches a GUI-thread QObject. Progress is pushed back with a
// queued invokeMethod, and a std::atomic cancel flag is polled between tracks
// and, through the scanner's progress callback, between decode chunks.
// =============================================================================

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>  // QML_ELEMENT
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>

#include <atomic>

namespace rawform {

class ReplayGainScanController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /// Progress surface for the dialog. doneTracks is the 0-based index of the
    /// track currently being scanned (so a dialog shows doneTracks + 1 of
    /// totalTracks); trackFraction is 0..1 within that track (0 when the source
    /// length is unknown); overallProgress folds the two into one 0..1 bar value.
    Q_PROPERTY(int     totalTracks     READ totalTracks     NOTIFY progressChanged)
    Q_PROPERTY(int     doneTracks      READ doneTracks      NOTIFY progressChanged)
    Q_PROPERTY(qreal   trackFraction   READ trackFraction   NOTIFY progressChanged)
    Q_PROPERTY(qreal   overallProgress READ overallProgress NOTIFY progressChanged)
    Q_PROPERTY(QString currentName     READ currentName     NOTIFY progressChanged)

public:
    explicit ReplayGainScanController(QObject* parent = nullptr);
    ~ReplayGainScanController() override;

    /// Which gain to produce. ScanTrack measures each row independently; ScanAlbum
    /// additionally combines every scanned row into one shared album gain and peak
    /// written onto all of them (the selection is treated as one album).
    /// ScanAlbumsByTags partitions the batch into albums by each item's albumKey
    /// (built by the pane from the album artist / date / album tags, foobar's
    /// default grouping) and runs the ScanAlbum combine per group, one shared
    /// album value per album instead of one for the whole selection. Exposed to
    /// QML so the menu can pass it as a plain int.
    enum ScanMode { ScanTrack = 0, ScanAlbum = 1, ScanAlbumsByTags = 2 };
    Q_ENUM(ScanMode)

    [[nodiscard]] bool    busy() const { return m_busy; }
    [[nodiscard]] int     totalTracks() const { return m_totalTracks; }
    [[nodiscard]] int     doneTracks() const { return m_doneTracks; }
    [[nodiscard]] qreal   trackFraction() const { return m_trackFraction; }
    [[nodiscard]] QString currentName() const { return m_currentName; }
    [[nodiscard]] qreal   overallProgress() const {
        return (m_totalTracks > 0)
                   ? (static_cast<qreal>(m_doneTracks) + m_trackFraction) /
                         static_cast<qreal>(m_totalTracks)
                   : 0.0;
    }

    /// The batch's measured output, carried through the future. Kept in the header
    /// so QFutureWatcher can be a value member (its result type must be complete).
    /// results is a list of per-row edit maps { row:int, and any of trackGain /
    /// trackPeak / albumGain / albumPeak as strings }, exactly the shape the pane
    /// stages and ReplayGainEditor::apply consumes. failedPaths could not be opened
    /// or measured. canceled is true when the user aborted mid-batch (then results
    /// is empty: an album needs every track, so a cancel discards the lot).
    struct RunResult {
        QVariantList results;
        QStringList  failedPaths;
        bool         canceled = false;
    };

    /// Scan the given items off-thread in @p mode (a ScanMode value). @p items is
    /// a list of { row:int, path:string, name:string } maps built by the pane
    /// from its on-open snapshot; `path` identifies the file, `name` feeds the
    /// progress readout, and `row` is echoed verbatim into the result maps as the
    /// pane's staging round-trip key. In ScanAlbumsByTags mode each item also
    /// carries `albumKey` (opaque group identity; the pane guarantees it non-empty,
    /// with singleton sentinels for untagged rows, and a defensively empty key
    /// still forms a group of its own). Ignored when already busy or when nothing
    /// is scannable (pathless entries are skipped). Dedupes by path (subsongs
    /// share a file, so one physical file is decoded once and its result fanned
    /// out to every row that points at it, which also keeps the album combine
    /// from double counting); in ScanAlbumsByTags the dedupe is per GROUP, so a
    /// file whose subsongs land in different albums is decoded once per album it
    /// feeds (each group's combine needs its own pass over the file). Runs the
    /// scan on the global thread pool.
    Q_INVOKABLE void scan(const QVariantList& items, int mode);

    /// Request cancellation of the in-flight scan. The worker stops at the next
    /// track or decode-chunk boundary and finishes with canceled == true. Safe to
    /// call when not busy (it is then a no-op for the next scan, since scan() clears
    /// the flag before starting).
    Q_INVOKABLE void cancel();

signals:
    void busyChanged();
    void progressChanged();
    /// GUI thread, when the scan pass ends. See RunResult for the payload meaning.
    void finished(const QVariantList& results, bool canceled,
                  const QStringList& failedPaths);

private:
    /// One physical file to scan, plus the display name for the progress readout.
    struct ScanItem {
        QString path;
        QString name;
    };
    /// The plain-data work order handed to the worker thread. In ScanAlbumsByTags
    /// mode groupOf parallels items with each file's album-group index; items are
    /// flattened group-contiguous and groups numbered in first-seen order, so the
    /// worker detects an album boundary as a plain groupOf[i] != groupOf[i-1] and
    /// resets the scanner there. Empty in the other modes (which are single-group
    /// by definition: the whole batch, or no combine at all).
    struct Batch {
        QVector<ScanItem>     items;    ///< unique (path[, group]) in first-seen order
        QVector<QVector<int>> rowsFor;  ///< parallel: all rows sharing each item
        QVector<int>          groupOf;  ///< parallel: album-group index (ByTags only)
        int                   mode = ScanTrack;
    };

    /// Runs on a pool thread: the whole sequential decode-and-measure pass.
    RunResult runScan(Batch batch);

    /// Pushes a progress update to the GUI thread (queued). Cheap; the worker
    /// already throttles to whole-percent steps before calling.
    void postProgress(int index, int total, const QString& name, qreal frac);

    void onFinished();
    void setBusy(bool on);

    QFutureWatcher<RunResult> m_watcher;
    std::atomic<bool>         m_cancelRequested{false};

    bool    m_busy          = false;
    int     m_totalTracks   = 0;
    int     m_doneTracks    = 0;
    qreal   m_trackFraction = 0.0;
    QString m_currentName;
};

}  // namespace rawform
