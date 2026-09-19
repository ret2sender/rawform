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
// This file is the app's wiring layer for the custom-columns manager: it
// deliberately reaches the C++ context property `customColumns`, which qmllint
// cannot see, so the unqualified-access category is disabled file-wide. Under
// the Bound pragma this directive covers that context property ONLY; the
// row delegate declares its injected names (index, modelData) and every
// outer-id capture is statically checked under the pragma. Cost: a typo'd
// global name here surfaces at runtime, not lint.

// Bound component behavior: the ListView row delegate resolves outer document
// ids statically instead of through dynamic context lookup, and declares
// `index` and `modelData` as required properties (the contract and the qmllint
// proof); child items inside it qualify those reads through the delegate root
// id. The captures of `customColumnsWindow` throughout are exactly what the
// pragma makes statically valid.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window


// =============================================================================
// CustomColumnsWindow.qml
//
// The manager for user-defined custom playlist columns, opened from the header
// right-click menu ("Manage custom columns...", or "Edit custom column X..."
// scrolled to a record). A frameless top-level Window that reuses the main
// window's chrome, in the same vein as Settings and Properties: transparent
// Window, rounded Theme.surfacePage body at radius 8, a slim custom title bar
// with window-drag and a close button, over a Close footer. A top-level
// Window rather than a QtQuick.Controls Dialog because an in-overlay popup
// (modal, stock chrome) can never join the tool-window stacking layer the
// other manager windows live in.
//
// CITIZENSHIP: Qt.Tool, not Qt.Window. A tool window floats above its transient
// parent (the main window, resolved automatically from the QML visual parent:
// this is declared as a child of PlaylistView, which lives inside the main
// window), so it competes for focus and stacking alongside the Properties and
// Settings windows instead of sinking behind them. Non-modal. The macOS
// tool-window trade-off applies exactly as it does for Properties/Settings:
// the window hides while the app is deactivated and reappears with it, and it
// is absent from the Dock / cmd-tab (moot: frameless anyway).
//
// LIFETIME: a SINGLE reused instance (show/hide), not a fresh-instance-per-open
// like Properties. It is one manager over the shared `customColumns` registry;
// reusing the instance keeps its position and size across opens, as Settings
// does. There is no staging: edits commit straight to the registry (name and
// pattern on editing-finished, align on combo change), which persists and
// refreshes any SHOWN custom column live. `rows` is a SNAPSHOT taken on open
// and re-taken only on add/remove (structural changes); a field edit updates
// its cell in place and does NOT rebuild the list, so the editor is never
// destroyed mid-edit. (Built as a ListView of full-row delegates rather than a
// TableView + selectionModel: same look, a selection/edit model simple enough
// to reason about without surprises.)
//
// Fonts resolve through the Theme singleton, which is scope-independent, so
// this being a separate top-level Window (outside PlaylistView's id scope) does
// not matter for font resolution.
// =============================================================================
Window {
    id: customColumnsWindow

    // UI family for this window's own labels; defaults to the Theme singleton,
    // kept as a property purely as an override surface.
    property string uiFont: Theme.uiFont

    title: "Custom Playlist Columns"
    width: 680
    height: 490
    minimumWidth: 560
    minimumHeight: 380
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal

    // Shared column geometry: header and body use the same offsets so the
    // columns line up. Pattern fills whatever remains.
    readonly property int nameW: 170
    readonly property int alignW: 96

    property string focusFieldId: "" // scroll-to target on open
    property var selectedRows: []    // selected row INDICES into rows
    property int selectionAnchor: -1 // for Shift-range
    property var rows: []            // snapshot of customColumns.catalog()
    // How many inline cell editors are open (0 or 1 in practice, a count
    // for symmetry). Maintained at the EditableCell USAGE sites, not inside
    // the component (which references no outer ids by contract). Gates the
    // Escape shortcut below: the window-phase Shortcut matches BEFORE Keys
    // delivery, so without the gate Escape during an edit would close the
    // window instead of reaching the editor's discard handler (the Escape
    // phase-ordering trap, in reverse).
    property int _editingCells: 0

    // Escape deselects first, then closes. Window context, so this map
    // stays scoped to this window; the Shortcut phase for the same macOS
    // delivery reason as the dialogs. Selection present: clear it (and the anchor,
    // so a follow-up Shift-click starts fresh). Nothing selected: hide(),
    // the Close button's exact path (edits are live, nothing to revert). An
    // open combo popup consumes Escape itself (CloseOnEscape) before this
    // can match, so a dropdown never falls through to a close.
    //
    // The visible && strict-focus gate, doubly load-bearing here. This
    // window is a hide()-reused instance AND there is one per live
    // PlaylistView (per tab), so without the gate several forever-enabled
    // copies of this shortcut sit in Qt's shortcut map at once; a
    // still-enabled shortcut in a HIDDEN window keeps matching, every Escape
    // anywhere goes ambiguous (Qt fires neither onActivated, rotating
    // activatedAmbiguously instead), and other windows' Escape reads as dead
    // or double-press. Disabling ungrabs. The first attempt used Window.active
    // and failed with two windows open: that is QWindow::isActive(), a
    // transient-GROUP activation every visible secondary window reports
    // together. WindowFocus.focusWindow (QGuiApplication::focusWindow()) is
    // singular, so identity against it keeps the grab exclusive: at most one
    // of these shortcuts app-wide can hold the sequence at any instant.
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: customColumnsWindow.visible
                 && WindowFocus.focusWindow === customColumnsWindow
                 && customColumnsWindow._editingCells === 0
        onActivated: {
            if (customColumnsWindow.selectedRows.length > 0) {
                customColumnsWindow.selectedRows = []
                customColumnsWindow.selectionAnchor = -1
            } else {
                customColumnsWindow.hide()
            }
        }
    }

    function refresh() {
        rows = (typeof customColumns !== "undefined" && customColumns)
            ? customColumns.catalog() : []
        selectedRows = []
        selectionAnchor = -1
    }
    function indexOfFieldId(fid) {
        for (var i = 0; i < rows.length; i++)
            if (rows[i].fieldId === fid) return i
        return -1
    }
    function isSelected(i) { return selectedRows.indexOf(i) !== -1 }

    // align keyword <-> combo index <-> Qt alignment flag.
    function alignIndex(a) { return a === "center" ? 1 : a === "right" ? 2 : 0 }
    function alignFlag(i)  { return i === 1 ? Qt.AlignHCenter
                                  : i === 2 ? Qt.AlignRight
                                            : Qt.AlignLeft }

    // Selection click with the usual modifier semantics (plain / toggle / range).
    function clickRow(i, mods) {
        if (mods & Qt.ShiftModifier) {
            var a = selectionAnchor < 0 ? i : selectionAnchor
            var lo = Math.min(a, i), hi = Math.max(a, i), r = []
            for (var k = lo; k <= hi; k++) r.push(k)
            selectedRows = r
        } else if (mods & (Qt.ControlModifier | Qt.MetaModifier)) {
            var c = selectedRows.slice()
            var at = c.indexOf(i)
            if (at === -1) c.push(i); else c.splice(at, 1)
            selectedRows = c
            selectionAnchor = i
        } else {
            selectedRows = [i]
            selectionAnchor = i
        }
    }

    function addRow() {
        if (typeof customColumns === "undefined" || !customColumns) return
        customColumns.add() // new blank record, appended + persisted
        refresh()
        if (rows.length > 0) { // select the freshly added last row
            selectedRows = [rows.length - 1]
            selectionAnchor = rows.length - 1
        }
    }
    function removeSelected() {
        if (typeof customColumns === "undefined" || !customColumns) return
        if (selectedRows.length === 0) return
        // Resolve ids BEFORE removing, indices shift as rows go.
        var ids = []
        for (var k = 0; k < selectedRows.length; k++) {
            var r = rows[selectedRows[k]]
            if (r) ids.push(r.id)
        }
        for (var j = 0; j < ids.length; j++)
            customColumns.remove(ids[j])
        refresh()
    }

    // Entry point from the header menu: seed the snapshot, show + raise, then
    // (if opened on a specific record) scroll to and select it. A Window has
    // no onOpened, and the scroll-to must run after the ListView has had a
    // layout pass, so it is deferred one turn.
    function openManager(fieldId) {
        focusFieldId = (fieldId === undefined) ? "" : fieldId
        refresh()
        show()
        raise()
        requestActivate()
        if (focusFieldId !== "")
            Qt.callLater(_scrollToFocus)
    }

    function _scrollToFocus() {
        var i = indexOfFieldId(focusFieldId)
        if (i >= 0) {
            customList.positionViewAtIndex(i, ListView.Contain)
            selectedRows = [i]
            selectionAnchor = i
        }
        focusFieldId = ""
    }

    // -------------------------------------------------------------------------
    // One editable text cell: a display Text by default, swapped to a TextField
    // on DOUBLE-CLICK, committing on editing-finished and discarding on Esc. A
    // single click is forwarded as rowClicked (the row uses it for selection).
    // Self-contained, it references no outer ids (the font family is passed in),
    // reused by both the Name and Pattern cells. Moved here with the window: it
    // was used nowhere else in PlaylistView.
    // -------------------------------------------------------------------------
    component EditableCell: Item {
        id: editableCell

        property string value: ""
        property string placeholder: ""
        property string fontFamily: ""
        property bool editing: false

        signal rowClicked(int modifiers)
        signal committed(string newText)

        Text {
            visible: !editableCell.editing
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            color: editableCell.value.length > 0 ? Theme.textPrimary : Theme.textDisabled
            elide: Text.ElideRight
            font.family: editableCell.fontFamily
            font.pixelSize: 12
            font.italic: editableCell.value.length === 0
            text: editableCell.value.length > 0 ? editableCell.value : editableCell.placeholder
            verticalAlignment: Text.AlignVCenter
        }
        MouseArea {
            acceptedButtons: Qt.LeftButton
            anchors.fill: parent
            enabled: !editableCell.editing

            onClicked: function (mouse) { editableCell.rowClicked(mouse.modifiers) }

            onDoubleClicked: {
                editableCell.editing = true
                editor.text = editableCell.value
                editor.forceActiveFocus()
                editor.selectAll()
            }
        }
        TextField {
            id: editor
            anchors.fill: parent
            font.family: editableCell.fontFamily
            font.pixelSize: 12
            visible: editableCell.editing

            onEditingFinished: {
                if (editableCell.editing) {
                    editableCell.editing = false
                    editableCell.value = text
                    editableCell.committed(text)
                }
            }

            Keys.onEscapePressed: function (event) {
                text = editableCell.value // discard the edit
                editableCell.editing = false
                event.accepted = true // swallow so the discard stops here
            }
        }
    }

    // -------------------------------------------------------------------------
    // Body: rounded frame matching the main window and the other manager windows.
    // -------------------------------------------------------------------------
    Rectangle {
        id: windowBody
        anchors.fill: parent
        color: Theme.surfacePage
        radius: 8
        border.color: Theme.separatorStrong
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1   // sit inside the 1 px border
            spacing: 0

            // ----- title bar: drag + close -----------------------------------
            Item {
                id: titleBar
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                // Drag the window from the title bar (frameless, so manual).
                MouseArea {
                    anchors.fill: parent
                    onPressed: customColumnsWindow.startSystemMove()
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Custom Playlist Columns"
                    color: Theme.textPrimary
                    font.family: customColumnsWindow.uiFont
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }

                // Close: just hide (edits are live, nothing to revert).
                ToolDialogCloseButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: customColumnsWindow.hide()
                }
            }

            // ----- content: +/- strip and the Name/Align/Pattern table -------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 16
                spacing: 8

                // Top strip: a segmented +/- on the right. + adds a blank row; -
                // deletes the selected rows (disabled with no selection).
                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 28

                    Rectangle {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 66
                        height: 26
                        radius: 6
                        color: "#3A3A3C"

                        Row {
                            anchors.centerIn: parent
                            spacing: 0

                            Item {
                                width: 32; height: 26
                                Text {
                                    anchors.centerIn: parent
                                    text: "+"; color: "#FFFFFF"; font.pixelSize: 17
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: customColumnsWindow.addRow()
                                }
                            }
                            Rectangle {
                                width: 1; height: 16
                                anchors.verticalCenter: parent.verticalCenter
                                color: "#5A5A5C"
                            }
                            Item {
                                id: minusHalf
                                width: 32; height: 26
                                property bool en: customColumnsWindow.selectedRows.length > 0
                                Text {
                                    anchors.centerIn: parent
                                    text: "\u2212"
                                    color: minusHalf.en ? "#FFFFFF" : Theme.textFaint
                                    font.pixelSize: 17
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    enabled: minusHalf.en
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: customColumnsWindow.removeSelected()
                                }
                            }
                        }
                    }
                }

                // The table: header strip + alternating-row list, in a rounded
                // frame (metadata-view palette). Fills the remaining body height;
                // the window is fixed-size (frameless, no resize grip), so this
                // just adapts to the body rather than needing a hand-set height.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.surfacePage
                    radius: 8
                    border.color: Theme.separator
                    border.width: 1
                    clip: true

                    Column {
                        anchors.fill: parent
                        anchors.margins: 1
                        spacing: 0

                        // Header.
                        Rectangle {
                            width: parent.width
                            height: 26
                            color: Theme.headerBand

                            Text {
                                x: 8
                                width: customColumnsWindow.nameW - 8
                                height: parent.height
                                text: "Name"; color: Theme.textPrimary
                                font.family: Theme.uiFont; font.pixelSize: 12; font.weight: Font.Bold
                                verticalAlignment: Text.AlignVCenter
                            }
                            Rectangle {
                                x: customColumnsWindow.nameW; width: 1; height: parent.height
                                color: Theme.separator
                            }
                            Text {
                                x: customColumnsWindow.nameW + 8
                                width: customColumnsWindow.alignW - 8
                                height: parent.height
                                text: "Align"; color: Theme.textPrimary
                                font.family: Theme.uiFont; font.pixelSize: 12; font.weight: Font.Bold
                                verticalAlignment: Text.AlignVCenter
                            }
                            Rectangle {
                                x: customColumnsWindow.nameW + customColumnsWindow.alignW
                                width: 1; height: parent.height
                                color: Theme.separator
                            }
                            Text {
                                x: customColumnsWindow.nameW + customColumnsWindow.alignW + 8
                                height: parent.height
                                text: "Pattern"; color: Theme.textPrimary
                                font.family: Theme.uiFont; font.pixelSize: 12; font.weight: Font.Bold
                                verticalAlignment: Text.AlignVCenter
                            }
                        }

                        ListView {
                            id: customList
                            width: parent.width
                            height: parent.height - 26
                            clip: true
                            model: customColumnsWindow.rows
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar {}

                            Text {
                                anchors.centerIn: parent
                                visible: customList.count === 0
                                text: "No custom columns yet. Click +"
                                color: Theme.textFaint
                                font.family: Theme.uiFont
                                font.pixelSize: 12
                            }

                            delegate: Rectangle {
                                id: rowDelegate
                                required property int index
                                required property var modelData
                                width: ListView.view.width
                                height: 30
                                readonly property string colId: modelData.id

                                // Selection wins; else alternating bands (metadata view).
                                // Reading selectedRows here makes the color track it.
                                color: {
                                    customColumnsWindow.selectedRows
                                    return customColumnsWindow.isSelected(index) ? "#332B40"
                                         : (index % 2 === 0 ? Theme.rowEven : Theme.rowOdd)
                                }

                                // Background (gaps) selects the row too.
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton
                                    onClicked: function (mouse) {
                                        customColumnsWindow.clickRow(rowDelegate.index, mouse.modifiers)
                                    }
                                }

                                EditableCell {
                                    x: 0
                                    width: customColumnsWindow.nameW
                                    height: parent.height
                                    fontFamily: Theme.uiFont
                                    value: rowDelegate.modelData.name
                                    placeholder: "(unnamed)"
                                    // Editor-open bookkeeping for the
                                    // window's Escape gate; the destruction
                                    // hook compensates for a delegate torn
                                    // down mid-edit (refresh() rebuilding the
                                    // list), where editingChanged never fires.
                                    onEditingChanged:
                                        customColumnsWindow._editingCells += editing ? 1 : -1
                                    Component.onDestruction:
                                        if (editing) customColumnsWindow._editingCells--
                                    onRowClicked: function (mods) { customColumnsWindow.clickRow(rowDelegate.index, mods) }
                                    onCommitted: function (t) {
                                        if (typeof customColumns !== "undefined" && customColumns)
                                            customColumns.setName(rowDelegate.colId, t)
                                    }
                                }

                                Item {
                                    x: customColumnsWindow.nameW
                                    width: customColumnsWindow.alignW
                                    height: parent.height
                                    ComboBox {
                                        id: alignCombo
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.leftMargin: 6
                                        width: parent.width - 12
                                        // The Basic style's implicit height is taller than the
                                        // 30 px row; pin an explicit height that fits and zero
                                        // the insets so the background matches it. Centered,
                                        // this leaves 3 px clearance top and bottom and no
                                        // longer spills into the next row.
                                        height: 24
                                        topInset: 0
                                        bottomInset: 0
                                        model: ["Left", "Center", "Right"]
                                        font.family: Theme.uiFont
                                        font.pixelSize: 12

                                        // Basic's default label contentItem is a TextField that
                                        // reserves ~6 px of vertical padding; a 24 px control has
                                        // no room for it and clips the glyphs. A plain Text with
                                        // no vertical padding sits flush in the row. The control's
                                        // own rightPadding already reserves the chevron, so no
                                        // right inset is needed here (adding one shrank the box
                                        // until even "Left" elided). With no elide set the label
                                        // can never show an ellipsis, and the widest option,
                                        // "Center", fits the ~49 px box with room to spare.
                                        contentItem: Text {
                                            leftPadding: 2
                                            text: alignCombo.displayText
                                            font: alignCombo.font
                                            color: Theme.textPrimary
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        Component.onCompleted:
                                            currentIndex = customColumnsWindow.alignIndex(rowDelegate.modelData.align)
                                        onActivated: {
                                            if (typeof customColumns !== "undefined" && customColumns)
                                                customColumns.setAlignment(
                                                    rowDelegate.colId,
                                                    customColumnsWindow.alignFlag(currentIndex))
                                        }
                                    }
                                }

                                EditableCell {
                                    x: customColumnsWindow.nameW + customColumnsWindow.alignW
                                    width: parent.width - customColumnsWindow.nameW - customColumnsWindow.alignW
                                    height: parent.height
                                    fontFamily: Theme.uiFont
                                    value: rowDelegate.modelData.pattern
                                    placeholder: "%artist% - %album%"
                                    // Same bookkeeping as the Name cell.
                                    onEditingChanged:
                                        customColumnsWindow._editingCells += editing ? 1 : -1
                                    Component.onDestruction:
                                        if (editing) customColumnsWindow._editingCells--
                                    onRowClicked: function (mods) { customColumnsWindow.clickRow(rowDelegate.index, mods) }
                                    onCommitted: function (t) {
                                        if (typeof customColumns !== "undefined" && customColumns)
                                            customColumns.setPattern(rowDelegate.colId, t)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ----- footer: Close ---------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                color: Theme.headerBand

                RowLayout {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    FooterButton {
                        label: "Close"
                        onClicked: customColumnsWindow.hide()
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    // A small themed footer button (same shape as the Settings window's).
    // -------------------------------------------------------------------------
    component FooterButton: Rectangle {
        id: fbtn
        property string label: ""
        property bool accent: false
        signal clicked()

        implicitWidth: Math.max(72, btnText.implicitWidth + 28)
        implicitHeight: 30
        radius: 5
        color: !enabled ? Theme.surfaceControl
             : fbtnHover.hovered ? (accent ? Theme.accentButtonHover : Theme.surfaceControlHover)
             : (accent ? Theme.accentSoft : Theme.buttonFace)
        border.color: accent ? "transparent" : Theme.border
        border.width: accent ? 0 : 1
        opacity: enabled ? 1.0 : 0.5

        Text {
            id: btnText
            anchors.centerIn: parent
            text: fbtn.label
            color: fbtn.accent ? Theme.textOnAccent : Theme.textPrimary
            font.family: customColumnsWindow.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
        }
        HoverHandler { id: fbtnHover }
        TapHandler { onTapped: if (fbtn.enabled) fbtn.clicked() }
    }
}
