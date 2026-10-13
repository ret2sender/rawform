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
// saves) -> applied(ok, failedPaths, renames) -> PlaylistTabs.applyPathRenames
// patches EVERY open tab's rows in place (paths persist via each tab's normal
// autosave) -> this dialog remaps its own snapshot keys through the same map
// (the old paths just stopped existing; without the remap every row would
// preview as "gone") -> re-preview, where successes now read "identity",
// which doubles as the visible receipt that the rename landed.
//
// The captured model belongs to the owning tab, which can close under this
// non-modal window; it is a typed object property, which reads back null once
// the model is destroyed, and every touch tests for that. A dead model
// degrades the dialog to a static view whose Apply is disabled (jobs need a
// live preview).

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: renameDialog

    property string uiFont: Theme.uiFont

    title: "Rename Files"
    // Size restores from window.yaml's keyed store, else the built-in
    // default; clamped against the minimums here because the store does not
    // know them. Initial values, not live bindings (startup-only read).
    // Position is NOT restored: the host cascades each instance.
    width: Math.max(minimumWidth, WindowGeometryStore.savedWidth("rename", 720))
    height: Math.max(minimumHeight, WindowGeometryStore.savedHeight("rename", 480))
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
    // accepted, since that is the standard dialog behavior.
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

    // --- captured world (openFor) + live state -----------------------------
    property PlaylistModel _model: null  // owning tab's model; null once it dies
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
            var pat = presetRow.selectName(last)
            if (pat.length > 0)
                patternField.text = pat
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
        if (renameDialog._model === null) {
            renameDialog._previewRows = []
            renameDialog._statusText = "playlist is gone; nothing to rename"
            return
        }
        renameDialog._previewRows = previewer.preview(
            renameDialog._model, renameDialog._keys, patternField.text)
        renameDialog._statusText = ""
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

    // --- engines -----------------------------------------------------------
    RenamePreviewer { id: previewer }
    RenamePatternStore { id: presetStore }
    FileRenamer {
        id: renamer
        audio: AudioController
        onApplied: function (okCount, failedPaths, renames) {
            // Bookkeeping first: every open tab's rows re-point at the
            // new paths; the tabs' autosaves persist them.
            PlaylistTabs.applyPathRenames(renames)
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
            WindowGeometryStore.saveSize("rename",
                                    renameDialog.width, renameDialog.height)
        renameDialog.destroy()
        // Hand the freed heap pages back once the deferred delete has run
        // (see HeapTrim.h).
        HeapTrim.trimSoon()
    }

    // ThemedMenu resolves its blur backdrop through the hosting window's
    // menuBlurSource (PropertiesWindow precedent, line-for-line): the edit
    // context menus below need it or they render over nothing.
    property Item menuBlurSource: windowBody

    // --- chrome ------------------------------------------------------------
    Rectangle {
        id: windowBody
        anchors.fill: parent
        color: Theme.surfacePage
        radius: 8
        border.color: Theme.separatorStrong
        border.width: 1

        // The default button: an Enter that no field or control claimed on
        // the way up activates the accent button (OK), when it is enabled.
        Keys.onReturnPressed: function (event) { event.accepted = okButton.activate() }
        Keys.onEnterPressed: function (event) { event.accepted = okButton.activate() }

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
                    onPressed: renameDialog.startSystemMove()
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Rename Files"
                    color: Theme.textPrimary
                    font.family: renameDialog.uiFont
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Font.Bold
                }

                ToolDialogCloseButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: !renameDialog._applying
                    onClicked: renameDialog.close()
                }
            }

            // ----- pattern + presets ---------------------------------------
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
                        font.pixelSize: Theme.fontSizeBody
                    }
                    TextField {
                        id: patternField
                        Layout.fillWidth: true
                        font.family: Theme.monoFont
                        font.pixelSize: Theme.fontSizeBody
                        placeholderText: "%track_no%-%artist%-%title%"
                        enabled: !renameDialog._applying

                        background: Rectangle {
                            radius: 4
                            color: Theme.rowEven
                        }

                        onTextChanged: previewDebounce.restart()
                        onAccepted: renameDialog._refreshPreview()
                        // Replace Qt's stock edit menu with the app-themed one
                        // (the stock one renders in platform style and clashes
                        // with every other rawform menu).
                        ContextMenu.menu: ThemedEditMenu { editor: patternField }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text {
                        text: "Preset "
                        color: Theme.textPrimary
                        font.family: renameDialog.uiFont
                        font.pixelSize: Theme.fontSizeBody
                    }
                    PresetRow {
                        id: presetRow
                        Layout.fillWidth: true
                        store: presetStore
                        pattern: patternField.text
                        busy: renameDialog._applying
                        onLoaded: function (pattern) {
                            patternField.text = pattern
                            renameDialog._refreshPreview()
                            previewDebounce.stop() // text set armed it; done
                        }
                        onFlash: function (message) { renameDialog._flash(message) }
                    }
                }
            }

            // ----- preview table -------------------------------------------
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
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        Text {
                            x: previewHeader.width * renameDialog._colSplit + 8
                            width: previewHeader.width - x - 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: "New name"
                            color: Theme.textFaint
                            font.family: renameDialog.uiFont
                            font.pixelSize: Theme.fontSizeSmall
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
                                font.pixelSize: Theme.fontSizeSmall
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
                                font.pixelSize: Theme.fontSizeSmall
                                font.italic: previewRow.modelData.status !== "ok"
                                elide: Text.ElideMiddle
                            }
                        }
                    }
                }
            }

            // ----- footer: status + Cancel / Apply / OK --------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                bottomLeftRadius: 8
                bottomRightRadius: 8
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
                    font.pixelSize: Theme.fontSizeSmall
                }
                RowLayout {
                    id: footerButtons
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    ToolDialogButton {
                        label: "Cancel"
                        enabled: !renameDialog._applying
                        onClicked: renameDialog.close()
                    }
                    ToolDialogButton {
                        label: "Apply"
                        enabled: renameDialog._canApply
                        onClicked: renameDialog._apply(false)
                    }
                    ToolDialogButton {
                        id: okButton
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
}
