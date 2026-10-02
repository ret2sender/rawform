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

// FindDialog.qml
//
// Edit > Find (Ctrl+F): live search over the ACTIVE playlist. SettingsWindow's
// citizenship: a frameless Qt.Tool Window, our chrome, NON-MODAL, ONE
// hide()-reused instance declared by MainWindow, so reopening brings back the
// last String and Filter text of the session.
//
// Layout: String (what to find); Filter (where to look: ':'-separated column
// titles, custom column names, field ids or %token% patterns, see
// search/PlaylistSearchFilter.h; empty searches the visible columns); Preset
// (the Filter text under a name, RenameFilesDialog's row verbatim, backed by
// SearchPresetStore); a footer with the match count and Close.
//
// The search itself is PlaylistSearch (C++): this file only pushes the two
// texts in and reacts to matchesChanged. Every O(rows) step is coalesced
// there (120 ms), so the fields bind their text directly with no timer here,
// and suspended while this window is hidden (enabled follows visible), so a
// scan into the playlist costs nothing to a Find nobody has open.
// The Filter parse is synchronous, which is what lets the invalid-entry
// underline (drawn from entryDiagnostics spans through positionToRectangle)
// track the typed text exactly.
//
// How a search acts on the playlist: while this dialog is shown, its search
// is installed as playlistTabs.activeSearch, and the active tab's
// PlaylistFilterProxy hides every row that does not match, so the playlist
// itself shows the hits, with no second list. When a filter lands
// (view.filterApplied) the current row is placed on the first visible row if
// the filter hid it, and revealed (revealRow -> the host's PlaylistView);
// Enter steps the current row down through the visible rows, Shift+Enter up,
// wrapping. A blank String, or a Filter with no valid entry, is no search at
// all and every row shows; a search with zero hits shows an empty playlist.
// Closing the dialog lifts the filter (activeSearch goes null); the texts
// stay for the next open.

// qmllint disable unqualified
// Wiring layer: this file reaches the C++ context properties (windowGeometry,
// customColumns), which qmllint cannot see, so the unqualified-access category
// is disabled file-wide, as in the other views.

pragma ComponentBehavior: Bound

import QtQml.Models
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window
import com.rawform.app

Window {
    id: findDialog

    property string uiFont: Theme.uiFont

    // The searched playlist (its SOURCE model, what the search reads), its
    // filter proxy (what the view shows; rows below are ITS rows) and its
    // selection: the host binds all three to the active tab, so a tab switch
    // re-runs the search against the new tab and moves the filter with it.
    property var model: null
    property var view: null
    property var selection: null
    // The registry, captured at Window scope: inside the PlaylistSearch block
    // below, an unqualified `customColumns` would resolve to its own property
    // of that name (a self-binding), so the context property is read here.
    readonly property var _registry: customColumns

    /// Ask the host to make @p row current and center it in the playlist.
    signal revealRow(int row)

    title: "Find"
    // Width restores from window.yaml's keyed store; the height is the
    // content's, fixed: nothing in this dialog grows.
    width: Math.max(minimumWidth, windowGeometry.savedWidth("find", 640))
    height: 224
    minimumWidth: 480
    minimumHeight: 224
    maximumHeight: 224
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal

    // Escape closes (the X's exact path). Window context, and the same
    // visible && strict-focus gate as SettingsWindow: this is a hide()-reused
    // instance whose Shortcut outlives every close, and an enabled shortcut
    // in a hidden window keeps a grab in Qt's map that turns every later
    // Escape ambiguous (see the SettingsWindow note). The preset combo popup
    // consumes Escape itself (CloseOnEscape) before this can match.
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: findDialog.visible && WindowFocus.focusWindow === findDialog
        onActivated: findDialog.dismiss()
    }

    // --- engines -----------------------------------------------------------
    PlaylistSearch {
        id: search
        model: findDialog.model
        customColumns: findDialog._registry
        // Hidden means suspended: the kept texts stay, the row work stops.
        enabled: findDialog.visible
    }
    // The proxy announces each applied filter AFTER the selection model has
    // remapped, which is when the current row can be placed; the search's
    // own matchesChanged fires before the proxy has re-filtered.
    Connections {
        target: findDialog.view
        function onFilterApplied() { findDialog._onFilterApplied() }
    }

    // Shown: the active tab filters by this search. Hidden: the filter lifts.
    // PlaylistTabs moves the search to whichever tab becomes active.
    onVisibleChanged: playlistTabs.activeSearch = findDialog.visible ? search : null
    SearchPresetStore { id: presetStore }

    // --- state -------------------------------------------------------------
    property string _statusText: ""
    // Transient (flash) statuses: preset Load/Save/Delete confirmations show
    // for 10 s, then the footer reverts to the match count.
    property bool _statusFlash: false

    // The invalid entries' spans, for the underline overlay.
    readonly property var _invalidSpans: {
        var d = search.entryDiagnostics
        var out = []
        for (var i = 0; i < d.length; i++)
            if (!d[i].valid)
                out.push(d[i])
        return out
    }

    function open() {
        show()
        raise()
        requestActivate()
        stringField.forceActiveFocus()
        stringField.selectAll()
    }

    // The single hide path for Close, the title bar X and Escape.
    function dismiss() {
        _saveSize()
        hide()
    }

    function _saveSize() {
        if (findDialog.visibility === Window.Windowed)
            windowGeometry.saveSize("find", findDialog.width, findDialog.height)
    }

    // A window-manager close request bypasses the in-window paths above;
    // accepting it hides the reused instance, and the next open() shows it.
    onClosing: findDialog._saveSize()

    // A filter landed (see the file comment): give the search a current row
    // to step from when the filter took it away, and reveal it. A current row
    // that survived the filter is left where it is, so typing one more letter
    // never yanks the view.
    function _onFilterApplied() {
        if (!findDialog.visible || !search.active || !findDialog.view
                || !findDialog.selection)
            return
        var ci = findDialog.selection.currentIndex
        if (ci && ci.valid)
            return
        if (findDialog.view.rowCount() > 0)
            findDialog._focusRow(0)
    }

    // Enter / Shift+Enter: step the current row through the visible rows
    // (all of them matches while a search is in force), wrapping.
    function _step(backward) {
        if (!search.active || !findDialog.view || !findDialog.selection)
            return
        var n = findDialog.view.rowCount()
        if (n <= 0)
            return
        var ci = findDialog.selection.currentIndex
        var cur = (ci && ci.valid) ? ci.row : -1
        var next = backward ? (cur <= 0 ? n - 1 : cur - 1)
                            : (cur + 1 >= n ? 0 : cur + 1)
        findDialog._focusRow(next)
    }

    // Make proxy row @p row current (NoUpdate: the outline only) and reveal.
    function _focusRow(row) {
        findDialog.selection.setCurrentIndex(findDialog.view.index(row, 0),
                                             ItemSelectionModel.NoUpdate)
        findDialog.revealRow(row)
    }

    // Sets the preset box text through BOTH channels (the contentItem's text
    // binding is severed the first time the user types; see the same helper
    // in RenameFilesDialog).
    function _setPresetText(t) {
        presetCombo.editText = t
        presetEditor.text = t
    }

    function _flash(msg) {
        findDialog._statusText = msg
        findDialog._statusFlash = true
        statusClear.restart()
    }

    // Reverts a flash status back to the standing match count.
    Timer {
        id: statusClear
        interval: 10000
        repeat: false
        onTriggered: {
            findDialog._statusText = ""
            findDialog._statusFlash = false
        }
    }

    // ThemedMenu resolves its blur backdrop through the hosting window's
    // menuBlurSource (the tool-window precedent): the edit context menus
    // below need it or they render over nothing.
    property Item menuBlurSource: windowBody

    // --- chrome ------------------------------------------------------------
    Rectangle {
        id: windowBody
        anchors.fill: parent
        color: Theme.surfacePage
        radius: 8
        border.color: Theme.separatorStrong
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            // ----- title bar: drag + close ---------------------------------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                MouseArea {
                    anchors.fill: parent
                    onPressed: findDialog.startSystemMove()
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Find"
                    color: Theme.textPrimary
                    font.family: findDialog.uiFont
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }
                ToolDialogCloseButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: findDialog.dismiss()
                }
            }

            // ----- string / filter / preset --------------------------------
            GridLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 4
                columns: 2
                columnSpacing: 8
                rowSpacing: 8

                Text {
                    text: "String"
                    color: Theme.textPrimary
                    font.family: findDialog.uiFont
                    font.pixelSize: 12
                }
                TextField {
                    id: stringField
                    Layout.fillWidth: true
                    font.family: findDialog.uiFont
                    font.pixelSize: 12
                    selectByMouse: true
                    background: Rectangle {
                        radius: 4
                        color: Theme.rowEven
                    }
                    onTextChanged: search.queryText = text
                    // Enter steps forward, Shift+Enter back; the keypad Enter
                    // rides the same handlers.
                    Keys.onReturnPressed: function (event) {
                        findDialog._step(!!(event.modifiers & Qt.ShiftModifier))
                        event.accepted = true
                    }
                    Keys.onEnterPressed: function (event) {
                        findDialog._step(!!(event.modifiers & Qt.ShiftModifier))
                        event.accepted = true
                    }
                    ContextMenu.menu: EditMenu { editor: stringField }
                }

                Text {
                    text: "Filter"
                    color: Theme.textPrimary
                    font.family: findDialog.uiFont
                    font.pixelSize: 12
                }
                TextField {
                    id: filterField
                    Layout.fillWidth: true
                    font.family: Theme.monoFont
                    font.pixelSize: 12
                    selectByMouse: true
                    placeholderText: "Artist:Album:%composer%  (empty: the visible columns)"
                    background: Rectangle {
                        radius: 4
                        color: Theme.rowEven
                        // The danger stroke while any entry is invalid; the
                        // focus stroke otherwise, as the other fields draw it.
                        border.width: search.filterHasInvalid || filterField.activeFocus ? 1 : 0
                        border.color: search.filterHasInvalid ? Theme.danger : Theme.accentSoft
                    }
                    onTextChanged: search.filterText = text
                    ContextMenu.menu: EditMenu { editor: filterField }

                    // The invalid-entry underlines, one per span, drawn in the
                    // field's own coordinates just under the glyph baseline.
                    // positionToRectangle reports TEXT-space x (the horizontal
                    // scroll applied, the left padding not; cursorRectangle is
                    // the one that adds it), measured against a screenshot, so
                    // leftPadding is added here to land in item space. It is
                    // not a property, so each x reads the field's text, width
                    // and cursorPosition too: the reads are what re-evaluate
                    // the binding when the field scrolls or reflows (a
                    // standard QML dependency nudge).
                    Repeater {
                        model: findDialog._invalidSpans
                        delegate: Rectangle {
                            id: underline
                            required property var modelData
                            readonly property real _x0: {
                                void filterField.text
                                void filterField.width
                                void filterField.cursorPosition
                                return filterField.leftPadding
                                    + filterField.positionToRectangle(underline.modelData.start).x
                            }
                            readonly property real _x1: {
                                void filterField.text
                                void filterField.width
                                void filterField.cursorPosition
                                return filterField.leftPadding
                                    + filterField.positionToRectangle(
                                          underline.modelData.start + underline.modelData.length).x
                            }
                            // Clipped to the text area so a span scrolled out
                            // of view does not paint over the padding.
                            x: Math.max(filterField.leftPadding, underline._x0)
                            width: Math.max(0, Math.min(filterField.width - filterField.rightPadding,
                                                        underline._x1) - underline.x)
                            y: filterField.height - 7
                            height: 1.5
                            color: Theme.danger
                            visible: width > 0
                        }
                    }
                }

                Text {
                    text: "Preset"
                    color: Theme.textPrimary
                    font.family: findDialog.uiFont
                    font.pixelSize: 12
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    ThemedComboBox {
                        id: presetCombo
                        Layout.fillWidth: true
                        editable: true   // the edit text doubles as the save-as name
                        textRole: "name"
                        model: presetStore.catalog()
                        // The editable content item, RenameFilesDialog's
                        // verbatim: selectByMouse on, the themed edit menu,
                        // no background (the combo draws its own frame).
                        contentItem: TextField {
                            id: presetEditor
                            background: null
                            enabled: presetCombo.editable
                            font: presetCombo.font
                            text: presetCombo.editable ? presetCombo.editText
                                                       : presetCombo.displayText
                            selectByMouse: true
                            verticalAlignment: Text.AlignVCenter
                            ContextMenu.menu: EditMenu { editor: presetEditor }
                        }
                        onActivated: function (index) {
                            findDialog._setPresetText(presetCombo.textAt(index))
                        }
                        Connections {
                            target: presetStore
                            function onPatternsChanged() {
                                // Reassigning the model stomps the edit text;
                                // keep a typed name that still exists (the
                                // save flow), clear one that does not (the
                                // delete flow).
                                var typed = presetCombo.editText
                                presetCombo.model = presetStore.catalog()
                                var i = presetCombo.find(typed)
                                if (i >= 0) {
                                    presetCombo.currentIndex = i
                                    findDialog._setPresetText(typed)
                                } else {
                                    presetCombo.currentIndex = -1
                                    findDialog._setPresetText("")
                                }
                            }
                        }
                    }
                    FooterButton {
                        label: "Load"
                        enabled: presetCombo.editText.length > 0
                                 && presetStore.patternFor(presetCombo.editText).length > 0
                        onClicked: {
                            presetStore.setLastUsed(presetCombo.editText)
                            filterField.text = presetStore.patternFor(presetCombo.editText)
                            findDialog._flash("Loaded preset '" + presetCombo.editText + "'")
                        }
                    }
                    FooterButton {
                        label: "Save"
                        enabled: presetCombo.editText.trim().length > 0
                                 && filterField.text.length > 0
                        onClicked: {
                            presetStore.save(presetCombo.editText, filterField.text)
                            presetStore.setLastUsed(presetCombo.editText)
                            findDialog._flash("Saved preset '" + presetCombo.editText.trim() + "'")
                        }
                    }
                    FooterButton {
                        label: "Delete"
                        enabled: presetCombo.editText.length > 0
                                 && presetStore.patternFor(presetCombo.editText).length > 0
                        onClicked: {
                            var name = presetCombo.editText
                            presetStore.remove(name)
                            presetCombo.currentIndex = -1
                            findDialog._setPresetText("")
                            findDialog._flash("Deleted preset '" + name + "'")
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }

            // ----- footer: match count + Close -----------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                bottomLeftRadius: 8
                bottomRightRadius: 8
                color: Theme.headerBand

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.right: footerButtons.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: findDialog._statusText.length > 0 ? findDialog._statusText
                        : !search.active ? ""
                        : search.matchCount === 0 ? "no matches"
                        : search.matchCount === 1 ? "1 match"
                        : search.matchCount + " matches"
                    color: findDialog._statusFlash ? Theme.textInactive
                         : (search.active && search.matchCount === 0) ? Theme.danger
                         : Theme.textFaint
                    font.family: findDialog.uiFont
                    font.pixelSize: 11
                }
                RowLayout {
                    id: footerButtons
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    FooterButton {
                        label: "Close"
                        onClicked: findDialog.dismiss()
                    }
                }
            }
        }
    }

    // Resize edges (width only: the height is pinned above, so the top and
    // bottom strips have nothing to move). topInset keeps the top strip off
    // the title bar's move zone.
    WindowResizeGrips {
        target: findDialog
        topInset: 40
    }

    // The app-themed replacement for Qt's stock text-edit context menu
    // (RenameFilesDialog's component, verbatim).
    component EditMenu: ThemedMenu {
        id: editMenu
        property TextField editor: null

        ThemedMenuItem {
            text: "Undo"
            enabled: editMenu.editor !== null && editMenu.editor.canUndo
            onTriggered: editMenu.editor.undo()
        }
        ThemedMenuItem {
            text: "Redo"
            enabled: editMenu.editor !== null && editMenu.editor.canRedo
            onTriggered: editMenu.editor.redo()
        }
        MenuSeparator {}
        ThemedMenuItem {
            text: "Cut"
            enabled: editMenu.editor !== null
                     && editMenu.editor.selectedText.length > 0
            onTriggered: editMenu.editor.cut()
        }
        ThemedMenuItem {
            text: "Copy"
            enabled: editMenu.editor !== null
                     && editMenu.editor.selectedText.length > 0
            onTriggered: editMenu.editor.copy()
        }
        ThemedMenuItem {
            text: "Paste"
            enabled: editMenu.editor !== null && editMenu.editor.canPaste
            onTriggered: editMenu.editor.paste()
        }
        ThemedMenuItem {
            text: "Delete"
            enabled: editMenu.editor !== null
                     && editMenu.editor.selectedText.length > 0
            onTriggered: editMenu.editor.remove(editMenu.editor.selectionStart,
                                                editMenu.editor.selectionEnd)
        }
        MenuSeparator {}
        ThemedMenuItem {
            text: "Select All"
            enabled: editMenu.editor !== null && editMenu.editor.length > 0
            onTriggered: editMenu.editor.selectAll()
        }
    }

    // Same shape as the other tool windows' footer buttons.
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
            color: !fbtn.enabled ? Theme.textDisabled
                 : fbtn.accent ? Theme.textOnAccent : Theme.textPrimary
            font.family: findDialog.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
        }
        HoverHandler { id: fbtnHover }
        TapHandler { onTapped: if (fbtn.enabled) fbtn.clicked() }
    }
}
