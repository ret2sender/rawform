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

// PropertiesWindow.qml
//
// The track Properties window. Frameless, NON-MODAL,
// our chrome, MULTIPLE INSTANCES: PlaylistView instantiates a fresh window per
// "Properties" invocation and each instance destroys itself
// on close. There is no hide/reuse and therefore no stale-backing-store reopen
// flash to fight: a new instance has never painted anything. All close paths
// (Cancel, the titlebar X, OK on success) funnel through Window.close(); the
// onClosing handler destroys the instance, taking its per-window editors,
// models, and scan controller with it.
//
// The window snapshots the selection on open and stages edits in the panes;
// Apply/OK commit through MetadataEditor and ReplayGainEditor (off-thread
// TagLib writes), then the OWNING TAB's MetadataReloader (captured at
// creation, deliberately not a live binding: the active tab can change under
// a non-modal window) force-reloads the written files so the model, metadata
// pane, and (via the controller's dataRevision subscription) the playing
// track's RG factor all refresh, and the pane re-pulls fresh values. That
// whole sequence, the captured selection, the staging flags and the window
// status live in PropertiesApplyController (see its banner for the phases,
// the token and the liveness rules); this file keeps the widgets and the
// three things only they can do: commit or discard an open cell editor
// around a pass, close the window on a finished OK, and flush the playlist
// cache after a write.
//
// Cross-window semantics (multiple windows over the same tracks): each window
// snapshots at open and there is no cross-window sync; last write wins,
// by design. collectEdits diffs against the open-time baseline, so a
// stale window only rewrites fields it actually staged; two windows clobber
// each other only when both staged the same field.
//
//   OK     : apply, then close (stays open if any file failed to write)
//   Apply  : apply, stay open
//   Cancel : discard staged edits, close
//
// The controller's `busy` gates the footer AND the titlebar X so a pass
// cannot be double-fired and the instance cannot be destroyed mid-write from
// the UI (the editors' destructors wait out an in-flight pass as the backstop
// for OS-initiated closes).
//
// Scanning: the pane's ReplayGain right-click menu also GENERATES values. Its
// scanRequested signal drives ReplayGainScanController off-thread (a progress
// overlay covers the window meanwhile, with Cancel); on completion the measured
// values are staged back into the pane via stageScanResults, exactly like a typed
// edit, so the user reviews them and commits through the same Apply / OK path. The
// scan never writes on its own. A canceled scan stages nothing.
//
// Tools menu (footer left): window-scoped ReplayGain and file actions. The scan / clear
// entries reuse the pane's staging machinery but act over EVERY row (requestScanAll /
// clearAllReplayGain), ignoring the pane's row sub-selection; the pane's own right-click
// menu stays selection-scoped. "Reload info" (the controller's reloadInfo)
// force-re-reads the selection's files from disk and re-pulls all three
// tabs, DISCARDING staged edits by design.
//
// Messages: ONE surface, the PropertiesLogBar between the tab strip and the
// tab content. Everything transient the window or its panes produce is composed
// into it by priority (see the bar's binding): a menu hover hint (the Tools
// menu, or the visible pane's right-click menu) while a menu is open, else
// the busy label while a pass runs, else the visible pane's own error, else
// the window status (the controller's statusText / statusLevel).
// The panes render no messages of their own; they expose menuHint / hintText
// properties and this window reads them. Sitting above the panes, the bar is
// out of every popup's path (the Tools menu opens upward from the footer, the
// panes' context menus downward from the cursor).
//
// Menu layout: metadata actions first (Add new field, Auto track number,
// Remove tags), then a ReplayGain submenu holding all five RG entries, then
// Reload info. Remove tags is STAGED like everything else in this window
// (the controller's stageStripAll); Apply / OK then hands the selection's
// file paths to MetadataEditor as stripAllPaths, whose write pass
// structurally strips MP3 tag blocks (deliberately overriding the ID3v1 /
// APE Preserve settings) and, on other formats, replaces the whole property
// map and clears unsupported and complex properties (embedded pictures
// included).

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic  // MenuSeparator in the Tools menu
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: propertiesWindow

    // UI family for this window's own labels; children resolve Theme on
    // their own, nothing is forwarded. Kept as a property purely as an
    // override surface.
    property string uiFont: Theme.uiFont

    // The owning tab's MetadataReloader, passed by the host as an initial value
    // at createObject time. A VALUE capture on purpose, never a live binding:
    // the host's root.reloader tracks the ACTIVE tab, and a tab switch under
    // this non-modal window must not silently retarget the post-apply reload.
    // Handed to the controller once, in openFor.
    property MetadataReloader reloader: null

    property Item menuBlurSource: windowBody
    property int currentTab: 0   // Metadata is the default tab

    // Hover hint of the highlighted Tools menu item, shown in the log bar
    // while non-empty; the menu clears it on close.
    property string toolsHint: ""

    // Edit Value dialog (multi-track editing) overlay visibility.
    property bool _editDialogActive: false

    title: "Properties"
    // Size restores from window.yaml's keyed store, else the built-in
    // default; clamped against the minimums here because the store does not
    // know them. Initial values, not live bindings: invokable calls are not
    // tracked as dependencies, which is exactly right for a startup-only
    // read. Position is NOT restored: the host cascades each instance.
    width: Math.max(minimumWidth, WindowGeometryStore.savedWidth("properties", 720))
    height: Math.max(minimumHeight, WindowGeometryStore.savedHeight("properties", 560))
    minimumWidth: 560
    minimumHeight: 360
    color: "transparent"
    // Qt.Tool, not Qt.Window: a tool window floats above its transient parent
    // (the main window, resolved automatically from the QML visual parent), so
    // clicking the playlist never buries an open Properties window; owned
    // windows behave this way on Windows, and macOS does not give plain
    // sibling windows that relationship. The standard utility-window
    // trade-off applies: on macOS the tool windows hide while the app is
    // deactivated and reappear with it (arguably the desired citizenship), and
    // they do not appear in the Dock / cmd-tab (moot: frameless anyway).
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal

    // Fresh instance per open: closing IS destruction, so there is no deferred hide, no
    // transparent close paint, and no reopen-flash dance to manage: a window that is
    // never re-shown has no stale backing store to flash. destroy() defers actual
    // deletion to the event loop, so finishing this handler (and anything else on the
    // current stack) is safe. The per-window editors' C++ destructors wait out an
    // in-flight write pass, so even an OS-initiated close mid-apply cannot strand a pool
    // worker (it just blocks the GUI for the tail of the write).
    onClosing: {
        // Persist the size for the next instance, windowed frames only
        // (the visibility guard, same as the main window). Several instances closing
        // just means last write wins. Then destruction, as before.
        if (propertiesWindow.visibility === Window.Windowed)
            WindowGeometryStore.saveSize("properties",
                                    propertiesWindow.width,
                                    propertiesWindow.height)
        propertiesWindow.destroy()
        // Hand the freed heap pages back once the deferred delete has run;
        // this window is the heaviest tool window to tear down (see HeapTrim.h).
        HeapTrim.trimSoon()
    }

    function _closeWindow() {
        propertiesWindow.close() // -> onClosing -> destroy
    }

    // Open the Edit Value dialog for a field row: any multi-track edit, the
    // pane's explicit single-track "Edit" action, or the multiline routing
    // from an edit-in-place entry point. Feeds the dialog from the model,
    // then reveals the overlay; for a single track the field is trivially
    // uniform, so the dialog opens on Single Value.
    function _openEditDialog(row) {
        editDialog.addMode = false
        editDialog.keyError = ""
        editDialog.fieldLabel = metaModel.fieldLabel(row)
        editDialog.fieldKey = metaModel.fieldKey(row)
        editDialog.validatorPattern = metaModel.editValidatorPattern(row)
        editDialog.singleSeed = metaModel.sharedEditValue(row)
        editDialog.startUniform = metaModel.fieldUniform(row)
        editDialog.perTrackValues = metaModel.perTrackEditValues(row)
        editDialog.trackLabels = metaModel.trackLabels()
        editDialog.reopen()
        propertiesWindow._editDialogActive = true
    }

    // Open the dialog in add mode for the "+ add new" row: an editable, empty name
    // box and an empty Single Value tab over the whole selection.
    function _openAddDialog() {
        editDialog.addMode = true
        editDialog.keyError = ""
        editDialog.fieldLabel = ""
        editDialog.fieldKey = ""
        editDialog.validatorPattern = "^.*$"   // custom keys are free-text list fields
        editDialog.singleSeed = ""
        editDialog.startUniform = true
        editDialog.perTrackValues = metaModel.blankPerTrack()
        editDialog.trackLabels = metaModel.trackLabels()
        editDialog.reopen()
        propertiesWindow._editDialogActive = true
    }

    // Feed the fresh instance its selection and show it. Called by the host
    // exactly once, right after createObject; the defaults of a new instance
    // cover everything a reused window would have to reset by hand (tab,
    // staging, status, apply bookkeeping, half-open editors). The controller
    // is wired here, with the reloader the host set at creation, and its
    // setPlaylist takes the identity snapshot and fills the three models.
    function openFor(model, rows) {
        if (!model || !rows || rows.length === 0) {
            propertiesWindow.destroy() // never shown; do not leak the instance
            return
        }
        applyController.setModels(metaModel, rgModel, detailsModel)
        applyController.setEditors(metaEditor, rgEditor)
        applyController.setReloader(propertiesWindow.reloader)
        applyController.setPlaylist(model, rows.slice())
        show()
        raise()
        requestActivate()
        propertiesWindow._focusCurrentTab() // Seed keyboard focus
    }

    // Escape clears the visible pane's row selection, through the Shortcut phase because
    // Keys-based Escape delivery was observed dead on macOS (the panes' Keys branches
    // remain as fallback; shortcuts match first where they do fire, so the action never
    // runs twice). The enabled gate hands Escape back to everything that owns it more
    // locally: the Edit Value / Add Field overlay (its root cancels on Escape), the
    // metadata inline editor (editingRow), a ReplayGain cell editor (keysFocused is false
    // while one holds focus), and the apply pass. Window context (the default), so each
    // Properties instance carries its own map. Context alone does NOT prevent ambiguity,
    // though: a still-enabled shortcut in a hidden or unfocused window keeps
    // participating in Qt's shortcut map, and two enabled matches on one press fire
    // NEITHER onActivated (Qt rotates activatedAmbiguously instead). Hence the
    // strict-focus terms below. NOT Window.active: that is QWindow::isActive(), a
    // transient-GROUP activation, so every visible secondary window reports active
    // together and that gate left two-open-window presses ambiguous (app-wide dead
    // Escape). WindowFocus.focusWindow is QGuiApplication::focusWindow(), singular by
    // definition: at most one secondary-window Escape shortcut is grabbed at any instant,
    // so several open Properties instances can never go ambiguous with each other, with
    // the reused Settings / About / Custom Columns instances, or with the main window. A
    // window that can receive the Escape key IS the focus window, so the gate never
    // starves a legitimate press.
    //
    // The action is two-stage. A visible selection is cleared first; with nothing to
    // clear, Escape CLOSES the window through _cancel(), byte-identical to the Cancel
    // button (staged edits die with the instance). The probes are the exact complements
    // of the clear actions: hasRowSelection() is true iff clearRowSelection() would
    // change something, hasSelection likewise for clearSelection(), so the two stages can
    // never both fire on one press and the second press always closes. The gate includes
    // the Details tab (currentTab 1), which has no selection concept: it closes on the
    // first press.
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: propertiesWindow.visible
                 && WindowFocus.focusWindow === propertiesWindow
                 && !applyController.busy
                 && !propertiesWindow._editDialogActive
                 && ((propertiesWindow.currentTab === 0
                      && metaPane.editingRow < 0)
                     || propertiesWindow.currentTab === 1
                     || (propertiesWindow.currentTab === 2
                         && rgPane.keysFocused))
        onActivated: {
            if (propertiesWindow.currentTab === 0) {
                if (metaPane.hasRowSelection())
                    metaPane.clearRowSelection()
                else
                    propertiesWindow._cancel()
            } else if (propertiesWindow.currentTab === 2) {
                if (rgModel.hasSelection)
                    rgPane.clearSelection()
                else
                    propertiesWindow._cancel()
            } else {
                propertiesWindow._cancel()
            }
        }
    }

    // Switch tabs, first closing any editor open on the tab being left so edit mode
    // never persists across a tab change. Both discard the in-progress edit (like
    // Cancel): Metadata clears its inline editor, ReplayGain hides its cell editor.
    // The keyboard focus moves WITH the tab: without the handoff the leaving
    // tab's focus item kept receiving shortcuts, so Ctrl+A pressed on the visible
    // tab select-all'd the hidden one.
    function _switchTab(idx) {
        if (idx === propertiesWindow.currentTab)
            return
        if (propertiesWindow.currentTab === 0)
            metaPane.closeEdit()
        else if (propertiesWindow.currentTab === 2)
            rgPane.closeAnyEditor()
        propertiesWindow.currentTab = idx
        propertiesWindow._focusCurrentTab()
    }

    // Park keyboard focus on the current tab's key surface; the Details tab
    // has no shortcuts of its own, but taking focus there is exactly the point,
    // it keeps the hidden panes' handlers out of the delivery path.
    function _focusCurrentTab() {
        if (propertiesWindow.currentTab === 0)
            metaPane.focusKeys()
        else if (propertiesWindow.currentTab === 2)
            rgPane.focusKeys()
        else
            detailsView.forceActiveFocus()
    }

    // Tools > Remove tags: stage the whole removal for review. Discards any
    // open cell editor first (the row set may shift as session-only customs
    // drop), then the controller blanks every metadata field row, stages RG
    // clears across all rows and raises the strip flag. Nothing touches disk
    // until Apply / OK; Cancel closes the window with it, Reload info
    // discards it.
    function _removeAllTags() {
        metaPane.cancelEdit()
        rgPane.closeAnyEditor()
        applyController.stageStripAll()
    }

    // Apply / OK: stage an edit the user left open in the cell, then hand
    // the pass to the controller; finished(true) closes the window.
    function _apply(closeAfter) {
        if (applyController.busy)
            return
        metaPane.commitOpenEditor()
        applyController.apply(closeAfter)
    }

    function _cancel() {
        // No staging to revert on the way out: the instance dies with its
        // staged edits, its models, and its editors.
        propertiesWindow._closeWindow()
    }

    // Tools "Reload info": the controller's reload pass, which locks the
    // footer, titlebar X and panes like an Apply pass. A running scan holds
    // it off; reloadStarting() below discards the half-open cell editors.
    function _reloadInfo() {
        if (scanController.busy)
            return
        applyController.reloadInfo()
    }

    // Both write editors get the playback link so apply() can stop the
    // engine before rewriting the loaded track's file (the tag-write interlock;
    // see AudioController::stopIfPlayingAny). AudioController is a singleton,
    // reachable from any window.
    MetadataEditor { id: metaEditor; audio: AudioController }
    ReplayGainEditor { id: rgEditor; audio: AudioController }

    // Backing models for the two read-only tabs. Both are fed in openFor and copy
    // their TrackData, so they survive selection changes while the window is open.
    //  - metaModel:    the editable tag schema (the base model renders it read-only).
    //  - detailsModel: a MetadataModel in Details mode (Location + General only,
    //                  no "Metadata" section and no "Items Selected" row, since the
    //                  window title already carries the count).
    PropertiesMetadataModel { id: metaModel }
    MetadataModel { id: detailsModel; detailsMode: true }
    // The RG tab's backing model: rows, selection, staging, scan scope,
    // and the Summary in C++. Fed in openFor and on every re-pull, exactly
    // like the two above; the pane only renders it and forwards gestures.
    ReplayGainRowsModel { id: rgModel }

    // Off-thread RG scanner. Driven by the pane's scanRequested; on completion it
    // stages the measured values back into the pane (so they commit through the
    // normal Apply / OK), or, if canceled, stages nothing. Failures and the noop
    // case surface in the log bar.
    ReplayGainScanController { id: scanController }

    Connections {
        target: scanController
        function onFinished(results, canceled, failedPaths) {
            if (canceled)
                return  // discard the whole batch; an album needs every track
            if (results.length > 0)
                rgModel.stageScanResults(results)
            if (failedPaths.length > 0)
                applyController.setStatus(
                    failedPaths.length + " file(s) could not be scanned.", "error")
        }
    }

    // The apply and reload sequencer (see its banner). The window answers
    // its three signals with what only the window can do.
    PropertiesApplyController {
        id: applyController
        onFinished: function (closeRequested) {
            if (closeRequested)
                propertiesWindow._closeWindow()
        }
        // The data underneath is about to be replaced: discard any half-open
        // cell editor first, like a tab switch does.
        onReloadStarting: {
            metaPane.closeEdit()
            rgPane.closeAnyEditor()
        }
        // A write marks the playlist tab dirty (dataChanged -> markDirty ->
        // debounced autosave). Force that write now so the .rwfpl cache
        // reflects the edit immediately, not just on the debounce.
        onFilesWritten: PlaylistTabs.flushPendingWrites()
    }

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

            // ----- title bar -----------------------------------------------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                MouseArea {
                    anchors.fill: parent
                    onPressed: propertiesWindow.startSystemMove()
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        var n = applyController.rowCount
                        return n > 0 ? ("Properties - " + n + (n === 1 ? " item" : " items"))
                                     : "Properties"
                    }
                    color: Theme.textPrimary
                    font.family: propertiesWindow.uiFont
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Font.Bold
                }

                ToolDialogCloseButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: !applyController.busy
                    onClicked: propertiesWindow._cancel()
                }
            }

            // ----- tab strip -----------------------------------------------
            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                Layout.leftMargin: 12
                Layout.rightMargin: 12

                // The tab strip. Each tab is a button, so the strip is in the
                // Tab chain and reads as page tabs to assistive technology;
                // Left/Right move the current tab and the focus together.
                Row {
                    id: tabStrip
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    spacing: 4
                    Accessible.role: Accessible.PageTabList

                    // Switch from the keyboard: the focus ring follows the tab.
                    function switchFromKey(index) {
                        var next = Math.max(0, Math.min(tabs.count - 1, index))
                        propertiesWindow._switchTab(next)
                        var item = tabs.itemAt(next)
                        if (item)
                            item.forceActiveFocus(Qt.TabFocusReason)
                    }

                    Repeater {
                        id: tabs
                        model: ["Metadata", "Details", "ReplayGain"]
                        delegate: AbstractButton {
                            id: tab
                            required property int index
                            required property string modelData
                            readonly property bool current:
                                propertiesWindow.currentTab === tab.index
                            width: tabLabel.implicitWidth + 28
                            height: 28
                            padding: 0
                            onClicked: propertiesWindow._switchTab(tab.index)

                            Accessible.role: Accessible.PageTab
                            Accessible.name: tab.modelData
                            Accessible.selected: tab.current

                            Keys.onLeftPressed: function (event) {
                                tabStrip.switchFromKey(tab.index - 1)
                                event.accepted = true
                            }
                            Keys.onRightPressed: function (event) {
                                tabStrip.switchFromKey(tab.index + 1)
                                event.accepted = true
                            }

                            background: Rectangle {
                                radius: 2
                                color: tab.current ? Theme.surfaceSelected
                                                   : Theme.surfaceTabIdle
                                border.color: Theme.textPrimary
                                border.width: tab.visualFocus ? 1 : 0
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: 2
                                    color: tab.current ? Theme.accent : "transparent"
                                }
                            }
                            contentItem: Text {
                                id: tabLabel
                                text: tab.modelData
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                                color: tab.current ? Theme.textPrimary : Theme.textDim
                                font.family: propertiesWindow.uiFont
                                font.pixelSize: Theme.fontSizeBody
                            }
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.separatorStrong }

            // ----- log bar: the window's one message surface ---------------
            // Above the panes, where no popup reaches it (see the component's
            // banner). Aligned with the panes: 14 px sides, 10 px under the
            // separator, and the panes' own 10 px top margin is the gap below.
            // Priority, top to bottom: a menu hover hint (Tools, or the
            // VISIBLE pane's right-click menu), the busy label, the visible
            // pane's own error, the window status. The pane hints clear
            // themselves on menu close and the busy label is gated by the
            // controller's busy, so each tenant yields as soon as it is
            // stale; only the window status persists until the next
            // setStatus. Hints and the busy label read as info; the status
            // carries its own level.
            PropertiesLogBar {
                Layout.fillWidth: true
                Layout.leftMargin: 14
                Layout.rightMargin: 14
                Layout.topMargin: 10
                readonly property string _paneHint:
                    propertiesWindow.currentTab === 0 ? metaPane.menuHint
                    : propertiesWindow.currentTab === 2 ? rgPane.menuHint
                    : ""
                readonly property string _paneError:
                    propertiesWindow.currentTab === 0 ? metaPane.hintText : ""
                text: propertiesWindow.toolsHint.length > 0 ? propertiesWindow.toolsHint
                    : _paneHint.length > 0 ? _paneHint
                    : applyController.busy ? applyController.busyText
                    : _paneError.length > 0 ? _paneError
                    : applyController.statusText
                level: (propertiesWindow.toolsHint.length > 0
                        || _paneHint.length > 0
                        || applyController.busy) ? "info"
                     : _paneError.length > 0 ? "error"
                     : applyController.statusLevel
            }

            // ----- tab content ---------------------------------------------
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                // Tab 0: editable tag schema (read-only at the base-model level).
                MetadataPropertiesPane {
                    id: metaPane
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    anchors.topMargin: 10
                    anchors.bottomMargin: 6
                    visible: propertiesWindow.currentTab === 0
                    enabled: !applyController.busy
                    model: metaModel
                    onEditFieldRequested: function (row) { propertiesWindow._openEditDialog(row) }
                    onAddFieldRequested: function () { propertiesWindow._openAddDialog() }
                }

                // Tab 1: read-only Location + General details, rendered by the same
                // Name/Value view the dock uses, in Details mode. Its font comes
                // from the Theme singleton by default, which resolves here too.
                // The id exists for the focus handoff (_focusCurrentTab).
                MetadataView {
                    id: detailsView
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    anchors.topMargin: 10
                    anchors.bottomMargin: 6
                    visible: propertiesWindow.currentTab === 1
                    model: detailsModel
                }

                ReplayGainPropertiesPane {
                    id: rgPane
                    anchors.fill: parent
                    anchors.leftMargin: 14
                    anchors.rightMargin: 14
                    anchors.topMargin: 10
                    anchors.bottomMargin: 6
                    visible: propertiesWindow.currentTab === 2
                    enabled: !applyController.busy && !scanController.busy
                    controller: AudioController
                    model: rgModel

                    onScanRequested: function (items, mode) {
                        applyController.setStatus("")
                        scanController.scan(items, mode)
                    }
                    onScanNoop: function (message) {
                        applyController.setStatus(message, "warning")
                    }
                }
            }

            // ----- footer: Tools + Apply / OK / Cancel ---------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 52
                bottomLeftRadius: 8
                bottomRightRadius: 8
                color: Theme.headerBand

                ToolDialogButton {
                    id: toolsBtn
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    label: "Tools"
                    enabled: !applyController.busy && !scanController.busy
                    onClicked: toolsMenu.open()

                    // The window-scoped Tools menu (footer left). Parented to
                    // the button and opened UPWARD via the
                    // y: -height binding, which settles once the popup sizes
                    // itself. Menu layout: metadata actions first, then the
                    // ReplayGain submenu (all five RG entries), then Reload
                    // info. Scan / clear act over EVERY row through the pane's
                    // all-rows entry points; results stage exactly like the
                    // pane's own right-click menu and commit through the normal
                    // Apply / OK path. Hover hints feed the log bar through
                    // toolsHint and clear when the menu closes; the
                    // auto-generated submenu TITLE item carries no hint (the
                    // parent menu's delegate creates it, so there is no
                    // per-item hint hook), only its children do.
                    ThemedMenu {
                        id: toolsMenu
                        x: 0
                        y: -height - 6
                        onClosed: propertiesWindow.toolsHint = ""

                        ThemedMenuItem {
                            text: "Add new field"
                            property string hint: "Add a new metadata field"
                            onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                            onTriggered: propertiesWindow._openAddDialog()
                        }
                        ThemedMenuItem {
                            text: "Auto track number"
                            property string hint: "Number the tracks 1..N and set the total"
                            onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                            onTriggered: {
                                metaPane.commitOpenEditor()
                                metaModel.autoNumberTracks()
                            }
                        }
                        ThemedMenuItem {
                            text: "Remove tags"
                            property string hint: "Discard all metadata and strip known tag types; written on Apply / OK"
                            onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                            onTriggered: propertiesWindow._removeAllTags()
                        }
                        MenuSeparator {}
                        ThemedMenu {
                            title: "ReplayGain"

                            ThemedMenuItem {
                                text: "Clear ReplayGain information"
                                property string hint: "Stage empty ReplayGain values on every track"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                onTriggered: rgModel.clearAllReplayGain()
                            }
                            ThemedMenuItem {
                                text: "Scan track gain"
                                property string hint: "Measure every track's gain and peak"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                onTriggered: rgPane.requestScanAll(0)
                            }
                            ThemedMenuItem {
                                text: "Scan album gain (as one album)"
                                property string hint: "Measure all tracks as one album"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                onTriggered: rgPane.requestScanAll(1)
                            }
                            ThemedMenuItem {
                                text: "Scan album gain (multiple albums, by tags)"
                                property string hint: "Group by album artist / date / album tags; measure each album separately"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                onTriggered: rgPane.requestScanAll(2)
                            }
                            MenuSeparator {}
                            ThemedMenuItem {
                                id: skipExistingItem
                                text: "Skip scanning tracks/albums with info present"
                                checkable: true
                                property string hint: "When set, scans skip tracks (or whole albums) that already carry values"
                                onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                                // Clicking a checkable item auto-toggles `checked`,
                                // which would break a plain binding to the setting;
                                // the Binding element re-asserts the settings-backed
                                // truth whenever it changes, and the trigger just
                                // pushes the freshly toggled state into it.
                                onTriggered: AudioController.replayGainScanSkipExisting =
                                                 skipExistingItem.checked
                                Binding {
                                    target: skipExistingItem
                                    property: "checked"
                                    value: AudioController.replayGainScanSkipExisting
                                }
                            }
                        }
                        MenuSeparator {}
                        ThemedMenuItem {
                            text: "Reload info"
                            property string hint: "Re-read tags from disk, discarding any staged edits"
                            onHoveredChanged: if (hovered) propertiesWindow.toolsHint = hint
                            onTriggered: propertiesWindow._reloadInfo()
                        }
                    }
                }

                RowLayout {
                    id: footerButtons
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    ToolDialogButton {
                        label: "Cancel"
                        enabled: !applyController.busy
                        onClicked: propertiesWindow._cancel()
                    }
                    ToolDialogButton {
                        label: "Apply"
                        // The strip flag is an enabler in its own right:
                        // Remove tags on already-blank fields stages nothing in
                        // either model, yet the strip still has work to do.
                        enabled: (metaModel.dirty || rgModel.dirty
                                  || applyController.stripStaged)
                                 && !applyController.busy
                        onClicked: propertiesWindow._apply(false)
                    }
                    ToolDialogButton {
                        id: okButton
                        label: "OK"
                        accent: true
                        enabled: !applyController.busy
                        onClicked: propertiesWindow._apply(true)
                    }
                }
            }
        }

        // Scan progress overlay: dims and blocks the window body while a scan
        // runs. Bound to the controller's busy flag, so it shows and hides itself.
        ReplayGainScanProgress {
            anchors.fill: parent
            controller: scanController
        }

        // Edit Value dialog overlay (multi-track editing): dims and blocks the body
        // and centers the dialog. Hosted in-window rather than as a second OS window
        // so its shortcuts and focus stay scoped to THIS instance even when several
        // Properties windows are open.
        Item {
            anchors.fill: parent
            visible: propertiesWindow._editDialogActive
            z: 50

            Rectangle {
                anchors.fill: parent
                color: "#000000"
                opacity: 0.5
                MouseArea { anchors.fill: parent } // swallow clicks to the body behind
            }

            EditValueDialog {
                id: editDialog
                anchors.centerIn: parent
                onCommitted: function (perTrackValues) {
                    metaModel.stageFieldValues(editDialog.fieldKey, perTrackValues)
                    propertiesWindow._editDialogActive = false
                }
                onAddRequested: function (key, perTrackValues) {
                    var err = metaModel.addCustomFieldValues(key, perTrackValues)
                    if (err !== "")
                        editDialog.keyError = err   // stay open, show the inline reason
                    else
                        propertiesWindow._editDialogActive = false
                }
                onCanceled: propertiesWindow._editDialogActive = false
            }
        }
    }

    // Resize edges: this window is frameless on every platform, so the grips
    // are unconditional. Declared after windowBody so they sit above the scan
    // and Edit Value overlays; resizing during a write or scan is harmless
    // (both overlays keep blocking the body), so no busy gate. topInset
    // keeps the top strip off the title bar's move zone.
    WindowResizeGrips {
        target: propertiesWindow
        topInset: 40
    }
}
