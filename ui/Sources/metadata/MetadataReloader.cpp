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

#include "metadata/MetadataReloader.h"

#include "playlist/PlaylistModel.h"
#include "media/TrackReader.h"

#include <QFileInfo>
#include <QModelIndex>
#include <QModelIndexList>
#include <QSet>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

namespace rawform {
namespace {

/// Pool worker: stat-compare (unless forced) then re-read. Pure and stateless
/// (each call owns its own QFileInfo + FileRef inside readTrack), so it is safe
/// to fan across the thread pool.
///
///   - gone                         -> Missing
///   - present, mtime+size match    -> UnchangedPresent (and clears a stale flag)
///   - present, changed/forced, ok  -> Changed (fresh data)
///   - present but unreadable       -> Missing
ReloadResult reloadOne(const ReloadItem& item) {
    ReloadResult r;
    r.row = item.row;
    r.path = item.path;

    const QFileInfo fi(item.path);
    if (!fi.exists()) {
        r.verdict = ReloadVerdict::Missing;
        return r;
    }
    if (!item.force
        && fi.lastModified() == item.cachedModified
        && fi.size() == item.cachedSize) {
        r.verdict = ReloadVerdict::UnchangedPresent;
        return r;
    }

    TrackData fresh = readTrack(item.path);
    if (!fresh.valid) {
        r.verdict = ReloadVerdict::Missing; // exists but won't parse
        return r;
    }

    r.verdict = ReloadVerdict::Changed;
    r.data = std::move(fresh);
    return r;
}

} // namespace

MetadataReloader::MetadataReloader(PlaylistModel* model,
                                   QItemSelectionModel* selection,
                                   QObject* parent)
    : QObject(parent), m_model(model), m_selection(selection) {
    // Debounce so a held arrow key (selection changing every keystroke) settles
    // into ONE pass instead of dispatching pool work on every step.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_debounce, &QTimer::timeout, this, &MetadataReloader::revalidateSelection);

    connect(&m_watcher, &QFutureWatcher<ReloadResult>::resultsReadyAt,
            this, &MetadataReloader::onResultsReady);
    connect(&m_watcher, &QFutureWatcher<ReloadResult>::finished,
            this, &MetadataReloader::onFinished);
}

MetadataReloader::~MetadataReloader() {
    // A Properties window can be waiting on a reloadPaths token when the owning
    // tab (and this reloader with it) is torn down; nothing would ever deliver
    // those completions, and the window would sit in Applying forever. Flush
    // the obligations here, in OUR destructor body, while the signal
    // connections are still intact (QObject's own destructor is what severs
    // them, and it runs after this). The waiters' handlers guard their dead
    // tab references and just finish. The running pass itself needs no
    // cancellation: its workers hold plain snapshot data, and their results
    // are dropped with the watcher.
    if (m_activeToken >= 0)
        emit pathsReloaded(m_activeToken);
    for (const PathPass& pass : std::as_const(m_pathQueue))
        emit pathsReloaded(pass.token);
    m_pathQueue.clear();
}

void MetadataReloader::scheduleRevalidation() {
    m_debounce.start();
}

void MetadataReloader::revalidateSelection() {
    const QList<int> rows = selectedRowNumbers();
    if (rows.isEmpty() || rows.size() > kAutoRevalidateCap)
        return; // nothing to check, or too many to bother auto-checking
    dispatch(buildItems(rows, /*force=*/false), /*force=*/false);
}

void MetadataReloader::reloadSelected() {
    const QList<int> rows = selectedRowNumbers();
    if (!rows.isEmpty()) {
        // Targeted: the caller named these files, so force the re-read.
        dispatch(buildItems(rows, /*force=*/true), /*force=*/true);
        return;
    }
    // Nothing selected -> whole playlist, but CONDITIONAL: stat everything and
    // re-read only what changed (and gray what's gone), so "reload all" never
    // force-parses thousands of unchanged files.
    dispatch(buildItems(allRowNumbers(), /*force=*/false), /*force=*/false);
}

int MetadataReloader::reloadPaths(const QStringList& paths) {
    const int token = m_nextToken++;

    // Resolve which rows CURRENTLY hold these paths. All subsong rows of a
    // shared file are included: the file changed for every one of them.
    QList<ReloadItem> items;
    if (m_model && !paths.isEmpty()) {
        const QSet<QString> wanted(paths.cbegin(), paths.cend());
        const int n = m_model->rowCount();
        QList<int> rows;
        for (int r = 0; r < n; ++r) {
            const TrackData* td = m_model->trackAt(r);
            if (td && wanted.contains(td->filePath))
                rows << r;
        }
        items = buildItems(rows, /*force=*/true);
    }

    // An EMPTY resolution (no row currently holds any of these paths: the
    // tracks left the playlist since the caller snapshotted them) is
    // deliberately NOT special-cased into an immediate emission: it runs as a
    // normal, empty pass through the machinery below, so the token travels the
    // exact same lifecycle as a real one. Delivery is then always asynchronous
    // via the watcher (it can never fire re-entrantly before the caller has
    // stored the returned token), and the destructor's token flush covers it
    // if the tab dies first.

    if (m_busy) {
        // Queue behind the running pass. Never dropped: the caller holds this
        // token and waits on it (the conditional pending slot's latest-wins
        // rule would silently starve one of two concurrent Properties windows).
        m_pathQueue.append(PathPass{ token, std::move(items) });
        return token;
    }

    m_activeToken = token;
    start(std::move(items));
    return token;
}

void MetadataReloader::validateAll() {
    // Load-time cache audit: conditional whole-playlist sweep. Same shape as the
    // empty-selection reload; named for the load path that calls it.
    dispatch(buildItems(allRowNumbers(), /*force=*/false), /*force=*/false);
}

QList<ReloadItem> MetadataReloader::buildItems(const QList<int>& rows, bool force) const {
    QList<ReloadItem> items;
    if (!m_model)
        return items;
    items.reserve(rows.size());
    for (const int row : rows) {
        const TrackData* cached = m_model->trackAt(row);
        if (!cached || cached->filePath.isEmpty())
            continue;
        items.push_back(ReloadItem{ row, cached->filePath,
                                    cached->modified, cached->fileSize, force });
    }
    return items;
}

void MetadataReloader::dispatch(QList<ReloadItem> items, bool force) {
    if (items.isEmpty())
        return;

    if (m_busy) {
        // Hold the latest request, but never let a conditional pass clobber a
        // pending FORCE one (so an explicit reload can't be lost behind churn).
        if (force || !m_pendingForce) {
            m_pending = std::move(items);
            m_pendingForce = force;
            m_hasPending = true;
        }
        return;
    }
    start(std::move(items));
}

void MetadataReloader::start(QList<ReloadItem> items) {
    m_changed = 0;
    m_nextApply = 0;
    setBusy(true);
    // Move the sequence into the future so it owns the work for the whole run.
    m_watcher.setFuture(QtConcurrent::mapped(std::move(items), reloadOne));
}

void MetadataReloader::onResultsReady(int /*begin*/, int /*end*/) {
    drainReady();
}

void MetadataReloader::drainReady() {
    if (!m_model)
        return;
    const QFuture<ReloadResult> future = m_watcher.future();
    while (future.isResultReadyAt(m_nextApply)) {
        const ReloadResult r = future.resultAt(m_nextApply);
        ++m_nextApply;

        // Drift guard: only touch the row if it still holds this exact file, so a
        // concurrent insert/remove/move can never write onto the wrong row.
        if (r.row < 0 || m_model->trackFilePath(r.row) != r.path)
            continue;

        switch (r.verdict) {
        case ReloadVerdict::Changed:
            // Fresh data carries available = true by construction, so this also
            // clears a stale missing flag when a changed file reappears.
            m_model->refreshTrack(r.row, r.data);
            ++m_changed;
            break;

        case ReloadVerdict::Missing: {
            const TrackData* cur = m_model->trackAt(r.row);
            if (cur && cur->available) {
                TrackData flagged = *cur;
                flagged.available = false;
                m_model->refreshTrack(r.row, flagged);
                ++m_changed;
            }
            break;
        }

        case ReloadVerdict::UnchangedPresent: {
            // Nothing to re-read, but if the row was grayed and the file is back,
            // clear the flag so it un-grays (and its art can load again).
            const TrackData* cur = m_model->trackAt(r.row);
            if (cur && !cur->available) {
                TrackData restored = *cur;
                restored.available = true;
                m_model->refreshTrack(r.row, restored);
                ++m_changed;
            }
            break;
        }
        }
    }
}

void MetadataReloader::onFinished() {
    drainReady(); // defensive: pick up any results not yet seen via resultsReadyAt
    const int finishedToken = m_activeToken;
    m_activeToken = -1;
    if (m_changed > 0)
        emit refreshed();
    setBusy(false);
    // After setBusy(false), so a handler that immediately issues another
    // reloadPaths() sees an idle reloader and starts (or queues) cleanly.
    if (finishedToken >= 0)
        emit pathsReloaded(finishedToken);

    // Start the next pass, path-directed passes first: they are commands with a
    // caller waiting on their token, while the conditional pending slot is
    // best-effort churn. Defer to the event loop rather than re-entering
    // setFuture from inside this watcher's finished handler.
    if (!m_pathQueue.isEmpty()) {
        PathPass next = m_pathQueue.takeFirst();
        QMetaObject::invokeMethod(
            this,
            [this, next = std::move(next)]() mutable {
                if (m_busy) {
                    // Something started a pass in the meantime; go back to the
                    // FRONT so path passes keep their FIFO order.
                    m_pathQueue.prepend(std::move(next));
                    return;
                }
                m_activeToken = next.token;
                start(std::move(next.items));
            },
            Qt::QueuedConnection);
        return; // the conditional pending slot (if any) waits its turn
    }

    if (m_hasPending) {
        m_hasPending = false;
        const bool pendingForce = m_pendingForce;
        m_pendingForce = false;
        QList<ReloadItem> next = std::move(m_pending);
        m_pending.clear();
        // Route through dispatch() so it re-checks busy (in case something
        // started a pass in the meantime).
        QMetaObject::invokeMethod(
            this,
            [this, next = std::move(next), pendingForce]() mutable {
                dispatch(std::move(next), pendingForce);
            },
            Qt::QueuedConnection);
    }
}

QList<int> MetadataReloader::selectedRowNumbers() const {
    QList<int> rows;
    if (!m_selection)
        return rows;
    // selectedRows(0) yields one index per fully-selected row (we always select
    // with the Rows flag), so these are already distinct.
    const QModelIndexList indexes = m_selection->selectedRows(0);
    rows.reserve(indexes.size());
    for (const QModelIndex& idx : indexes)
        rows << idx.row();
    std::ranges::sort(rows);
    return rows;
}

QList<int> MetadataReloader::allRowNumbers() const {
    QList<int> rows;
    if (!m_model)
        return rows;
    const int n = m_model->rowCount();
    rows.reserve(n);
    for (int r = 0; r < n; ++r)
        rows << r;
    return rows;
}

void MetadataReloader::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

} // namespace rawform
