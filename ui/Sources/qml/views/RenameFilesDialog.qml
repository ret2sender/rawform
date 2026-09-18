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
// Wiring layer, same license as PropertiesWindow: this file reaches the C++
// context properties (audioController, playlistTabs), which qmllint cannot
// see, so the unqualified-access category is disabled file-wide.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window

// =============================================================================
// RenameFilesDialog.qml
//
// File Operations > Rename To: pattern-driven on-disk renaming of the
// selected tracks. PropertiesWindow's citizenship and lifetime verbatim:
// frameless Qt.Tool Window, our chrome, NON-MODAL, a FRESH INSTANCE per
// invocation created by PlaylistView, destroyed on close; openFor(model, rows)
// snapshots the selection as durable { path, subsong } keys immediately, so a
// playlist mutation after opening can never redirect a rename.
//
// Layout: pattern field; preset dropdown (editable:
// its text is the save-as name) with Save / Load; the old-name vs new-name
// preview table with per-row status flags; Cancel / Apply / OK.
//
// The preview is recomputed through RenamePreviewer (C++) on every pattern
// edit, debounced 250 ms so typing does not thrash filesystem existence
// checks. Apply/OK enable only when at least one row is "ok" and no row is
// blocking (invalid / conflict_*). Apply renames and STAYS OPEN; OK renames
// and closes on full success (stays open when anything failed, so the
// per-row state can be read); Cancel and the titlebar X just close.
//
// Apply sequence: FileRenamer.apply(jobs) off-thread (stops playback first if
// it holds one of the files, TagWriteLock per file against concurrent tag
// saves) -> applied(ok, failedPaths, renames) -> playlistTabs.applyPathRenames
// patches EVERY open tab's rows in place (paths persist via each tab's normal
// autosave) -> this dialog remaps its own snapshot keys through the same map
// (the old paths just stopped existing; without the remap every row would
// preview as "gone") -> re-preview, where successes now read "identity",
// which doubles as the visible receipt that the rename landed.
//
// The captured model belongs to the owning tab, which can close under this
// non-modal window; every touch goes through _alive (playlistTabs.objectAlive,
// the destroyed-object probe). A dead model degrades the dialog to a
// static view whose Apply is disabled (jobs need a live preview).
// =============================================================================
Window {
    id: renameDialog

    property string uiFont: Theme.uiFont

    title: "Rename Files"
    // Size restores from window.yaml's keyed store, else the built-in
    // default; clamped against the minimums here because the store does not
    // know them. Initial values, not live bindings (startup-only read).
    // Position is NOT restored: the host cascades each instance.
    width: Math.max(minimumWidth, windowGeometry.savedWidth("rename", 720))
    height: Math.max(minimumHeight, windowGeometry.savedHeight("rename", 480))
    minimumWidth: 560
    minimumHeight: 360
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal

    // Escape closes, the Cancel/X buttons' exact path, and shares their
    // gate: never mid-apply (the rename pass must run to completion; its
    // buttons are disabled for the same reason). No selection concept here,
    // so the close is single-stage. Window context, Shortcut phase (the Escape
    // macOS delivery rationale). The pattern field's edit menu and the preset
    // combo popup consume Escape themselves (CloseOnEscape) before this can
    // match; Escape while TYPING in the pattern field does close the dialog,
    // accepted, matching foobar's dialog behavior.
    //
    // The visible && strict-focus gate. This instance dies on close,
    // so it cannot linger hidden like the reused windows, but SEVERAL Rename
    // (and Properties) windows can be open at once, and Window.active (the
    // first attempt here) is QWindow::isActive(), a transient-GROUP activation every
    // visible secondary window reports together, which left multi-window
    // presses ambiguous (ambiguity fires neither window's onActivated).
    // WindowFocus.focusWindow (QGuiApplication::focusWindow()) is singular,
    // so identity against it makes the grab mutually exclusive across every
    // secondary window.
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: renameDialog.visible
                 && WindowFocus.focusWindow === renameDialog
                 && !renameDialog._applying
        onActivated: renameDialog.close()
    }

    // --- captured world (openFor) + live state -------------------------------
    property var _model: null      // owning tab's PlaylistModel (guard via _alive)
    property var _keys: []         // durable { path, subsong } snapshot
    property var _previewRows: []  // last RenamePreviewer result
    property bool _applying: false
    property string _statusText: ""
    // Transient (flash) statuses: preset Load/Save confirmations show in the
    // hint gray for 10 s, then the footer reverts to the rename count.
    // Persistent statuses (apply progress/results) never set this.
    property bool _statusFlash: false
    // Old/new column split (fraction of the table width given to the left
    // column), dragged via the header splitter. Session-local on purpose.
    property real _colSplit: 0.5

    readonly property int _okCount: {
        var n = 0
        for (var i = 0; i < _previewRows.length; i++)
            if (_previewRows[i].status === "ok") n++
        return n
    }
    readonly property bool _hasBlocking: {
        for (var i = 0; i < _previewRows.length; i++) {
            var s = _previewRows[i].status
            if (s === "invalid" || s === "conflict_batch" || s === "conflict_disk")
                return true
        }
        return false
    }
    readonly property bool _canApply:
        !_applying && !renamer.busy && _okCount > 0 && !_hasBlocking
        && patternField.text.length > 0

    function _alive(obj) {
        if (!obj)
            return false
        return playlistTabs.objectAlive(obj)
    }

    function openFor(model, rows) {
        if (!model || !rows || rows.length === 0) {
            renameDialog.destroy() // never shown; do not leak the instance
            return
        }
        renameDialog._model = model
        // The durable identity snapshot: everything after this moment
        // addresses these tracks by (path, subsong), never by position.
        renameDialog._keys = model.trackKeys(rows)
        // Last-used preset, if it still exists: preselect it in the combo and
        // preload its pattern so the dialog opens ready to Apply.
        var last = presetStore.lastUsed()
        if (last.length > 0) {
            var pat = presetStore.patternFor(last)
            if (pat.length > 0) {
                var idx = presetCombo.find(last)
                if (idx >= 0)
                    presetCombo.currentIndex = idx
                renameDialog._setPresetText(last)
                patternField.text = pat
            }
        }
        renameDialog._refreshPreview()
        previewDebounce.stop() // the text set above armed it; already refreshed
        show()
        raise()
        requestActivate()
        patternField.forceActiveFocus()
    }

    function _refreshPreview() {
        renameDialog._statusFlash = false
        statusClear.stop()
        if (!renameDialog._alive(renameDialog._model)) {
            renameDialog._previewRows = []
            renameDialog._statusText = "playlist is gone; nothing to rename"
            return
        }
        renameDialog._previewRows = previewer.preview(
            renameDialog._model, renameDialog._keys, patternField.text)
        renameDialog._statusText = ""
    }

    // Sets the preset box text through BOTH channels. The contentItem's
    // declarative text binding is severed the first time the user types in
    // the field (standard QML binding-break on user input), after which
    // editText changes stop reaching the display; observed as the deleted
    // preset's name sticking in the box and selections not rendering. Every
    // programmatic change goes through here so the display can never desync.
    function _setPresetText(t) {
        presetCombo.editText = t
        presetEditor.text = t
    }

    function _flash(msg) {
        renameDialog._statusText = msg
        renameDialog._statusFlash = true
        statusClear.restart()
    }

    function _buildJobs() {
        var jobs = []
        for (var i = 0; i < renameDialog._previewRows.length; i++) {
            var r = renameDialog._previewRows[i]
            if (r.status === "ok")
                jobs.push({ path: r.path, newFileName: r.newName })
        }
        return jobs
    }

    property bool _closeAfterApply: false
    function _apply(closeAfter) {
        if (!renameDialog._canApply)
            return
        renameDialog._closeAfterApply = closeAfter
        renameDialog._applying = true
        renameDialog._statusText = "renaming..."
        renamer.apply(renameDialog._buildJobs())
    }

    // --- engines -------------------------------------------------------------
    RenamePreviewer { id: previewer }
    RenamePatternStore { id: presetStore }
    FileRenamer {
        id: renamer
        audio: audioController
        onApplied: function (okCount, failedPaths, renames) {
            // Bookkeeping first: every open tab's rows re-point at the
            // new paths; the tabs' autosaves persist them.
            playlistTabs.applyPathRenames(renames)
            // Remap our own snapshot: the old paths in _keys just stopped
            // existing. Without this every renamed row would preview "gone";
            // with it they preview "identity", the visible receipt.
            var keys = renameDialog._keys.slice()
            for (var i = 0; i < keys.length; i++) {
                var np = renames[keys[i].path]
                if (np !== undefined)
                    keys[i] = { path: np, subsong: keys[i].subsong }
            }
            renameDialog._keys = keys
            renameDialog._applying = false
            renameDialog._refreshPreview()
            if (failedPaths.length > 0) {
                renameDialog._statusText =
                    "renamed " + okCount + ", failed " + failedPaths.length
                        + " (see rows)"
                // A failed row keeps its old path/name and re-previews as
                // "ok" or "conflict_disk", pointing at the cause.
            } else {
                renameDialog._statusText = "renamed " + okCount + " file(s)"
                if (renameDialog._closeAfterApply)
                    renameDialog.close()
            }
        }
    }

    // Debounce for pattern edits (typing must not thrash disk checks).
    Timer {
        id: previewDebounce
        interval: 250
        repeat: false
        onTriggered: renameDialog._refreshPreview()
    }

    // Reverts a flash status back to the standing count line.
    Timer {
        id: statusClear
        interval: 10000
        repeat: false
        onTriggered: {
            renameDialog._statusText = ""
            renameDialog._statusFlash = false
        }
    }

    // Persist the size for the next instance, windowed frames only (the
    // visibility guard, same as the main window), then destruction as before.
    onClosing: {
        if (renameDialog.visibility === Window.Windowed)
            windowGeometry.saveSize("rename",
                                    renameDialog.width, renameDialog.height)
        renameDialog.destroy()
    }

    // ThemedMenu resolves its blur backdrop through the hosting window's
    // menuBlurSource (PropertiesWindow precedent, line-for-line): the edit
    // context menus below need it or they render over nothing.
    property Item menuBlurSource: windowBody

    // --- chrome --------------------------------------------------------------
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

            // ----- title bar: drag + close -----------------------------------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                MouseArea {
                    anchors.fill: parent
                    onPressed: renameDialog.startSystemMove()
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Rename Files"
                    color: Theme.textPrimary
                    font.family: renameDialog.uiFont
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }
                Button {
                    id: closeButtonX
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: !renameDialog._applying
                    implicitHeight: 22
                    implicitWidth: 22
                    padding: 1  // 20 x 20 content area (Basic/Fusion default is 6)

                    onClicked: renameDialog.close()

                    background: Rectangle {
                        color: closeButtonX.enabled && closeButtonX.hovered
                            ? Theme.dangerSurface : "transparent"
                        radius: 4
                    }

                    contentItem: AppIcon {
                        id: svgCloseButtonX
                        iconSize: 20
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

            // ----- pattern + presets -----------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Layout.topMargin: 4
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        text: "Pattern"
                        color: Theme.textPrimary
                        font.family: renameDialog.uiFont
                        font.pixelSize: 12
                    }
                    TextField {
                        id: patternField
                        Layout.fillWidth: true
                        font.family: Theme.monoFont
                        font.pixelSize: 12
                        placeholderText: "%track_no%-%artist%-%title%"
                        enabled: !renameDialog._applying
                        onTextChanged: previewDebounce.restart()
                        onAccepted: renameDialog._refreshPreview()
                        // Replace Qt's stock edit menu with the app-themed one
                        // (the stock one renders in platform style and clashes
                        // with every other rawform menu).
                        ContextMenu.menu: EditMenu { editor: patternField }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        text: "Preset"
                        color: Theme.textPrimary
                        font.family: renameDialog.uiFont
                        font.pixelSize: 12
                    }
                    ComboBox {
                        id: presetCombo
                        Layout.fillWidth: true
                        editable: true   // the edit text doubles as the save-as name
                        font.family: renameDialog.uiFont
                        font.pixelSize: 12
                        enabled: !renameDialog._applying
                        textRole: "name"
                        model: presetStore.catalog()
                        // Custom content item, three birds: selectByMouse ON
                        // declaratively (the Basic style ships its editor with
                        // it off, the drag-select asymmetry against the
                        // pattern field), the themed edit context menu, and no
                        // duck-typed onCompleted poke at style internals.
                        // ComboBox auto-syncs editText with a TextInput-based
                        // contentItem, so the one-way text binding here is the
                        // whole wiring. background null: the combo draws its
                        // own frame; the TextField's would double-border.
                        contentItem: TextField {
                            id: presetEditor
                            text: presetCombo.editable ? presetCombo.editText
                                                       : presetCombo.displayText
                            enabled: presetCombo.editable
                            selectByMouse: true
                            font: presetCombo.font
                            verticalAlignment: Text.AlignVCenter
                            background: null
                            ContextMenu.menu: EditMenu { editor: presetEditor }
                        }
                        // Selection must repaint through the helper too:
                        // with the text binding severed (see _setPresetText),
                        // activating an entry updates editText but not the
                        // display, so clicking another preset looked dead.
                        onActivated: function (index) {
                            renameDialog._setPresetText(presetCombo.textAt(index))
                        }
                        Connections {
                            target: presetStore
                            function onPatternsChanged() {
                                // Reassigning the model recreates the entries
                                // and stomps the edit text; preserve a typed
                                // name that still EXISTS (the save flow), and
                                // clear one that does not (the delete flow;
                                // restoring it left a ghost the combo could
                                // not select away from).
                                var typed = presetCombo.editText
                                presetCombo.model = presetStore.catalog()
                                var i = presetCombo.find(typed)
                                if (i >= 0) {
                                    presetCombo.currentIndex = i
                                    renameDialog._setPresetText(typed)
                                } else {
                                    presetCombo.currentIndex = -1
                                    renameDialog._setPresetText("")
                                }
                            }
                        }
                    }
                    FooterButton {
                        label: "Load"
                        enabled: !renameDialog._applying
                                 && presetCombo.editText.length > 0
                                 && presetStore.patternFor(presetCombo.editText).length > 0
                        onClicked: {
                            presetStore.setLastUsed(presetCombo.editText)
                            patternField.text =
                                presetStore.patternFor(presetCombo.editText)
                            renameDialog._refreshPreview()
                            previewDebounce.stop() // text set armed it; done
                            renameDialog._flash(
                                "Loaded preset '" + presetCombo.editText + "'")
                        }
                    }
                    FooterButton {
                        label: "Save"
                        enabled: !renameDialog._applying
                                 && presetCombo.editText.trim().length > 0
                                 && patternField.text.length > 0
                        onClicked: {
                            presetStore.save(presetCombo.editText,
                                             patternField.text)
                            presetStore.setLastUsed(presetCombo.editText)
                            renameDialog._flash(
                                "Saved preset '" + presetCombo.editText.trim() + "'")
                        }
                    }
                    FooterButton {
                        label: "Delete"
                        // Only a name that actually resolves to a preset can
                        // be deleted (same existence test Load uses).
                        enabled: !renameDialog._applying
                                 && presetCombo.editText.length > 0
                                 && presetStore.patternFor(presetCombo.editText).length > 0
                        onClicked: {
                            var name = presetCombo.editText
                            presetStore.remove(name)
                            // patternsChanged already cleared the box (name no
                            // longer resolves); this is the belt to that brace.
                            presetCombo.currentIndex = -1
                            renameDialog._setPresetText("")
                            renameDialog._flash("Deleted preset '" + name + "'")
                        }
                    }
                }
            }

            // ----- preview table ---------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 16
                color: Theme.surfaceControl
                radius: 5
                border.color: Theme.separator
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 1
                    spacing: 0

                    // header
                    Rectangle {
                        id: previewHeader
                        Layout.fillWidth: true
                        Layout.preferredHeight: 26
                        color: Theme.headerBand
                        Text {
                            x: 8
                            width: previewHeader.width * renameDialog._colSplit - 16
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Current name"
                            color: Theme.textFaint
                            font.family: renameDialog.uiFont
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Text {
                            x: previewHeader.width * renameDialog._colSplit + 8
                            width: previewHeader.width - x - 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "New name"
                            color: Theme.textFaint
                            font.family: renameDialog.uiFont
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        // The column divider + its drag handle. The visible
                        // line spans header and list (the list draws its own
                        // segment per row is overkill; one full-height line
                        // lives in the list container below the header).
                        MouseArea {
                            id: splitHandle
                            x: previewHeader.width * renameDialog._colSplit - 4
                            width: 8
                            height: parent.height
                            cursorShape: Qt.SplitHCursor
                            onPositionChanged: function (mouse) {
                                if (!pressed)
                                    return
                                var pos = (splitHandle.x + mouse.x) / previewHeader.width
                                renameDialog._colSplit =
                                    Math.max(0.15, Math.min(0.85, pos))
                            }
                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 1
                                height: parent.height
                                color: Theme.separator
                            }
                        }
                    }

                    ListView {
                        id: previewList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: renameDialog._previewRows
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}

                        delegate: Rectangle {
                            id: previewRow
                            required property var modelData
                            required property int index

                            readonly property bool blocking:
                                modelData.status === "invalid"
                                || modelData.status === "conflict_batch"
                                || modelData.status === "conflict_disk"
                            readonly property bool grayed:
                                modelData.status === "identity"
                                || modelData.status === "subsong"
                                || modelData.status === "duplicate_entry"
                                || modelData.status === "gone"

                            width: previewList.width
                            height: 24
                            color: index % 2 === 0 ? Theme.rowEven : "transparent"

                            Text {
                                x: 8
                                width: previewRow.width * renameDialog._colSplit - 16
                                anchors.verticalCenter: parent.verticalCenter
                                text: previewRow.modelData.oldName
                                color: previewRow.grayed ? Theme.textDisabled
                                                         : Theme.textPrimary
                                font.family: Theme.monoFont
                                font.pixelSize: 11
                                elide: Text.ElideMiddle
                            }
                            Text {
                                x: previewRow.width * renameDialog._colSplit + 8
                                width: previewRow.width - x - 8
                                anchors.verticalCenter: parent.verticalCenter
                                text: {
                                    var m = previewRow.modelData
                                    // By design: flagged rows show the REASON
                                    // alone; the left column already carries
                                    // identity, and repeating the candidate
                                    // name here just collided with the
                                    // message.
                                    if (m.status === "ok") return m.newName
                                    return m.note.length > 0 ? m.note : m.newName
                                }
                                color: previewRow.blocking ? Theme.danger
                                     : previewRow.grayed ? Theme.textDisabled
                                                         : Theme.textPrimary
                                font.family: Theme.monoFont
                                font.pixelSize: 11
                                font.italic: previewRow.modelData.status !== "ok"
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }
            }

            // ----- footer: status + Cancel / Apply / OK ----------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                color: Theme.headerBand

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    // Bounded against the button row: an unbounded left-anchored
                    // text ran underneath Cancel/Apply/OK exactly when it turned
                    // red (long conflict statuses), the worst possible moment.
                    anchors.right: footerButtons.left
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    elide: Text.ElideRight
                    text: renameDialog._statusText.length > 0
                        ? renameDialog._statusText
                        : renameDialog._okCount + " of "
                              + renameDialog._previewRows.length
                              + " will be renamed"
                    color: renameDialog._statusFlash ? Theme.textInactive
                         : renameDialog._hasBlocking ? Theme.danger : Theme.textFaint
                    font.family: renameDialog.uiFont
                    font.pixelSize: 11
                }
                RowLayout {
                    id: footerButtons
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    FooterButton {
                        label: "Cancel"
                        enabled: !renameDialog._applying
                        onClicked: renameDialog.close()
                    }
                    FooterButton {
                        label: "Apply"
                        enabled: renameDialog._canApply
                        onClicked: renameDialog._apply(false)
                    }
                    FooterButton {
                        label: "OK"
                        accent: true
                        enabled: renameDialog._canApply
                        onClicked: renameDialog._apply(true)
                    }
                }
            }
        }
    }

    // Resize edges, from the shared component (this dialog's private grips
    // were the first in the app and moved to WindowResizeGrips when the main
    // window and Properties adopted them). topInset keeps the top strip off
    // the title bar's move zone.
    WindowResizeGrips {
        target: renameDialog
        topInset: 40
    }

    // The app-themed replacement for Qt's stock text-edit context menu, shared
    // by the pattern field and the preset editor. Typed against TextField so
    // the enabled bindings (canUndo etc) are statically checked. Scoped to
    // this dialog; the other text fields (Properties, Settings, Custom
    // Columns) keep Qt's stock edit menu.
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
            // Disabled accent buttons kept textOnAccent over the disabled
            // gray face, which vanished (the invisible-OK bug); the disabled
            // state now overrides the accent text color.
            color: !fbtn.enabled ? Theme.textDisabled
                 : fbtn.accent ? Theme.textOnAccent : Theme.textPrimary
            font.family: renameDialog.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
        }
        HoverHandler { id: fbtnHover }
        TapHandler { onTapped: if (fbtn.enabled) fbtn.clicked() }
    }
}
