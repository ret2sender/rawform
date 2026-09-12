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
import QtQuick.Effects
import com.rawform.app

// =============================================================================
// EditValueDialog.qml
//
// The Edit Value panel for metadata editing,
// modeled on foobar2000's dialog. It is NOT an OS window: the Properties window
// hosts it as an in-window overlay (a dim layer plus this centered panel), which
// keeps it off the backing-store / modal machinery, so a non-modal,
// multi-instance Properties window never has a modal dialog in its way.
//
// It serves BOTH selection sizes: a multi-track selection edits here
// always, and a single-track selection reaches it through the pane's "Edit"
// menu action or through the multiline routing (a value containing a
// newline cannot be edited inline). For a single track everything degenerates
// cleanly: the field is trivially uniform, so the dialog opens on Single Value,
// and Individual Values is just a one-row grid.
//
// Two modes for the field named in @ref fieldKey:
//   - Single Value: one editor, broadcast to every selected track on commit.
//     Free-text fields (the text scalars, every list field, and all customs;
//     recognized by the unrestricted validator pattern) get a MULTILINE
//     editor: Return confirms like everywhere else, and Cmd/Ctrl+Return inserts a
//     line break. The validated fields (numbers, Date) keep the one-line
//     editor with its field-policy validator; a newline is never meaningful
//     there, and the validator mechanism is single-line-only anyway. Newlines
//     are literal value content, never a list separator: "; " remains the
//     only list separator, in the grid, here, and on the clipboard.
//   - Individual Values: one row per track (# / Track / Value), each cell
//     editable and pre-filled with that track's current value. Rows are
//     selectable (click, Ctrl/Cmd-click, Shift-click) and support Copy / Paste
//     of the value column, so values round-trip with a spreadsheet column.
//     Cells stay SINGLE-LINE: the line-per-row clipboard exchange depends
//     on it, so this tab is multiline-hostile by design; untouched cells still
//     commit back byte-identical (the ListModel stores strings verbatim), and
//     the pane-level escaped Copy / Paste is the lossless channel.
// The dialog opens on Single Value when the field is uniform across the selection
// and on Individual Values when it disagrees (@ref startUniform).
//
// On OK the active tab is turned into a per-track value list and emitted via
// committed(); the host stages it through PropertiesMetadataModel.stageFieldValues
// (which conforms each value to the field policy and treats an empty value as a
// removal). The Field name box is read-only here; the editable-name "+ add new"
// flow opens through addMode.
//
// The host sets the data properties, then calls reopen() to (re)seed the internal
// editable state, then makes the overlay visible.
// =============================================================================
Item {
    id: dlg

    property string uiFont: Theme.uiFont
    property string fieldLabel: ""
    property string fieldKey: ""
    property string validatorPattern: "^.*$"
    property string singleSeed: ""
    property bool startUniform: true  // true -> open Single Value, false -> Individual
    property var perTrackValues: []   // effective per-track values (Individual seed)
    property var trackLabels: []      // per-track Title / file name (middle column)

    // Whether the field is free text: the unrestricted pattern is what the
    // model hands back for every non-validated field (the text scalars, all list
    // fields, all customs), so it doubles as the multiline gate with no extra
    // model surface. Validated fields (numbers, Date) keep the one-line editor:
    // a newline is never meaningful there, and RegularExpressionValidator is a
    // single-line (TextInput) mechanism, so multiline and validation are
    // structurally exclusive anyway.
    readonly property bool freeText: dlg.validatorPattern === "^.*$"

    // Add mode (the "+ add new" flow): the Field name box becomes an editable,
    // uppercased entry, the title changes, and OK routes through addRequested.
    property bool addMode: false
    property string keyError: ""      // inline error under the name box (set by host)
    property string _keyText: ""      // normalized key currently typed

    // Emitted on OK with one display string per selected track (already broadcast
    // for Single mode); on Cancel / Esc / close nothing is staged.
    signal committed(var perTrackValues)
    // Emitted on OK in add mode; the host creates + stages the custom field and, on
    // failure, sets keyError and leaves the dialog open.
    signal addRequested(string key, var perTrackValues)
    signal canceled()

    // Editable state, seeded by reopen().
    property int _tab: 0              // 0 = Single Value, 1 = Individual Values
    property string _single: ""
    readonly property int _count: dlg.perTrackValues.length

    // Individual-tab selection + clipboard state.
    property var _sel: ({})           // map row index -> true
    property int _anchor: -1          // shift-range anchor
    property int _focusedCell: -1     // row whose Value cell holds focus (-1 = none)
    property string _pasteError: ""   // count-mismatch message
    readonly property bool _editingCell: dlg._focusedCell >= 0

    Clipboard { id: clip }

    // Per-track editable values live in a ListModel (role "value"), not a JS array.
    // A ListModel notifies on setProperty (dataChanged), so each cell's read-only
    // display Text, bound to model.value, always reflects the current value,
    // including after a Cancel reverts it on reopen. A JS array mutated in place
    // never notifies, which is why an edit could linger on screen after Cancel.
    ListModel { id: cellModel }

    // Snapshot the model's values into a plain array (for commit / copy).
    function _valuesArray() {
        var out = []
        for (var i = 0; i < cellModel.count; ++i) out.push(cellModel.get(i).value)
        return out
    }

    function reopen() {
        dlg._tab = dlg.startUniform ? 0 : 1
        dlg._single = dlg.singleSeed
        dlg._keyText = ""
        dlg.keyError = ""
        cellModel.clear()
        for (var i = 0; i < dlg.perTrackValues.length; ++i)
            cellModel.append({ value: String(dlg.perTrackValues[i]) })
        dlg._sel = ({})
        dlg._anchor = -1
        dlg._focusedCell = -1
        dlg._pasteError = ""
        // Seed the single-instance fields imperatively, so a binding broken by prior
        // typing cannot leave stale text behind on a later reopen. Both single-tab
        // editors are seeded (only one is visible, per freeText); the hidden one
        // just parks the seed.
        singleField.text = dlg.singleSeed
        singleArea.text = dlg.singleSeed
        keyEditor.text = ""
        if (dlg.addMode)
            Qt.callLater(function () { keyEditor.forceActiveFocus() })
        else if (dlg._tab === 0)
            Qt.callLater(function () {
                if (dlg.freeText) { singleArea.forceActiveFocus(); singleArea.selectAll() }
                else { singleField.forceActiveFocus(); singleField.selectAll() }
            })
        else
            // Focus a neutral sink (never a cell) so no editor opens on reopen.
            Qt.callLater(function () { focusSink.forceActiveFocus() })
    }

    // The per-track values from the active tab: Single broadcasts one value to all
    // tracks; Individual takes the model's cells.
    function _collectValues() {
        var out = []
        if (dlg._tab === 0) {
            for (var i = 0; i < dlg._count; ++i)
                out.push(dlg._single)
        } else {
            out = dlg._valuesArray()
        }
        return out
    }

    function _commit() {
        dlg.committed(dlg._collectValues())
    }

    // OK: add mode routes through addRequested (the host validates and may keep the
    // dialog open with an error); otherwise it stages the edit and closes.
    function _ok() {
        if (dlg.addMode)
            dlg.addRequested(dlg._keyText, dlg._collectValues())
        else
            dlg.committed(dlg._collectValues())
    }

    // Commit one cell edit from the transient editor.
    function _setCell(index, value) {
        if (index >= 0 && index < cellModel.count)
            cellModel.setProperty(index, "value", value)
    }

    // --- selection helpers ---------------------------------------------------
    function _selectedIndices() {
        var out = []
        for (var k in dlg._sel)
            if (dlg._sel[k]) out.push(parseInt(k))
        out.sort(function (a, b) { return a - b })
        return out
    }
    function _isSelected(i) { return dlg._sel[i] === true }
    function _clickRow(index, mods) {
        var s
        if ((mods & Qt.ShiftModifier) && dlg._anchor >= 0) {
            s = ({})
            var lo = Math.min(dlg._anchor, index)
            var hi = Math.max(dlg._anchor, index)
            for (var i = lo; i <= hi; ++i) s[i] = true
        } else if (mods & Qt.ControlModifier) {
            s = Object.assign({}, dlg._sel)
            if (s[index]) delete s[index]; else s[index] = true
            dlg._anchor = index
        } else {
            s = ({}); s[index] = true; dlg._anchor = index
        }
        dlg._sel = s
        dlg._pasteError = ""
    }
    function _selectAll() {
        var s = ({})
        for (var i = 0; i < dlg._count; ++i) s[i] = true
        dlg._sel = s
        dlg._anchor = dlg._count > 0 ? 0 : -1
        dlg._pasteError = ""
    }

    // --- clipboard -----------------------------------------------------------
    function _copy() {
        var idx = dlg._selectedIndices()
        if (idx.length === 0) return
        var rows = idx.map(function (i) { return cellModel.get(i).value })
        clip.setText(rows.join("\n"))
        dlg._pasteError = ""
    }
    function _paste() {
        var targets = dlg._selectedIndices()
        var hasSel = targets.length > 0
        if (!hasSel) {
            targets = []
            for (var i = 0; i < dlg._count; ++i) targets.push(i)
        }
        var raw = clip.text().replace(/\r/g, "")
        var lines = raw.split("\n")
        if (lines.length > 0 && lines[lines.length - 1] === "")
            lines.pop()                        // spreadsheets append a trailing newline
        if (lines.length === 0) return

        if (lines.length === 1) {
            for (var t = 0; t < targets.length; ++t)
                cellModel.setProperty(targets[t], "value", lines[0])
        } else if (lines.length === targets.length) {
            for (var k = 0; k < targets.length; ++k)
                cellModel.setProperty(targets[k], "value", lines[k])
        } else {
            dlg._pasteError = "Pasted " + lines.length + " values, "
                + targets.length + (hasSel ? " selected." : " rows.")
            return
        }
        dlg._pasteError = ""
    }

    implicitWidth: 560
    implicitHeight: 480

    // ----- panel -------------------------------------------------------------
    Rectangle {
        id: panel
        anchors.fill: parent
        color: "#1E1E1E"
        radius: 8
        border.color: Theme.separatorStrong
        border.width: 1

        // Title row.
        Item {
            id: titleRow
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 14
            height: 22
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: dlg.addMode ? "Add Field" : ("Edit Value : " + dlg.fieldLabel)
                color: Theme.textPrimary
                font.family: dlg.uiFont; font.pixelSize: 14; font.weight: Font.Bold
                elide: Text.ElideRight
                width: parent.width - 24
            }

            Button {
                id: closeButtonX
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                implicitHeight: 22
                implicitWidth: 22

                onClicked: dlg.canceled()

                background: Rectangle {
                    color: "transparent"
                }

                contentItem: Image {
                    id: svgCloseButtonX
                    asynchronous: true
                    fillMode: Image.Pad
                    source: "../../icons/app/dialogs/tool_dialog_close_x.svg"
                }

                MultiEffect {
                    anchors.fill: svgCloseButtonX
                    source: svgCloseButtonX
                    colorization: 1.0
                    colorizationColor: !closeButtonX.enabled ? Theme.textDisabled
                        : closeButtonX.hovered ? Theme.danger : Theme.textInactive
                }
            }
        }

        Rectangle {
            id: titleRule
            anchors.top: titleRow.bottom; anchors.topMargin: 10
            anchors.left: parent.left; anchors.right: parent.right
            height: 1; color: Theme.separator
        }

        // Field name: a read-only label when editing an existing field, an editable
        // uppercased entry when adding a new one.
        Text {
            id: fnLabel
            anchors.top: titleRule.bottom; anchors.topMargin: 12
            anchors.left: parent.left; anchors.leftMargin: 14
            text: "Field name:"
            color: Theme.textMuted
            font.family: dlg.uiFont; font.pixelSize: 12
        }
        // Inline key error sits on the label line, right-aligned, so it never shifts
        // the layout.
        Text {
            visible: dlg.addMode && dlg.keyError !== ""
            anchors.verticalCenter: fnLabel.verticalCenter
            anchors.left: fnLabel.right; anchors.leftMargin: 8
            anchors.right: parent.right; anchors.rightMargin: 14
            horizontalAlignment: Text.AlignRight
            elide: Text.ElideLeft
            text: dlg.keyError
            color: Theme.dangerSoft
            font.family: dlg.uiFont; font.pixelSize: 11
        }
        Rectangle {
            id: fnBox
            anchors.top: fnLabel.bottom; anchors.topMargin: 5
            anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 14; anchors.rightMargin: 14
            height: 26
            color: "#151515"; radius: 3
            border.color: (dlg.addMode && keyEditor.activeFocus) ? Theme.accentSoft : Theme.separatorStrong
            border.width: 1

            // Read-only key (existing field).
            Text {
                visible: !dlg.addMode
                anchors.fill: parent
                anchors.leftMargin: 6
                verticalAlignment: Text.AlignVCenter
                text: dlg.fieldKey
                color: "#8C8C8C" // read-only: dimmed
                font.family: dlg.uiFont; font.pixelSize: 12
                elide: Text.ElideRight
            }

            // Editable key (add mode): uppercased as typed; Enter triggers OK.
            TextField {
                id: keyEditor
                visible: dlg.addMode
                anchors.fill: parent
                anchors.margins: 1
                leftPadding: 6; rightPadding: 6; topPadding: 0; bottomPadding: 0
                verticalAlignment: TextInput.AlignVCenter
                background: null
                color: Theme.textPrimary
                font.family: dlg.uiFont; font.pixelSize: 12
                selectByMouse: true
                placeholderText: "New field name"
                placeholderTextColor: Theme.textDisabled
                onTextEdited: {
                    var up = text.toUpperCase()
                    if (up !== text) {
                        var p = cursorPosition
                        text = up
                        cursorPosition = p
                    }
                    dlg._keyText = up
                    dlg.keyError = ""   // clear the inline error as soon as the key changes
                }
                Keys.onReturnPressed: dlg._ok()
                Keys.onEnterPressed: dlg._ok()
            }
        }

        // Tab bar.
        Row {
            id: tabBar
            anchors.top: fnBox.bottom; anchors.topMargin: 12
            anchors.left: parent.left; anchors.leftMargin: 14
            spacing: 0

            component TabButton: Rectangle {
                property string label: ""
                property int tabIndex: 0
                width: tabText.implicitWidth + 24
                height: 26
                color: dlg._tab === tabIndex ? Theme.surfacePanel : "transparent"
                Text {
                    id: tabText
                    anchors.centerIn: parent
                    text: parent.label
                    color: dlg._tab === parent.tabIndex ? Theme.textPrimary : Theme.textMuted
                    font.family: dlg.uiFont; font.pixelSize: 12
                }
                Rectangle { // active underline
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left; anchors.right: parent.right
                    height: 2
                    visible: dlg._tab === parent.tabIndex
                    color: Theme.accentSoft
                }
                TapHandler { onTapped: dlg._tab = parent.tabIndex }
            }

            TabButton { label: "Single Value"; tabIndex: 0 }
            TabButton { label: "Individual Values"; tabIndex: 1 }
        }
        Rectangle {
            id: tabRule
            anchors.top: tabBar.bottom
            anchors.left: parent.left; anchors.right: parent.right
            height: 1; color: Theme.separator
        }

        // Content area (between the tab bar and the footer).
        Item {
            id: content
            anchors.top: tabRule.bottom; anchors.topMargin: 12
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: footer.top; anchors.bottomMargin: 10
            anchors.leftMargin: 14; anchors.rightMargin: 14

            // --- Single Value -----------------------------------------------
            Rectangle {
                id: singleBox
                visible: dlg._tab === 0
                anchors.top: parent.top
                anchors.left: parent.left; anchors.right: parent.right
                // One line for validated fields; roughly six lines for free text
                //, tall enough that a typical multi-line COMMENT edits
                // without immediate scrolling.
                height: dlg.freeText ? 118 : 28
                color: "#151515"; radius: 3
                border.color: Theme.accentSoft; border.width: 1

                // Validated one-line editor (numbers, Date). Return confirms, as
                // everywhere; the field-policy validator applies per keystroke.
                TextField {
                    id: singleField
                    visible: !dlg.freeText
                    anchors.fill: parent
                    anchors.margins: 1
                    leftPadding: 6; rightPadding: 6; topPadding: 0; bottomPadding: 0
                    verticalAlignment: TextInput.AlignVCenter
                    background: null
                    color: "#FFFFFF"
                    font.family: dlg.uiFont; font.pixelSize: 12
                    selectByMouse: true
                    // Seeded imperatively by reopen() (see there); onTextEdited keeps
                    // _single in sync without a binding that user input could break.
                    onTextEdited: dlg._single = text
                    validator: RegularExpressionValidator {
                        regularExpression: new RegExp(dlg.validatorPattern)
                    }
                    Keys.onReturnPressed: dlg._ok()
                    Keys.onEnterPressed: dlg._ok()
                }

                // Free-text multiline editor, inside the standard scrolling
                // Flickable attachment. Key policy: Return CONFIRMS, the
                // same muscle memory as every other editor in the app, and
                // Cmd/Ctrl+Return inserts the line break. Qt maps Command to
                // Qt.ControlModifier on macOS by default, so one modifier test
                // covers both platforms, exactly like the pane's shortcuts. The
                // Keys handler runs BeforeItem (the attached-property default),
                // so accepting the event keeps the TextArea's own Return
                // handling (insert newline) from also firing. Esc is left
                // unaccepted so the dialog-level cancel still catches it.
                Flickable {
                    id: singleFlick
                    visible: dlg.freeText
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    TextArea.flickable: TextArea {
                        id: singleArea
                        leftPadding: 6; rightPadding: 6; topPadding: 5; bottomPadding: 5
                        background: null
                        color: "#FFFFFF"
                        font.family: dlg.uiFont; font.pixelSize: 12
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        // Seeded imperatively by reopen(). onTextChanged (TextEdit
                        // has no textEdited) also fires on that programmatic seed,
                        // which harmlessly re-assigns the value reopen just set.
                        onTextChanged: dlg._single = text
                        Keys.onPressed: function (e) {
                            if (e.key === Qt.Key_Return || e.key === Qt.Key_Enter) {
                                e.accepted = true
                                if (e.modifiers & Qt.ControlModifier)
                                    singleArea.insert(singleArea.cursorPosition, "\n")
                                else
                                    dlg._ok()
                            }
                        }
                    }
                }
            }

            // Discoverability hint for the key policy, shown only when the
            // multiline editor is the active one. Dim and static; it never
            // shifts the layout (the Individual tab overlays this area only
            // when _tab is 1, and this is hidden there).
            Text {
                visible: dlg._tab === 0 && dlg.freeText
                anchors.top: singleBox.bottom; anchors.topMargin: 6
                anchors.left: parent.left
                text: "Return confirms; Cmd/Ctrl+Return inserts a line break"
                color: "#7C7C7C"
                font.family: dlg.uiFont; font.pixelSize: 11
            }

            // --- Individual Values ------------------------------------------
            Item {
                id: individualBox
                visible: dlg._tab === 1
                anchors.fill: parent

                // Toolbar: Copy / Paste + selection / error hint.
                component MiniButton: Rectangle {
                    property string label: ""
                    property bool on: true
                    signal clicked()
                    width: mbText.implicitWidth + 18; height: 22; radius: 3
                    color: !on ? "#1C1C1C" : (mbHover.hovered ? Theme.surfaceControlHover : "#242424")
                    border.color: Theme.border; border.width: 1
                    opacity: on ? 1 : 0.5
                    Text {
                        id: mbText; anchors.centerIn: parent; text: parent.label
                        color: Theme.textSecondary; font.family: dlg.uiFont; font.pixelSize: 11
                    }
                    HoverHandler { id: mbHover; enabled: parent.on }
                    TapHandler { enabled: parent.on; onTapped: parent.clicked() }
                }

                Row {
                    id: indivBar
                    anchors.top: parent.top
                    anchors.left: parent.left
                    height: 22
                    spacing: 8
                    MiniButton {
                        label: "Copy"
                        on: dlg._selectedIndices().length > 0
                        onClicked: dlg._copy()
                    }
                    MiniButton {
                        label: "Paste"
                        onClicked: dlg._paste()
                    }
                }
                Text {
                    anchors.verticalCenter: indivBar.verticalCenter
                    anchors.right: parent.right
                    width: parent.width - indivBar.width - 12
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideLeft
                    text: dlg._pasteError !== "" ? dlg._pasteError
                          : (dlg._selectedIndices().length > 0
                             ? (dlg._selectedIndices().length + " selected")
                             : "Click # / name to select rows")
                    color: dlg._pasteError !== "" ? Theme.dangerSoft : "#7C7C7C"
                    font.family: dlg.uiFont; font.pixelSize: 11
                }

                // Column header.
                Rectangle {
                    id: tblHeader
                    anchors.top: indivBar.bottom; anchors.topMargin: 6
                    anchors.left: parent.left; anchors.right: parent.right
                    height: 20
                    color: Theme.surfacePage
                    Text {
                        x: 4; anchors.verticalCenter: parent.verticalCenter
                        width: 28; horizontalAlignment: Text.AlignRight
                        text: "#"; color: Theme.textDim
                        font.family: dlg.uiFont; font.pixelSize: 11
                    }
                    Text {
                        x: 40; anchors.verticalCenter: parent.verticalCenter
                        text: "Track"; color: Theme.textDim
                        font.family: dlg.uiFont; font.pixelSize: 11
                    }
                    Text {
                        x: parent.width / 2 + 8; anchors.verticalCenter: parent.verticalCenter
                        text: "Value"; color: Theme.textDim
                        font.family: dlg.uiFont; font.pixelSize: 11
                    }
                    Rectangle {
                        anchors.bottom: parent.bottom
                        width: parent.width; height: 1; color: Theme.separator
                    }
                }

                // A neutral focus target: row-selection clicks and reopen move focus
                // here so no cell editor is left holding focus.
                Item { id: focusSink }

                // Per-track rows over the ListModel. Each cell is a read-only display
                // Text bound to model.value (its binding never breaks, so it always
                // reflects the current value, including a reverted one after Cancel),
                // swapped to a transient editor only while actively editing. reuseItems
                // is off so a recycled delegate never carries a stale editing flag.
                ListView {
                    id: cellList
                    anchors.top: tblHeader.bottom
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    clip: true
                    model: cellModel
                    reuseItems: false
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Item {
                        id: cell
                        required property int index
                        required property var model
                        readonly property bool _selected: dlg._isSelected(cell.index)
                        property bool editing: false
                        width: cellList.width
                        height: 22

                        Rectangle {
                            anchors.fill: parent
                            color: cell._selected ? Theme.selectionSoft
                                   : ((cell.index % 2 === 0) ? Theme.rowEven : Theme.rowOdd)

                            // When not editing: single click selects (modifiers extend /
                            // toggle), double click on the Value column opens the editor.
                            // Disabled while editing so clicks reach the editor / commit
                            // the current one by moving focus.
                            MouseArea {
                                anchors.fill: parent
                                enabled: !cell.editing
                                onClicked: function (mouse) {
                                    focusSink.forceActiveFocus()
                                    dlg._clickRow(cell.index, mouse.modifiers)
                                }
                                onDoubleClicked: function (mouse) {
                                    if (mouse.x >= valueCell.x) {
                                        cell.editing = true
                                        editor.text = cell.model.value
                                        editor.forceActiveFocus()
                                        editor.selectAll()
                                    }
                                }
                            }

                            Text {
                                x: 4; anchors.verticalCenter: parent.verticalCenter
                                width: 28; horizontalAlignment: Text.AlignRight
                                text: (cell.index + 1)
                                color: "#7C7C7C"
                                font.family: dlg.uiFont; font.pixelSize: 11
                            }
                            Text {
                                x: 40; anchors.verticalCenter: parent.verticalCenter
                                width: parent.width / 2 - 48
                                text: dlg.trackLabels[cell.index] !== undefined ? dlg.trackLabels[cell.index] : ""
                                color: Theme.textMuted
                                font.family: dlg.uiFont; font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            Rectangle {
                                id: valueCell
                                x: parent.width / 2 + 6
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width / 2 - 12
                                height: 18
                                color: cell.editing ? "#151515" : "transparent"
                                radius: 2
                                border.color: cell.editing ? Theme.accentSoft : "transparent"
                                border.width: 1

                                // Read-only display, bound to the model role. A
                                // multiline value is flattened to one line with a
                                // return glyph per line break (display
                                // only): this is the tab that CANNOT edit such a
                                // value faithfully, so the marker is a warning as
                                // much as a preview; the stored value and an
                                // untouched cell's commit stay byte-identical.
                                Text {
                                    visible: !cell.editing
                                    anchors.fill: parent
                                    anchors.leftMargin: 5; anchors.rightMargin: 5
                                    verticalAlignment: Text.AlignVCenter
                                    text: String(cell.model.value).replace(/\r?\n/g, "\u21B5")
                                    color: Theme.textPrimary
                                    font.family: dlg.uiFont; font.pixelSize: 11
                                    elide: Text.ElideRight
                                }

                                // Transient editor: seeded imperatively on open, commits
                                // to the model on Enter / focus-loss, discards on Esc.
                                TextField {
                                    id: editor
                                    visible: cell.editing
                                    anchors.fill: parent
                                    anchors.margins: 1
                                    leftPadding: 5; rightPadding: 5; topPadding: 0; bottomPadding: 0
                                    verticalAlignment: TextInput.AlignVCenter
                                    background: null
                                    color: Theme.textPrimary
                                    font.family: dlg.uiFont; font.pixelSize: 11
                                    selectByMouse: true
                                    validator: RegularExpressionValidator {
                                        regularExpression: new RegExp(dlg.validatorPattern)
                                    }
                                    onEditingFinished: {
                                        // Fires on Enter and on focus-loss (clicking away).
                                        if (cell.editing) {
                                            cell.editing = false
                                            dlg._setCell(cell.index, text)
                                        }
                                    }
                                    Keys.onEscapePressed: function (event) {
                                        cell.editing = false   // discard; model untouched
                                        event.accepted = true  // do not also cancel the dialog
                                    }
                                    onActiveFocusChanged: {
                                        if (activeFocus) dlg._focusedCell = cell.index
                                        else if (dlg._focusedCell === cell.index) dlg._focusedCell = -1
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Footer (OK / Cancel).
        Item {
            id: footer
            anchors.bottom: parent.bottom
            anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 12
            height: 28

            component DialogButton: Rectangle {
                property string label: ""
                property bool primary: false
                signal clicked()
                width: 84; height: 28; radius: 4
                color: btnHover.hovered ? (primary ? "#3A3458" : Theme.surfaceRaised)
                                        : (primary ? Theme.selectionSoft : Theme.surfacePanel)
                border.color: primary ? Theme.accentSoft : Theme.border
                border.width: 1
                Text {
                    anchors.centerIn: parent
                    text: parent.label
                    color: Theme.textPrimary
                    font.family: dlg.uiFont; font.pixelSize: 12
                }
                HoverHandler { id: btnHover }
                TapHandler { onTapped: parent.clicked() }
            }

            DialogButton {
                anchors.right: cancelBtn.left; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                label: "OK"; primary: true
                onClicked: dlg._ok()
            }
            DialogButton {
                id: cancelBtn
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                label: "Cancel"
                onClicked: dlg.canceled()
            }
        }
    }

    // Copy / Paste of the value column, active only on the Individual tab and only
    // when a cell editor does NOT hold focus (so a cell's own Cmd-C/V still edits
    // its text). Cross-platform via StandardKey (Cmd on macOS, Ctrl elsewhere). The
    // 'sequences' (plural) form binds every key combination a StandardKey maps to,
    // which silences the "only binding to one of multiple key bindings" warning.
    Shortcut {
        sequences: [ StandardKey.SelectAll ]
        enabled: dlg.visible && dlg._tab === 1 && !dlg._editingCell
        onActivated: dlg._selectAll()
    }
    Shortcut {
        sequences: [ StandardKey.Copy ]
        enabled: dlg.visible && dlg._tab === 1 && !dlg._editingCell
        onActivated: dlg._copy()
    }
    Shortcut {
        sequences: [ StandardKey.Paste ]
        enabled: dlg.visible && dlg._tab === 1 && !dlg._editingCell
        onActivated: dlg._paste()
    }

    // Esc anywhere in the dialog cancels.
    Keys.onEscapePressed: function (e) { dlg.canceled(); e.accepted = true }

    // On hide, clear selection / focus / error state. An invisible item also loses
    // active focus, which drops any cell editor's accent border, so the dialog never
    // reopens looking like a row is still selected.
    onVisibleChanged: {
        if (!visible) {
            dlg._sel = ({})
            dlg._anchor = -1
            dlg._focusedCell = -1
            dlg._pasteError = ""
        }
    }
}
