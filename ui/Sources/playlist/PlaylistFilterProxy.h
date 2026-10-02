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

// PlaylistFilterProxy.h
//
// The row filter between a tab's PlaylistModel and the playlist view: what
// the view, and the selection model bound to it, actually see. One per tab,
// built and owned by PlaylistTabs next to the model.
//
// TWO COORDINATE SPACES, one rule. The view and its QItemSelectionModel live
// in PROXY rows; everything else (the playback cursor, the scanner's insert
// cursor, the reloader, the Properties and Rename windows, the metadata
// pane) lives in SOURCE rows against the PlaylistModel. This class is the
// only place the two meet: it re-exports the row-taking invokables the view
// calls, mapping on the way through, and hands the view the mapping helpers
// it needs to talk to source-space consumers (mapRowToSource for playAt,
// mapRowsToSource for the dialogs, mapGapToSource for a drop). A consumer
// that reads rows off the selection model and hands them to the source
// model directly is a bug, and it only shows while a filter is active
// (unfiltered, the two spaces coincide), which is why the source's
// selection-taking helpers were moved here rather than left as traps.
//
// FILTER SOURCE. A PlaylistSearch (Edit > Find) installed through setSearch
// decides row visibility: filterAcceptsRow is its accepts(row). No search, an
// inactive one, or one bound to another model, means every row passes. Each
// matchesChanged re-runs the filter through invalidate(): ONE
// layoutAboutToBeChanged / layoutChanged pair, so the selection model remaps
// its ranges (rows now hidden drop out of the selection) and TableView
// rebuilds the viewport once; invalidateRowsFilter would instead emit one
// rowsRemoved/rowsInserted pair per contiguous run, thousands per keystroke
// on a scattered match set.
//
// WHAT QSortFilterProxyModel CHANGES UNDERNEATH THE VIEW. Source row and
// column MOVES arrive here as layoutAboutToBeChanged / layoutChanged, not as
// moves (Qt's proxy does not forward them as moves even unsorted). The
// selection survives (its ranges are persistent indexes) and TableView does
// the same viewport rebuild it does for a move. Row reorder by drag is refused
// while a filter is active (a gap between two visible rows has no single
// source position); drops append to the source end in that state.
//
// The three PlaylistModel signals the view listens to by name
// (columnLayoutChanged, bulkRemovalStarted, bulkRemovalFinished) are re-emitted
// here so `Connections { target: model }` in the view keeps working. RowIndexRole
// (zebra striping) is answered in proxy rows so stripes stay alternating over a
// filtered list. Not creatable from QML: PlaylistTabs constructs it and exposes
// it as activeView.

#pragma once

#include "playlist/PlaylistModel.h"

#include <QItemSelection>
#include <QList>
#include <QPointer>
#include <QQmlEngine> // QML_ELEMENT / QML_UNCREATABLE
#include <QSortFilterProxyModel>
#include <QString>
#include <QStringList>
#include <QVariantList>

class QItemSelectionModel;

namespace rawform {

class PlaylistSearch;

class PlaylistFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("PlaylistFilterProxy is created by PlaylistTabs, one per tab")

    /// The tab's PlaylistModel: the source-space handle the view passes to
    /// source-space consumers (AudioController::playAt, the dialogs' openFor).
    Q_PROPERTY(PlaylistModel* source READ source CONSTANT)
    /// True while an installed search hides rows (see setSearch). Row reorder
    /// and mid-list drops are refused in this state.
    Q_PROPERTY(bool filtering READ filtering NOTIFY filteringChanged)

public:
    /// Binds to @p source for life (the source never changes; a tab's model is
    /// its identity). Parented to @p parent, normally the source itself so the
    /// tab teardown cascades; the source's destroyed() detaches this proxy
    /// before the children are deleted, so the order is safe.
    explicit PlaylistFilterProxy(PlaylistModel* source, QObject* parent = nullptr);
    ~PlaylistFilterProxy() override = default;

    [[nodiscard]] PlaylistModel* source() const { return m_source; }
    [[nodiscard]] bool filtering() const;

    /// Install (or remove, with nullptr) the search that decides visibility.
    /// Re-filters at once, and on every matchesChanged of @p search.
    void setSearch(PlaylistSearch* search);

    /// RowIndexRole answered in PROXY rows (the stripe parity); everything
    /// else forwarded.
    [[nodiscard]] QVariant data(const QModelIndex& index,
                                int role = Qt::DisplayRole) const override;

    // --- Row mapping for the view ------------------------------------------

    /// The source row shown at proxy row @p row; -1 if out of range.
    Q_INVOKABLE [[nodiscard]] int mapRowToSource(int row) const;

    /// The proxy row showing source row @p sourceRow; -1 when hidden or out
    /// of range.
    Q_INVOKABLE [[nodiscard]] int mapRowFromSource(int sourceRow) const;

    /// mapRowToSource over a list; rows that do not map are dropped, so the
    /// result can be shorter than the input. The dialogs' openFor rows.
    Q_INVOKABLE [[nodiscard]] QList<int> mapRowsToSource(const QList<int>& rows) const;

    /// A proxy GAP (0..rowCount, the boundary before proxy row @p gap) as a
    /// source insert position: the source row of the visible row at the gap,
    /// or the source row count for the end gap and for any gap while
    /// filtering (a gap between visible rows has no single source position;
    /// appending is the predictable reading).
    Q_INVOKABLE [[nodiscard]] int mapGapToSource(int gap) const;

    /// The SOURCE row span [first, last] as a proxy-space selection: one
    /// full-width range per run of contiguous proxy rows (hidden rows split
    /// a run; a span entirely hidden maps to an empty selection). This is the
    /// row-aware twin of mapSelectionFromSource, which Qt implements by
    /// expanding the span to every CELL and emitting one range per cell: a
    /// 15k-row load selected through it handed select() 120k ranges, froze
    /// the UI for its merge, and left a selection of 120k fragments behind.
    /// Unfiltered, this returns the single range the source span is.
    [[nodiscard]] QItemSelection mapRowSpanFromSource(int first, int last) const;

    // --- Column ops, forwarded (column space is shared) --------------------
    Q_INVOKABLE bool moveVisualColumn(int from, int to);
    Q_INVOKABLE [[nodiscard]] int columnAlignment(int column) const;
    Q_INVOKABLE [[nodiscard]] QVariantList defaultColumnWidths() const;
    Q_INVOKABLE [[nodiscard]] QStringList currentColumnOrder() const;
    Q_INVOKABLE QVariantList applyColumnLayout(const QStringList& fieldIds,
                                               const QVariantList& widths);
    Q_INVOKABLE [[nodiscard]] QVariantList availableColumns() const;
    Q_INVOKABLE [[nodiscard]] QVariantList customColumnCatalog() const;
    Q_INVOKABLE [[nodiscard]] QString columnFieldId(int column) const;
    Q_INVOKABLE bool showColumn(const QString& fieldId);
    Q_INVOKABLE bool hideColumn(const QString& fieldId);
    Q_INVOKABLE [[nodiscard]] bool isColumnShown(const QString& fieldId) const;

    // --- Row ops, mapped ---------------------------------------------------

    /// PlaylistModel::removeTracks over PROXY rows (mapped; unmapped rows are
    /// ignored, as out-of-range ones are there).
    Q_INVOKABLE void removeTracks(const QVariantList& rows);

    /// PlaylistModel::removeUnavailableTracks, forwarded (no coordinates).
    Q_INVOKABLE int removeUnavailableTracks();

    /// PlaylistModel::moveTracks in PROXY coordinates. Identity while not
    /// filtering; refused (false, nothing moved) while filtering, see the
    /// file comment.
    Q_INVOKABLE bool moveTracks(int first, int count, int dest);

    // --- Selection ops (the selection model is bound to THIS model) --------
    // The bodies and contracts are PlaylistModel's former ones, moved here
    // unchanged apart from the coordinate space; see each for the why.

    /// The selection's distinct row numbers, ascending, from its RANGES (never
    /// per-cell: a library-sized selection materialized cell by cell in JS
    /// was quadratic). Null / empty / foreign selection returns empty.
    Q_INVOKABLE [[nodiscard]] QList<int> selectedRowList(QItemSelectionModel* sel) const;

    /// Select rows [lo, hi] in ONE select(), full width; the multi-row
    /// selection entry point (Ctrl+A, Shift ranges). @p clearFirst replaces,
    /// else adds. Clamped; a fully out-of-range request is a no-op, never a
    /// clear. Does not touch the current index. Refused for a foreign
    /// selection model.
    Q_INVOKABLE void selectRowRange(QItemSelectionModel* selection, int lo, int hi,
                                    bool clearFirst);

    /// Re-assert the current selection as merged full-width row spans in ONE
    /// select(); needed after showColumn (stored ranges predate the new
    /// column). Current index and anchor untouched.
    Q_INVOKABLE void reselectFullWidth(QItemSelectionModel* selection);

    /// The Ctrl+select-drag session: begin snapshots the baseline, update
    /// re-applies baseline UNION [lo, hi] in ONE ClearAndSelect (so a
    /// reversing drag shrinks truthfully), end drops the snapshot. update
    /// refuses outside a session so a stray call can never collapse the
    /// selection to the range alone.
    Q_INVOKABLE void beginAdditiveRangeDrag(QItemSelectionModel* selection);
    Q_INVOKABLE void updateAdditiveRangeDrag(QItemSelectionModel* selection, int lo,
                                             int hi);
    Q_INVOKABLE void endAdditiveRangeDrag();

    /// The art pane's selection resolver: the selection's rows and current
    /// row mapped to source, then PlaylistModel::albumArtSourceForRows. "" for
    /// a null / foreign / empty selection.
    Q_INVOKABLE [[nodiscard]] QString
    albumArtSourceForSelection(QItemSelectionModel* selection) const;

signals:
    void filteringChanged();

    /// Emitted after each re-filter has been applied (the layoutChanged is
    /// done, the selection model has remapped). The Find dialog's cue to
    /// place the current row.
    void filterApplied();

    // Re-emissions of the source's view-facing signals (see the file comment).
    void columnLayoutChanged();
    void bulkRemovalStarted();
    void bulkRemovalFinished();

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow,
                                        const QModelIndex& sourceParent) const override;

private:
    /// Re-run the filter (invalidate) and announce it.
    void refilter();

    PlaylistModel*           m_source;
    QPointer<PlaylistSearch> m_search;
    QMetaObject::Connection  m_searchConnection;
    /// filtering() as of the last refilter, so filteringChanged can be
    /// emitted only on a real flip.
    bool                     m_lastFiltering = false;
    /// The additive-drag baseline and gate (see beginAdditiveRangeDrag).
    QItemSelection m_additiveDragBase;
    bool           m_additiveDragActive = false;
};

} // namespace rawform
