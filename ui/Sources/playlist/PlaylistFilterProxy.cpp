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

// PlaylistFilterProxy.cpp
//
// See the header for the coordinate rule. The selection ops at the bottom are
// PlaylistModel's former bodies, moved here because the selection model they
// operate on is bound to this proxy; their rationale comments came with them.

#include "playlist/PlaylistFilterProxy.h"

#include "search/PlaylistSearch.h"

#include <QItemSelectionModel>
#include <QModelIndex>
#include <QVariant>

#include <algorithm>
#include <utility>
#include <vector>

namespace rawform {

PlaylistFilterProxy::PlaylistFilterProxy(PlaylistModel* source, QObject* parent)
    : QSortFilterProxyModel(parent), m_source(source) {
    setSourceModel(source);
    // The view-facing signals PlaylistModel emits by name, re-emitted so the
    // view's `Connections { target: model }` blocks see them on this model.
    connect(source, &PlaylistModel::columnLayoutChanged, this,
            &PlaylistFilterProxy::columnLayoutChanged);
    connect(source, &PlaylistModel::bulkRemovalStarted, this,
            &PlaylistFilterProxy::bulkRemovalStarted);
    connect(source, &PlaylistModel::bulkRemovalFinished, this,
            &PlaylistFilterProxy::bulkRemovalFinished);
}

bool PlaylistFilterProxy::filtering() const {
    return m_search && m_search->active() && m_search->model() == m_source;
}

void PlaylistFilterProxy::setSearch(PlaylistSearch* search) {
    if (search == m_search.data()) {
        return;
    }
    QObject::disconnect(m_searchConnection);
    m_search = search;
    if (search) {
        m_searchConnection = connect(search, &PlaylistSearch::matchesChanged, this,
                                     &PlaylistFilterProxy::refilter);
    }
    refilter();
}

void PlaylistFilterProxy::refilter() {
    // filtering() can flip in either direction here (a search installed or
    // removed, its query blanked or filled, its model rebound), so the
    // property is compared across the invalidate rather than assumed.
    const bool before = m_lastFiltering;
    invalidate();
    m_lastFiltering = filtering();
    if (m_lastFiltering != before) {
        emit filteringChanged();
    }
    emit filterApplied();
}

bool PlaylistFilterProxy::filterAcceptsRow(int sourceRow,
                                           const QModelIndex& sourceParent) const {
    Q_UNUSED(sourceParent);
    if (!filtering()) {
        return true;
    }
    return m_search->accepts(sourceRow);
}

QVariant PlaylistFilterProxy::data(const QModelIndex& index, int role) const {
    if (role == PlaylistModel::RowIndexRole) {
        // Stripe parity is a VIEW property: over a filtered list the visible
        // rows must still alternate, so the proxy row is the answer, not the
        // source row the source would report.
        return index.isValid() ? index.row() : -1;
    }
    return QSortFilterProxyModel::data(index, role);
}

// ---------------------------------------------------------------------------
// Row mapping
// ---------------------------------------------------------------------------

int PlaylistFilterProxy::mapRowToSource(int row) const {
    if (row < 0 || row >= rowCount()) {
        return -1;
    }
    const QModelIndex src = mapToSource(index(row, 0));
    return src.isValid() ? src.row() : -1;
}

int PlaylistFilterProxy::mapRowFromSource(int sourceRow) const {
    if (sourceRow < 0 || sourceRow >= m_source->rowCount()) {
        return -1;
    }
    const QModelIndex idx = mapFromSource(m_source->index(sourceRow, 0));
    return idx.isValid() ? idx.row() : -1;
}

int PlaylistFilterProxy::firstVisibleRowFromSource(int sourceRow) const {
    if (!filtering() || sourceRow < 0) {
        return sourceRow; // identity, and -1 ("never parked") stays -1
    }
    // Proxy rows are monotonic in source rows (no sorting), so the first
    // source row at or after the parked one that maps is the first visible
    // row below the parked position. A parked row beyond the source (the
    // file was written against a longer list) starts the walk at the end and
    // falls through to the last visible row, as the clamp would unfiltered.
    const int n = m_source->rowCount();
    for (int src = sourceRow; src < n; ++src) {
        const int px = mapRowFromSource(src);
        if (px >= 0) {
            return px;
        }
    }
    return rowCount() - 1;
}

QList<int> PlaylistFilterProxy::mapRowsToSource(const QList<int>& rows) const {
    QList<int> out;
    out.reserve(rows.size());
    for (const int row : rows) {
        const int src = mapRowToSource(row);
        if (src >= 0) {
            out.append(src);
        }
    }
    return out;
}

int PlaylistFilterProxy::mapGapToSource(int gap) const {
    if (filtering() || gap < 0 || gap >= rowCount()) {
        return m_source->rowCount();
    }
    return mapRowToSource(gap);
}

QItemSelection PlaylistFilterProxy::mapRowSpanFromSource(int first, int last) const {
    QItemSelection out;
    const int cols = columnCount();
    if (cols <= 0 || first > last) {
        return out;
    }
    first = std::max(first, 0);
    last  = std::min(last, m_source->rowCount() - 1);
    // Walk the source rows once; a hidden row (or a jump in the mapping)
    // closes the run being built. Proxy rows are monotonic in source rows
    // (no sorting), so a run is contiguous exactly when each mapped row is
    // the previous one plus one.
    int runLo = -1;
    int runHi = -1;
    for (int src = first; src <= last; ++src) {
        const int px = mapRowFromSource(src);
        if (px < 0) {
            continue;
        }
        if (runLo >= 0 && px == runHi + 1) {
            runHi = px;
            continue;
        }
        if (runLo >= 0) {
            out.select(index(runLo, 0), index(runHi, cols - 1));
        }
        runLo = px;
        runHi = px;
    }
    if (runLo >= 0) {
        out.select(index(runLo, 0), index(runHi, cols - 1));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Column ops (forwarded)
// ---------------------------------------------------------------------------

bool PlaylistFilterProxy::moveVisualColumn(int from, int to) {
    return m_source->moveVisualColumn(from, to);
}

int PlaylistFilterProxy::columnAlignment(int column) const {
    return m_source->columnAlignment(column);
}

QVariantList PlaylistFilterProxy::defaultColumnWidths() const {
    return m_source->defaultColumnWidths();
}

QStringList PlaylistFilterProxy::currentColumnOrder() const {
    return m_source->currentColumnOrder();
}

QVariantList PlaylistFilterProxy::applyColumnLayout(const QStringList& fieldIds,
                                                    const QVariantList& widths) {
    return m_source->applyColumnLayout(fieldIds, widths);
}

QVariantList PlaylistFilterProxy::availableColumns() const {
    return m_source->availableColumns();
}

QVariantList PlaylistFilterProxy::customColumnCatalog() const {
    return m_source->customColumnCatalog();
}

QString PlaylistFilterProxy::columnFieldId(int column) const {
    return m_source->columnFieldId(column);
}

bool PlaylistFilterProxy::showColumn(const QString& fieldId) {
    return m_source->showColumn(fieldId);
}

bool PlaylistFilterProxy::hideColumn(const QString& fieldId) {
    return m_source->hideColumn(fieldId);
}

bool PlaylistFilterProxy::isColumnShown(const QString& fieldId) const {
    return m_source->isColumnShown(fieldId);
}

// ---------------------------------------------------------------------------
// Row ops (mapped)
// ---------------------------------------------------------------------------

void PlaylistFilterProxy::removeTracks(const QVariantList& rows) {
    QVariantList sourceRows;
    sourceRows.reserve(rows.size());
    for (const QVariant& v : rows) {
        const int src = mapRowToSource(v.toInt());
        if (src >= 0) {
            sourceRows.append(src);
        }
    }
    m_source->removeTracks(sourceRows);
}

int PlaylistFilterProxy::removeUnavailableTracks() {
    return m_source->removeUnavailableTracks();
}

bool PlaylistFilterProxy::moveTracks(int first, int count, int dest) {
    if (filtering()) {
        return false;
    }
    return m_source->moveTracks(first, count, dest);
}

// ---------------------------------------------------------------------------
// Selection ops
// ---------------------------------------------------------------------------

QList<int> PlaylistFilterProxy::selectedRowList(QItemSelectionModel* sel) const {
    // Range walk + bitmap sweep: never per-cell. The bitmap doubles as the
    // dedupe and the sort, since the sweep emits ascending.
    QList<int> out;
    if (!sel || sel->model() != this) {
        return out;
    }
    const QItemSelection ranges = sel->selection();
    if (ranges.isEmpty()) {
        return out;
    }
    const int n = rowCount();
    std::vector<bool> mark(static_cast<size_t>(n), false);
    int selected = 0;
    for (const QItemSelectionRange& r : ranges) {
        const int top    = std::max(0, r.top());
        const int bottom = std::min(n - 1, r.bottom());
        for (int i = top; i <= bottom; ++i) {
            if (!mark[static_cast<size_t>(i)]) {
                mark[static_cast<size_t>(i)] = true;
                ++selected;
            }
        }
    }
    out.reserve(selected);
    for (int i = 0; i < n; ++i) {
        if (mark[static_cast<size_t>(i)]) {
            out.push_back(i);
        }
    }
    return out;
}

void PlaylistFilterProxy::selectRowRange(QItemSelectionModel* selection, int lo, int hi,
                                         bool clearFirst) {
    // Guard the wiring, not just the arguments: selecting THIS model's indexes
    // on a selection model bound to a DIFFERENT model (a stale QML binding
    // mid tab-switch) would corrupt that model's selection, so it is refused
    // outright rather than "best effort".
    if (!selection || selection->model() != this) {
        return;
    }
    const int rows = rowCount();
    const int cols = columnCount();
    if (rows <= 0 || cols <= 0) {
        return;
    }
    if (lo > hi) {
        std::swap(lo, hi);
    }
    lo = std::max(lo, 0);
    hi = std::min(hi, rows - 1);
    if (lo > hi) {
        return; // fully out of range: a no-op, never an implicit clear
    }
    // ONE full-width range, applied with ONE select(). Spanning every column
    // explicitly (instead of the Rows flag on a column-0 range) keeps the
    // STORED selection in the shape per-row selects produce, so per-cell
    // `selected` reads in the view and selectedIndexes() consumers (delete,
    // drag-block detection) are undisturbed, while the selection model holds a
    // single range and emits a single selectionChanged. That single emission
    // is the whole point: the metadata pane re-aggregation and the album-art
    // scan run once per GESTURE, not once per row (a per-row loop is
    // quadratic; observed at minutes for 5000 rows).
    const QItemSelection sel(index(lo, 0), index(hi, cols - 1));
    selection->select(sel, clearFirst ? QItemSelectionModel::ClearAndSelect
                                      : QItemSelectionModel::Select);
}

void PlaylistFilterProxy::beginAdditiveRangeDrag(QItemSelectionModel* selection) {
    // Same wrong-model refusal as selectRowRange: snapshotting a foreign
    // model's selection would seed the session with indexes select() below
    // silently drops, so the baseline would lie.
    if (!selection || selection->model() != this) {
        return;
    }
    m_additiveDragBase   = selection->selection();
    m_additiveDragActive = true;
}

void PlaylistFilterProxy::updateAdditiveRangeDrag(QItemSelectionModel* selection, int lo,
                                                  int hi) {
    if (!selection || selection->model() != this) {
        return;
    }
    // No open session: refuse. Applying against an empty baseline would be a
    // ClearAndSelect down to just the range, destroying the selection this
    // gesture exists to preserve.
    if (!m_additiveDragActive) {
        return;
    }
    const int rows = rowCount();
    const int cols = columnCount();
    if (rows <= 0 || cols <= 0) {
        return;
    }
    if (lo > hi) {
        std::swap(lo, hi);
    }
    lo = std::max(lo, 0);
    hi = std::min(hi, rows - 1);
    if (lo > hi) {
        return; // fully out of range: a no-op, same contract as selectRowRange
    }
    // Baseline UNION range, applied as ONE ClearAndSelect. merge() with
    // Select is the union operator: it splits/dedupes overlapping ranges, so
    // a drag sweeping across baseline rows never stores duplicate rows (which
    // would double-count in selectedRows() consumers, e.g. the metadata
    // pane's aggregate fields). One select() means one selectionChanged per
    // ROW CHANGE of the drag, the same batching discipline as selectRowRange.
    QItemSelection out = m_additiveDragBase;
    out.merge(QItemSelection(index(lo, 0), index(hi, cols - 1)),
              QItemSelectionModel::Select);
    selection->select(out, QItemSelectionModel::ClearAndSelect);
}

void PlaylistFilterProxy::endAdditiveRangeDrag() {
    // Drop the snapshot, not just the gate: its ranges hold persistent
    // indexes the model would otherwise keep updating on every row change
    // for as long as the (invisible) baseline lingered.
    m_additiveDragBase   = QItemSelection();
    m_additiveDragActive = false;
}

void PlaylistFilterProxy::reselectFullWidth(QItemSelectionModel* selection) {
    if (!selection || selection->model() != this) {
        return; // same wrong-model refusal as selectRowRange
    }
    const int cols = columnCount();
    if (cols <= 0) {
        return;
    }
    const QItemSelection cur = selection->selection();
    if (cur.isEmpty()) {
        return;
    }
    // Collect the selected ROW spans and merge overlapping/adjacent ones. The
    // stored ranges are normally disjoint (QItemSelectionModel maintains that
    // through merge), but this helper must not rely on it: hand-built overlaps
    // passed straight to select() would be stored as-is, and duplicate rows
    // then double-count in selectedRows() consumers (the metadata pane's
    // aggregate fields, e.g. Total size).
    QList<std::pair<int, int>> spans;
    spans.reserve(static_cast<int>(cur.size()));
    for (const QItemSelectionRange& r : cur) {
        if (r.isValid()) {
            spans.append({ r.top(), r.bottom() });
        }
    }
    if (spans.isEmpty()) {
        return;
    }
    std::ranges::sort(spans);

    QItemSelection full;
    int top = spans.first().first;
    int bot = spans.first().second;
    for (qsizetype i = 1; i < spans.size(); ++i) {
        const auto& s = spans.at(i);
        if (s.first <= bot + 1) { // overlapping or adjacent: extend the span
            bot = std::max(bot, s.second);
            continue;
        }
        full.append(QItemSelectionRange(index(top, 0), index(bot, cols - 1)));
        top = s.first;
        bot = s.second;
    }
    full.append(QItemSelectionRange(index(top, 0), index(bot, cols - 1)));

    // ONE replace, one selectionChanged; current index and anchor untouched.
    selection->select(full, QItemSelectionModel::ClearAndSelect);
}

QString
PlaylistFilterProxy::albumArtSourceForSelection(QItemSelectionModel* selection) const {
    if (!selection || selection->model() != this) {
        return {};
    }
    const QModelIndex cur = selection->currentIndex();
    const int currentSourceRow = cur.isValid() ? mapRowToSource(cur.row()) : -1;
    return m_source->albumArtSourceForRows(mapRowsToSource(selectedRowList(selection)),
                                           currentSourceRow);
}

} // namespace rawform
