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

// qmllint disable unqualified
// This file is the app's wiring layer: it deliberately reaches the C++
// context property (audioController), which qmllint cannot see, so the
// unqualified-access category is disabled file-wide. Under the Bound
// pragma this directive covers context properties ONLY; all four
// delegate surfaces declare their injected names and every outer-id capture
// is statically checked under the pragma.
// Components stay fully linted; keep global wiring HERE so they can.
// Cost: a typo'd global name in this file surfaces at runtime, not lint.

// Bound component behavior: nested components and delegates resolve outer
// document ids statically instead of through dynamic context lookup. The
// delegate inventory: header (headerCell: column, display), body row (cell:
// row, column, display, rowAvailable), and the two column-menu Instantiator
// delegates (modelData). Instantiator delegates are backed by the same delegate
// model machinery, so their required properties are filled the same way and
// fail loudly at creation if not. Child items inside a delegate qualify
// injected reads through the delegate root id; the root's own bindings may
// read them bare.
pragma ComponentBehavior: Bound
import QtQml.Models
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import com.rawform.app

/*
 * Playlist view: a HorizontalHeaderView above a TableView in a rounded,
 * hover-reactive frame. Both bind to the same PlaylistModel, but the header is
 * independent (no syncView): it renders its own cells from the model's
 * columnCount/headerData. That independence is the point:
 *
 *  - Column widths live in ONE place, the header's explicit column widths.
 *    Because the header always has realized cells, its built-in resizableColumns
 *    and setColumnWidth work even when the playlist is empty (the case a
 *    body-synced header cannot handle without a separate width mirror).
 *    The body's columnWidthProvider just reads header.explicitColumnWidth, and a
 *    Connections follow-trigger relayouts the body when the header width changes.
 *  - Horizontal scroll is driven by the body; the header mirrors its contentX
 *    (interactive:false) so the two stay aligned.
 *  - Reorder is a custom edge-inset MouseArea on the header delegate driving
 *    moveVisualColumn; the edge band is left free for the built-in resize.
 *  - Titles come straight from headerData and update live via headerDataChanged.
 *
 * Persistence surface (currentColumnOrder/currentColumnWidths on save,
 * applyColumnLayout on load) reads/writes the header; `columnWidths` is only the
 * default seed when nothing is saved.
 */
Item {
    id: root

    // The native selection model, also created in C++ and assigned from
    // MainWindow. Bound to the TableView below; its currentIndex drives the
    // metadata pane (wired in C++). Implements the FileQueue selection flow
    // (plain / ctrl-toggle / shift-range / ctrl+shift additive) via the
    // click handler in the cell delegate.
    property ItemSelectionModel selectionModel

    // The CURRENT (focus) row, for the delegate's foobar-style focus outline
    //. Driven by the DATA (the root's selection model, which stays bound
    // to the active tab regardless of the TableView's detach window), so
    // the outline is correct even while the view's attachment is mid-swap.
    // selectionModel.currentIndex notifies on currentChanged; the
    // selectionModel term re-evaluates across tab switches.
    readonly property int _currentRow:
        selectionModel && selectionModel.currentIndex.valid
            ? selectionModel.currentIndex.row : -1

    // The model is created in C++ and assigned from MainWindow so this view
    // stays reusable; tab switching swaps the model underneath it.
    property PlaylistModel model

    // The playlist session manager (PlaylistTabs, created in C++, assigned from
    // MainWindow). The body DropArea hands dropped URLs to it in ONE call
    // (dropUrlsIntoActive with the drop-point row): .rwfpl files are
    // concatenated into THIS (active) playlist at that row, everything else
    // goes to the scanner. The split lives in C++ (see the note at the drop).
    property var tabs

    // The active tab's MetadataReloader (assigned from MainWindow as
    // playlistTabs.activeReloader). The row context menu's "Reload info" forces
    // a re-read of the current selection through it.
    property var reloader

    // The session LogStore (assigned from MainWindow). The file DropArea logs
    // one line per drop (url count + handler time) to the status console; a
    // null log degrades to silent.
    property var log

    // Shift-range anchor: the row a range extends FROM. Mirrors FileQueue's
    // __lastClickedIndex. Updated on every click.
    property int _anchorRow: -1

    // --- Column reorder drag state ---------------------------------------
    // The model owns the authoritative column order (a permutation over the
    // schema); these are purely the transient gesture state for the header
    // drag. _dragColumn is the column being dragged. _dropBoundary is the GAP
    // the column would drop into, indexed 0..columnCount (boundary b sits
    // before column b; boundary N is the far right edge), chosen as the gap
    // nearest the cursor, so the drop point is always next to the pointer
    // rather than snapping to a whole column's far edge. Both -1 when idle.
    property int  _dragColumn: -1
    property int  _dropBoundary: -1
    // Pointer x during a drag, in contentClip coords, so the floating ghost can
    // track the cursor. Title of the column being dragged, captured at drag
    // start so the ghost can show it without a headerData round-trip.
    property real _dragPointerX: 0
    property string _dragTitle: ""
    // Px the pointer must travel before a header press becomes a reorder drag
    // (so a plain click isn't swallowed), and the edge band on
    // each header cell the reorder MouseArea stays clear of so the boundary is
    // free for the built-in resize. Both adjacent cells inset by this, so each
    // column boundary has a 2x-wide grab band centered on it.
    readonly property int _dragThreshold: 6
    readonly property int _resizeGrip: 8

    // --- Row reorder drag state ------------------------------------------
    // Vertical analog of the column drag, but it shares the body's click
    // overlay, so press handling interleaves selection and drag-arming.
    // _dragRowFirst/_dragRowLast are the contiguous block being dragged;
    // _rowDropBoundary is the gap (0..rowCount) it would drop into. The shared
    // row height (also fed to the rowHeightProvider) lets boundary math be
    // pure arithmetic instead of hit-testing.
    property bool _rowDragging: false
    // A click-hold-drag that EXTENDS the selection instead of moving rows.
    // Armed by a plain press on a row that was NOT already selected (the press
    // exclusive-selects it, making it the anchor) or by a shift press (anchor
    // unchanged); a press on an already-selected row keeps arming the reorder
    // drag as before, so select-then-drag still moves blocks. Ctrl presses stay
    // click-only toggles (a live additive range cannot shrink truthfully).
    property bool _rowSelDragging: false
    // A Ctrl+click-hold-drag that APPENDS a live range to the selection
    // that existed at drag start. The press's toggle runs first (click
    // semantics unchanged); crossing the threshold opens a baseline session in
    // the model (beginAdditiveRangeDrag) and each row change re-applies
    // baseline UNION pressRow..cursorRow (updateAdditiveRangeDrag), so
    // reversing the drag truthfully retracts rows the baseline doesn't hold.
    // That baseline-restore is what the click-only Ctrl+Shift form lacks (its
    // live range could never shrink, see the note at the shift press).
    property bool _rowCtrlSelDragging: false
    // A click-hold-drag that starts on EMPTY body space and selects every
    // row its slim frame spans, live. Plain left presses only; ctrl / shift
    // empty-space presses stay click-only clears. Machinery lives on bodyMouse.
    property bool _banding: false
    property int  _dragRowFirst: -1
    property int  _dragRowLast: -1
    property int  _rowDropBoundary: -1
    readonly property int _rowHeight: 19

    // Row the right-click context menu acts on (the menu also reflects the
    // current multi-selection where that matters; this is the click target).
    property int _contextRow: -1

    // Row most recently handed to audioController.playAt by an activation gesture
    // (double-click or Enter), awaiting the engine's verdict. If the start fails
    // (a missing/unreadable file), we deselect this row so it drops the selection
    // highlight and reads as the grayed-out unavailable entry rather than staying
    // selected over the gray. Cleared on a confirmed start (kept selected) or once
    // a failure is handled. -1 when nothing is pending.
    property int _pendingActivateRow: -1

    // A Ctrl+P reveal parked across a tab switch. When the playing
    // track lives in ANOTHER tab, the key handler sets this and flips
    // tabs.currentIndex; tableView.onModelChanged consumes it (there, not
    // on root, so the position call always acts on the swapped-in model) once
    // the incoming model IS the playing model, reading the LIVE playingRow at
    // consumption in case playback advanced during the swap. Any unrelated
    // model change clears it, so a stale reveal can never fire on a different
    // switch. -1 when nothing is parked.
    property int _pendingRevealRow: -1

    // Bumped on every column-order change. Per-cell alignment is read from the
    // model via the columnAlignment() invokable, which QML can't track as a
    // binding dependency; after a moveColumn() a delegate keeps its visual
    // `column` index while the field shown there changes, so nothing the binding
    // watches updates and the alignment goes stale (some columns right, some
    // left). Referencing this revision inside those bindings gives them a
    // dependency to re-evaluate on. (applyColumnLayout() resets the model, which
    // rebuilds delegates, so it doesn't need this; only the in-place move does.)
    property int _layoutRevision: 0

    // Model-driven layout changes (a custom column's alignment edit, or an
    // add/remove from the registry) can't be observed by the alignment bindings,
    // which call the columnAlignment() invokable. Bump _layoutRevision so they
    // re-evaluate, the same nudge reorderColumn() does by hand for the interactive
    // move. Targeting a possibly-null model is a harmless no-op.
    Connections {
        target: root.model
        function onColumnLayoutChanged() { root._layoutRevision++ }
    }

    // Centralized width fix-up for COLUMN REMOVALS, wherever they originate.
    // Header explicit widths are stored BY INDEX and never shift when the model
    // removes a column, so every column right of the removal would wear its old
    // neighbor's width. Centralized here rather than in the QML toggle
    // (hideColumn) because C++-initiated removals never pass through the
    // toggle: deleting a shown custom column's DEFINITION in the manager goes
    // registry -> onCustomColumnRemoved -> begin/endRemoveColumns with no view
    // involvement, which is exactly the "each column takes its neighbor's
    // width" scenario this prevents.
    // Snapshot on aboutToBe (columnCount is still the OLD count there, so the
    // positions are pre-removal and include the doomed span), splice, re-assert
    // after. _toggleColumn's hide branch does NOT touch widths anymore; both
    // paths fixing up would splice twice.
    property var _widthsAcrossColumnRemoval: null
    Connections {
        target: root.model
        function onColumnsAboutToBeRemoved(parent, first, last) {
            var w = root.currentColumnWidths()
            w.splice(first, last - first + 1)
            root._widthsAcrossColumnRemoval = w
        }
        function onColumnsRemoved(parent, first, last) {
            var w = root._widthsAcrossColumnRemoval
            root._widthsAcrossColumnRemoval = null
            if (!w)
                return
            root.applyWidths(w) // re-assert + relayout + alignment-binding kick
            // Re-park so an autosave pairs field ids with post-removal positions.
            if (root.tabs)
                root.tabs.stashActiveWidths(root.currentColumnWidths())
        }
    }

    // Viewport re-anchoring across a ROW REMOVAL batch. TableView's own
    // rowsRemoved rebuild keeps the top row's pixel position and only clamps
    // its index to the new count, so when rows ABOVE the viewport vanish the
    // loaded rows stay drawn where the old rows were, with a phantom gap above
    // them, and contentHeight (the bottom of the loaded table plus the rows
    // still below it) stays at the old extent: the scrollbar keeps its 5k-row
    // size after 4k deletions until a page-sized scroll makes the view
    // recompute its top-left. positionViewAtRow is the one public call that
    // forces that recomputation (row * rowHeight, exact for fixed-height
    // rows), so the anchor is the top visible row carried through the batch:
    //   - a run entirely above it shifts it up by the run length,
    //   - a run covering it lands it on the row that took its place (first),
    //   - a run below it leaves it alone.
    // The runs arrive bottom-up in current coordinates, so the mapping
    // composes run by run. Applied ONCE, synchronously, on the batch-end
    // signal, before the deferred rebuild: per-run positioning would take
    // the adjacent-row synchronous edge-load path for every one-row run
    // (thousands of delegate loads on a sparse selection), and a Qt.callLater
    // can land AFTER the rebuild, where the anchor may coincide with the
    // clamped top row and positionViewAtRow becomes a no-op on the stale
    // geometry. The sub-row pixel offset makes the reposition exact rather
    // than row-snapped. An unmoved anchor (all runs at or below the viewport)
    // is left to Qt: that rebuild is correct, and Flickable clamps contentY
    // when the content shrinks under the viewport.
    property int _removalAnchorRow: -1
    property real _removalAnchorOffset: 0
    Connections {
        target: root.model
        function onBulkRemovalStarted() {
            var top = tableView.topRow
            if (top < 0) {
                root._removalAnchorRow = -1
                return
            }
            root._removalAnchorRow = top
            root._removalAnchorOffset =
                tableView.contentY - top * root._rowHeight
        }
        function onRowsRemoved(parent, first, last) {
            var a = root._removalAnchorRow
            if (a < 0)
                return
            if (last < a)
                root._removalAnchorRow = a - (last - first + 1)
            else if (first <= a)
                root._removalAnchorRow = first
        }
        function onBulkRemovalFinished() {
            var a = root._removalAnchorRow
            root._removalAnchorRow = -1
            if (a < 0 || !root.model)
                return
            var n = root.model.rowCount()
            if (n <= 0 || a === tableView.topRow)
                return
            centerAnim.stop()
            tableView.positionViewAtRow(Math.min(a, n - 1), TableView.AlignTop,
                                        root._removalAnchorOffset)
        }
    }

    // Resolve a pending activation (see _pendingActivateRow) against the
    // controller's verdict. trackChanged with the cursor on the attempted row is
    // the CONFIRMED start (the optimistic cursor set in playAt emits playingChanged
    // only, not trackChanged), so it just clears the marker and leaves the row
    // selected/playing. errorOccurred is the failed start: deselect the row so the
    // missing entry reverts to its grayed-out look instead of staying highlighted.
    Connections {
        target: audioController
        function onTrackChanged() {
            if (root._pendingActivateRow >= 0
                    && audioController.playingModel === root.model
                    && audioController.playingRow === root._pendingActivateRow)
                root._pendingActivateRow = -1
        }
        function onErrorOccurred(message) {
            if (root._pendingActivateRow >= 0) {
                root._deselectRow(root._pendingActivateRow)
                root._pendingActivateRow = -1
            }
        }
    }

    // Escape-deselect through the Shortcut phase. The Keys.onPressed
    // branch below stays as fallback, but on macOS the raw Escape key event
    // was observed never reaching the focused item's Keys handler (here and in
    // both Properties panes alike), while the app's Shortcut-based Escape
    // handlers (log console, About window) do fire there: shortcuts match in
    // the ShortcutOverride phase, before focus-item key delivery, so Escape
    // rides the proven idiom. Enabled ONLY while the table itself holds active
    // focus, which (a) keeps it from swallowing the Escape that cancels a tab
    // rename or a dialog editor, and (b) guarantees at most one of the live
    // per-tab PlaylistView instances arms this sequence at a time (two enabled
    // matches would be ambiguous and Qt would fire neither's onActivated).
    // onActivatedAmbiguously covers the one legal overlap, the log console's
    // own Escape-to-close while the table is focused: both ambiguous handlers
    // run, so the console still closes and the selection clears with it.
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: tableView.activeFocus
        onActivated: root._clearSelection()
        onActivatedAmbiguously: root._clearSelection()
    }

    // Clear all selection (empty-space click, Escape). ORDER MATTERS: the
    // current index goes first. clearSelection() emits selectionChanged, and
    // observers (the art binding, the metadata pane) evaluate synchronously
    // inside that emission; ranges-first would let them sample a half-cleared
    // state (ranges gone, current still valid), and the art resolver's
    // current-index courtesy would serve the stale row. Current-first
    // means every emission along the way describes a state that only shrinks
    // toward fully-cleared, and the final selectionChanged evaluation sees
    // exactly that.
    function _clearSelection() {
        selectionModel.clearCurrentIndex()
        selectionModel.clearSelection()
        _anchorRow = -1
    }

    // Delete the selected rows. Selection spans all columns of each selected
    // row (Rows flag), so dedup to row numbers first. Afterwards, land the
    // selection on the row that took the first deleted row's place (or the new
    // last row if the tail was removed); clear if the playlist is now empty.
    // The removal itself remaps the selection model, we only set the new
    // current row, which also refreshes the metadata pane via selectionChanged.
    function _deleteSelected() {
        // Same C++ range walk as _selectedRows (and the same reason): a
        // selectedIndexes dedupe is one JS wrapper per selected CELL.
        var rows = root._selectedRows()
        if (rows.length === 0)
            return
        var firstDeleted = rows[0]

        model.removeTracks(rows)

        var n = model.rowCount()
        if (n <= 0) {
            _clearSelection()
            return
        }
        _selectExclusive(Math.min(firstDeleted, n - 1))
    }

    // The selected rows as a sorted, de-duplicated array of ints, via the C++
    // range walk (PlaylistModel.selectedRowList). Feeds the Properties window,
    // the rename dialog, and row deletion.
    function _selectedRows() {
        // C++ range walk (PlaylistModel.selectedRowList). The scenario this
        // prevents: reading selectionModel.selectedIndexes materializes one
        // QModelIndex wrapper per CELL (rows x columns JS allocations on a
        // library-sized selection) just to dedupe rows in JS. The helper walks
        // the selection's rectangular RANGES instead and returns the rows
        // already sorted and unique, so this stays O(selected rows) with no
        // per-cell cost. Contract: ascending distinct row numbers.
        if (!root.model || !selectionModel)
            return []
        return root.model.selectedRowList(selectionModel)
    }

    // Arrow-key navigation, owned here (TableView's built-in nav is disabled)
    // so it reuses the mouse path's helpers and the SAME _anchorRow.
    //   delta  : +1 for Down, -1 for Up.
    //   extend : true on Shift, grow the contiguous range from the anchor,
    //            exactly like a Shift+click; false, exclusive select + anchor.
    // The lead row is the current index, which both _selectExclusive and
    // _selectRange keep up to date, so repeated presses walk predictably and
    // Shift after a mouse multi-select extends from the right place.
    function _keyMoveCurrent(delta, extend) {
        var n = tableView.rows
        if (n <= 0)
            return
        var cur = selectionModel.currentIndex
        var curRow = cur.valid ? cur.row : -1
        var target = curRow < 0 ? (delta > 0 ? 0 : n - 1) : curRow + delta
        target = Math.max(0, Math.min(target, n - 1))
        if (extend) {
            if (_anchorRow < 0)
                _anchorRow = curRow >= 0 ? curRow : target
            _selectRange(target, false)
        } else {
            _selectExclusive(target)
        }
        tableView.positionViewAtRow(target, TableView.Contain)
    }

    // Shared reveal machinery. Rows are fixed-height (_rowHeight, the
    // rowHeightProvider constant), so visibility is a pure band test; no
    // delegate probing.
    function _rowFullyVisible(row) {
        if (row < 0 || row >= tableView.rows)
            return false
        var top = row * root._rowHeight
        return top >= tableView.contentY
            && top + root._rowHeight <= tableView.contentY + tableView.height
    }

    // The smooth-centering animation, ONLY for the deliberate
    // commands (Ctrl+F, the in-tab Ctrl+P, the Enter reveal), where the glide
    // from the current view to the target reads as intent. Restores, the
    // launch positioning, and the cross-tab reveal stay INSTANT: they run
    // right after a model swap, where an animation would fly in from an
    // arbitrary carryover position (the original from-the-top glitch), and
    // TableView.animate stays false so nothing here can be canceled by
    // relayout churn; this animation drives contentY directly and is stopped
    // explicitly at every instant-positioning site and on switch-away. A
    // wheel or press during the ~160 ms glide briefly competes with it,
    // which is harmless at this duration.
    NumberAnimation {
        id: centerAnim
        target: tableView
        property: "contentY"
        duration: 160
        easing.type: Easing.OutCubic
    }

    function _centerRow(row, smooth) {
        if (row < 0 || row >= tableView.rows)
            return
        centerAnim.stop()
        if (!smooth) {
            // The INSTANT path must go through positionViewAtRow, which
            // Qt queues past a pending rebuild. The launch (and switch-time)
            // applies fire from rowsChanged / contentHeightChanged, which the
            // initial build emits MID-rebuild, before its own final viewport
            // placement; a direct contentY write lands there and is overridden
            // by that placement moments later, stranding the launch restore
            // at the top. positionViewAtRow survives it, and
            // with TableView.animate false it is an immediate jump otherwise.
            tableView.positionViewAtRow(row, Qt.AlignVCenter)
            return
        }
        // The SMOOTH glide keeps the direct contentY math: its three callers
        // (Ctrl+F, in-tab Ctrl+P, the Enter reveal) run on a settled table by
        // construction (a user keypress inside the built view), where a
        // driven contentY is safe and animatable.
        var h = root._rowHeight
        var target = row * h - (tableView.height - h) / 2
        var maxY = Math.max(0, tableView.contentHeight - tableView.height)
        target = Math.max(0, Math.min(target, maxY))
        centerAnim.from = tableView.contentY
        centerAnim.to = target
        centerAnim.start()
    }

    // The tab's scroll position as its first visible row, for the
    // per-tab parking. -1 on an empty tab (nothing to park).
    function _firstVisibleRow() {
        if (tableView.rows <= 0)
            return -1
        return Math.max(0, Math.min(tableView.rows - 1,
                        Math.floor(tableView.contentY / root._rowHeight)))
    }

    // The position intent parked across a model swap, applied only
    // once the LAYOUT has caught up with the model. TableView.rows is a
    // product of the rebuild and lags onModelChanged by at least a polish
    // frame (passing through intermediate values on the way), so any position
    // call guarded or clamped against it at swap time acts on the OUTGOING
    // count: a parked row 5000 clamped against yesterday's 91 restores to the
    // top, and the reveal's row guard bails against a stale or mid-rebuild
    // zero count (the observed switch-without-centering Ctrl+P, cured by a
    // second press once the rebuild had finished). The intent is therefore
    // applied only when tableView.rows === model.rowCount() (the model is the
    // authority; its count is correct the instant it swaps), attempted once
    // via Qt.callLater (covers equal-count swaps, where rowsChanged never
    // fires) and re-attempted on every rowsChanged until the counts match.
    //   mode 0: nothing parked
    //   mode 1: per-tab scroll restore, _pendingPositionRow to the top
    //   mode 2: reveal the playing track (plant focus row + center)
    //   mode 3: center _pendingPositionRow (the launch reveal)
    property int _pendingPositionMode: 0
    property int _pendingPositionRow: -1
    // The verification state. The apply positions by exact contentY math
    // and then CHECKS, on the next event-loop tick, that the viewport
    // actually sits at the target: positioning issued from inside the FIRST
    // build's own signal cascade (rowsChanged / contentHeightChanged are
    // emitted mid-rebuild) gets overridden by that build's final viewport
    // placement no matter which API issued it, which is why the launch-time
    // apply kept landing at the top while the switch-time applies (running
    // after fast rebuilds via Qt.callLater) succeeded. Verify-and-reapply
    // converges deterministically: after the rebuild's placement runs once,
    // the re-applied position has nothing left to fight it. Bounded so a
    // pathological layout can never loop forever; at most one wrong frame is
    // ever visible.
    property real _pendingTargetY: -1
    property int _pendingTries: 0

    function _resetPendingPosition() {
        _pendingPositionMode = 0
        _pendingPositionRow = -1
        _pendingTargetY = -1
        _pendingTries = 0
    }

    function _applyPendingPosition() {
        if (_pendingPositionMode === 0)
            return
        if (!model) {
            _resetPendingPosition()
            return
        }
        var want = model.rowCount()
        if (want <= 0) {
            _resetPendingPosition() // empty tab: nothing to position at
            return
        }
        if (tableView.rows !== want)
            return // layout still catching up; onRowsChanged retries
        // Also wait for the content size to reach the true extent
        // (rows are fixed-height, so the exact value is knowable). A position
        // applied against a lagging contentHeight estimate can be clamped or
        // rubber-banded back toward the top; onContentHeightChanged retries.
        // Half a row of tolerance absorbs float noise.
        if (tableView.contentHeight
                < want * root._rowHeight - root._rowHeight / 2)
            return
        // An in-flight smooth center must never keep writing contentY
        // over an instant positioning.
        centerAnim.stop()
        var h = root._rowHeight
        var maxY = Math.max(0, want * h - tableView.height)
        var target
        if (_pendingPositionMode === 1) {
            var r = Math.min(_pendingPositionRow, want - 1)
            if (r < 0) {
                _resetPendingPosition()
                return
            }
            target = Math.min(r * h, maxY)
        } else {
            var cr
            if (_pendingPositionMode === 2) {
                cr = audioController.playingRow
                if (audioController.playingModel !== model
                        || cr < 0 || cr >= want) {
                    _resetPendingPosition()
                    return
                }
                // The plant half of the reveal (idempotent across retries).
                if (selectionModel && selectionModel.model === model)
                    selectionModel.setCurrentIndex(_rowIndex(cr),
                                                   ItemSelectionModel.NoUpdate)
            } else {
                cr = Math.min(_pendingPositionRow, want - 1)
                if (cr < 0) {
                    _resetPendingPosition()
                    return
                }
            }
            target = Math.max(0, Math.min(cr * h - (tableView.height - h) / 2,
                                          maxY))
        }
        tableView.contentY = target
        _pendingTargetY = target
        _pendingTries += 1
        Qt.callLater(root._verifyPendingPosition)
    }

    function _verifyPendingPosition() {
        if (_pendingPositionMode === 0)
            return
        if (Math.abs(tableView.contentY - _pendingTargetY)
                <= root._rowHeight / 2
            || _pendingTries >= 6) {
            _resetPendingPosition()
            return
        }
        _applyPendingPosition()
    }

    // The in-tab half of the Ctrl+P reveal: plant the focus row on
    // the playing track (NoUpdate: the outline only, the selection is never
    // touched, foobar's focus semantics) and center it. Reads the LIVE
    // playing row so the deferred cross-tab path lands on the track playing
    // NOW, not the one playing when the switch was requested. Note: smooth is
    // true only for the direct in-tab keypress; the cross-tab consumption
    // passes false (see the centerAnim note).
    function _revealPlayingHere(smooth) {
        var r = audioController.playingRow
        if (r < 0 || r >= tableView.rows)
            return
        if (selectionModel && audioController.playingModel === root.model)
            selectionModel.setCurrentIndex(_rowIndex(r),
                                           ItemSelectionModel.NoUpdate)
        _centerRow(r, smooth)
    }

    // The public quit-time capture, called by MainWindow.onClosing (the
    // stashActiveWidths line's twin) so the ACTIVE tab's live position reaches
    // the parking before the shutdown flush writes it to the SCRL chunk.
    // Kept as the one public entry so _firstVisibleRow stays private to the
    // view.
    function stashScrollPosition() {
        if (root.tabs)
            root.tabs.stashActiveScrollRow(root._firstVisibleRow())
    }

    // The SAVE AS position snapshot, read by PlaylistDialogs and
    // handed to PlaylistStore.save so a saved .rwfpl embeds the live CURR
    // focus row and SCRL scroll position (the store owns neither the
    // selection nor this view). Public pair for the same encapsulation
    // reason as stashScrollPosition: the underscore state stays private.
    function currentFocusRow() {
        return root._currentRow
    }

    function currentScrollRow() {
        return root._firstVisibleRow()
    }

    // The launch positioning, corrected after a real-world miss: playlist reads
    // are ASYNC, so restoreSession only STARTS them before the engine loads
    // this file; at completion time here the active tab is usually still
    // loading and both activeScrollRow() and the CURR focus row read -1, so
    // this parker finds nothing and the REAL launch restore is parked by
    // onActiveTabLoadCompleted (the Connections above) when the load lands.
    // This parker still matters for the cases where the state IS already
    // here: a tiny playlist whose read won the race, and any synchronous
    // load path. Precedence when it does park: the persisted
    // scroll position WINS (launch means "everything where you left it");
    // the CURR focus-row centering is the fallback for a tab that never
    // parked a position. Parked as a position intent, applied only once
    // the layout's counts catch up with the model (see the intent's note).
    Component.onCompleted: {
        var sr = root.tabs ? root.tabs.activeScrollRow() : -1
        if (sr >= 0) {
            root._pendingPositionMode = 1
            root._pendingPositionRow = sr
        } else if (root._currentRow >= 0) {
            root._pendingPositionMode = 3
            root._pendingPositionRow = root._currentRow
        }
        root._pendingTargetY = -1
        root._pendingTries = 0
        if (root._pendingPositionMode !== 0)
            Qt.callLater(root._applyPendingPosition)
    }

    // Build a row-wide model index (column 0) for selection ops. We always
    // select with the Rows flag so the whole row highlights.
    function _rowIndex(row) {
        return model.index(row, 0)
    }

    // Select every row (Ctrl/Cmd+A). ONE batched C++ call, never a per-row
    // select() loop: each per-row select fragments the stored selection AND
    // fires the whole selectionChanged pipeline (metadata pane re-aggregation,
    // art scan) per row, which is quadratic and took minutes at 5000 rows.
    // clearFirst=true so the stored selection collapses to a single canonical
    // range even when a partial selection already existed (behaviorally
    // identical: everything ends up selected either way).
    function _selectAll() {
        var n = tableView.rows
        if (n <= 0 || !model || !selectionModel)
            return
        model.selectRowRange(selectionModel, 0, n - 1, true)
    }

    // Plain click: exclusive select + make current.
    function _selectExclusive(row) {
        selectionModel.select(_rowIndex(row),
            ItemSelectionModel.ClearAndSelect | ItemSelectionModel.Rows)
        selectionModel.setCurrentIndex(_rowIndex(row), ItemSelectionModel.NoUpdate)
        _anchorRow = row
    }

    // Drop a single row from the selection (used when an activation fails on a
    // missing file): removes the selection highlight so the unavailable row shows
    // its grayed-out color, and clears it as the current index so the metadata
    // pane empties, the track is gone, there is nothing to inspect.
    function _deselectRow(row) {
        if (!selectionModel || row < 0)
            return
        selectionModel.select(_rowIndex(row),
            ItemSelectionModel.Deselect | ItemSelectionModel.Rows)
        var ci = selectionModel.currentIndex
        if (ci && ci.valid && ci.row === row)
            selectionModel.clearCurrentIndex()
    }

    // Shift click: select the contiguous range anchor..row. If additive is
    // false the range replaces the selection; if true it's added to it.
    // ONE batched C++ call (see _selectAll for why a per-row loop is banned);
    // the replace case rides the same call as ClearAndSelect rather than a
    // separate clearSelection(), so the whole gesture emits exactly one
    // selectionChanged.
    function _selectRange(row, additive) {
        if (!model || !selectionModel)
            return
        var anchor = _anchorRow >= 0 ? _anchorRow : row
        var lo = Math.min(anchor, row)
        var hi = Math.max(anchor, row)
        model.selectRowRange(selectionModel, lo, hi, !additive)
        selectionModel.setCurrentIndex(_rowIndex(row), ItemSelectionModel.NoUpdate)
        // anchor stays put for shift-range (matches FileQueue).
    }

    // Ctrl/Cmd click: toggle this row, leave others, make current.
    function _toggle(row) {
        selectionModel.select(_rowIndex(row),
            ItemSelectionModel.Toggle | ItemSelectionModel.Rows)
        selectionModel.setCurrentIndex(_rowIndex(row), ItemSelectionModel.NoUpdate)
        _anchorRow = row
    }

    // --- Row drag-reorder helpers ----------------------------------------

    // Establish the block to drag: the maximal contiguous run of SELECTED rows
    // containing pressRow (so a contiguous multi-selection drags as one), or
    // just pressRow if it isn't selected. Always contiguous, that's all a
    // single beginMoveRows can move; a non-contiguous multi-drag is outside
    // this gesture's contract.
    function _beginRowDrag(pressRow) {
        var lo = pressRow
        var hi = pressRow
        if (selectionModel.isSelected(_rowIndex(pressRow))) {
            while (lo - 1 >= 0 && selectionModel.isSelected(_rowIndex(lo - 1)))
                lo--
            var n = model.rowCount()
            while (hi + 1 < n && selectionModel.isSelected(_rowIndex(hi + 1)))
                hi++
        }
        _dragRowFirst = lo
        _dragRowLast = hi
        _rowDragging = true
    }

    // The row boundary (gap) for an external file drop, given a y in
    // contentClip space (the DropArea fills contentClip). Above the body band
    // (over the header) snaps to the top; otherwise it reuses the same
    // nearest-boundary arithmetic as the internal row drag, accounting for the
    // header height and the vertical scroll.
    function _dropBoundaryFromClipY(y) {
        var bodyY = y - header.height
        if (bodyY < 0)
            return 0
        return _rowBoundaryAtContentY(bodyY + tableView.contentY)
    }

    function _endRowDrag() {
        _rowDragging = false
        _dragRowFirst = -1
        _dragRowLast = -1
        _rowDropBoundary = -1
    }

    // Commit the drag: move the block to the drop boundary. The model move
    // carries the block's selection and current index with it; we only fix up
    // the anchor and current to the block's new start so shift-range still
    // behaves. No-op boundaries (inside the block or at its own edges) are ignored.
    function _performRowMove() {
        if (_rowDropBoundary < 0 || _dragRowFirst < 0)
            return
        var first = _dragRowFirst
        var count = _dragRowLast - _dragRowFirst + 1
        var b = _rowDropBoundary
        if (b >= first && b <= _dragRowLast + 1)
            return
        if (model.moveTracks(first, count, b)) {
            var newFirst = (b <= first) ? b : b - count
            _anchorRow = newFirst
            selectionModel.setCurrentIndex(_rowIndex(newFirst),
                ItemSelectionModel.NoUpdate)
        }
    }

    // The row boundary (gap) nearest a content-space y: 0..rowCount, where
    // boundary b is the top edge of row b. Rows are uniform height, so this is
    // just rounding. Mirrors the column nearest-boundary logic on the y axis.
    function _rowBoundaryAtContentY(cy) {
        var n = model ? model.rowCount() : 0
        var b = Math.round(cy / _rowHeight)
        return Math.max(0, Math.min(b, n))
    }

    // DEFAULT column widths (the seed), sourced from the column schema via the
    // model rather than hard-coded here, so the schema is the single source of
    // truth for column layout. Order matches the schema's entry order. These are
    // only the fallback when a column has no explicit width yet; the LIVE,
    // possibly-resized widths are owned by the header (header.explicitColumnWidth),
    // which both providers fall back from to this seed. Re-binds automatically if
    // the model (and thus its schema) is swapped later.
    readonly property var columnWidths: root.model ? root.model.defaultColumnWidths() : []

    function _defaultColumnWidth(column) {
        if (column >= 0 && column < columnWidths.length)
            return columnWidths[column]
        return 120
    }

    // Column widths live in ONE place: the header's own explicit column
    // widths (header.explicitColumnWidth / header.setColumnWidth). The header is
    // an INDEPENDENT HorizontalHeaderView (its own model, NOT synced to the
    // body), so it always has realized cells, which means setColumnWidth and
    // the built-in resize work even when the playlist is empty, the exact case
    // a synced header cannot handle. The body's columnWidthProvider just
    // READS header.explicitColumnWidth, and a follow-trigger relayouts the body
    // whenever the header's total width changes (see the Connections below).
    // There is no second width store to keep in step with.

    // --- Column-layout persistence surface -------------------------------
    // Three functions, read and written by the hosts: PlaylistDialogs
    // snapshots currentColumnOrder() + currentColumnWidths() into a saved
    // .rwfpl / preset, the tab bar and this file stash currentColumnWidths()
    // on a tab switch (PlaylistTabs.stashActiveWidths), and applyColumnLayout()
    // restores a tab's stored layout on activation and load.

    // Read the current (possibly resized) widths, in VISUAL order, for saving
    // alongside the order below.
    function currentColumnWidths() {
        var out = []
        var n = root.model ? root.model.columnCount() : columnWidths.length
        // The header always has its cells realized (own model), so its explicit
        // widths are valid whether or not the body has rows, no empty/realized
        // branch needed. Unset columns (no resize, no preset yet) fall back to
        // the schema default.
        for (var c = 0; c < n; ++c) {
            var w = header.explicitColumnWidth(c)
            out.push(w >= 0 ? w : _defaultColumnWidth(c))
        }
        return out
    }

    // The current column order as field-id strings, in VISUAL order. Pairs
    // index-for-index with currentColumnWidths(); together they ARE the saved
    // layout. Keyed by field id (not position) so it survives schema edits.
    function currentColumnOrder() {
        return root.model ? root.model.currentColumnOrder() : []
    }

    // Restore a saved layout on playlist load. Order and widths are reconciled
    // TOGETHER by the model against the live schema (drop retired fields, append
    // new ones at their defaults); the model hands back widths already
    // re-sequenced to the resulting visual order, which we apply positionally,
    // so order and width can never drift out of step.
    function applyColumnLayout(fieldIds, widths) {
        if (!root.model)
            return
        var aligned = root.model.applyColumnLayout(fieldIds, widths)
        // The model reset above rebuilds the header's columns (it's bound to the
        // model); seed each one's width onto the header, which is now the single
        // owner. setColumnWidth records the width by index even before the cell
        // re-realizes, so ordering vs. the reset is safe. The body re-reads these
        // through its provider; forceLayout both so the change shows immediately.
        for (var c = 0; c < aligned.length; ++c)
            header.setColumnWidth(c, aligned[c])
        header.forceLayout()
        tableView.forceLayout()
    }

    // Width-only seed, for a TAB SWITCH. The model swap (model: activeModel) has
    // already rebuilt the header's columns to the new tab's order; here we just
    // push that tab's PARKED widths onto the single header (which isn't
    // per-model) and relayout. No model reset, the order is already correct.
    // `widths` is the new tab's stored widths in its own visual order.
    function applyWidths(widths) {
        if (!widths)
            return
        for (var c = 0; c < widths.length; ++c)
            header.setColumnWidth(c, widths[c])
        header.forceLayout()
        tableView.forceLayout()
        root._layoutRevision++ // re-evaluate alignment bindings for the new model
    }

    // When the bound model changes (a tab switch swapped activeModel underneath
    // us), re-seed the header from the now-active tab's parked widths. The
    // OUTGOING tab's widths were stashed by the tab bar just before the switch,
    // so they are safe in PlaylistTabs by the time we read activeWidths() here.
    onModelChanged: {
        // A pending removal snapshot belongs to the OUTGOING model; applying it
        // to the incoming tab's header would be positional nonsense. (Can't
        // happen mid begin/end pair, this is pure paranoia against a switch
        // landing between the two signals somehow.)
        _widthsAcrossColumnRemoval = null
        if (tabs)
            applyWidths(tabs.activeWidths())
        // The pending Ctrl+P reveal and the per-tab scroll restore
        // both live in tableView.onModelChanged, NOT here: this handler can
        // run before the TableView's own model binding has propagated
        // (sibling bindings of root.model, unspecified order), and a position
        // call issued then acts on the OUTGOING model and is clobbered by the
        // incoming rebuild.
    }

    // Detach half: pull the selection model off the TableView BEFORE the
    // active tab flips, so the model swap's internal selection clear hits
    // nothing instead of the outgoing tab's ranges. Reattach happens in
    // tableView.onModelChanged.
    Connections {
        target: root.tabs
        function onActiveAboutToChange() {
            // Freeze any in-flight smooth center first, so the stash
            // below parks a settled position and the glide can't write the
            // OLD tab's contentY into the incoming one.
            centerAnim.stop()
            // Park the OUTGOING tab's scroll position (first visible
            // row) while the active pointers still read it, the same timing
            // contract the detach below relies on. Restored by
            // tableView.onModelChanged on the way back in; persisted to the
            // SCRL chunk (the stash dirties the tab on a real
            // change, the debounced autosave writes it).
            root.stashScrollPosition()
            tableView.selectionModel = null
        }
        // THE launch-restore trigger. Playlist reads are ASYNC, so at
        // launch this view initializes against a still-loading model: both
        // launch parkers (onCompleted, the initial onModelChanged) find
        // scrollRow and the CURR focus row at -1, because the C++ load
        // applies them only when its watcher fires, later, and the model
        // POINTER never changes when the tracks land, so onModelChanged never
        // refires. This signal is the load's completion edge for the ACTIVE
        // tab: park the restore intent now, with the same precedence as the
        // launch parker (persisted position first, focus-row centering as the
        // fallback), and let the gated, verified apply land it as the rows
        // and content size settle. Also fires for a file opened into the
        // active tab, which thereby restores ITS saved position, foobar-like.
        function onActiveTabLoadCompleted() {
            if (!root.model)
                return
            var sr = root.tabs ? root.tabs.activeScrollRow() : -1
            if (sr >= 0) {
                root._pendingPositionMode = 1
                root._pendingPositionRow = sr
            } else if (root._currentRow >= 0) {
                root._pendingPositionMode = 3
                root._pendingPositionRow = root._currentRow
            } else {
                root._pendingPositionMode = 0
            }
            root._pendingTargetY = -1
            root._pendingTries = 0
            if (root._pendingPositionMode !== 0)
                Qt.callLater(root._applyPendingPosition)
        }
    }

    // Late-binding half: root.selectionModel's own binding may re-evaluate
    // after tableView.onModelChanged already ran (dependent-binding order is
    // unspecified). Re-assert the pairing whenever it settles; assigning the
    // already-attached object is a harmless no-op.
    onSelectionModelChanged: {
        if (selectionModel && tableView.model === selectionModel.model)
            tableView.selectionModel = selectionModel
    }

    // Show or hide a column by field id, preserving the rest of the current
    // arrangement. Routed through the model's GRANULAR ops (showColumn /
    // hideColumn: begin/endInsert/RemoveColumns), NOT applyColumnLayout: that
    // reset is a load-time semantic which clears the selection model and makes
    // AudioController treat the playlist as replaced (cursor detached, green
    // marker lost, no continuation to the next track). A granular op remaps
    // the selection and never touches the cursor.
    //
    // SHOW seeds the appended column's width and re-asserts positionally (the
    // reorderColumn pattern). HIDE deliberately does NO width bookkeeping: the
    // centralized columnsAboutToBeRemoved/columnsRemoved Connections above
    // handles every removal source (this toggle AND the registry-driven
    // custom-column delete); fixing up here too would splice the widths twice.
    // Never removes the last remaining column.
    function _toggleColumn(fieldId, width, makeVisible) {
        if (!root.model)
            return
        var order = currentColumnOrder()
        var i = order.indexOf(fieldId)
        if (makeVisible) {
            if (i !== -1)
                return // already shown
            var widths = currentColumnWidths() // pre-insert snapshot, by position
            if (!root.model.showColumn(fieldId))
                return // unknown/retired id: the model refused, nothing changed
            widths.push(width > 0 ? width : 120)
            applyWidths(widths) // re-assert + relayout + alignment-binding kick
            // Re-park in the new positional order, the same belt-and-suspenders
            // reorderColumn() does: an autosave racing the header's
            // onContentWidthChanged restash must not pair field ids with
            // pre-toggle positions.
            if (root.tabs)
                root.tabs.stashActiveWidths(currentColumnWidths())
            // A freshly appended column is not covered by the pre-insert
            // selection ranges (its cells would render unselected mid-row);
            // re-assert the selection full-width in ONE batched op.
            if (root.selectionModel)
                root.model.reselectFullWidth(root.selectionModel)
        } else {
            if (i === -1)
                return // already hidden
            if (order.length <= 1)
                return // keep at least one column
            root.model.hideColumn(fieldId)
        }
    }

    // Perform a reorder: move the column at visual index `from` to `to`. The
    // model move (a begin/endMoveColumns, NOT a reset) preserves selection,
    // current row and anchor; the header, being bound to the model, reorders its
    // own sections off the same columnsMoved. TableView keeps explicit widths by
    // POSITION, so we snapshot widths first, permute them so the dragged column
    // carries its own width to its new home, and re-assert onto the header.
    function reorderColumn(from, to) {
        if (from === to || from < 0 || to < 0 || !root.model)
            return
        var widths = currentColumnWidths()
        if (!root.model.moveVisualColumn(from, to))
            return
        var moved = widths.splice(from, 1)[0]
        widths.splice(to, 0, moved)
        for (var c = 0; c < widths.length; ++c)
            header.setColumnWidth(c, widths[c])
        header.forceLayout()
        tableView.forceLayout()
        // A reorder is a pure permutation, so the header's TOTAL content width is
        // unchanged: onContentWidthChanged never fires, and the parked widths would
        // otherwise stay in the PRE-reorder order while currentColumnOrder() is now
        // POST-reorder. An autosave/close then pairs each field-id with the wrong
        // width and the columns swap widths on reload. Re-park here so the stored
        // widths stay positionally aligned with the model's column order.
        if (root.tabs)
            root.tabs.stashActiveWidths(currentColumnWidths())
        root._layoutRevision++ // kick the alignment bindings (see _layoutRevision)
    }

    // Left edge x (in tableView viewport coords) of a visual column, from the
    // cumulative widths minus the horizontal scroll. Used to place the drop
    // indicator. col === columnCount returns the right edge of the last column.
    function _columnLeftEdge(col) {
        var w = currentColumnWidths()
        var x = 0
        for (var i = 0; i < col && i < w.length; ++i)
            x += w[i]
        return x - tableView.contentX
    }

    // The column boundary (gap) nearest a viewport x: index 0..columnCount,
    // where boundary b is the left edge of column b. This is what makes the
    // drop point track the cursor, we pick the closest gap rather than the
    // far edge of whatever column the pointer happens to be inside.
    function _nearestBoundary(px) {
        var w = currentColumnWidths()
        var acc = -tableView.contentX // viewport x of boundary 0
        var best = 0
        var bestDist = Number.POSITIVE_INFINITY
        for (var b = 0; b <= w.length; ++b) {
            var d = Math.abs(acc - px)
            if (d < bestDist) {
                bestDist = d
                best = b
            }
            if (b < w.length)
                acc += w[b]
        }
        return best
    }

    // The rounded frame. The content sits inside, rounded and clipped; the
    // 1px stroke is drawn LAST as a sibling overlay on top of the content, so
    // opaque cells (e.g. the header) can never paint over the corner arc.
    // 10% opacity at rest, animating to 100% on hover over the whole area.
    Item {
        id: playlistFrame
        anchors.fill: parent

        // Hover over the entire playlist region (header + rows).
        HoverHandler {
            id: playlistHover
        }

        // Rounded, clipped content. The radius matches the stroke overlay so
        // the content corners tuck neatly inside the border.
        Rectangle {
            id: contentClip
            anchors.fill: parent
            clip: true
            color: "transparent"
            radius: 4

            Column {
                anchors.fill: parent
                spacing: 0

                HorizontalHeaderView {
                    id: header
                    clip: true
                    // INDEPENDENT header: bound to the model directly, NOT synced
                    // to the body. It renders its own cells from the model's
                    // columnCount and headerData (titles below come from `display`),
                    // so it always has realized columns, which is what lets the
                    // built-in resize and setColumnWidth work even with zero rows.
                    model: root.model
                    width: parent.width

                    // A synced header would scroll in lockstep with the body for
                    // free; independent, this does it by hand. The
                    // header doesn't flick on its own (interactive:false) and its
                    // horizontal scroll mirrors the body's, so columns stay aligned
                    // under the header when the body is scrolled right, and the
                    // reorder drop math (which works in body-scroll coords) lines up.
                    interactive: false
                    contentX: tableView.contentX

                    // Built-in column resize. Because the header is independent
                    // (its own cells, never empty), this works whether or not the
                    // playlist has rows, the case a synced header cannot handle
                    // without a custom grip. The reorder MouseArea is inset from
                    // the edges so this owns the boundary band.
                    resizableColumns: true

                    // The header owns the column widths. Its provider returns the
                    // explicit width once set (by a resize drag or applyColumnLayout),
                    // else the schema default, so columns show sensible widths from
                    // the first frame with no seeding step.
                    columnWidthProvider: function (column) {
                        var w = explicitColumnWidth(column)
                        return w >= 0 ? w : root._defaultColumnWidth(column)
                    }

                    delegate: Item {
                        id: headerCell
                        // Header has no rowHeightProvider, so this implicitHeight
                        // DOES set the header height (unlike the body cells). The
                        // wrapper is now a plain Item; the visible cell is the
                        // shared HeaderCell below, with the reorder and right-click
                        // MouseAreas layered on top as siblings. That keeps the
                        // load-bearing interaction here and the look in one shared
                        // place, the metadata header uses the same component.
                        implicitHeight: 26

                        required property int column
                        required property var display
                        // Injected alignment role (see ColumnAlignmentRole in the
                        // model): refreshed by the same headerDataChanged that
                        // refreshes `display`, so a move/hide updates both
                        // atomically per cell.
                        required property int columnAlignment

                        // Shared header-cell visual: the Theme.headerBand band, bold elided
                        // title, and right-edge divider, identical to what it drew
                        // inline before (and to the metadata header). showDivider is
                        // on for the playlist's inter-column boundaries.
                        HeaderCell {
                            anchors.fill: parent
                            display: headerCell.display
                            showDivider: true

                            // While this column is the drag source the visual "lifts
                            // out": faded in place (leaving a gap) and shown instead
                            // as the floating ghost that tracks the cursor. Opacity
                            // sits on the visual, not the wrapper, so the reorder
                            // MouseArea below keeps its grab through the drag.
                            opacity: (root._dragColumn === headerCell.column) ? 0.0 : 1.0

                            // Alignment is the injected columnAlignment ROLE,
                            // refreshed by the same headerDataChanged that
                            // refreshes titles (the scenario this prevents: an
                            // invokable plus a _layoutRevision kick does not
                            // re-reach these in-place header cells after a
                            // column move, so each keeps its pre-move by-index
                            // alignment until a tab switch rebuilds it), while
                            // the dataChanged-driven `display` injection does
                            // reach them; alignment rides that same
                            // channel instead of racing delegate reuse.
                            alignment: headerCell.columnAlignment === Qt.AlignRight
                                         ? Text.AlignRight
                                     : headerCell.columnAlignment === Qt.AlignHCenter
                                         ? Text.AlignHCenter
                                     : Text.AlignLeft
                        }

                        // Drag-to-reorder. Inset from each edge by _resizeGrip so
                        // the header's built-in resize owns the boundary band; the
                        // center of the cell initiates a reorder. A press that
                        // never exceeds _dragThreshold does nothing, so it never
                        // competes with resize and doesn't steal plain clicks.
                        // The inset split is clean: center => reorder, edge =>
                        // resize.
                        MouseArea {
                            anchors.fill: parent
                            anchors.leftMargin: root._resizeGrip
                            anchors.rightMargin: root._resizeGrip
                            acceptedButtons: Qt.LeftButton
                            // The header is a Flickable; without this it reclaims
                            // the grab for flicking once the drag passes the flick
                            // threshold, canceling our drag. (interactive:false on
                            // the header reduces this, but preventStealing keeps the
                            // grab unambiguously ours for the whole drag.)
                            preventStealing: true
                            cursorShape: _dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor

                            property real _pressX: 0
                            property bool _dragging: false

                            onPressed: function (mouse) {
                                _pressX = mouse.x
                                _dragging = false
                            }

                            onPositionChanged: function (mouse) {
                                if (!_dragging) {
                                    if (Math.abs(mouse.x - _pressX) < root._dragThreshold)
                                        return
                                    _dragging = true
                                    root._dragColumn = headerCell.column
                                    root._dragTitle = headerCell.display
                                }
                                // Pointer x in contentClip space drives both the
                                // ghost and the drop point. The drop is the gap
                                // nearest the cursor (see _nearestBoundary), no
                                // cellAtPosition, no direction-dependent snapping.
                                root._dragPointerX =
                                    mapToItem(contentClip, mouse.x, mouse.y).x
                                root._dropBoundary =
                                    root._nearestBoundary(root._dragPointerX)
                            }

                            onReleased: function (mouse) {
                                if (_dragging) {
                                    // Boundary b in the CURRENT layout -> final
                                    // index for the move: dropping into a gap past
                                    // the source shifts the target left by one once
                                    // the source is removed.
                                    var b = root._dropBoundary
                                    var from = root._dragColumn
                                    var to = (b <= from) ? b : b - 1
                                    // Defer the commit one tick. reorderColumn no
                                    // longer resets the model (no empty-reset path),
                                    // but moveVisualColumn emits columnsMoved, and
                                    // running that synchronously from inside THIS
                                    // delegate's own handler risks the view churning
                                    // the delegate mid-handler. callLater runs it
                                    // after this handler has fully unwound.
                                    Qt.callLater(root.reorderColumn, from, to)
                                }
                                _dragging = false
                                root._dragColumn = -1
                                root._dropBoundary = -1
                                root._dragTitle = ""
                            }

                            onCanceled: {
                                _dragging = false
                                root._dragColumn = -1
                                root._dropBoundary = -1
                                root._dragTitle = ""
                            }
                        }

                        // Right-click anywhere on a header section opens the
                        // column menu (toggle which columns are shown). A
                        // separate MouseArea accepting only RightButton, so it
                        // never competes with the left-button reorder drag above.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onPressed: {
                                // Clear-then-assign forces a full Instantiator
                                // teardown/rebuild cycle for the submenu items,
                                // so no delegate instance survives a reopen --
                                // including one whose `checked` binding a
                                // previous interactive toggle broke (a click
                                // WRITES checked, severing the declarative
                                // binding on that instance for good).
                                headerMenu.colsPlain = []
                                headerMenu.colsComposite = []
                                headerMenu.customCols = []
                                var catalog = root.model
                                    ? root.model.availableColumns() : []
                                headerMenu.colsPlain = catalog.filter(
                                    function (e) { return !e.composite })
                                headerMenu.colsComposite = catalog.filter(
                                    function (e) { return e.composite })
                                headerMenu.customCols = root.model
                                    ? root.model.customColumnCatalog() : []
                                // Kick the live-query check-mark bindings (see
                                // catalogRevision on the menu).
                                headerMenu.catalogRevision++
                                if (root.model) {
                                    headerMenu.targetFieldId =
                                        root.model.columnFieldId(headerCell.column)
                                    headerMenu.targetTitle = headerCell.display !== undefined
                                        ? String(headerCell.display) : ""
                                    headerMenu.targetIsCustom =
                                        headerMenu.targetFieldId.indexOf("custom:") === 0
                                } else {
                                    headerMenu.targetFieldId = ""
                                    headerMenu.targetTitle = ""
                                    headerMenu.targetIsCustom = false
                                }
                                headerMenu.popup()
                            }
                        }
                    }
                }

                TableView {
                    id: tableView
                    width: parent.width
                    height: parent.height - header.height
                    clip: true

                    model: root.model

                    // Native selection model, shared with our mouse path. Both
                    // of TableView's built-in interaction drivers are OFF and we
                    // own selection entirely:
                    //
                    //  - pointerNavigationEnabled:false, clicks go to the
                    //    delegate's modifier-aware handler (plain/ctrl/shift).
                    //  - keyNavigationEnabled:false, arrows are handled in
                    //    Keys.onPressed below, through the SAME _select* helpers
                    //    and the SAME _anchorRow the mouse uses.
                    //
                    // This is deliberate. TableView's built-in keyboard nav
                    // keeps its OWN selection anchor, independent of our
                    // _anchorRow. Letting it co-drive the shared model made
                    // plain-arrow collapse the row selection (current moved, so
                    // only the cover art followed) and made Shift+arrow extend
                    // from the wrong anchor after a mouse multi-select. One
                    // owner, one anchor, no fighting.
                    // The selection model is managed IMPERATIVELY, never a
                    // declarative binding: TableView clears whichever selection
                    // model is attached when its data model swaps, so a tab
                    // switch with a live binding wiped the OUTGOING tab's
                    // ranges in C++ (probe-confirmed). The lifecycle is
                    // detach on activeAboutToChange (below, while every binding
                    // still reads the outgoing tab), swap, reattach in
                    // onModelChanged once the incoming pair is consistent.
                    onModelChanged: {
                        // Reattach AFTER the swap. Reading root.selectionModel
                        // forces its (lazy) binding to evaluate against the
                        // already-flipped active tab; the pairing guard makes a
                        // mid-swap mismatch attach nothing rather than crossed
                        // wires.
                        var s = root.selectionModel
                        selectionModel = (s && s.model === model) ? s : null
                        // DECIDE the position intent here (the model
                        // has definitely swapped at this point), but APPLY it
                        // through _applyPendingPosition, which waits for the
                        // layout's row count to catch up with the model's (see
                        // the note at the property). A parked Ctrl+P reveal
                        // WINS over the scroll restore (the whole point of the
                        // switch was the reveal); the parking is cleared
                        // consumed or not, so an unrelated switch can never
                        // fire a stale reveal. The restore realigns the parked
                        // first-visible row to the top; -1 (never parked / was
                        // empty) leaves the rebuild's natural position. The
                        // Qt.callLater attempt covers equal-count swaps, where
                        // rowsChanged never fires; it also settles the
                        // root.selectionModel late binding the reveal's plant
                        // half needs.
                        if (root._pendingRevealRow >= 0) {
                            root._pendingPositionMode =
                                (model && audioController.playingModel === model)
                                    ? 2 : 0
                            root._pendingPositionRow = -1
                            root._pendingRevealRow = -1
                        } else if (root.tabs) {
                            var sr = root.tabs.activeScrollRow()
                            root._pendingPositionMode = sr >= 0 ? 1 : 0
                            root._pendingPositionRow = sr
                        } else {
                            root._pendingPositionMode = 0
                        }
                        // A fresh intent starts a fresh verification
                        // budget.
                        root._pendingTargetY = -1
                        root._pendingTries = 0
                        if (root._pendingPositionMode !== 0)
                            Qt.callLater(root._applyPendingPosition)
                    }

                    // The rebuild reports its progress through rows;
                    // each change is a chance the layout now matches the model
                    // (the apply itself re-checks and stays parked otherwise).
                    onRowsChanged: {
                        if (root._pendingPositionMode !== 0)
                            root._applyPendingPosition()
                    }

                    // The content size catching up to the true extent
                    // is the other readiness edge the apply gates on (equal
                    // row counts across a swap never fire rowsChanged, but the
                    // rebuild still re-derives contentHeight).
                    onContentHeightChanged: {
                        if (root._pendingPositionMode !== 0)
                            root._applyPendingPosition()
                    }
                    pointerNavigationEnabled: false
                    keyNavigationEnabled: false
                    focus: true

                    columnSpacing: 0
                    rowSpacing: 0
                    boundsBehavior: Flickable.StopAtBounds
                    // TableView.animate defaults to TRUE, so every
                    // positionViewAtRow in this file was an animated contentY
                    // flight toward the target, and Qt cancels that flight on
                    // any relayout, rebuild, or competing viewport work,
                    // stranding the view wherever the animation was. Around
                    // launch and tab switches such work is routine (header
                    // width seeding, selection-model reattach, first-build polish), so the
                    // launch restore and the first switch-in restores were
                    // canceled a frame after takeoff and every tab "opened at
                    // the top" despite a correctly persisted and parked
                    // position. False makes every programmatic positioning a
                    // synchronous jump with no flight to cancel, which is also
                    // the foobar-correct instant feel for the arrows, Ctrl+F,
                    // Ctrl+P, and the restores. User flicking is unaffected
                    // (animate only governs positionViewAt* calls).
                    animate: false

                    // Keyboard navigation + selection, owned here so it shares
                    // the mouse path's _anchorRow (see the selection note above).
                    //   Up / Down        -> move current row, select it only
                    //   Shift+Up / Down   -> grow the range from _anchorRow
                    // Ctrl/Cmd+A selects all; Delete/Backspace removes the
                    // selection (Backspace is the "Delete" key on macOS).
                    // Enter also reveals an off-screen current row;
                    // Ctrl/Cmd+F centers the selected tracks' area;
                    // Ctrl/Cmd+P reveals the playing track, switching to its
                    // owning tab first when needed.
                    Keys.onPressed: function (event) {
                        var shift = (event.modifiers & Qt.ShiftModifier) !== 0
                        if (event.key === Qt.Key_Down) {
                            root._keyMoveCurrent(1, shift)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Up) {
                            root._keyMoveCurrent(-1, shift)
                            event.accepted = true
                        } else if ((event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter)
                                   && (event.modifiers & Qt.AltModifier)) {
                            // Alt+Enter opens the Properties window for
                            // the selection (foobar's binding), the keyboard
                            // twin of the context menu's Properties entry.
                            // Checked BEFORE the plain Enter branch below.
                            root._openPropertiesWindow()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Return
                                   || event.key === Qt.Key_Enter) {
                            // Play the current row, the same gesture as a
                            // double-click, if a row is current. Also: a row
                            // activated from OUTSIDE the visible band is also
                            // centered, so the view lands where the music
                            // started; an already-visible row plays without
                            // the view jerking.
                            var ci = root.selectionModel
                                   ? root.selectionModel.currentIndex : null
                            if (ci && ci.valid && root.model) {
                                root._pendingActivateRow = ci.row
                                audioController.playAt(root.model, ci.row)
                                if (!root._rowFullyVisible(ci.row))
                                    root._centerRow(ci.row, true)
                            }
                            event.accepted = true
                        } else if (event.key === Qt.Key_A
                                && (event.modifiers & Qt.ControlModifier
                                    || (Qt.platform.os === "osx"
                                        && event.modifiers & Qt.MetaModifier))) {
                            root._selectAll()
                            event.accepted = true
                        } else if (event.key === Qt.Key_F
                                && (event.modifiers & Qt.ControlModifier
                                    || (Qt.platform.os === "osx"
                                        && event.modifiers & Qt.MetaModifier))) {
                            // Center the selected tracks' area: the
                            // MIDPOINT of the selection's bounding range, so a
                            // scattered selection centers on its span, not its
                            // first row. No selection: the focus row. Neither:
                            // no-op. Pure scroll, the selection and the
                            // current index are untouched, and it centers even
                            // when already visible: the command IS "center".
                            // Accepted regardless, so a dead press can't leak.
                            var selRows = root._selectedRows()
                            if (selRows.length > 0) {
                                var lo = selRows[0]
                                var hi = selRows[0]
                                for (var si = 1; si < selRows.length; ++si) {
                                    if (selRows[si] < lo) lo = selRows[si]
                                    if (selRows[si] > hi) hi = selRows[si]
                                }
                                root._centerRow(Math.floor((lo + hi) / 2), true)
                            } else if (root._currentRow >= 0) {
                                root._centerRow(root._currentRow, true)
                            }
                            event.accepted = true
                        } else if (event.key === Qt.Key_P
                                && (event.modifiers & Qt.ControlModifier
                                    || (Qt.platform.os === "osx"
                                        && event.modifiers & Qt.MetaModifier))) {
                            // Reveal the playing track. In this tab:
                            // plant the focus row on it and center
                            // (_revealPlayingHere). In another tab: park the
                            // reveal and switch; onModelChanged consumes it
                            // once the incoming model IS the playing model.
                            // No live cursor (stopped, detached, or the owning
                            // tab closed so indexOfModel finds nothing):
                            // no-op. Accepted regardless.
                            var pm = audioController.playingModel
                            var pr = audioController.playingRow
                            if (pm && pr >= 0) {
                                if (pm === root.model) {
                                    root._revealPlayingHere(true)
                                } else if (root.tabs) {
                                    var ti = root.tabs.indexOfModel(pm)
                                    if (ti >= 0) {
                                        root._pendingRevealRow = pr
                                        root.tabs.currentIndex = ti
                                    }
                                }
                            }
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape) {
                            root._clearSelection()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Delete
                                   || event.key === Qt.Key_Backspace) {
                            root._deleteSelected()
                            event.accepted = true
                        }
                    }

                    // The body does not own widths anymore, it reads the
                    // header's explicit widths (the single source). The
                    // follow-trigger Connections below relayouts the body whenever
                    // the header's total width changes (a resize or applyColumnLayout),
                    // so this provider is re-queried with the new values. Unset
                    // columns fall back to the schema default, matching the header's
                    // own provider so the two never disagree.
                    columnWidthProvider: function (column) {
                        var w = header.explicitColumnWidth(column)
                        return w >= 0 ? w : root._defaultColumnWidth(column)
                    }
                    rowHeightProvider: function (/*row*/) {
                        return root._rowHeight
                    }                    ScrollBar.vertical: ScrollBar { id: vScroll }
                    ScrollBar.horizontal: ScrollBar { id: hScroll }

                    delegate: Rectangle {
                        id: cell
                        // No implicitHeight: the body row height is set by the
                        // TableView's rowHeightProvider and would override it
                        // anyway. (The header delegate is different, it has no
                        // provider, so its implicitHeight is meaningful.)

                        required property int row
                        required property int column
                        required property var display

                        // Backing file present + readable. False on rows the
                        // load-time cache validator flagged as missing/unreadable
                        // (kept in the playlist, grayed rather than dropped).
                        required property bool rowAvailable

                        // Fed by the TableView from the bound selectionModel.
                        // Because we select with the Rows flag, every cell in a
                        // selected row reports selected:true, so tinting each
                        // cell lights up the whole row, no separate row overlay.
                        required property bool selected
                        required property bool current

                        // True when this is the engine's now-playing row AND this
                        // view's model is the one playback is following. The
                        // model-identity term matters: the same playlist can be
                        // open in another tab, and only the playing one lights up.
                        // Re-evaluates on the controller's playingChanged and on
                        // delegate recycle (row is a required property).
                        readonly property bool isPlayingRow:
                            audioController.playingModel === root.model
                            && audioController.playingRow === cell.row

                        // Base zebra striping; selection tint takes precedence.
                        readonly property color _stripe: (row % 2 === 0) ? Theme.rowEven : Theme.rowOdd
                        // Selection tint is applied INSTANTLY (no Behavior on
                        // color). TableView recycles delegates onto rows that
                        // shift during a delete, and animating the tint across
                        // that reassignment makes the purple smear/jerk for a
                        // frame before settling. Instant tint matches the
                        // selection model exactly and is the conventional
                        // playlist feel anyway.
                        color: selected ? Theme.selectionFill : _stripe

                        // Fade the body of the column being dragged, or the rows
                        // being drag-reordered, so the in-flight target reads as
                        // lifted alongside its drop indicator.
                        opacity: {
                            if (root._dragColumn === column)
                                return 0.45
                            if (root._rowDragging
                                    && row >= root._dragRowFirst
                                    && row <= root._dragRowLast)
                                return 0.5
                            return 1.0
                        }

                        Text {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            text: cell.display
                            // Precedence: the now-playing row reads green (tying
                            // it to the player bar's green status line), then a
                            // selected row gets the lavender FileQueue look, then
                            // an unavailable (missing-file) row is dimmed, then the
                            // normal gray. The green is gated on rowAvailable: a row
                            // the engine is "on" but that is missing/unreadable (a
                            // failed open) must not read as playing, it drops
                            // straight to the dimmed gray the moment it is detected.
                            color: (cell.isPlayingRow && cell.rowAvailable) ? Theme.success
                                 : cell.selected ? Theme.accentHover
                                 : (cell.rowAvailable ? Theme.textPrimary : Theme.textFaint)
                            font.family: Theme.uiFont
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                            // Alignment driven by the column schema (via the
                            // model). Touch _layoutRevision so a reorder re-runs
                            // this binding (see the header note).
                            horizontalAlignment: {
                                root._layoutRevision
                                if (!root.model)
                                    return Text.AlignLeft
                                var a = root.model.columnAlignment(cell.column)
                                return a === Qt.AlignRight ? Text.AlignRight
                                     : a === Qt.AlignHCenter ? Text.AlignHCenter
                                     : Text.AlignLeft
                            }
                        }

                        // Now-playing marker: a slim accent down the left edge of
                        // the playing row. Drawn on column 0 only, so it reads as a
                        // single row marker rather than a line between every cell;
                        // the green row text above carries the cue if the first
                        // column is ever scrolled out of view. Independent of
                        // selection, so it still shows on a selected playing row.
                        Rectangle {
                            visible: cell.isPlayingRow && cell.rowAvailable
                                     && cell.column === 0
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 3
                            color: Theme.success
                        }

                        // foobar-style focus outline on the CURRENT row (the
                        // remembered last-selected track; also the live focus row
                        // during normal use). An outline, never a fill, matching
                        // foobar: current is focus, selection is the tint. The
                        // strokeOverlay pattern, drawn last; per-cell delegates
                        // can't draw one row-wide rectangle, so each cell of the
                        // row draws the top+bottom runs and only the outermost
                        // columns close the left/right edges (interior cell
                        // boundaries stay open so the strokes read as a single
                        // row-wide rectangle).
                        Rectangle {
                            visible: cell.row === root._currentRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: 1
                            color: Theme.focusOutline
                        }
                        Rectangle {
                            visible: cell.row === root._currentRow
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 1
                            color: Theme.focusOutline
                        }
                        Rectangle {
                            visible: cell.row === root._currentRow && cell.column === 0
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 1
                            color: Theme.focusOutline
                        }
                        Rectangle {
                            visible: cell.row === root._currentRow
                                     && cell.column === tableView.columns - 1
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 1
                            color: Theme.focusOutline
                        }

                        // Note: clicks are handled by the single overlay
                        // MouseArea (sibling of the Column below), not here.
                        // Keeping the cell free of its own MouseArea avoids any
                        // event-routing competition between the two.
                    }
                }
            }

            // All playlist click handling lives here, in ONE place, to avoid
            // any event-forwarding/propagation subtleties between competing
            // MouseAreas. This overlay is a sibling of the Column (not a child
            // of the TableView), so it sits on top of the whole content area
            // and spans the full viewport, including the empty region below
            // the last row and right of the last column, and, being outside
            // the Flickable, can't have its press stolen for flicking.
            //
            // We map the press into the table's content space and ask
            // cellAtPosition which cell (if any) is under the cursor:
            //   - no cell (y < 0)        -> empty space -> clear selection
            //   - a cell, with modifiers -> the FileQueue selection flow:
            //       plain      -> exclusive select
            //       ctrl/cmd   -> toggle that row
            //       shift      -> range from anchor (replace)
            //       ctrl+shift -> range from anchor (additive)
            // Holding the press and moving is a drag, arbitrated in
            // onPressed: from an already-selected row it is the block REORDER drag,
            // from a freshly-selected row (plain press) or a shift press it is
            // the SELECT drag that extends the range live. A ctrl press arms
            // the ADDITIVE select drag: holding and moving appends the
            // live range pressRow..cursor to the selection as it stood at drag
            // start. A plain press on EMPTY space arms the rubber band:
            // holding and moving draws a slim frame and live-selects every row
            // its vertical span crosses.
            // Covers the table BODY only: anchored to parent (contentClip,
            // which is a legal sibling/parent anchor) with the top pushed down
            // by the header's height. This leaves the header free for column-
            // resize drags. (Anchoring directly to tableView is illegal here,
            // it's nested inside the Column, so it's neither parent nor sibling
            // of this overlay.)
            MouseArea {
                id: bodyMouse
                anchors.fill: parent
                anchors.topMargin: header.height
                // Leave the scrollbar gutters UNCOVERED so the bars receive
                // their own presses (drag the handle, click the track). This
                // overlay is a later sibling of the Column, so it otherwise
                // sits on top of the bars and eats every press, which is why
                // the bars couldn't be grabbed and why clicking the gutter
                // selected rows. Inset only when the table is actually
                // scrollable in that direction.
                anchors.rightMargin: (tableView.contentHeight > tableView.height)
                    ? vScroll.width : 0
                anchors.bottomMargin: (tableView.contentWidth > tableView.width)
                    ? hScroll.height : 0
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: root._rowDragging ? Qt.ClosedHandCursor : Qt.ArrowCursor

                // Drag-arming state (only for plain, unmodified presses).
                property real _pressY: 0
                property real _lastY: 0
                property int  _pressRow: -1
                property bool _armed: false           // could become a row drag
                property bool _selDrag: false         // armed gesture is a
                                                      // SELECT drag, not
                                                      // a reorder
                property bool _ctrlSelDrag: false     // armed gesture is the
                                                      // ADDITIVE select drag
                                                      //
                property int  _lastSelRow: -1         // last row the select
                                                      // drag ranged to; gates
                                                      // redundant re-selects
                property bool _pendingCollapse: false // pressed an already-
                                                      // selected row; collapse
                                                      // to it on release iff no
                                                      // drag happened

                // Rubber-band state. The press point is stored in CONTENT
                // space so the band stays planted while the autoscroll moves
                // the view; the pointer is tracked in local coords (_lastX /
                // _lastY, shared with the other drags). _bandLo / _bandHi is
                // the last row range applied, gating the per-move batched
                // select to range CHANGES; -1/-1 means "no rows in the band".
                // The _bandV* four are the frame rectangle in bodyMouse
                // coords, refreshed by _bandApply (bindings through mapFromItem
                // would not react to scrolling).
                property bool _bandArmed: false
                property real _bandCX: 0
                property real _bandCY: 0
                property int  _bandLo: -1
                property int  _bandHi: -1
                property real _bandVX: 0
                property real _bandVY: 0
                property real _bandVW: 0
                property real _bandVH: 0
                property real _lastX: 0

                // Recompute the band from the stored press point and a local
                // pointer position: refresh the frame rectangle, intersect the
                // band's vertical span with the uniform-height row lattice, and
                // re-apply the selection as ONE batched range call iff the row
                // range changed. A band spanning no rows clears (and keeps
                // clearing exactly once until rows enter it).
                function _bandApply(lx, ly) {
                    var p = mapToItem(tableView.contentItem, lx, ly)
                    var q = mapFromItem(tableView.contentItem,
                                        Math.min(_bandCX, p.x),
                                        Math.min(_bandCY, p.y))
                    _bandVX = q.x
                    _bandVY = q.y
                    _bandVW = Math.abs(p.x - _bandCX)
                    _bandVH = Math.abs(p.y - _bandCY)

                    var n = root.model ? root.model.rowCount() : 0
                    var lo = Math.floor(Math.min(_bandCY, p.y) / root._rowHeight)
                    var hi = Math.floor(Math.max(_bandCY, p.y) / root._rowHeight)
                    if (n <= 0 || hi < 0 || lo >= n) {
                        if (_bandLo !== -1 || _bandHi !== -1) {
                            _bandLo = -1
                            _bandHi = -1
                            root._clearSelection()
                        }
                        return
                    }
                    lo = Math.max(0, lo)
                    hi = Math.min(n - 1, hi)
                    if (lo === _bandLo && hi === _bandHi)
                        return
                    _bandLo = lo
                    _bandHi = hi
                    root.model.selectRowRange(root.selectionModel, lo, hi, true)
                }

                // Row under a press, in content space (-1 if none).
                function _rowAt(mx, my) {
                    var p = mapToItem(tableView.contentItem, mx, my)
                    return tableView.cellAtPosition(p.x, p.y).y
                }
                // Content-space y for a local y (accounts for scroll).
                function _contentY(my) {
                    return mapToItem(tableView.contentItem, 0, my).y
                }
                // Row for a local y, CLAMPED into 0..n-1: the select drag
                // must keep tracking when the pointer leaves the row band
                // (above the first row, below the last, past either side), so
                // it cannot use cellAtPosition's -1. Rows are uniform height,
                // same premise as _rowBoundaryAtContentY; x is irrelevant.
                function _rowAtClamped(my) {
                    var n = root.model ? root.model.rowCount() : 0
                    if (n <= 0)
                        return -1
                    var r = Math.floor(_contentY(my) / root._rowHeight)
                    return Math.max(0, Math.min(r, n - 1))
                }

                onPressed: function (mouse) {
                    tableView.forceActiveFocus()

                    // Right-click: target the row (selecting it if it isn't part
                    // of the current selection, so the menu acts on something
                    // sensible) and open the context menu. Never arms a drag.
                    if (mouse.button === Qt.RightButton) {
                        _armed = false
                        _pendingCollapse = false
                        var rr = _rowAt(mouse.x, mouse.y)
                        if (rr >= 0) {
                            if (!root.selectionModel.isSelected(root._rowIndex(rr)))
                                root._selectExclusive(rr)
                            root._contextRow = rr
                            rowContextMenu.popup()
                        }
                        return
                    }

                    _pressY = mouse.y
                    _lastX = mouse.x
                    _lastY = mouse.y
                    _armed = false
                    _selDrag = false
                    _ctrlSelDrag = false
                    _lastSelRow = -1
                    _pendingCollapse = false
                    _bandArmed = false

                    var ctrl = (mouse.modifiers & Qt.ControlModifier)
                        || (Qt.platform.os === "osx"
                            && mouse.modifiers & Qt.MetaModifier)
                    var shift = mouse.modifiers & Qt.ShiftModifier

                    var rowIdx = _rowAt(mouse.x, mouse.y)
                    _pressRow = rowIdx
                    if (rowIdx < 0) {
                        // Empty space within the table viewport -> clear, and a
                        // PLAIN press also arms the rubber band: holding
                        // and moving draws the frame and selects the rows it
                        // spans. Modified presses stay click-only clears.
                        root._clearSelection()
                        if (!ctrl && !shift) {
                            var cp = mapToItem(tableView.contentItem,
                                               mouse.x, mouse.y)
                            _bandArmed = true
                            _bandCX = cp.x
                            _bandCY = cp.y
                            _bandLo = -1
                            _bandHi = -1
                        }
                        return
                    }

                    if (shift) {
                        root._selectRange(rowIdx, ctrl /*additive*/)
                        // A plain shift press also arms the select drag,
                        // continuing the range from the SAME anchor. The
                        // additive (ctrl+shift) form stays click-only: a live
                        // additive range never shrinks when the drag reverses,
                        // so its feedback would lie. (The Ctrl-only press below
                        // gets a truthful additive drag anyway, via the additive
                        // baseline-restore.)
                        if (!ctrl) {
                            _armed = true
                            _selDrag = true
                            _lastSelRow = rowIdx
                        }
                    } else if (ctrl) {
                        // Click semantics unchanged: the press toggles the row.
                        // It ALSO arms the additive select drag. The
                        // baseline is NOT snapshotted here but at threshold
                        // crossing (strictly later, so nothing can slip between
                        // snapshot and first apply), which also means a
                        // dragless Ctrl+click never opens a session at all.
                        // _lastSelRow starts at -1, unlike the plain-drag arm: the
                        // FIRST post-threshold move must apply the (possibly
                        // degenerate single-row) range immediately, so the
                        // feedback is baseline-union-range from the instant
                        // the drag exists, even before the cursor changes row.
                        root._toggle(rowIdx)
                        _armed = true
                        _ctrlSelDrag = true
                    } else {
                        // Plain press: arm for a possible drag, with the usual
                        // arbitration. A row that was ALREADY selected arms the
                        // reorder drag (dragging moves the block; we don't
                        // collapse the multi-selection now, only on a dragless
                        // release). A row that was NOT selected is exclusive-
                        // selected here (also planting the range anchor) and
                        // arms the SELECT drag: holding and moving extends the
                        // selection from it. Moving a single unselected row
                        // therefore takes select-then-drag, two gestures;
                        // accepted, and block reorder is unchanged.
                        _armed = true
                        if (root.selectionModel.isSelected(root._rowIndex(rowIdx))) {
                            _pendingCollapse = true
                        } else {
                            root._selectExclusive(rowIdx)
                            _selDrag = true
                            _lastSelRow = rowIdx
                        }
                    }
                }

                onPositionChanged: function (mouse) {
                    // The armed rubber band, checked first (it never
                    // coexists with the row-press arming). Same start threshold
                    // as the other drags, measured on either axis.
                    if (_bandArmed) {
                        _lastX = mouse.x
                        _lastY = mouse.y
                        if (!root._banding) {
                            var bp = mapToItem(tableView.contentItem,
                                               mouse.x, mouse.y)
                            if (Math.abs(bp.x - _bandCX) < root._dragThreshold
                                && Math.abs(bp.y - _bandCY) < root._dragThreshold)
                                return
                            root._banding = true
                        }
                        _bandApply(mouse.x, mouse.y)
                        return
                    }
                    if (!_armed)
                        return
                    _lastX = mouse.x
                    _lastY = mouse.y
                    // The armed ADDITIVE select drag. Same start threshold;
                    // the baseline session opens exactly at threshold crossing.
                    // Each apply is baseline UNION pressRow..cursor (clamped),
                    // one batched call per ROW CHANGE, except the first apply,
                    // which _lastSelRow = -1 lets through unconditionally (see
                    // the arming note in onPressed).
                    if (_ctrlSelDrag) {
                        if (!root._rowCtrlSelDragging) {
                            if (Math.abs(mouse.y - _pressY) < root._dragThreshold)
                                return
                            root.model.beginAdditiveRangeDrag(root.selectionModel)
                            root._rowCtrlSelDragging = true
                        }
                        var cr = _rowAtClamped(mouse.y)
                        if (cr >= 0 && cr !== _lastSelRow) {
                            _lastSelRow = cr
                            root.model.updateAdditiveRangeDrag(
                                root.selectionModel,
                                Math.min(_pressRow, cr),
                                Math.max(_pressRow, cr))
                        }
                        return
                    }
                    // The armed select drag. Same start threshold as the
                    // reorder, then each move ranges anchor..row-under-cursor
                    // (clamped), replacing, exactly one batched select per ROW
                    // CHANGE (_lastSelRow gates the no-move jitter).
                    if (_selDrag) {
                        if (!root._rowSelDragging) {
                            if (Math.abs(mouse.y - _pressY) < root._dragThreshold)
                                return
                            root._rowSelDragging = true
                        }
                        var sr = _rowAtClamped(mouse.y)
                        if (sr >= 0 && sr !== _lastSelRow) {
                            _lastSelRow = sr
                            root._selectRange(sr, false)
                        }
                        return
                    }
                    if (!root._rowDragging) {
                        if (Math.abs(mouse.y - _pressY) < root._dragThreshold)
                            return
                        root._beginRowDrag(_pressRow)
                        _pendingCollapse = false // it's a drag, not a click
                    }
                    root._rowDropBoundary =
                        root._rowBoundaryAtContentY(_contentY(mouse.y))
                }

                onReleased: function (mouse) {
                    // A finished band plants the anchor at its PRESS end
                    // and the current index at the pointer end, so a follow-up
                    // shift-click extends from where the band began.
                    if (root._banding && _bandLo >= 0) {
                        var down = mapToItem(tableView.contentItem,
                                             mouse.x, mouse.y).y >= _bandCY
                        root._anchorRow = down ? _bandLo : _bandHi
                        root.selectionModel.setCurrentIndex(
                            root._rowIndex(down ? _bandHi : _bandLo),
                            ItemSelectionModel.NoUpdate)
                    }
                    // A finished additive drag closes the model session
                    // and plants the anchor at the PRESS row, the current
                    // index at the pointer's row, so a follow-up shift-click
                    // extends from where the appended range began (the rubber-band
                    // band's release convention). A dragless Ctrl+click never
                    // opened a session, so there is nothing to close and the
                    // click's toggle semantics stand untouched.
                    if (root._rowCtrlSelDragging) {
                        root.model.endAdditiveRangeDrag()
                        root._anchorRow = _pressRow
                        var er = _rowAtClamped(mouse.y)
                        if (er >= 0)
                            root.selectionModel.setCurrentIndex(
                                root._rowIndex(er),
                                ItemSelectionModel.NoUpdate)
                    }
                    if (root._rowDragging)
                        root._performRowMove()
                    else if (_pendingCollapse)
                        root._selectExclusive(_pressRow)
                    root._endRowDrag()
                    root._rowSelDragging = false
                    root._rowCtrlSelDragging = false
                    root._banding = false
                    _bandArmed = false
                    _bandLo = -1
                    _bandHi = -1
                    _armed = false
                    _selDrag = false
                    _ctrlSelDrag = false
                    _lastSelRow = -1
                    _pendingCollapse = false
                }

                onCanceled: {
                    // A canceled additive drag still closes the model
                    // session; the selection stays as the last apply left it.
                    if (root._rowCtrlSelDragging)
                        root.model.endAdditiveRangeDrag()
                    root._endRowDrag()
                    root._rowSelDragging = false
                    root._rowCtrlSelDragging = false
                    root._banding = false
                    _bandArmed = false
                    _bandLo = -1
                    _bandHi = -1
                    _armed = false
                    _selDrag = false
                    _ctrlSelDrag = false
                    _lastSelRow = -1
                    _pendingCollapse = false
                }

                // Double-click: collapse to that one row and play it. The
                // single-row selection happens naturally via the click flow;
                // doing it explicitly here guarantees it regardless of any prior
                // multi-selection. Playback: hand the (model, row) to the audio
                // controller, which makes this playlist the playing playlist,
                // cuts to this track, and seeds the engine with the organic tail
                // after it.
                onDoubleClicked: function (mouse) {
                    if (mouse.button !== Qt.LeftButton)
                        return
                    var rr = _rowAt(mouse.x, mouse.y)
                    if (rr < 0)
                        return
                    // The second press set _pendingCollapse (it landed on the row
                    // click 1 selected), which would make this gesture's release
                    // re-run _selectExclusive. That release can land AFTER the
                    // engine's failure comes back and deselects a missing row,
                    // re-selecting it and undoing the deselect. The double-click
                    // already selects the row below, so the collapse-reselect is
                    // redundant here, clear it so the release does nothing and the
                    // failure deselect is final.
                    _pendingCollapse = false
                    root._selectExclusive(rr)
                    if (root.model) {
                        root._pendingActivateRow = rr
                        audioController.playAt(root.model, rr)
                    }
                }

                // Edge autoscroll: while dragging rows near the top/bottom of
                // the body, nudge the view and recompute the drag's target from
                // the last cursor position so it keeps tracking as we scroll:
                // the drop boundary for a reorder drag, the ranged-to row for a
                // select drag.
                Timer {
                    interval: 16
                    repeat: true
                    running: root._rowDragging || root._rowSelDragging
                             || root._rowCtrlSelDragging || root._banding
                    readonly property real edge: 26
                    readonly property real step: 14
                    onTriggered: {
                        var maxY = Math.max(0, tableView.contentHeight - tableView.height)
                        var before = tableView.contentY
                        if (bodyMouse._lastY < edge)
                            tableView.contentY = Math.max(0, tableView.contentY - step)
                        else if (bodyMouse._lastY > bodyMouse.height - edge)
                            tableView.contentY = Math.min(maxY, tableView.contentY + step)
                        if (tableView.contentY === before)
                            return
                        if (root._rowDragging) {
                            root._rowDropBoundary = root._rowBoundaryAtContentY(
                                bodyMouse._contentY(bodyMouse._lastY))
                        } else if (root._banding) {
                            bodyMouse._bandApply(bodyMouse._lastX, bodyMouse._lastY)
                        } else if (root._rowCtrlSelDragging) {
                            // Same recompute as the pointer path, so the
                            // appended range keeps growing while the scroll
                            // moves rows under a stationary cursor.
                            var cr = bodyMouse._rowAtClamped(bodyMouse._lastY)
                            if (cr >= 0 && cr !== bodyMouse._lastSelRow) {
                                bodyMouse._lastSelRow = cr
                                root.model.updateAdditiveRangeDrag(
                                    root.selectionModel,
                                    Math.min(bodyMouse._pressRow, cr),
                                    Math.max(bodyMouse._pressRow, cr))
                            }
                        } else {
                            var sr = bodyMouse._rowAtClamped(bodyMouse._lastY)
                            if (sr >= 0 && sr !== bodyMouse._lastSelRow) {
                                bodyMouse._lastSelRow = sr
                                root._selectRange(sr, false)
                            }
                        }
                    }
                }

                // The band's slim frame. Child of bodyMouse, so its
                // coordinates are the _bandV* rectangle _bandApply maintains;
                // contentClip clips whatever runs past the viewport.
                Rectangle {
                    visible: root._banding
                    x: bodyMouse._bandVX
                    y: bodyMouse._bandVY
                    width: bodyMouse._bandVW
                    height: bodyMouse._bandVH
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.accentSoft
                }
            }

            // Column drop indicator: a vertical accent line sitting in the gap
            // nearest the cursor. Drawn on top of the header and cells, clipped
            // to the rounded content. Purely visual, only mid-drag. Hidden on
            // the two gaps flanking the source column, since dropping there is a
            // no-op (the column wouldn't move).
            Rectangle {
                id: dropIndicator
                width: 2
                color: Theme.accentSoft
                y: 0
                height: parent.height
                visible: root._dragColumn >= 0 && root._dropBoundary >= 0
                         && root._dropBoundary !== root._dragColumn
                         && root._dropBoundary !== root._dragColumn + 1

                x: {
                    if (root._dropBoundary < 0)
                        return 0
                    var edge = root._columnLeftEdge(root._dropBoundary)
                    // Keep the 2px line visible at the extreme edges.
                    return Math.max(0, Math.min(edge, parent.width - width))
                }
            }

            // Row drop indicator: a horizontal accent line in the gap nearest
            // the cursor during a row drag. Hidden on the no-op gaps flanking
            // the dragged block (dropping there wouldn't move it).
            Rectangle {
                id: rowDropIndicator
                x: 0
                width: parent.width
                height: 2
                color: Theme.accentSoft
                visible: root._rowDragging && root._rowDropBoundary >= 0
                         && !(root._rowDropBoundary >= root._dragRowFirst
                              && root._rowDropBoundary <= root._dragRowLast + 1)

                y: {
                    if (root._rowDropBoundary < 0)
                        return header.height
                    var yy = header.height
                           + root._rowDropBoundary * root._rowHeight
                           - tableView.contentY
                    // Clamp into the body band so it's always visible.
                    return Math.max(header.height,
                                    Math.min(yy, parent.height - height))
                }
            }

            // Floating "ghost" of the dragged column: a full-height slab that
            // tracks the cursor while the source column sits lifted out. Sheets
            // style, a translucent fill a touch lighter than the playlist body
            // with a thin accent stroke, no snapshot. Its title sits in the
            // header band at the top.
            Rectangle {
                id: dragGhost
                visible: root._dragColumn >= 0
                z: 10

                y: 0
                height: parent.height // full playlist height (header + body)
                // The dragged column's actual on-screen width.
                width: {
                    var w = root.currentColumnWidths()
                    return (root._dragColumn >= 0 && root._dragColumn < w.length)
                        ? w[root._dragColumn] : 120
                }
                x: Math.max(0, Math.min(root._dragPointerX - width / 2,
                                        parent.width - width))

                // A little lighter than the playlist background, translucent so
                // the columns underneath read through as a placement hint.
                color: Qt.rgba(1, 1, 1, 0.12)
                radius: 2
                border.width: 1
                border.color: Qt.rgba(0xBD / 255, 0xB2 / 255, 0xFF / 255, 0.7)

                // Column title, kept within the header-height band at the top.
                Text {
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    height: header.height
                    text: root._dragTitle
                    color: "#FFFFFF"
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignLeft
                }
            }

            // External file drag-and-drop. Accepts URL drags from the OS, shows
            // a drop indicator at the nearest row gap, and on drop hands the
            // URLs + that gap to PlaylistTabs so files insert WHERE dropped
            // (.rwfpl concatenates; folders recurse; .m3u expands; see
            // dropUrlsIntoActive and TrackScanner). Drag-only, it carries no
            // MouseArea, so selection and row-reorder are untouched.
            DropArea {
                id: fileDrop
                anchors.fill: parent
                property int _boundary: -1

                onEntered: function (drag) {
                    if (!drag.hasUrls) {
                        drag.accepted = false
                        return
                    }
                    _boundary = root._dropBoundaryFromClipY(drag.y)
                }
                onPositionChanged: function (drag) {
                    if (drag.hasUrls)
                        _boundary = root._dropBoundaryFromClipY(drag.y)
                }
                onExited: _boundary = -1
                onDropped: function (drop) {
                    if (drop.hasUrls && root.tabs) {
                        var t0 = Date.now()
                        var at = root._dropBoundaryFromClipY(drop.y)
                        // Hand drop.urls to C++ in ONE property access.
                        // The getter re-decodes the whole text/uri-list payload
                        // per access, and a QML sequence value is a live
                        // REFERENCE back to it (urls[i] re-invokes the getter),
                        // so ANY per-element JS access is O(N^2) over a
                        // mass-file drop; hoisting into a var did not
                        // help for that reason. The invoke-argument conversion
                        // is the one decode this drop pays; the .rwfpl/scanner
                        // split lives in PlaylistTabs::dropUrlsIntoActive.
                        var n = root.tabs.dropUrlsIntoActive(drop.urls, at)
                        drop.accept(Qt.CopyAction)
                        if (root.log)
                            root.log.append("info", "drop: " + n
                                            + " url(s) dispatched in "
                                            + (Date.now() - t0) + " ms")
                    }
                    _boundary = -1
                }
            }

            // Drop indicator for an external file drag: a horizontal accent
            // line in the gap nearest the cursor, mirroring the row-reorder one.
            Rectangle {
                id: fileDropIndicator
                x: 0
                width: parent.width
                height: 2
                color: Theme.accentSoft
                visible: fileDrop.containsDrag && fileDrop._boundary >= 0
                y: {
                    var yy = header.height + fileDrop._boundary * root._rowHeight
                           - tableView.contentY
                    return Math.max(header.height, Math.min(yy, parent.height - height))
                }
            }
        }

        // Stroke overlay: drawn LAST, on top of the content, so the rounded
        // border is never erased by opaque cells at the corners. Transparent
        // fill; only the border is visible. Hover animates its alpha.
        Rectangle {
            id: strokeOverlay
            anchors.fill: parent
            color: "transparent"
            radius: 4
            border.width: 2

            property real strokeOpacity: playlistHover.hovered ? 0.25 : 0.10

            Behavior on strokeOpacity {
                NumberAnimation { duration: 150; easing.type: Easing.InOutQuad }
            }
            border.color: Qt.rgba(0xBD / 255, 0xB2 / 255, 0xFF / 255, strokeOpacity)
        }
    }

    // Follow-trigger. The header owns the column widths; the body reads them via
    // its columnWidthProvider, but TableView caches provider results and only
    // re-queries on a relayout. So whenever the header's total content width
    // changes, a built-in resize drag, or applyColumnLayout, relayout the body
    // so it picks up the new widths. The spike showed this tracks live and
    // smoothly with no "ongoing layout" warning (header and body are separate
    // views, so the body isn't mid-layout when this fires).
    Connections {
        target: header
        function onContentWidthChanged() {
            tableView.forceLayout()
            // Keep the active tab's PARKED widths current on every resize, so a
            // debounced autosave (and a later switch-away) write the real widths
            // rather than a stale snapshot. Fires for the single active header,
            // so it always targets the active tab.
            if (root.tabs)
                root.tabs.stashActiveWidths(root.currentColumnWidths())
        }
    }

    // Right-click column menu on the header. For a custom column, an "Edit custom
    // column..." entry jumps to the manager; a "Columns" submenu is the checkable
    // catalog of every field (checked = shown) for add/remove. `cols` + the
    // target* props are refreshed from the model each time the menu opens (see the
    // header MouseArea).
    ThemedMenu {
        id: headerMenu
        // The native catalog, split on the composite flag (see
        // availableColumns: pre-sorted plain-alpha then composite-alpha; the
        // split decides which side of the submenu separator an entry lands on).
        property var colsPlain: []
        property var colsComposite: []
        property var customCols: []
        // Bumped on every open; the check-mark bindings read it so they
        // re-query isColumnShown() fresh per popup (they are invokable calls,
        // not tracked properties).
        property int catalogRevision: 0
        property string targetFieldId: ""
        property string targetTitle: ""
        // The right-clicked column is a custom one ("custom:<id>"). Native
        // columns have no per-column context action (their header label is
        // code-only; build a custom column to show a field under another name).
        property bool targetIsCustom: false

        // Custom columns only: jump to the manager, scrolled to this record.
        // ThemedMenuItem rather than a bare MenuItem: direct children bypass
        // the delegate, so the bare form rendered unthemed AND had no
        // fitWidth, meaning the (arbitrarily long) title never drove the
        // popup width.
        ThemedMenuItem {
            text: "Edit custom column \u201C" + headerMenu.targetTitle + "\u201D\u2026"
            collapsed: !headerMenu.targetIsCustom

            onTriggered: {
                customColumnsWindow.openManager(headerMenu.targetFieldId)
            }
        }
        MenuSeparator {
            visible: headerMenu.targetIsCustom
            height: visible ? implicitHeight : 0
        }
        ThemedMenu {
            id: columnsSubmenu
            title: "Columns"

            // Two Instantiators, one per catalog group, around a static
            // separator. availableColumns() delivers the sort (plain fields
            // alphabetically, then the composite presentation fields
            // alphabetically); the split here only decides which side of the
            // separator an entry lands on. Insert mechanics: the separator is
            // a declared child (item 0 at creation); the plain group inserts
            // at 0..n-1, pushing the separator to position n; the composite
            // group APPENDS, landing after it. The per-popup clear-then-assign
            // rebuild replays that same sequence, so the layout is stable
            // across reopens.
            Instantiator {
                model: headerMenu.colsPlain
                delegate: ThemedMenuItem {
                    // Injected by the Instantiator's delegate model; under Bound
                    // it must be declared or creation fails with a loud
                    // "required property not set" (never a silent undefined).
                    required property var modelData
                    checkable: true
                    // Live query against the CURRENT model, kicked per popup;
                    // never the catalog snapshot's `visible`, so the mark can't
                    // show another tab's or an earlier open's state.
                    checked: {
                        headerMenu.catalogRevision
                        root.model ? root.model.isColumnShown(modelData.fieldId)
                                   : false
                    }
                    text: modelData.title

                    // `checked` is the NEW state after the click toggled it, so it
                    // is exactly the desired visibility to apply.
                    onTriggered: root._toggleColumn(modelData.fieldId, modelData.width, checked)
                }
                onObjectAdded: function (index, object) { columnsSubmenu.insertItem(index, object) }
                onObjectRemoved: function (index, object) { columnsSubmenu.removeItem(object) }
            }
            MenuSeparator {
                visible: headerMenu.colsPlain.length > 0
                         && headerMenu.colsComposite.length > 0
            }
            Instantiator {
                model: headerMenu.colsComposite
                delegate: ThemedMenuItem {
                    required property var modelData
                    checkable: true
                    // Same live query as the plain group above.
                    checked: {
                        headerMenu.catalogRevision
                        root.model ? root.model.isColumnShown(modelData.fieldId)
                                   : false
                    }
                    text: modelData.title

                    onTriggered: root._toggleColumn(modelData.fieldId, modelData.width, checked)
                }
                onObjectAdded: function (index, object) { columnsSubmenu.addItem(object) }
                onObjectRemoved: function (index, object) { columnsSubmenu.removeItem(object) }
            }
        }

        // Defined custom columns, toggled exactly like the built-in catalog
        // (same _toggleColumn path; the field ids are "custom:<id>", which
        // applyColumnLayout resolves against the registry). Definitions are
        // created/edited in the "Manage custom columns..." window below.
        ThemedMenu {
            id: customColumnsSubmenu
            title: "Custom columns"

            // A bare disabled hint when none are defined yet, so the submenu is
            // never confusingly empty.
            ThemedMenuItem {
                collapsed: headerMenu.customCols.length !== 0
                enabled: false
                text: "Nothing to see here!"
                textItalic: true
            }
            Instantiator {
                delegate: ThemedMenuItem {
                    required property var modelData
                    checkable: true
                    // Same live query as the native catalog above.
                    checked: {
                        headerMenu.catalogRevision
                        root.model ? root.model.isColumnShown(modelData.fieldId)
                                   : false
                    }
                    text: modelData.title.length > 0 ? modelData.title : "(unnamed)"

                    onTriggered: root._toggleColumn(modelData.fieldId, modelData.width, checked)
                }
                model: headerMenu.customCols

                onObjectAdded: function (index, object) { customColumnsSubmenu.insertItem(1 + index, object) }
                onObjectRemoved: function (index, object) { customColumnsSubmenu.removeItem(object) }
            }
        }
        MenuSeparator {}
        ThemedMenuItem {
            text: "Manage custom columns\u2026"

            onTriggered: customColumnsWindow.openManager("")
        }
    }

    // The custom-columns manager window ("Manage custom columns..."). A
    // frameless, NON-MODAL tool Window hosted here because this view owns the
    // header menu that opens it and the `customColumns` registry it edits; its
    // transient parent resolves to the main window automatically (this is a
    // child of the view), so it floats and competes for focus alongside the
    // Properties and Settings windows. Single reused instance (show/hide). See
    // CustomColumnsWindow.qml; opened via openManager(fieldId).
    CustomColumnsWindow {
        id: customColumnsWindow
    }

    // Right-click context menu for playlist rows. Themed (blurred translucent)
    // to match the menu bar and header menus.
    ThemedMenu {
        id: rowContextMenu
        ThemedMenuItem {
            // Right-click already selected the row, so this force-reloads the
            // current selection's tags from disk (mtime/size be damned).
            text: "Reload info from file(s)"
            onTriggered: if (root.reloader) root.reloader.reloadSelected()
        }
        ThemedMenuItem {
            // Whole-playlist cleanup: drop every grayed (missing/unreadable) row.
            text: "Remove missing tracks"
            onTriggered: if (root.model) root.model.removeUnavailableTracks()
        }
        MenuSeparator {}
        // A submenu, not a flat item, so sibling file operations have a home.
        // The submenu's TITLE item is generated by the parent menu's delegate;
        // ThemedMenuItem there keeps it themed and (via fitWidth) inside the
        // fit-to-content width machinery, per the Qt-Menu-sizing lessons.
        delegate: ThemedMenuItem {}
        ThemedMenu {
            title: "File Operations"
            ThemedMenuItem {
                text: "Rename to..."
                onTriggered: root._openRenameDialog()
            }
        }
        MenuSeparator {}
        ThemedMenuItem {
            text: "Properties"
            onTriggered: root._openPropertiesWindow()
        }
    }

    // The track Properties window factory (right-click "Properties"). Hosted here
    // because this view owns the model and the selection it operates on. Windows
    // are NON-MODAL and MULTI-INSTANCE (foobar2000 style): every invocation
    // creates a fresh instance that snapshots the selection on open and destroys
    // itself on close.
    //
    // The instance is QML-parented to this view (long-lived: tab switching swaps
    // the model underneath it, never the view), so open windows survive both a
    // tab switch and a tab close; app teardown reaps any stragglers. reloader
    // is passed as a createObject INITIAL VALUE, deliberately a capture rather
    // than a binding: root.reloader tracks the ACTIVE tab, and a later tab
    // switch must not retarget an open window's post-apply reload. Fonts come
    // from the Theme singleton, so nothing font-related is passed anymore.
    Component {
        id: propertiesWindowComponent
        PropertiesWindow {}
    }

    // Cascade counter: each new window opens 24 px further down-right from the
    // main window's center (wrapping after 8) so stacked opens never sit exactly
    // on top of each other. Monotonic; never reset, the modulo does the work.
    property int _propertiesSpawnCount: 0

    function _openPropertiesWindow() {
        var rows = root._selectedRows()
        if (rows.length === 0)
            return
        // The `as` assertion types the handle for the tooling; createObject
        // alone returns a bare QObject as far as qmllint can see.
        var win = propertiesWindowComponent.createObject(root, {
            reloader: root.reloader
        }) as PropertiesWindow
        if (!win) {
            console.warn("PropertiesWindow creation failed:",
                         propertiesWindowComponent.errorString())
            return
        }
        var host = root.Window.window
        if (host) {
            var off = (root._propertiesSpawnCount % 8) * 24
            win.x = host.x + Math.max(0, Math.round((host.width - win.width) / 2)) + off
            win.y = host.y + Math.max(0, Math.round((host.height - win.height) / 2)) + off
        }
        root._propertiesSpawnCount++
        win.openFor(root.model, rows)
    }

    // The Rename Files dialog factory ("File Operations > Rename to...").
    // PropertiesWindow's lifetime verbatim: NON-MODAL, MULTI-INSTANCE, a fresh
    // window per invocation that snapshots the selection in openFor and
    // destroys itself on close; parented to this long-lived view so it
    // survives tab switches (its captured model is guarded through
    // playlistTabs.objectAlive inside the dialog). Shares the Properties
    // cascade counter so mixed stacks of both window kinds fan out together.
    Component {
        id: renameDialogComponent
        RenameFilesDialog {}
    }

    function _openRenameDialog() {
        var rows = root._selectedRows()
        if (rows.length === 0)
            return
        var win = renameDialogComponent.createObject(root, {}) as RenameFilesDialog
        if (!win) {
            console.warn("RenameFilesDialog creation failed:",
                         renameDialogComponent.errorString())
            return
        }
        var host = root.Window.window
        if (host) {
            var off = (root._propertiesSpawnCount % 8) * 24
            win.x = host.x + Math.max(0, Math.round((host.width - win.width) / 2)) + off
            win.y = host.y + Math.max(0, Math.round((host.height - win.height) / 2)) + off
        }
        root._propertiesSpawnCount++
        win.openFor(root.model, rows)
    }
}
