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

#include "columns/ColumnSchema.h"
#include "media/TrackData.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QItemSelectionModel> // selectedRowList's parameter type
#include <QList>
#include <QQmlEngine>  // QML_ELEMENT / QML_ANONYMOUS macros
#include <QStringList>
#include <QVariantList>

// Forward-declared (a plain pointer param on one invokable below). It is a known
// Qt meta-type, so moc needs only the name here; the full include lives in the
// .cpp where selectedRows()/currentIndex() are actually called.
class QItemSelectionModel;

namespace rawform {

class CustomColumnRegistry; // owns the user-defined custom columns (injected, read-only here)

/**
 * @brief Table model backing the playlist view: rows are tracks, columns are
 *        the visible playlist fields.
 *
 * Tracks are held by value in a QList<TrackData> (no manual heap ownership);
 * swapping the active playlist is a single setTracks() reset.
 *
 * Columns are schema-driven. The model owns a ColumnSchema (the catalog of
 * offerable fields), but WHICH columns show, in WHAT order, is a separate
 * mutable overlay: m_columns, one VisualColumn per visible column, each naming
 * either a native field or a custom-column id. headerData(), data(),
 * columnAlignment() and defaultColumnWidths() all read through that list via
 * columnAt(), so "what is at visual column N" is resolved in one place, and the
 * native/custom split with it. Custom columns are not catalog entries; their
 * definitions live in the injected CustomColumnRegistry, resolved by id at
 * render time.
 *
 * Interactive reordering is moveVisualColumn(), a begin/endMoveColumns()
 * permutation (a MOVE, not a reset) so the bound QItemSelectionModel remaps and
 * the metadata pane is undisturbed. Persistence is by field id (bare for native,
 * "custom:<id>" for custom), never by position, so a saved order survives the
 * user editing the schema or custom columns between sessions.
 *
 * I/O-free: it receives a ready ColumnSchema via setSchema() (loaded in
 * main.cpp). Registered into the com.rawform.app QML module via QML_ELEMENT.
 */
class PlaylistModel : public QAbstractTableModel {
    Q_OBJECT
    QML_ELEMENT

    /// Bumped on every in-place row refresh (refreshTrack), so a QML binding can
    /// depend on it to re-evaluate when row data changes underneath it. Used by
    /// the album-art source: dataChanged alone won't re-run a binding whose
    /// value comes from an invokable (albumArtSourceForSelection /
    /// albumArtSourceForRow), so a reloaded cover needs this nudge.
    Q_PROPERTY(int dataRevision READ dataRevision NOTIFY dataRevisionChanged)

public:
    /// Custom data roles for QML delegates. DisplayRole is the per-column text;
    /// the extras let a delegate query whole-row state without re-deriving it.
    enum Roles {
        DisplayRole = Qt::DisplayRole,
        RowIndexRole = Qt::UserRole + 1, ///< source-row index (int), for striping/selection
        RowAvailableRole = Qt::UserRole + 2, ///< bool: backing file present/readable (grays missing rows)
        /// int Qt::Alignment for the COLUMN (same value for every row and for
        /// the header section). Exists so header cells take alignment as an
        /// injected role refreshed by headerDataChanged, the same channel that
        /// refreshes their titles (the scenario this prevents: an alignment
        /// read through an invokable plus a revision kick does not re-evaluate
        /// on in-place header cells after a column move, so cells keep their
        /// pre-move by-index alignment until a tab switch rebuilds them).
        ColumnAlignmentRole = Qt::UserRole + 3,
    };

    explicit PlaylistModel(QObject* parent = nullptr);
    ~PlaylistModel() override = default;

    // --- QAbstractTableModel interface -----------------------------------
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // --- Column schema ---------------------------------------------------

    /// Install the column layout, resetting the model and the visual order to the
    /// schema's authored order. Call before assigning the model to the view.
    void setSchema(ColumnSchema schema);

    /// Inject the app-global custom-column registry (owned elsewhere; read-only
    /// here). The model resolves a shown custom column's name/alignment/pattern
    /// through it and reacts to its signals (a definition edit refreshes the
    /// shown column live; a removal drops the bound column). Call once after
    /// setSchema(), before the view binds. nullptr is valid (no custom columns).
    void setCustomColumns(const CustomColumnRegistry* registry);

    /// Move the column at visual index @p from to index @p to, permuting the
    /// order overlay. A begin/endMoveColumns() MOVE (never a reset) so the bound
    /// selection model remaps and selection/current/anchor survive. No-op
    /// (false) for out-of-range or from==to.
    ///
    /// Not named moveColumn(): QAbstractItemModel exposes an invokable
    /// moveColumn(QModelIndex,int,QModelIndex,int), and the clash makes a QML
    /// call resolve to that 4-arg overload. The distinct name avoids it.
    Q_INVOKABLE bool moveVisualColumn(int from, int to);

    /// Horizontal alignment for a column as a Qt::AlignmentFlag value, so both
    /// the header and cell delegates align from metadata, not column-index
    /// checks. Qt::AlignLeft for out-of-range columns.
    Q_INVOKABLE [[nodiscard]] int columnAlignment(int column) const;

    /// Seed widths, in visual order, for the view to apply as initial explicit
    /// column widths (from defaultColumnArrangement(), or kDefaultColumnWidth for
    /// any other field toggled on). User resizing overrides and persists per
    /// playlist (.rwftp); this is only the starting point.
    Q_INVOKABLE [[nodiscard]] QVariantList defaultColumnWidths() const;

    // --- Per-playlist layout persistence (.rwfpl) ------------------------
    // Keyed by field id rather than position so it survives schema edits. The
    // save path pairs currentColumnOrder()[v] with the view's
    // currentColumnWidths()[v] (same visual order); the load path hands both back
    // to applyColumnLayout().

    /// The active column order as canonical field-id strings, in visual order
    /// (leftmost first). The companion to the view's currentColumnWidths().
    Q_INVOKABLE [[nodiscard]] QStringList currentColumnOrder() const;

    /// Restore a saved layout, reconciling against the current schema:
    ///   - listed fields still present are placed in the given order;
    ///   - listed fields the schema no longer has are dropped;
    ///   - a non-empty list is taken literally (visible set + order), so a field
    ///     the user hid that isn't listed stays hidden;
    ///   - an empty list takes all schema defaults (fresh playlist / reset).
    /// (Duplicate field ids are consumed left-to-right.) Sets the order overlay
    /// and returns the widths re-sequenced to the resulting visual order, each
    /// new/unmatched column seeded from its schema default, so the view applies
    /// them positionally without desync. @p widths is parallel to @p fieldIds.
    /// Resets the model (load-time op; no live selection to preserve).
    Q_INVOKABLE QVariantList applyColumnLayout(const QStringList& fieldIds,
                                               const QVariantList& widths);

    /// The full column catalog for the header right-click menu: one entry per
    /// schema field as { fieldId, title, width, visible } (`visible` reflecting
    /// whether it is currently shown). The view toggles a field by rebuilding the
    /// order/width lists and calling applyColumnLayout().
    Q_INVOKABLE [[nodiscard]] QVariantList availableColumns() const;

    /// The custom-column catalog for the menu's "Custom columns" section: one map
    /// per defined custom column { fieldId ("custom:<id>"), title (its name),
    /// width (seed), visible }. Parallels availableColumns() so the same QML
    /// toggle path drives it. Empty when no registry is set or none are defined.
    Q_INVOKABLE [[nodiscard]] QVariantList customColumnCatalog() const;

    /// The canonical field-id of the column at @p column (visual index), or empty
    /// if out of range. "custom:<id>" for a custom column. Used by the header
    /// menu to address a column.
    Q_INVOKABLE [[nodiscard]] QString columnFieldId(int column) const;

    // --- Playlist data management ----------------------------------------

    /// Replace the entire track list, resetting the model.
    void setTracks(QList<TrackData> tracks);

    /// Insert @p tracks so the first lands at row @p at (0..rowCount; out of
    /// range appends). A begin/endInsertRows insertion, never a reset, so the
    /// selection model and metadata pane are undisturbed. The ingestion entry
    /// point: TrackScanner's batches arrive here through
    /// PlaylistTabs::onTracksReady at an advancing cursor. Empty list is a
    /// no-op.
    void insertTracks(int at, QList<TrackData> tracks);

    /// Remove the tracks at the given row indices (any order; out-of-range and
    /// duplicates ignored). Deletes in contiguous runs bottom-up via
    /// begin/endRemoveRows, so the selection model remaps; a removal, never a
    /// reset.
    ///
    /// Not named removeRows(): clashes with QAbstractItemModel's invokable
    /// removeRows(int,int,QModelIndex) (same trap as moveVisualColumn).
    Q_INVOKABLE void removeTracks(const QVariantList& rows);

    /// Remove every row flagged unavailable (the "remove missing tracks"
    /// cleanup). Deletes in contiguous runs like removeTracks(). Returns the
    /// number removed (0 if none).
    Q_INVOKABLE int removeUnavailableTracks();

    /// Move the contiguous run of @p count tracks starting at @p first to the
    /// boundary @p dest (0..rowCount, current coordinates: the gap before row
    /// @p dest). A begin/endMoveRows MOVE, never a reset, so the selection model
    /// carries the moved rows. No-op (false) for an invalid range or an in-place
    /// destination (dest within [first, first+count]).
    ///
    /// Not named moveRows(): shadows QAbstractItemModel's invokable moveRows()
    /// (same trap as moveVisualColumn / removeTracks).
    Q_INVOKABLE bool moveTracks(int first, int count, int dest);

    /// Replace the TrackData at @p row in place and emit dataChanged for the
    /// whole row (no reset, no move); bumps dataRevision so the album-art binding
    /// re-evaluates. Out-of-range is a no-op. The freshness path: MetadataReloader
    /// re-reads a changed file and pushes it here.
    void refreshTrack(int row, TrackData track);

    [[nodiscard]] int dataRevision() const { return m_dataRevision; }

    /// A by-value copy of the whole track list, in row order. The save path
    /// snapshots this on the GUI thread and hands the copy to a worker to
    /// serialize, so the live list is never touched off-thread.
    [[nodiscard]] QList<TrackData> snapshotTracks() const;

    /// Read-only access to a single track. nullptr if @p row is out of range.
    [[nodiscard]] const TrackData* trackAt(int row) const;

    /// The absolute file path at @p row (empty if out of range). The key the
    /// album-art provider is addressed by (artUrlForRow builds the URL), and
    /// the identity the reloader and the playback controller compare rows by.
    /// C++ callers only (MetadataReloader, AudioController); no QML caller, so
    /// not invokable.
    [[nodiscard]] QString trackFilePath(int row) const;

    /// ReplayGain data for the given rows, for the Properties window's
    /// ReplayGain pane. Returns one map per valid row: { row, path, name,
    /// albumKey, trackGain, albumGain, trackPeak, albumPeak } with the
    /// gains/peaks as display strings (verbatim tag value with a normalized dB
    /// unit; blank when absent). `path` is the track's durable write identity:
    /// the model threads it through its edit maps so ReplayGainEditor and the
    /// scanner address FILES, never positions, and a playlist reorder between
    /// opening the Properties window and Apply cannot retarget a write. `row`
    /// stays purely as the opaque staging round-trip key. Out-of-range rows
    /// are skipped. C++-only: ReplayGainRowsModel::setSelection is the sole
    /// caller (it ingests this list inside one model reset and computes the
    /// Summary from the strings), so there is no Q_INVOKABLE: the QML-facing
    /// API stays an honest map of what QML actually uses.
    [[nodiscard]] QVariantList replayGainRows(const QVariantList& rows) const;

    /// Durable identity keys for the given rows: one { path, subsong } map per
    /// valid row, in input order. The Properties window snapshots these on open
    /// so a later re-pull can find the same tracks again after the playlist
    /// mutated (rows are positions, not identities; subsong disambiguates
    /// siblings sharing one physical file). Out-of-range rows are skipped.
    Q_INVOKABLE [[nodiscard]] QVariantList trackKeys(const QVariantList& rows) const;

    /// The selection's distinct row numbers, ascending, straight from the
    /// selection model's RANGES (the scenario this prevents: QML materializing
    /// ItemSelectionModel.selectedIndexes, one QModelIndex wrapper per CELL,
    /// rows x columns JS allocations on a library-sized selection, then
    /// deduping rows in JS). A QItemSelection is a list of
    /// rectangular ranges, so the walk here is O(ranges + selected rows) with
    /// zero per-cell cost, and the bitmap sweep returns the rows already
    /// sorted and unique. Rows outside the model (a stale selection mid-
    /// mutation) are clamped away. Null / empty selection returns empty.
    Q_INVOKABLE [[nodiscard]] QList<int> selectedRowList(QItemSelectionModel* sel) const;

    /// Resolve identity keys (as produced by trackKeys) back to CURRENT row
    /// numbers, in key order. Keys whose track has left the playlist are
    /// skipped, so the result may be shorter than the input. Duplicate playlist
    /// entries are consumed greedily in ascending row order: N identical keys
    /// map to N distinct rows while the playlist still holds that many copies.
    Q_INVOKABLE [[nodiscard]] QVariantList rowsForKeys(const QVariantList& keys) const;

    /// The album-art URL to show for a multi-selection, or an empty string when
    /// the pane should clear to its placeholder. This is the single decision
    /// point behind the art frame: QML hands us the active tab's selection model
    /// and binds the frame's source straight to the result.
    ///
    /// The pane shows art only when EVERY selected row belongs to the same album,
    /// so selecting one album shows its cover and selecting across albums clears
    /// it. "Same album" is decided by a per-row identity key (see albumArtKey in
    /// the .cpp): the album tag (album title + album artist + year, case-folded)
    /// when tagged, falling back to the row's folder path when untagged so loose
    /// untagged files in different folders never collapse to one album. The check
    /// is pure field/path comparison (no filesystem stat), so a select-all over a
    /// large playlist stays cheap.
    ///
    /// When the selection is uniform, the URL is built for a REPRESENTATIVE row,
    /// the current/anchor index if it is in the selection and present, else the
    /// first present selected row, and resolved by AlbumArtProvider exactly as a
    /// single selection is (sidecar cover first, then the embedded picture). The
    /// "?v=" staleness tag is folded in so a re-read cover reloads. Returns "" for
    /// a null/empty selection, a non-uniform selection, or when no representative
    /// row is present/readable. A null currentIndex with an empty selection set
    /// is treated as "nothing selected"; a valid currentIndex with an otherwise
    /// empty set is treated as a single-row selection (so a click that did not
    /// register as a range selection still shows that row's art).
    Q_INVOKABLE [[nodiscard]] QString albumArtSourceForSelection(QItemSelectionModel* selection) const;

    /// Cover URL for a single row, resolved exactly like a one-row selection
    /// (same provider scheme, same "?v=" staleness tag); "" for an
    /// out-of-range row or a missing/unreadable file (which would only resolve
    /// to the placeholder anyway). Exists for the art pane's NOW-PLAYING
    /// fallback: when the active tab's selection is empty (a fresh tab, a
    /// dropped m3u, nothing clicked yet), the pane shows the playing track's
    /// cover via playingModel/playingRow instead of clearing.
    Q_INVOKABLE [[nodiscard]] QString albumArtSourceForRow(int row) const;

    /// Select the contiguous row range [lo, hi] in ONE QItemSelectionModel
    /// operation. This is the multi-row selection entry point (Ctrl+A,
    /// Shift-click/Shift-arrow ranges); QML must never loop per-row select()
    /// calls for a range.
    ///
    /// Why this exists: a per-row select() loop is quadratic-to-cubic. Each
    /// call (a) merges another single-row range into the stored QItemSelection,
    /// Qt does not coalesce adjacent ranges, so after k calls the selection
    /// holds k fragments and every operation on it scans them; and (b) emits
    /// selectionChanged, which re-runs the whole downstream pipeline
    /// (selectedRows() over the fragmented set, the metadata pane's full
    /// TrackData copy + re-aggregation, the album-art uniformity scan) once per
    /// row instead of once per gesture. Measured: Ctrl+A over 5000 rows took
    /// minutes. This helper builds ONE full-width range, applies it with ONE
    /// select(), so the stored selection is a single range and the pipeline
    /// runs exactly once.
    ///
    /// @p clearFirst true replaces the selection (ClearAndSelect, still a
    /// single selectionChanged); false adds the range to it. lo/hi are clamped
    /// to the model; a fully out-of-range or empty request is a no-op (never a
    /// clear, so a stray call can't destroy a selection). Does NOT touch the
    /// current index; the caller owns current/anchor semantics.
    Q_INVOKABLE void selectRowRange(QItemSelectionModel* selection,
                                    int lo, int hi, bool clearFirst);

    /// Show the column @p fieldId (a native field id, or "custom:<id>") by
    /// APPENDING it as the last visual column, via begin/endInsertColumns. A
    /// column-granular structural op, never a reset: the bound selection model
    /// remaps (selection/current/anchor survive) and the AudioController's
    /// playback cursor is untouched, which is why the interactive header-menu
    /// toggle must come here and NOT through applyColumnLayout (whose reset is
    /// a load-time semantic: it clears the selection and detaches the playback
    /// cursor, killing the green marker and track continuation).
    ///
    /// Returns false, with nothing changed, when the id is unknown/retired,
    /// names a custom column with no live definition, or is already shown.
    /// Width is the VIEW's concern (the header owns explicit widths); the
    /// caller seeds the new last column's width itself.
    ///
    /// Note for callers holding a multi-row selection: existing selection
    /// ranges do not cover a freshly appended column, so its cells would render
    /// unselected; follow up with reselectFullWidth() (one batched op).
    Q_INVOKABLE bool showColumn(const QString& fieldId);

    /// Hide the visual column bound to @p fieldId, via begin/endRemoveColumns
    /// (same granular-not-reset rationale as showColumn; the selection model
    /// shrinks its ranges itself, no fix-up needed). Refuses (returns false)
    /// when the id is not currently shown, or when it is the LAST remaining
    /// column (the header must never go blank).
    ///
    /// The playback-cursor caveat: the cursor is a QPersistentModelIndex pinned
    /// to column 0, and removing visual column 0 invalidates it even though the
    /// row is untouched; AudioController re-anchors it across
    /// columnsAboutToBeRemoved/columnsRemoved (see onPlayingColumns* there).
    Q_INVOKABLE bool hideColumn(const QString& fieldId);

    /// True when @p fieldId (native or "custom:<id>") is currently a visible
    /// column of THIS model. Exists for the header menu's check marks: they
    /// re-query this at popup time instead of trusting a `visible` flag
    /// snapshotted into the catalog arrays, so a check mark can never show
    /// another tab's (or an earlier open's) state.
    Q_INVOKABLE [[nodiscard]] bool isColumnShown(const QString& fieldId) const;

    /// Cover-art cache-buster. The "?v=" tag in art URLs is the AUDIO file's
    /// mtime+size, which cannot see a changed SIDECAR cover (folder.jpg
    /// overwritten while the track file is untouched): the rebuilt URL is
    /// identical and the QML image cache serves the stale pixmap without ever
    /// re-asking the provider. This epoch is folded into every art URL (see
    /// artUrlForRow) and bumped on each MetadataReloader refresh (explicit
    /// "Reload info from file(s)" included), so a reload deterministically
    /// produces new URLs and the provider re-reads sidecars. No signal of its
    /// own: consumers re-read URLs on their existing triggers (selection, tab
    /// and playing changes; the refresh forwarder in PlaylistTabs).
    void bumpArtEpoch() { ++m_artEpoch; }

    /// Re-assert the CURRENT selection as full-width ranges in ONE select()
    /// (one selectionChanged; current index and anchor untouched). Needed after
    /// showColumn(): stored ranges span the pre-insert columns only, so the new
    /// column's cells read as unselected. Reads the selection's own ranges
    /// (already coalesced) rather than marshaling per-row indexes, merges
    /// overlapping/adjacent row spans so the stored selection stays canonical
    /// (no duplicate rows, which would double-count aggregate fields in the
    /// metadata pane), and rebuilds each span over columns 0..columnCount()-1.
    /// No-op for a null/foreign/empty selection.
    Q_INVOKABLE void reselectFullWidth(QItemSelectionModel* selection);

    /// The Ctrl+select-drag session, a live range APPENDED to the
    /// selection that existed when the drag began. Three calls form a session:
    ///
    ///   beginAdditiveRangeDrag  snapshots the selection's current ranges as
    ///                           the BASELINE (taken at drag start, after the
    ///                           press's toggle, so the click's effect is part
    ///                           of it).
    ///   updateAdditiveRangeDrag re-applies baseline UNION [lo, hi] (full
    ///                           width) with ONE ClearAndSelect per call.
    ///   endAdditiveRangeDrag    closes the session and drops the snapshot.
    ///
    /// Why baseline-restore instead of plain additive selects: a naive
    /// additive live range never shrinks when the drag reverses, so its
    /// feedback lies.
    /// Rebuilding baseline-plus-range every move makes reversal truthful:
    /// rows the range no longer covers drop out unless the baseline holds
    /// them. Doing that rebuild here, as one merged QItemSelection and one
    /// select(), keeps the one-selectionChanged-per-row-change discipline;
    /// a QML per-row loop would fragment the stored selection and re-run the
    /// downstream pipeline per row (see selectRowRange).
    ///
    /// The snapshot's ranges hold persistent indexes, so it survives
    /// incidental model remaps. Same wrong-model refusal as selectRowRange;
    /// update additionally refuses when no session is open, so a stray call
    /// can never collapse the selection to just the range. None of the three
    /// touch the current index; the caller owns current/anchor semantics.
    Q_INVOKABLE void beginAdditiveRangeDrag(QItemSelectionModel* selection);
    Q_INVOKABLE void updateAdditiveRangeDrag(QItemSelectionModel* selection,
                                             int lo, int hi);
    Q_INVOKABLE void endAdditiveRangeDrag();

private:
    /// Render one track's value for a native field. The two composites
    /// (TrackIndex, AlbumGroup) combine fields; the rest are direct reads. A thin
    /// forwarder to PatternEvaluator::renderFieldValue (see .cpp).
    [[nodiscard]] static QString renderField(const TrackData& track, ColumnField field);

    /// What a single visual column shows: either a native field or a custom
    /// column (a record id resolved against the injected registry). Custom
    /// columns are not catalog entries, so the arrangement is a list of these
    /// descriptors rather than entry indices.
    struct VisualColumn {
        bool        isCustom = false; ///< true => a custom column (use customId)
        ColumnField field{};          ///< the native field, valid iff !isCustom
        QString     customId;         ///< the custom record id, valid iff isCustom
    };

    /// The descriptor at a given visual column, or nullptr if out of range. The
    /// single chokepoint every column read goes through (where native/custom is
    /// resolved).
    [[nodiscard]] const VisualColumn* columnAt(int column) const;

    /// The seed px width for @p field: its width in the default arrangement, or
    /// kDefaultColumnWidth otherwise. The single source of seed widths (the
    /// catalog carries none).
    [[nodiscard]] int seedWidthForField(ColumnField field) const;

    /// React to a registry change (name/alignment/pattern, or an add): refresh
    /// any shown custom column's header + cells and nudge QML to re-read
    /// alignment (columnLayoutChanged). Wired in setCustomColumns().
    void onCustomColumnsChanged();

    /// React to a registry removal: drop any visual column bound to @p id (a
    /// begin/endRemoveColumns, so the selection model remaps), falling back to
    /// the default arrangement if that would leave zero columns.
    void onCustomColumnRemoved(const QString& id);

signals:
    void dataRevisionChanged();

    /// The column layout changed in a way the QML alignment bindings can't
    /// observe (a custom column's alignment edit, an add/remove). The view bumps
    /// its _layoutRevision so columnAlignment() is re-read. The interactive
    /// reorder path bumps _layoutRevision itself and does not rely on this.
    void columnLayoutChanged();

private:
    /// A cache-busting tag for @p row's album art (mtime + size, the staleness
    /// key). artUrlForRow folds this in so a re-read cover (same path, new
    /// bytes) yields a new URL and reloads. Empty if out of range.
    [[nodiscard]] QString trackVersionTag(int row) const;

    /// Builds the provider URL ("image://rawformart/<encoded path>?v=<tag>")
    /// for @p row; "" for out-of-range or an empty path. The single URL-shape
    /// authority, shared by the selection resolver and albumArtSourceForRow so
    /// the two can never drift apart on encoding or the staleness tag.
    [[nodiscard]] QString artUrlForRow(int row) const;

    ColumnSchema     m_schema;
    /// The injected custom-column registry (not owned). nullptr if none was set.
    const CustomColumnRegistry* m_customColumns = nullptr;
    /// The visible columns, in display order, native or custom (see VisualColumn).
    /// Seeded from m_defaultArrangement in setSchema(); permuted by
    /// moveVisualColumn(); rebuilt by applyColumnLayout(). size() == columnCount().
    QList<VisualColumn> m_columns;
    /// The default arrangement as catalog indices, in display order, derived in
    /// setSchema() from defaultColumnArrangement(). The seed for m_columns and the
    /// fallback applyColumnLayout() resets to. Native-only.
    QList<int>       m_defaultArrangement;
    /// Field-id (as int) -> seed px width, from the code arrangement. Backs
    /// seedWidthForField(); fields absent here fall back to kDefaultColumnWidth.
    QHash<int, int>  m_seedWidths;
    QList<TrackData> m_tracks;
    /// The additive-drag baseline (the selection as it stood at drag
    /// start) and the session gate. The gate exists so updateAdditiveRangeDrag
    /// outside a session is a refusal, not a selection-clobbering ClearAndSelect
    /// against an empty baseline. QItemSelectionRange holds persistent indexes,
    /// so the snapshot tracks incidental row remaps on its own.
    QItemSelection   m_additiveDragBase;
    bool             m_additiveDragActive = false;
    int              m_dataRevision = 0;
    /// See bumpArtEpoch(). Folded into every art URL's "?v=" tag.
    int              m_artEpoch = 0;
};

} // namespace rawform
