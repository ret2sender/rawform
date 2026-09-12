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

#include <QDateTime>
#include <QFutureWatcher>
#include <QItemSelectionModel>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

namespace rawform {

class PlaylistModel;

/// One row's reload job, snapshotted on the GUI thread and handed to a pool
/// worker. Carries the cached mtime/size so the worker can do the stat-compare
/// itself (off-thread); @ref force bypasses that check and forces a re-read.
struct ReloadItem {
    int       row = -1;
    QString   path;
    QDateTime cachedModified;
    qint64    cachedSize = 0;
    bool      force = false;
};

/// A worker's verdict for one row. Exactly one state per row. @ref path is the
/// file the verdict is about (used to guard against row drift before applying).
/// @ref data carries fresh tags only for the Changed verdict.
enum class ReloadVerdict {
    UnchangedPresent, ///< present, mtime+size match: re-reads nothing, clears any stale "missing" flag
    Changed,          ///< re-read produced fresh, valid data
    Missing,          ///< file gone, or present but unreadable
};

struct ReloadResult {
    int           row = -1;
    ReloadVerdict verdict = ReloadVerdict::UnchangedPresent;
    QString       path;
    TrackData     data;
};

/**
 * @brief Keeps the in-memory TrackData fresh against on-disk changes, off the
 *        GUI thread, and validates a freshly loaded .rwfpl cache against disk.
 *
 * Tracks are read from disk once (when added, or from the .rwfpl cache) and live
 * in the model thereafter, so editing a file in another app or a file going
 * missing between sessions leaves our copy stale. This detects that via the
 * mtime + fileSize key stored in TrackData (a stat() compare, no content read)
 * and re-parses only what changed.
 *
 * All disk work runs on the global thread pool (QtConcurrent::mapped over
 * readTrack, the scanner's parser), so no pass blocks the UI. The GUI thread
 * only snapshots the work (rows + cached keys) and applies results via
 * PlaylistModel::refreshTrack (a dataChanged emit, never a reset, so selection,
 * columns, metadata pane and art all refresh in place).
 *
 * Each row is classified into one of three verdicts and availability reconciled:
 * a present+unchanged row clears any stale "missing" flag, a changed row gets
 * fresh tags, and a gone/unreadable row is flagged unavailable
 * (TrackData::available = false) so the view grays it. This is uniform across the
 * four entry points:
 *
 *  - AUTOMATIC, on selection (conditional, debounced so a held arrow key settles
 *    into one pass; capped at kAutoRevalidateCap).
 *  - EXPLICIT, "Reload info from file(s)": a selection forces a re-read of those
 *    rows; an empty selection conditionally sweeps the whole playlist.
 *  - LOAD-TIME, @ref validateAll(): one conditional whole-playlist sweep right
 *    after a .rwfpl loads (trust the cache, skip the read for unchanged files,
 *    re-read only what changed, gray what is gone).
 *  - PATH-DIRECTED, @ref reloadPaths(): the Properties window's post-write
 *    refresh. Forces a re-read of the rows currently holding the given paths
 *    and reports completion per request via a token (see the method doc).
 *
 * Re-entrancy: one pass at a time. A conditional/selected request arriving
 * mid-pass is held pending (latest wins), except a pending FORCE request is
 * never clobbered by a later conditional one. Path-directed requests are the
 * exception to latest-wins: they queue FIFO and are never dropped, because each
 * has a caller waiting on its token. Results apply in input order, guarded
 * against row drift: a result applies only if its row still holds the same file
 * path, so a concurrent insert/remove/move never writes onto the wrong row.
 */
class MetadataReloader : public QObject {
    Q_OBJECT

    /// True while a reload/validation pass is running. Bound by QML to a status
    /// indicator.
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)

public:
    MetadataReloader(PlaylistModel* model, QItemSelectionModel* selection,
                     QObject* parent = nullptr);

    /// Flushes pathsReloaded for the active and queued path-directed passes so
    /// a waiter never hangs on a reloader that died under it (see the .cpp).
    ~MetadataReloader() override;

    [[nodiscard]] bool isBusy() const { return m_busy; }

    /// Restart the debounce; once it settles, the current selection is
    /// stat-checked off-thread and changed rows re-read. Wired to
    /// selectionChanged.
    void scheduleRevalidation();

    /// Explicit command. Selected rows -> force re-read; empty selection ->
    /// conditional sweep of the whole playlist. Bound to the menus.
    Q_INVOKABLE void reloadSelected();

    /// Path-directed force reload, for the (non-modal) Properties window after a
    /// tag write: force a re-read of every row currently holding one of
    /// @p paths, all subsong rows of a shared file included (the file itself
    /// changed for all of them). Paths, not rows, because the playlist can have
    /// reordered or shrunk since the window snapshotted its selection.
    ///
    /// Returns a token identifying this request; pathsReloaded(token) fires when
    /// exactly this pass has finished. An empty resolution (no row currently
    /// holds any of the paths) still runs as a normal, empty pass, so delivery
    /// is always asynchronous via the watcher and can never fire re-entrantly
    /// before the caller has stored the returned token. If this reloader is
    /// destroyed first (the owning tab closed), the destructor flushes the
    /// pending tokens so waiters finish instead of hanging. Unlike
    /// the conditional pending slot, path requests QUEUE FIFO while a pass is
    /// running and are never dropped, so two Properties windows applying
    /// near-simultaneously each get their own completion signal. Waiting on the
    /// token instead of refreshed() is what makes the window's apply sequencer
    /// safe: refreshed() is emitted only when something changed and can belong
    /// to an unrelated conditional pass that happened to be in flight.
    Q_INVOKABLE int reloadPaths(const QStringList& paths);

    /// Load-time cache validation: stat the WHOLE playlist (conditional), re-read
    /// changed files, and flag missing/unreadable rows as unavailable. Call once
    /// after a .rwfpl load has populated the model. Safe to call when idle or
    /// busy (it serializes like any other pass).
    void validateAll();

signals:
    /// Emitted once per pass, after any rows were re-read or flagged, so the
    /// metadata pane (which snapshots the selection's values) can re-aggregate.
    void refreshed();

    /// Emitted when the reloadPaths() pass identified by @p token has finished,
    /// whether or not anything changed (unlike refreshed). The Properties
    /// window's apply sequencer waits on its own token here.
    void pathsReloaded(int token);

    void busyChanged();

private slots:
    void revalidateSelection();
    void onResultsReady(int begin, int end);
    void onFinished();

private:
    /// Snapshot the cached keys for @p rows into work items. @p force forces a
    /// re-read regardless of the stat compare.
    [[nodiscard]] QList<ReloadItem> buildItems(const QList<int>& rows, bool force) const;
    /// Distinct, ascending row numbers currently selected.
    [[nodiscard]] QList<int> selectedRowNumbers() const;
    /// Every row index in the model, ascending.
    [[nodiscard]] QList<int> allRowNumbers() const;

    /// Start an async pass now, or hold it pending if one is already running.
    void dispatch(QList<ReloadItem> items, bool force);
    /// Begin mapping @p items on the pool (caller guarantees not busy).
    void start(QList<ReloadItem> items);
    /// Drain ready results in order, applying changed/missing rows that still match.
    void drainReady();
    void setBusy(bool on);

    PlaylistModel*               m_model;
    QItemSelectionModel*         m_selection;
    QTimer                       m_debounce;
    QFutureWatcher<ReloadResult> m_watcher;

    int  m_nextApply = 0;   ///< next result index to apply (in-order)
    int  m_changed = 0;     ///< rows actually updated this pass
    bool m_busy = false;

    bool             m_hasPending = false;
    bool             m_pendingForce = false;
    QList<ReloadItem> m_pending;

    /// One queued path-directed force pass (reloadPaths). These are commands
    /// with a caller waiting on their token, so unlike the single conditional
    /// pending slot above they queue FIFO and are never dropped or coalesced.
    struct PathPass {
        int               token = -1;
        QList<ReloadItem> items;
    };
    QList<PathPass> m_pathQueue;  ///< path passes waiting behind the running one
    int m_activeToken = -1;       ///< running pass's token; -1 = not a path pass
    int m_nextToken = 1;          ///< monotonic token source (0 is never issued)

    /// Auto-revalidation is skipped above this selection size; the explicit
    /// command and validateAll() have no cap.
    static constexpr int kAutoRevalidateCap = 512;
};

} // namespace rawform
