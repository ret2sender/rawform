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

// Bound component behavior: nested components and delegates resolve outer
// document ids statically instead of through dynamic context lookup. The
// flip side is that views no longer inject model data into delegates via
// context; every delegate in this file declares what it consumes as
// `required property`, which is both the contract and the qmllint proof.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import com.rawform.app

// =============================================================================
// MetadataPropertiesPane.qml
//
// The Properties window's editable-Metadata tab.
//
// Renders PropertiesMetadataModel's schema (the "Metadata" header, the fixed
// fields shown even when empty, "<custom>" rows, the guillemet multi-value
// marker, and a trailing "+ add new") with the dock metadata palette, and edits:
//   - Single-track selection: double-click a value, or F2 / Enter on a single
//     selected row, edits IN PLACE (inline editor; Enter or clicking away
//     commits, Esc discards), and the context menu also offers "Edit", which
//     opens the Edit Value dialog for the same row. Numeric fields accept
//     digits only, Date digits and dash. A value that contains a newline cannot
//     be represented by the one-line inline editor, so every edit-in-place entry
//     point routes such a row to the dialog instead, whose free-text
//     editor is multiline.
//   - Multi-track selection: editing a field opens the Edit Value dialog (Single
//     Value / Individual Values). The "+ add new" row opens the dialog in add
//     mode for any selection size.
//   - A staged value renders in an accent tint with a modify dot until OK / Apply
//     commits or Cancel reverts. A multiline value renders on one line with a
//     return glyph marking each embedded line break (display only).
//
// On top of the base pane: field-row selection (click / Ctrl-click /
// Shift-click / Ctrl+A, orthogonal to the track selection) and a right-click
// context menu wiring Edit and Edit-in-place (single row only), Add field,
// Remove (Del), a blank-space Add menu, a bottom hover-hint line, and the
// clipboard and transform actions. Row selection happens on PRESS, not click,
// with click-hold-drag range selection (see the drag-select block below).
// This stays a separate component from MetadataView (the read-only renderer
// used by the dock and the Details tab).
// =============================================================================
Item {
    id: root

    property PropertiesMetadataModel model
    property string uiFont: Theme.uiFont

    // Fixed Name column width, matching the dock pane's default so the Metadata
    // and Details tabs line up visually.
    readonly property int nameColumnWidth: 120

    // Keyboard-focused row (for F2 / Enter / arrow nav), and the row whose inline
    // editor is open (-1 = none). The bottom hint line shows distribution errors.
    property int currentRow: -1
    property int editingRow: -1
    property string hintText: ""        // red: an error / status message
    property string menuHint: ""        // neutral: hovered context-menu description

    // Field-row multi-selection, over editable (Field / Custom) rows only,
    // and orthogonal to which TRACKS are being edited. Drives the row highlight and
    // is what the context-menu actions target. _rowAnchor is the Shift-range anchor.
    property var _rowSel: ({})          // map row index -> true
    property int _rowAnchor: -1
    property bool _menuBlank: false      // the next popup is the blank-space menu
    // Paste enabled state, recomputed just before each menu popup. The clipboard text
    // is not a reactive property, so a plain binding would never refresh; instead the
    // popup handlers set this from _canPaste() so the Paste item grays out correctly.
    property bool _pasteEnabled: false

    // Emitted whenever the Edit Value dialog should open for a field row: any
    // multi-track edit, the explicit single-track "Edit" menu action, and
    // the multiline routing from an edit-in-place entry point. The host
    // (Properties window) opens the dialog for that row.
    signal editFieldRequested(int row)

    // Emitted when the "+ add new" row is activated; the host opens the dialog in
    // add mode.
    signal addFieldRequested()

    // One-shot guard for the Enter-bounce: committing the inline editor with Enter
    // moves focus back to the list, where Enter is also the "open editor" shortcut,
    // so the same keystroke would immediately reopen the row. The commit sets this;
    // the list's open-editor branch consumes it once. Qt.callLater clears it next
    // cycle in case the event does not reach the list at all (so a later, genuine
    // Enter is never swallowed).
    property bool _suppressReopen: false

    // Live mirror of the open editor's text, so the window can commit an edit the
    // user left open when they click OK / Apply (or clicked away). Set on open and
    // on every keystroke by the inline TextField.
    property string _editorText: ""

    // --- helpers -------------------------------------------------------------
    function _editable(i) {
        return root.model && i >= 0 && i < rowList.count
            && !root.model.isSectionRow(i) && !root.model.isAddRow(i)
    }
    function _firstEditable(dir) {
        // dir +1 from top, -1 from bottom.
        var start = dir > 0 ? 0 : rowList.count - 1
        for (var i = start; i >= 0 && i < rowList.count; i += dir)
            if (root._editable(i))
                return i
        return -1
    }
    function _stepCurrent(dir) {
        if (root.currentRow < 0) { root.currentRow = root._firstEditable(dir); }
        else {
            for (var i = root.currentRow + dir; i >= 0 && i < rowList.count; i += dir) {
                if (root._editable(i)) { root.currentRow = i; break }
            }
        }
        if (root.currentRow >= 0) {
            rowList.positionViewAtIndex(root.currentRow, ListView.Contain)
            // Arrow nav collapses the selection to the focus row.
            var s = ({}); s[root.currentRow] = true
            root._rowSel = s; root._rowAnchor = root.currentRow
        }
    }

    // --- field-row selection -------------------------------------------------
    function _selRows() {
        var out = []
        for (var k in root._rowSel)
            if (root._rowSel[k]) out.push(parseInt(k))
        out.sort(function (a, b) { return a - b })
        return out
    }
    function _selCount() { return root._selRows().length }
    function _isRowSel(i) { return root._rowSel[i] === true }
    function _clearRowSel() { root._rowSel = ({}); root._rowAnchor = -1 }
    function _selectRow(index, mods) {
        if (!root._editable(index)) return
        var s
        if ((mods & Qt.ShiftModifier) && root._rowAnchor >= 0) {
            s = ({})
            var lo = Math.min(root._rowAnchor, index)
            var hi = Math.max(root._rowAnchor, index)
            for (var i = lo; i <= hi; ++i) if (root._editable(i)) s[i] = true
        } else if (mods & Qt.ControlModifier) {
            s = Object.assign({}, root._rowSel)
            if (s[index]) delete s[index]; else s[index] = true
            root._rowAnchor = index
        } else {
            s = ({}); s[index] = true; root._rowAnchor = index
        }
        root._rowSel = s
        root.currentRow = index   // last-clicked is the focus / anchor row
        root.hintText = ""
    }
    function _selectAllRows() {
        var s = ({})
        for (var i = 0; i < rowList.count; ++i) if (root._editable(i)) s[i] = true
        root._rowSel = s
        root._rowAnchor = root._firstEditable(1)
    }

    // --- click-hold-drag selection --------------------------------------
    // Press a field row and, without releasing, move up/down: the selection
    // ranges live from the press anchor, mirroring the playlist. The press
    // handling stays per-delegate (an overlay would eat the inline editor's
    // events), so the moves arrive in the PRESSED row's MouseArea coordinates;
    // each is mapped into rowList's viewport, then to content space, clamped,
    // and resolved to a row that _dragSelSnap walks onto the nearest editable
    // row TOWARD the anchor (so overshooting into a section header or the
    // "+ add new" strip selects through the first/last field instead of dying).
    // Ctrl presses never arm (a live toggle range cannot shrink truthfully);
    // shift presses arm and continue from the standing anchor. The edge
    // autoscroll Timer below rowList keeps the drag tracking when the pointer
    // parks beyond the list. _dragSelLast gates redundant re-selects to row
    // CHANGES.
    property bool _dragSelActive: false
    property real _dragSelY: 0           // last pointer y, rowList viewport coords
    property int  _dragSelLast: -1

    function _beginDragSel(item, y, modifiers) {
        if (modifiers & Qt.ControlModifier)
            return
        root._dragSelActive = true
        root._dragSelLast = -1
        root._dragSelY = item.mapToItem(rowList, 0, y).y
    }
    function _dragSelMove(item, y) {
        if (!root._dragSelActive)
            return
        root._dragSelY = item.mapToItem(rowList, 0, y).y
        root._dragSelApply()
    }
    function _dragSelSnap(idx) {
        if (root._editable(idx))
            return idx
        if (root._rowAnchor < 0)
            return -1
        var dir = (idx > root._rowAnchor) ? -1 : 1
        for (var i = idx; i >= 0 && i < rowList.count; i += dir)
            if (root._editable(i))
                return i
        return -1
    }
    function _dragSelApply() {
        if (rowList.count === 0)
            return
        var cy = Math.max(0, Math.min(root._dragSelY + rowList.contentY,
                                      rowList.contentHeight - 1))
        var idx = rowList.indexAt(1, cy)
        if (idx < 0)  // over the footer pad (or an empty seam): snap to an end
            idx = (cy <= 0) ? 0 : rowList.count - 1
        idx = root._dragSelSnap(idx)
        if (idx < 0 || idx === root._dragSelLast)
            return
        root._dragSelLast = idx
        root._selectRow(idx, Qt.ShiftModifier)  // range from _rowAnchor
    }
    function _endDragSel() {
        root._dragSelActive = false
        root._dragSelLast = -1
    }
    function _removeSelection() {
        var rows = root._selRows()
        if (rows.length === 0 || !root.model) return
        root.commitOpenEditor()
        root.model.removeFields(rows)
        root._clearRowSel()
        root.currentRow = -1
    }

    // --- clipboard ------------------------------------------------
    // Copy serializes the selected field rows to the system clipboard as one block
    // per selected track. Cut copies then removes. Paste distributes the clipboard's
    // blocks across the WHOLE track selection (it does not depend on which field rows
    // are selected), so the cleaning workflow is select-all, Copy, Remove, Apply,
    // then Paste, Apply. _canPaste gates the menu item and the Cmd/Ctrl+V shortcut.
    function _copy() {
        if (!root.model || root._selCount() < 1) return
        clip.setText(root.model.serializeFields(root._selRows()))
    }
    function _cut() {
        if (!root.model || root._selCount() < 1) return
        root._copy()
        root._removeSelection()
    }
    function _canPaste() {
        return !!root.model && root.model.canPasteFields(clip.text())
    }
    function _paste() {
        if (!root._canPaste()) return // nothing valid on the clipboard: no-op
        root.commitOpenEditor()
        var err = root.model.pasteFields(clip.text())
        root.hintText = (err !== "") ? err : ""
    }

    // --- transforms -----------------------------------------------
    // Capitalize and Clean up rewrite the selected fields' values in place (indices
    // unchanged, so the selection is kept for chaining). Crop removes everything
    // except the selection, which can shift row indices, so it clears the selection.
    // Auto track number targets Track Number / Total Tracks regardless of selection.
    function _capitalize() {
        if (!root.model || root._selCount() < 1) return
        root.commitOpenEditor()
        root.model.capitalizeFields(root._selRows())
    }
    function _cleanup() {
        if (!root.model || root._selCount() < 1) return
        root.commitOpenEditor()
        root.model.cleanWhitespaceFields(root._selRows())
    }
    function _crop() {
        if (!root.model || root._selCount() < 1) return
        root.commitOpenEditor()
        root.model.cropToFields(root._selRows())
        root._clearRowSel()
        root.currentRow = -1
    }
    function _autoNumber() {
        if (!root.model) return
        root.commitOpenEditor()
        root.model.autoNumberTracks()
    }
    function _openEditor(row) {
        // The "+ add new" row opens the dialog in add mode (any selection size).
        if (root.model && root.model.isAddRow(row)) {
            root.currentRow = row
            root.addFieldRequested()
            return
        }
        if (!root._editable(row))
            return
        // Default activation (double-click / Enter): edit in place when the
        // inline editor can represent the value, else the dialog; _editInPlace
        // owns that routing.
        root._editInPlace(row)
    }

    // Edit-in-place router. Every edit-in-place entry point (double-click,
    // Enter, F2, the "Edit in place" menu item) funnels through here. Two cases
    // route to the Edit Value dialog instead of the inline editor:
    //   - a multi-track selection (inline editing is single-track by design;
    //     applyInlineEdit no-ops defensively on anything else), and
    //   - a value containing a newline, which a one-line TextField cannot
    //     represent; committing through it would flatten the value, so the
    //     dialog's multiline editor takes it.
    function _editInPlace(row) {
        if (!root.model || !root._editable(row))
            return
        root.currentRow = row
        if (root.model.selectionCount() !== 1
            || root.model.editText(row).indexOf("\n") >= 0) {
            root.editFieldRequested(row)
            return
        }
        root.editingRow = row
    }
    function _closeEditor() {
        root.editingRow = -1
        rowList.forceActiveFocus()
    }
    function _commitEditor(row, text) {
        // Single-track inline commit; every such edit is accepted, so the
        // hint line clears.
        if (root.model) root.model.applyInlineEdit(row, text)
        root.hintText = ""
        root.editingRow = -1
        root._suppressReopen = true
        rowList.forceActiveFocus()
        Qt.callLater(function () { root._suppressReopen = false })
    }

    // Commit any open inline editor, staging its current text. Called when focus
    // leaves the editor by means other than Enter: clicking another row, clicking
    // empty space, or pressing OK / Apply.
    function commitOpenEditor() {
        if (root.editingRow >= 0)
            root._commitEditor(root.editingRow, root._editorText)
    }

    // Discard any open editor and clear the focus / hint. Called on window open (so
    // a reopen never reappears mid-edit) and on Cancel.
    function cancelEdit() {
        root.editingRow = -1
        root.currentRow = -1
        root.hintText = ""
        root._clearRowSel()
    }

    // Close an open inline editor without committing (discard), leaving the row
    // selection intact. Called when leaving the tab, so edit mode does not persist.
    function closeEdit() {
        root.editingRow = -1
        root.hintText = ""
    }

    // Hand keyboard focus to this pane's key surface. The host window calls
    // it on every tab switch INTO this tab and on window open; without the
    // handoff the previously focused pane keeps receiving shortcuts (Ctrl+A
    // landed on the hidden tab). Mirrors ReplayGainPropertiesPane.focusKeys().
    function focusKeys() { rowList.forceActiveFocus() }

    // The Escape-deselect action, callable from the host window's Shortcut
    // (Keys-based Escape delivery was observed dead on macOS). The rowList Keys
    // branch below routes here too.
    function clearRowSelection() {
        root.hintText = ""
        root.currentRow = -1
        root._clearRowSel()
    }

    // The host window's Escape probe, the exact complement of
    // clearRowSelection(): true iff calling it would change something (a
    // non-empty row-selection map OR a planted focus row). The host clears on
    // true and closes on false, so the pair guarantees deselect-then-close
    // across two presses and never both on one.
    function hasRowSelection() {
        return root._selCount() > 0 || root.currentRow >= 0
    }

    Rectangle {
        id: contentClip
        anchors.fill: parent
        color: Theme.surfacePage
        radius: 4
        clip: true

        Column {
            anchors.fill: parent
            spacing: 0

            // ----- header (Name | Value) -------------------------------------
            Rectangle {
                width: parent.width
                height: 22
                color: Theme.surfacePage
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: 8; width: root.nameColumnWidth - 8
                    text: "Name"; color: Theme.textDim
                    font.family: root.uiFont; font.pixelSize: 12
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    x: root.nameColumnWidth + 8
                    text: "Value"; color: Theme.textDim
                    font.family: root.uiFont; font.pixelSize: 12
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width; height: 1; color: Theme.separator
                }
            }

            // ----- rows ------------------------------------------------------
            // The wrapper carries a background click zone behind the list, so a
            // click on empty space (the footer padding or below short content)
            // commits any open editor and deselects.
            Item {
                id: listArea
                width: parent.width
                height: parent.height - 22 - hintBar.height

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: function (mouse) {
                        root.commitOpenEditor()
                        root.currentRow = -1
                        root._clearRowSel()
                        if (mouse.button === Qt.RightButton) {
                            root._menuBlank = true
                            root._pasteEnabled = root._canPaste()
                            fieldMenu.popup()
                        }
                    }
                }

                ListView {
                    id: rowList
                    width: parent.width
                    // Sized to its content (capped at the available height) so any
                    // empty space sits BELOW the list, where the background MouseArea
                    // catches a click to deselect rather than the flickable eating it.
                    height: Math.min(contentHeight, parent.height)
                    clip: true
                    focus: true
                    model: root.model
                    boundsBehavior: Flickable.StopAtBounds
                    // The drag-select is driven by the PRESSED delegate's
                    // MouseArea; if the view released that delegate while the
                    // autoscroll carried it out of the default cache band, the
                    // mouse grab would die and the drag with it. A generous
                    // cacheBuffer keeps delegates alive across any realistic
                    // field-list span. Cheap here: rows are two Text items.
                    cacheBuffer: 4096
                    ScrollBar.vertical: ScrollBar {}

                    // Edge autoscroll for the drag-select: while a drag is
                    // live and the pointer parks near (or beyond) the list's
                    // edges, nudge the content and re-resolve the ranged-to row
                    // from the parked pointer position, mirroring the playlist's
                    // reorder Timer.
                    Timer {
                        interval: 16
                        repeat: true
                        running: root._dragSelActive
                        readonly property real edge: 20
                        readonly property real step: 10
                        onTriggered: {
                            var maxY = Math.max(0, rowList.contentHeight - rowList.height)
                            var before = rowList.contentY
                            if (root._dragSelY < edge)
                                rowList.contentY = Math.max(0, rowList.contentY - step)
                            else if (root._dragSelY > rowList.height - edge)
                                rowList.contentY = Math.min(maxY, rowList.contentY + step)
                            if (rowList.contentY !== before)
                                root._dragSelApply()
                        }
                    }

                    // Bottom padding that is itself a deselect zone, so a click just
                    // under "+ add new" exits the editor even when the list is tall.
                    footer: MouseArea {
                        width: rowList.width
                        height: 16
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: function (mouse) {
                            root.commitOpenEditor()
                            root.currentRow = -1
                            root._clearRowSel()
                            if (mouse.button === Qt.RightButton) {
                                root._menuBlank = true
                                root._pasteEnabled = root._canPaste()
                                fieldMenu.popup()
                            }
                        }
                    }

                    // Keyboard nav. Only fires when no inline editor holds focus (the
                    // editor swallows its own keys), so these are the not-editing keys.
                    Keys.onPressed: function (e) {
                        var ctrl = (e.modifiers & Qt.ControlModifier)
                        if (e.key === Qt.Key_Up)        { root._stepCurrent(-1); e.accepted = true }
                        else if (e.key === Qt.Key_Down) { root._stepCurrent(1);  e.accepted = true }
                        else if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
                            e.accepted = true
                            if (root._suppressReopen)
                                root._suppressReopen = false   // swallow the commit's own Enter
                            else if (root._selCount() === 1)   // Edit is single-row only
                                root._openEditor(root.currentRow)
                        } else if (e.key === Qt.Key_F2) {
                            e.accepted = true
                            // Edit in place on the single focused field row.
                            // _editInPlace owns the single-track and multiline
                            // routing, so no further gating here.
                            if (root._selCount() === 1)
                                root._editInPlace(root.currentRow)
                        } else if (e.key === Qt.Key_Delete || e.key === Qt.Key_Backspace) {
                            e.accepted = true
                            root._removeSelection()
                        } else if (e.key === Qt.Key_N && ctrl) {
                            e.accepted = true
                            root.addFieldRequested()
                        } else if (e.key === Qt.Key_A && ctrl) {
                            e.accepted = true
                            root._selectAllRows()
                        } else if (e.key === Qt.Key_C && ctrl) {
                            e.accepted = true
                            if (root._selCount() >= 1)
                                root._copy()
                        } else if (e.key === Qt.Key_X && ctrl) {
                            e.accepted = true
                            if (root._selCount() >= 1)
                                root._cut()
                        } else if (e.key === Qt.Key_V && ctrl) {
                            e.accepted = true
                            root._paste()
                        } else if (e.key === Qt.Key_Escape) {
                            root.clearRowSelection(); e.accepted = true
                        }
                    }

                    delegate: Item {
                        id: rdel
                        required property int index
                        required property var display
                        required property string value
                        required property bool isSection
                        required property bool isAdd
                        required property bool isCustom
                        required property bool isMultiple
                        required property bool isDirty

                        width: rowList.width
                        height: rdel.isSection ? 26 : 19

                        // --- section header band ----------------------------
                        Rectangle {
                            visible: rdel.isSection
                            anchors.fill: parent
                            anchors.topMargin: 2; anchors.bottomMargin: 2
                            color: Theme.surfacePanel
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left; anchors.leftMargin: 8
                                text: rdel.display; color: Theme.textAccent
                                font.family: root.uiFont; font.pixelSize: 16; font.weight: Font.Bold
                            }
                        }

                        // --- "+ add new" -----------------------------------
                        // Resting color rides along on Theme.rowOdd: the add
                        // strip sits at the foot of the zebra table and reads
                        // as its final odd stripe, so it should track that
                        // token if the stripe value ever changes.
                        Rectangle {
                            visible: rdel.isAdd
                            anchors.fill: parent
                            color: addMA.containsMouse ? Theme.selectionSoft : Theme.rowOdd
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left; anchors.leftMargin: 8
                                text: rdel.display
                                color: addMA.containsMouse ? Theme.accentSoft : Theme.textFaint
                                font.family: root.uiFont; font.pixelSize: 12
                            }
                            // A single click opens the Add Field dialog (this row is an
                            // action affordance, not data). Commit any inline editor
                            // left open on another row first.
                            MouseArea {
                                id: addMA
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    root.commitOpenEditor()
                                    root._openEditor(rdel.index) // routes to addFieldRequested
                                }
                            }
                        }

                        // --- field / custom row -----------------------------
                        Rectangle {
                            id: fieldBg
                            visible: !rdel.isSection && !rdel.isAdd
                            anchors.fill: parent
                            color: {
                                if (!root.model) return Theme.surfacePanel
                                var i = root.model.fieldIndexInSection(rdel.index)
                                return (i % 2 === 0) ? Theme.rowEven : Theme.rowOdd
                            }

                            // Selection fill: behind the focus border and
                            // text, hidden while this row's editor is open.
                            Rectangle {
                                anchors.fill: parent
                                visible: root._isRowSel(rdel.index) && root.editingRow !== rdel.index
                                color: Theme.selectionSoft
                            }

                            // Focus highlight (not while editing; the editor's own
                            // border signals focus then).
                            Rectangle {
                                anchors.fill: parent
                                visible: root.currentRow === rdel.index && root.editingRow !== rdel.index
                                color: "transparent"
                                border.color: "#3C3658"; border.width: 1
                            }

                            Text {
                                id: nameLabel
                                anchors.verticalCenter: parent.verticalCenter
                                x: 8; width: root.nameColumnWidth - 12
                                text: rdel.display
                                color: rdel.isCustom ? "#8C8C8C" : Theme.textMuted
                                font.family: root.uiFont; font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            // Modify dot just after the field name (matching the
                            // Settings pane), clamped so a long name never pushes it
                            // into the value column; hidden while editing.
                            Rectangle {
                                x: Math.min(nameLabel.x + nameLabel.contentWidth + 7,
                                            root.nameColumnWidth - 6)
                                anchors.verticalCenter: parent.verticalCenter
                                visible: rdel.isDirty && root.editingRow !== rdel.index
                                width: 6; height: 6; radius: 3
                                color: Theme.accentSoft
                            }

                            // Value (read mode): accent tint while staged-dirty.
                            // Binds the value role so a staged edit refreshes via
                            // dataChanged. Newlines are flattened to a return
                            // glyph for DISPLAY ONLY, so a multiline value
                            // both fits the one-line cell and is visibly marked
                            // as multiline; the underlying value is untouched
                            // (the replace runs on a copy inside the binding).
                            // \u21B5 (downwards arrow with corner leftwards) is
                            // written as an escape so the source stays ASCII,
                            // like the guillemet marker in the model.
                            Text {
                                visible: root.editingRow !== rdel.index
                                anchors.verticalCenter: parent.verticalCenter
                                x: root.nameColumnWidth + 8
                                width: parent.width - root.nameColumnWidth - 16
                                text: String(rdel.value).replace(/\r?\n/g, "\u21B5")
                                color: rdel.isDirty ? Theme.accentSoft : Theme.textPrimary
                                font.family: root.uiFont; font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            // Row click area, BELOW the editor in z so the editor
                            // captures the value cell while this still catches the
                            // name side. Always enabled, so clicking the same row's
                            // name (or any non-editor part) commits an open editor.
                            // Selection moved from onClicked to onPressed:
                            // the press itself selects (matching the playlist) and
                            // arms the drag-select; holding and moving ranges from
                            // here. The context menu rides the press too.
                            MouseArea {
                                id: rowMA
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onPressed: function (mouse) {
                                    root.commitOpenEditor()   // commit an edit open elsewhere
                                    rowList.forceActiveFocus()
                                    if (mouse.button === Qt.RightButton) {
                                        // Right-click acts on the selection; if this row
                                        // is not in it, select just this row first.
                                        if (!root._isRowSel(rdel.index))
                                            root._selectRow(rdel.index, 0)
                                        root._menuBlank = false
                                        root._pasteEnabled = root._canPaste()
                                        fieldMenu.popup()
                                        return
                                    }
                                    root._selectRow(rdel.index, mouse.modifiers)
                                    root._beginDragSel(rowMA, mouse.y, mouse.modifiers)
                                }
                                onPositionChanged: function (mouse) {
                                    root._dragSelMove(rowMA, mouse.y)
                                }
                                onReleased: root._endDragSel()
                                onCanceled: root._endDragSel()
                                onDoubleClicked: {
                                    root.commitOpenEditor()
                                    root._selectRow(rdel.index, 0) // collapse to this row
                                    root._openEditor(rdel.index)
                                }
                            }

                            // Value (edit mode): inline editor over the value cell,
                            // with a field-policy validator (digits / digits+dash).
                            // Last child, so it sits above the row MouseArea.
                            TextField {
                                id: editor
                                visible: root.editingRow === rdel.index
                                x: root.nameColumnWidth + 6
                                width: parent.width - root.nameColumnWidth - 12
                                anchors.verticalCenter: parent.verticalCenter
                                height: 17
                                padding: 0; leftPadding: 4; rightPadding: 4
                                topPadding: 0; bottomPadding: 0
                                color: "#FFFFFF"
                                font.family: root.uiFont; font.pixelSize: 12
                                selectByMouse: true
                                verticalAlignment: TextInput.AlignVCenter
                                validator: RegularExpressionValidator {
                                    regularExpression: new RegExp(
                                        root.model ? root.model.editValidatorPattern(rdel.index) : "^.*$")
                                }
                                background: Rectangle {
                                    color: "#101010"; radius: 2
                                    border.color: Theme.accentSoft; border.width: 1
                                }
                                onVisibleChanged: {
                                    if (visible) {
                                        text = root.model ? root.model.editText(rdel.index) : ""
                                        root._editorText = text
                                        selectAll()
                                        forceActiveFocus()
                                    }
                                }
                                onTextChanged: if (visible) root._editorText = text
                                onAccepted: root._commitEditor(rdel.index, text)
                                Keys.onPressed: function (e) {
                                    if (e.key === Qt.Key_Escape) { root._closeEditor(); e.accepted = true }
                                }
                            }
                        }
                    }
                }
            }

            // ----- bottom hint line: red error, else neutral menu-hover hint -
            Rectangle {
                id: hintBar
                width: parent.width
                height: 18
                color: Theme.headerBand
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.separator }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 8
                    text: root.hintText !== "" ? root.hintText : root.menuHint
                    color: root.hintText !== "" ? Theme.dangerSoft : Theme.accentSoft
                    font.bold: root.hintText === "" && root.menuHint !== ""
                    font.family: root.uiFont; font.pixelSize: 11
                    elide: Text.ElideRight
                    width: parent.width - 16
                }
            }
        }
    }

    // Context menu (row actions) and blank-space menu (Add only), one ThemedMenu
    // switched by _menuBlank. Item visibility is gated by the selection; collapsing
    // height to 0 when hidden keeps the menu from leaving gaps. Hover descriptions
    // feed the bottom hint line.
    ThemedMenu {
        id: fieldMenu
        onClosed: root.menuHint = ""

        ThemedMenuItem {
            text: "Edit"
            // Any single field row: opens the Edit Value dialog directly,
            // single-track included, so a multiline value (or one about to
            // become multiline) is always reachable through the dialog's
            // free-text multiline editor. Deliberately NOT routed through
            // _openEditor / _editInPlace, which would prefer the inline editor
            // for a plain single-track value.
            collapsed: root._menuBlank || root._selCount() !== 1
            property string hint: "Edit this field's value in a dialog"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root.editFieldRequested(root.currentRow)
        }
        ThemedMenuItem {
            text: "Edit in place"
            // Single-track only: inline editing in the list. _editInPlace still
            // routes a multiline value to the dialog, since the one-line
            // editor cannot represent it.
            collapsed: root._menuBlank || root._selCount() !== 1
                       || !root.model || root.model.selectionCount() !== 1
            property string hint: "Edit this field's value inline"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._editInPlace(root.currentRow)
        }
        MenuSeparator {
            visible: !root._menuBlank && root._selCount() === 1
            height: visible ? implicitHeight : 0
        }
        ThemedMenuItem {
            text: "Add field"
            property string hint: "Add a new metadata field"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root.addFieldRequested()
        }
        ThemedMenuItem {
            text: "Remove"
            collapsed: root._menuBlank || root._selCount() < 1
            property string hint: "Remove the selected field(s)"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._removeSelection()
        }
        ThemedMenuItem {
            text: "Crop"
            collapsed: root._menuBlank || root._selCount() < 1
            property string hint: "Remove every field except the selected one(s)"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._crop()
        }
        MenuSeparator { // transforms group
            visible: !root._menuBlank && root._selCount() >= 1
            height: visible ? implicitHeight : 0
        }
        ThemedMenuItem {
            text: "Capitalize"
            collapsed: root._menuBlank || root._selCount() < 1
            property string hint: "Capitalize each word in the selected field(s)"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._capitalize()
        }
        ThemedMenuItem {
            text: "Clean up"
            collapsed: root._menuBlank || root._selCount() < 1
            property string hint: "Underscores to spaces, then collapse whitespace"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._cleanup()
        }
        ThemedMenuItem {
            text: "Auto track number"
            // Always available: targets Track Number / Total Tracks, not the field
            // selection. Numbers the tracks 1..N in playlist order.
            property string hint: "Number the tracks 1..N and set the total"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._autoNumber()
        }
        MenuSeparator {} // clipboard group; Paste is always present below it
        ThemedMenuItem {
            text: "Copy"
            collapsed: root._menuBlank || root._selCount() < 1
            property string hint: "Copy the selected field(s) to the clipboard"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._copy()
        }
        ThemedMenuItem {
            text: "Cut"
            collapsed: root._menuBlank || root._selCount() < 1
            property string hint: "Copy then remove the selected field(s)"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._cut()
        }
        ThemedMenuItem {
            text: "Paste"
            // Always shown; grayed (disabled) unless the clipboard holds a valid
            // block. _pasteEnabled is refreshed by each popup handler, since the
            // clipboard text is not a reactive property a binding could track.
            enabled: root._pasteEnabled
            property string hint: enabled ? "Paste fields from the clipboard"
                                          : "Nothing on the clipboard to paste"
            onHoveredChanged: if (hovered) root.menuHint = hint
            onTriggered: root._paste()
        }
    }

    // System clipboard bridge, reused here for Copy / Cut / Paste.
    Clipboard { id: clip }

    // Stroke overlay (hover-lit), matching the dock metadata pane.
    HoverHandler { id: paneHover }
    Rectangle {
        anchors.fill: parent
        color: "transparent"; radius: 4; border.width: 2
        property real strokeOpacity: paneHover.hovered ? 0.25 : 0.10
        Behavior on strokeOpacity { NumberAnimation { duration: 150; easing.type: Easing.InOutQuad } }
        border.color: Qt.rgba(0xBD / 255, 0xB2 / 255, 0xFF / 255, strokeOpacity)
    }
}
