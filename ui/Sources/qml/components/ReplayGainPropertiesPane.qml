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
import QtQuick.Layouts
import com.rawform.app


// =============================================================================
// ReplayGainPropertiesPane.qml
//
// Editable ReplayGain page with in-dialog row selection: the per-track
// table over a Summary, foobar style.
//
// State: everything row-shaped lives in ReplayGainRowsModel (C++),
// bound in as `model` by the host window, which also owns the model's
// lifecycle (setSelection on open and re-pull, collectEdits / adoptEdits /
// revertEdits on the apply path, clearAllReplayGain from the Tools menu).
// This file keeps only what belongs to Items: gestures (click modifiers,
// drag-select resolving pointer to row in view space, keyboard focus
// juggling), the right-click menu, the cell editors, and the scanNoop
// message wording. The model resets in one pass and aggregates in C++ (no
// per-row marshal into JS objects, no per-touch binding storm), so a
// library-sized selection is a non-event here.
//
// Selection scope: clicking rows selects a subset (plain = exclusive, Cmd/Ctrl =
// toggle, Shift = range). When a subset is selected, the Summary computes over and
// applies to JUST the selected rows; with nothing selected it covers all rows.
// Per-cell edits are always atomic (one cell). collectEdits diffs current vs
// original and reports only changed fields (empty == remove the tag).
//
// Scanning: the right-click menu also generates RG values. "Scan track / album
// gain" resolve the current scope to { row, path, name } items (honoring the
// skip-existing setting), then ask the host (via scanRequested) to run the
// off-thread scan; the host hands the measured values back to stageScanResults,
// which stages them like any other edit so they are reviewed and committed
// through the same Apply / OK path. The scan itself never writes. scanNoop
// reports a scope that had nothing to do (everything already tagged, with
// skip-existing on).
//
// Window scope: the host's footer Tools menu drives the same staging through
// requestScanAll / clearAllReplayGain, which act over EVERY row regardless of
// the pane's row sub-selection (the Tools menu is window-scoped by design);
// the right-click menu here stays selection-scoped. Both funnel into the same
// scope helpers via their allRows override, so the skip-existing rules and the
// revert-on-equal staging behave identically in either scope.
// =============================================================================
Item {
    id: pane

    // The C++ rows model. The host instantiates and feeds it; the pane
    // only renders it and forwards gestures. Null only before the host binds.
    property ReplayGainRowsModel model: null

    // The AudioController; the scan scope reads its live skip-existing flag.
    // Declared and bound by the host rather than probed as an ambient global
    // (a declared property is guaranteed in scope; a global is not, so a
    // typeof guard would be needed there).
    property AudioController controller: null
    property string uiFont: Theme.uiFont

    // Scanning. scanRequested asks the host to run an off-thread RG scan over the
    // given { row, path, name } items in the given mode (0 = track, 1 = album);
    // the host stages the result back through stageScanResults. scanNoop fires
    // when the scope had nothing to scan (e.g. everything already tagged and
    // skip-existing is on), so the host can show a brief reason instead of
    // appearing to do nothing.
    signal scanRequested(var items, int mode)
    signal scanNoop(string message)

    readonly property int _numW: 100

    // Bumped to ask any open cell editor to close (discard) without relying on focus
    // juggling, which is unreliable here (see focusKeys note below). The host calls
    // closeAnyEditor() when the Properties window leaves this tab, so edit mode never
    // persists across a tab change. Each EditCell watches this and hides its editor.
    property int closePulse: 0
    function closeAnyEditor() { pane.closePulse++ }

    // Dedicated, always-stable keyboard focus holder for the row list. We do NOT
    // hang key handling off the ListView: it is a FocusScope that re-delegates focus
    // to whichever delegate last held it, so after a cell editor hides, focus lands
    // on a now-invisible TextField and the list goes deaf to Cmd+A and the rest.
    // A plain Item takes, and reliably re-takes via focusKeys(), active focus, so the
    // shortcuts survive edits. While a cell is being edited the editor's TextField
    // holds focus instead, so Cmd+A selects the field text and Escape cancels the edit.
    // keysFocused exposes exactly that distinction to the host window: true
    // while this catcher holds focus, false while a cell editor does, gating the
    // window's Escape-deselect Shortcut so it never swallows the editor's Escape.
    function focusKeys() { keyCatcher.forceActiveFocus() }
    readonly property bool keysFocused: keyCatcher.activeFocus
    // Escape-deselect entry point for the host window's Shortcut (Keys-based
    // Escape delivery was observed dead on macOS; the keyCatcher branch stays as
    // fallback).
    function clearSelection() { if (pane.model) pane.model.clearSelection() }
    Item {
        id: keyCatcher
        focus: true
        Keys.onPressed: function (event) {
            // ControlModifier is the Command key on macOS (Qt swaps Ctrl and Cmd),
            // so this is Cmd+A on macOS and Ctrl+A elsewhere. Escape clears selection.
            if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                if (pane.model) pane.model.selectAll()
                event.accepted = true
            } else if (event.key === Qt.Key_Escape) {
                if (pane.model) pane.model.clearSelection()
                event.accepted = true
            }
        }
    }

    // Right-click menu for the row list. "Scan track / album gain" generate values
    // for the current scope; "Clear" stages empty values across it (the removal is
    // written on Apply / OK like any other staged edit, so Cancel undoes it). Themed
    // to match the app's other menus.
    ThemedMenu {
        id: rgRowMenu

        ThemedMenuItem {
            text: "Scan track gain"
            onTriggered: pane._requestScan(0)
        }
        ThemedMenuItem {
            text: "Scan album gain (as one album)"
            onTriggered: pane._requestScan(1)
        }
        ThemedMenuItem {
            text: "Scan album gain (multiple albums, by tags)"
            onTriggered: pane._requestScan(2)
        }
        MenuSeparator {}
        ThemedMenuItem {
            text: "Clear ReplayGain information"
            onTriggered: {
                // Selection-scoped (allRows false): the model applies the same
                // revert-on-equal staging per field, one recompute per call.
                pane.model.setScoped(ReplayGainRowsModel.TrackGain, "", false)
                pane.model.setScoped(ReplayGainRowsModel.AlbumGain, "", false)
                pane.model.setScoped(ReplayGainRowsModel.TrackPeak, "", false)
                pane.model.setScoped(ReplayGainRowsModel.AlbumPeak, "", false)
            }
        }
    }

    // --- selection forwarding ------------------------------------------
    // Gesture handlers call this; membership, the anchor, and the Summary all
    // live in the model. Focus re-take stays here with the Items it concerns.
    function _selectRow(i, modifiers) {
        if (!pane.model)
            return
        pane.model.select(i, modifiers)
        keyCatcher.forceActiveFocus()
    }

    // --- click-hold-drag selection -------------------------------------
    // Same shape as the metadata pane's: press a row (name area OR one of the
    // numeric EditCells, which forward their presses) and, holding, move up or
    // down to range the selection live from the press anchor. Moves arrive in
    // the pressed MouseArea's coordinates, get mapped into the list viewport,
    // then to content space, clamped, and resolved via indexAt; a resolve that
    // lands past the rows (the Summary footer is part of the content) snaps to
    // the nearest end. Ctrl presses never arm; shift presses arm and continue
    // from the standing anchor. _dragSelLast gates re-selects to row changes,
    // so the per-move cost is one _selectRow (the same as a shift click).
    property bool _dragSelActive: false
    property real _dragSelY: 0           // last pointer y, list viewport coords
    property int  _dragSelLast: -1

    function _beginDragSel(item, y, modifiers) {
        if ((modifiers & Qt.ControlModifier) || (modifiers & Qt.MetaModifier))
            return
        pane._dragSelActive = true
        pane._dragSelLast = -1
        pane._dragSelY = item.mapToItem(list, 0, y).y
    }
    function _dragSelMove(item, y) {
        if (!pane._dragSelActive)
            return
        pane._dragSelY = item.mapToItem(list, 0, y).y
        pane._dragSelApply()
    }
    function _dragSelApply() {
        if (!pane.model || pane.model.count === 0)
            return
        var cy = Math.max(0, Math.min(pane._dragSelY + list.contentY,
                                      list.contentHeight - 1))
        var i = list.indexAt(1, cy)
        if (i < 0)  // above the first row or into the footer: snap to an end
            i = (cy <= 0) ? 0 : pane.model.count - 1
        if (i === pane._dragSelLast)
            return
        pane._dragSelLast = i
        pane._selectRow(i, Qt.ShiftModifier)  // range from the model's anchor
    }
    function _endDragSel() {
        pane._dragSelActive = false
        pane._dragSelLast = -1
    }

    // collectEdits / adoptEdits / revertEdits moved to ReplayGainRowsModel
    //; the host window calls the model directly on the apply path.

    // --- scanning ------------------------------------------------------------
    // Scope resolution, grouping, and the skip-existing rules live in
    // ReplayGainRowsModel.scanItems; this wrapper keeps only what is
    // UI: reading the live skip-existing setting off the controller, wording
    // the noop message, and raising scanRequested for the host.
    function _requestScan(mode, allRows) {
        if (!pane.model)
            return
        var skip = pane.controller ? pane.controller.replayGainScanSkipExisting
                                   : false
        var items = pane.model.scanItems(mode, allRows === true, skip)
        if (items.length === 0) {
            if (pane.model.count === 0)
                pane.scanNoop("No tracks to scan.")
            else if (mode === 0)
                pane.scanNoop("Selection already has track info.")
            else
                pane.scanNoop("Selection already has album info.")
            return
        }
        pane.scanRequested(items, mode)
    }

    // Window-scope entry point for the host's footer Tools menu: identical to
    // the right-click actions but always over EVERY row, ignoring the pane's
    // row sub-selection (the Tools menu is window-scoped by design). The
    // matching Clear lives on the model (clearAllReplayGain), called by the
    // window directly.
    function requestScanAll(mode) {
        _requestScan(mode, true)
    }

    // --- edit seeding --------------------------------------------------------
    // The only parsing left UI-side: converting a formatted Summary value
    // ("+x.xx dB" / the guillemet marker) into a bare-number edit seed. This
    // consumes OUR OWN formatted output, never user input, so the lenient JS
    // parseFloat is fine here; user input validates through model.validInput.
    function _seedGain(text) {
        if (text === undefined || text === null) return ""
        var t = String(text).replace(/\s*dB\s*$/i, "").replace(",", ".").trim()
        if (t.length === 0) return ""
        var v = parseFloat(t)
        return isNaN(v) ? "" : String(v)
    }

    // -------------------------------------------------------------------------
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ----- sticky column header ------------------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 30

            Rectangle {
                anchors.right: headerCells.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 1
                color: Theme.separator
            }
            Text {
                anchors.left: parent.left
                anchors.leftMargin: 4
                anchors.right: headerCells.left
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                text: "Name"
                elide: Text.ElideRight
                color: Theme.textMuted
                font.family: pane.uiFont
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            Row {
                id: headerCells
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 0
                Repeater {
                    model: ["Track Gain", "Album Gain", "Track Peak", "Album Peak"]
                    delegate: Item {
                        required property string modelData
                        width: pane._numW
                        height: 26
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.modelData
                            color: Theme.textMuted
                            font.family: pane.uiFont
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.separatorStrong }

        // ----- track rows + Summary footer (scrolls together) ----------------
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: pane.model
            boundsBehavior: Flickable.StopAtBounds
            // The drag-select is driven by the pressed delegate's (or
            // cell's) MouseArea; keep delegates alive well past the viewport so
            // the autoscroll cannot release the grab-holder mid-drag.
            cacheBuffer: 4096
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            // Edge autoscroll for the drag-select, mirroring the metadata
            // pane's: nudge the content while the pointer parks near/beyond an
            // edge and re-resolve the ranged-to row from the parked position.
            Timer {
                interval: 16
                repeat: true
                running: pane._dragSelActive
                readonly property real edge: 20
                readonly property real step: 10
                onTriggered: {
                    var maxY = Math.max(0, list.contentHeight - list.height)
                    var before = list.contentY
                    if (pane._dragSelY < edge)
                        list.contentY = Math.max(0, list.contentY - step)
                    else if (pane._dragSelY > list.height - edge)
                        list.contentY = Math.min(maxY, list.contentY + step)
                    if (list.contentY !== before)
                        pane._dragSelApply()
                }
            }

            delegate: Item {
                id: rd
                required property int index
                required property var model
                width: ListView.view.width
                implicitHeight: 26

                Rectangle {
                    anchors.fill: parent
                    color: rd.model.sel ? (rdHover.hovered ? "#39305A" : "#2E2747")
                         : (rdHover.hovered ? "#202020" : "transparent")
                }
                Rectangle {  // left accent on selected rows
                    visible: rd.model.sel
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 2
                    color: Theme.accent
                }
                HoverHandler { id: rdHover }

                // Selection presses on the name / gaps (cells forward their
                // own). Press-based: the press selects and arms the
                // drag-select; the context menu rides the press too.
                MouseArea {
                    id: rowMA
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onPressed: function (mouse) {
                        if (mouse.button === Qt.RightButton) {
                            // Right-click acts on the selection; if this row is not
                            // part of it, select just this row first (standard).
                            if (!rd.model.sel)
                                pane._selectRow(rd.index, 0)
                            rgRowMenu.popup()
                            return
                        }
                        pane._selectRow(rd.index, mouse.modifiers)
                        pane._beginDragSel(rowMA, mouse.y, mouse.modifiers)
                    }
                    onPositionChanged: function (mouse) {
                        pane._dragSelMove(rowMA, mouse.y)
                    }
                    onReleased: pane._endDragSel()
                    onCanceled: pane._endDragSel()
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.right: cells.left
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: rd.model.name
                    elide: Text.ElideRight
                    color: Theme.textPrimary
                    font.family: pane.uiFont
                    font.pixelSize: 12
                }
                Row {
                    id: cells
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 0
                    EditCell {
                        width: pane._numW; field: ReplayGainRowsModel.TrackGain
                        text: rd.model.tg; dirty: rd.model.tg !== rd.model.tg0
                        onClicked: function (m) { pane._selectRow(rd.index, m) }
                        onCommitted: function (v) { pane.model.commitCell(rd.index, ReplayGainRowsModel.TrackGain, v) }
                    }
                    EditCell {
                        width: pane._numW; field: ReplayGainRowsModel.AlbumGain
                        text: rd.model.ag; dirty: rd.model.ag !== rd.model.ag0
                        onClicked: function (m) { pane._selectRow(rd.index, m) }
                        onCommitted: function (v) { pane.model.commitCell(rd.index, ReplayGainRowsModel.AlbumGain, v) }
                    }
                    EditCell {
                        width: pane._numW; field: ReplayGainRowsModel.TrackPeak
                        text: rd.model.tp; dirty: rd.model.tp !== rd.model.tp0
                        onClicked: function (m) { pane._selectRow(rd.index, m) }
                        onCommitted: function (v) { pane.model.commitCell(rd.index, ReplayGainRowsModel.TrackPeak, v) }
                    }
                    EditCell {
                        width: pane._numW; field: ReplayGainRowsModel.AlbumPeak
                        text: rd.model.ap; dirty: rd.model.ap !== rd.model.ap0
                        onClicked: function (m) { pane._selectRow(rd.index, m) }
                        onCommitted: function (v) { pane.model.commitCell(rd.index, ReplayGainRowsModel.AlbumPeak, v) }
                    }
                }
            }

            footer: Column {
                width: list.width
                spacing: 0

                Item { width: 1; height: 16 }
                Text {
                    leftPadding: 4
                    textFormat: Text.RichText
                    text: "<b>Summary</b>" + (pane.model && pane.model.hasSelection
                          ? " <font color='#7A7A7A'>(" + pane.model.selectionCount
                            + " of " + pane.model.count + " selected)</font>"
                          : "")
                    color: Theme.textAccent
                    font.family: pane.uiFont
                    font.pixelSize: 14
                }
                Item { width: 1; height: 6 }

                SummaryEditRow {
                    width: parent.width; label: "Track Gain"
                    value: pane.model ? pane.model.trackGainSummary : ""
                    onCommitted: function (v) { pane.model.setScoped(ReplayGainRowsModel.TrackGain, v, false) }
                }
                SummaryEditRow {
                    width: parent.width; label: "Album Gain"
                    value: pane.model ? pane.model.albumGainSummary : ""
                    onCommitted: function (v) { pane.model.setScoped(ReplayGainRowsModel.AlbumGain, v, false) }
                }
                SummaryRow { width: parent.width; label: "Total Peak"
                             value: pane.model ? pane.model.totalPeak : "n/a" }
                SummaryRow { width: parent.width; label: "Lowest gain (loudest track)"
                             value: pane.model ? pane.model.lowestGain : "n/a" }
                SummaryRow { width: parent.width; label: "Highest gain (quietest track)"
                             value: pane.model ? pane.model.highestGain : "n/a" }
            }
        }
    }

    // -------------------------------------------------------------------------
    // A double-click-to-edit numeric cell. A single PRESS forwards as
    // clicked(mods) for row selection (on PRESS; the cell also drives the pane's
    // drag-select, see the MouseArea); kind 0 validates a gain, 1 a peak; empty
    // commit clears.
    // -------------------------------------------------------------------------
    component EditCell: Item {
        id: ec
        property string text: ""
        property bool dirty: false
        property bool editable: true
        // The model Field this cell edits (validation picks gain vs peak
        // rules from it). Gain-kind by default.
        property int field: ReplayGainRowsModel.TrackGain
        property string editSeed: ec.text
        property int elideMode: Text.ElideRight
        signal committed(string value)
        signal clicked(int modifiers)
        implicitHeight: 26

        Text {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 10
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: Text.AlignRight
            elide: ec.elideMode
            visible: !ed.visible
            text: ec.text
            color: ec.dirty ? Theme.accentSoft : "#D6D6D6"
            font.family: pane.uiFont
            font.pixelSize: 12
        }
        // Modified-from-original accent dot, matching the Settings pane. Sits at the
        // left of the cell; the value is right-aligned so they never collide.
        Rectangle {
            anchors.left: parent.left
            anchors.leftMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            visible: ec.dirty && !ed.visible
            width: 6
            height: 6
            radius: 3
            color: Theme.accentSoft
        }
        MouseArea {
            id: ecMA
            anchors.fill: parent
            enabled: ec.editable && !ed.visible
            // The `clicked` signal fires on PRESS (press-select matches the row
            // area and the playlist), and the cell drives the pane's
            // drag-select directly,
            // ids being in scope for this inline component, so a drag starting
            // on a numeric cell ranges exactly like one starting on the name.
            // A double-click still opens the editor; opening it disables this
            // area mid-press, which cancels the grab, and onCanceled ends the
            // armed drag cleanly.
            onPressed: function (mouse) {
                ec.clicked(mouse.modifiers)
                pane._beginDragSel(ecMA, mouse.y, mouse.modifiers)
            }
            onPositionChanged: function (mouse) {
                pane._dragSelMove(ecMA, mouse.y)
            }
            onReleased: pane._endDragSel()
            onCanceled: pane._endDragSel()
            onDoubleClicked: {
                ed.text = ec.editSeed
                ed.visible = true
                ed.forceActiveFocus()
                ed.selectAll()
            }
        }
        TextField {
            id: ed
            anchors.fill: parent
            visible: false
            font.family: pane.uiFont
            font.pixelSize: 12
            color: Theme.textPrimary
            horizontalAlignment: TextInput.AlignRight
            leftPadding: 6
            rightPadding: 8
            topPadding: 0
            bottomPadding: 0
            selectByMouse: true
            background: Rectangle {
                color: Theme.surfaceInset
                border.color: Theme.accentSoft
                border.width: 1
                radius: 3
            }
            onEditingFinished: ec._commit()
            Keys.onEscapePressed: function (e) { ed.visible = false; pane.focusKeys(); e.accepted = true }
        }
        function _commit() {
            if (!ed.visible) return
            var raw = ed.text
            ed.visible = false
            if (pane.model && pane.model.validInput(raw, ec.field))
                ec.committed(raw)
            pane.focusKeys()
        }

        // Leaving the tab discards an open edit (exits edit mode like Cancel), not
        // committed, so a half-typed value is never staged just by switching tabs.
        Connections {
            target: pane
            function onClosePulseChanged() { if (ed.visible) ed.visible = false }
        }
    }

    // -------------------------------------------------------------------------
    // A read-only Summary row: label left, value right-aligned under Track Gain.
    // -------------------------------------------------------------------------
    component SummaryRow: Item {
        property string label: ""
        property string value: ""
        implicitHeight: 24
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: parent.label
            color: Theme.textPrimary
            font.family: pane.uiFont
            font.pixelSize: 12
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: pane._numW * 3 + 10
            anchors.verticalCenter: parent.verticalCenter
            text: parent.value
            color: "#D6D6D6"
            font.family: pane.uiFont
            font.pixelSize: 12
        }
    }

    // -------------------------------------------------------------------------
    // An editable Summary row (Track Gain / Album Gain). Applies the typed value
    // across the current scope (selected rows, or all when nothing is selected).
    // -------------------------------------------------------------------------
    component SummaryEditRow: Item {
        id: ser
        property string label: ""
        property string value: ""
        signal committed(string v)
        implicitHeight: 24

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: ser.label
            color: Theme.textPrimary
            font.family: pane.uiFont
            font.pixelSize: 12
        }
        EditCell {
            anchors.right: parent.right
            anchors.rightMargin: pane._numW * 3
            anchors.verticalCenter: parent.verticalCenter
            width: pane._numW
            height: 22
            field: ReplayGainRowsModel.TrackGain // gain-kind validation
            elideMode: Text.ElideNone
            text: ser.value
            editSeed: pane._seedGain(ser.value)
            onCommitted: function (v) { ser.committed(v) }
        }
    }
}
