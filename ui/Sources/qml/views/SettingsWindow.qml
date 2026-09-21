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

// SettingsWindow.qml
//
// The application Settings window, opened from Edit > Settings. A non-modal, frameless
// window that reuses the main window's chrome: transparent Window, rounded
// Theme.surfacePage body at radius 8, a slim custom title bar with window-drag and a
// close button. Text Theme.textPrimary, accent Theme.accentSoft, panels
// Theme.surfaceSunken. Resizable through WindowResizeGrips, the same edge strips
// the other tool windows carry; the size persists in window.yaml under the
// `settings` key (size only, never position: the reused instance keeps whatever
// spot the window system last gave it).
//
// STRUCTURE: a left navigation pane and a right content area, over an Apply / OK / Cancel
// footer. The nav is a list of { label, depth } entries: depth 0 rows are top-level
// pages, depth 1 rows are indented sub-sections of the page above them ("Playback" >
// "ReplayGain", "Tagging" > "MP3"). Every row, parent or child, is selectable and
// index-maps to one pane in the content switch; more panes drop in by extending navModel
// and the switch.
//
// STAGING (no live apply): edits are held in a local `staged` object and pushed to
// the controller only on Apply or OK. Apply commits and stays open; OK commits and
// closes; Cancel and the close button revert to the last applied state and close.
// Apply/OK enable only when `dirty` (staged differs from the applied controller
// state). This is distinct from the per-row modified marker in the pane, which
// flags a value that differs from its factory DEFAULT.
//
// Fonts resolve through the Theme singleton, which is scope-independent, so
// this being a separate top-level Window (outside MainWindow's id scope) is
// irrelevant to font resolution.

// qmllint disable unqualified
// Wiring layer: this file reaches the C++ context properties (audioController,
// settingsStore, spectrumProvider, windowGeometry), which qmllint cannot see, so the
// unqualified-access category is disabled file-wide. Components stay fully
// linted; keep global wiring in the views so they can. Cost: a typo'd global
// name here surfaces at runtime, not at lint.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: settingsWindow

    // UI family for this window's labels; defaults to the Theme singleton,
    // kept as a property purely as an override surface.
    property string uiFont: Theme.uiFont

    // ThemedMenu (used by the pane's right-click reset) blurs whatever this points
    // at. Must be a sibling subtree of the menu overlay, so the body Rectangle.
    property Item menuBlurSource: windowBody

    title: "Settings"
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint
    // Size restores from window.yaml's keyed store, else the built-in
    // default; clamped against the minimums here because the store does not
    // know them. Initial values, not live bindings (startup-only read).
    minimumHeight: 480
    minimumWidth: 720
    height: Math.max(minimumHeight, windowGeometry.savedHeight("settings", 480))
    width: Math.max(minimumWidth, windowGeometry.savedWidth("settings", 720))
    modality: Qt.NonModal

    // Escape closes, byte-identical to Cancel (reseed discards the
    // staged edits, hide keeps the reused instance). No selection concept in
    // this window, so the close is single-stage. Window context so the map
    // stays scoped here; the Shortcut phase for the same macOS delivery
    // reason as the dialogs. An open ComboBox popup in any pane consumes Escape
    // itself (CloseOnEscape) before this can match, so a dropdown closes
    // without touching the window.
    //
    // The visible && strict-focus gate. This is a hide()-reused
    // instance, so the Shortcut object outlives every close, and a
    // still-enabled shortcut in a HIDDEN window keeps participating in Qt's
    // shortcut map: every later Escape anywhere became ambiguous (Qt fires
    // neither onActivated when two enabled shortcuts match; it rotates
    // activatedAmbiguously), observed as dead Escape in Properties / Custom
    // Columns and double-press deselect in the playlist after this window had
    // been shown and hidden once. Disabling ungrabs from the map. The first
    // attempt gated on Window.active and failed with two windows open:
    // Window.active is QWindow::isActive(), which is transient-GROUP
    // activation, so every visible secondary window reports active together
    // and all their grabs stayed ambiguous. WindowFocus.focusWindow is
    // QGuiApplication::focusWindow(), singular by definition, so identity
    // against it leaves at most ONE secondary-window Escape grab alive at any
    // instant. A window that can receive the Escape key IS the focus window,
    // so the gate never starves a legitimate press.
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: settingsWindow.visible
                 && WindowFocus.focusWindow === settingsWindow
        onActivated: {
            settingsWindow.reseed()
            settingsWindow.dismiss()
        }
    }

    // Persist the size for the next open, windowed frames only (the same
    // visibility guard as the main window). Called from every path that
    // takes the window off screen.
    function _saveSize() {
        if (settingsWindow.visibility === Window.Windowed)
            windowGeometry.saveSize("settings",
                                    settingsWindow.width, settingsWindow.height)
    }

    // The single hide path for Cancel, OK, the title bar X and Escape. This is
    // a hide()-reused instance, so onClosing never fires for any of them and
    // the size save has to ride the hide itself. The caller decides what
    // happens to the staged edits first (reseed or applyStaged).
    function dismiss() {
        _saveSize()
        hide()
    }

    // A window-manager close request bypasses every in-window path above.
    // Close == Cancel by contract, so revert the staged edits and save the
    // size the same way; accepting the close hides the reused instance, and
    // the next openSettings() shows it again.
    onClosing: {
        settingsWindow.reseed()
        settingsWindow._saveSize()
    }

    // -----------------------------------------------------------------------
    // Staged edits. Seeded from the controller's APPLIED values on open; the pane
    // reads and writes these, and Apply/OK copy them back to the controller.
    // -----------------------------------------------------------------------
    // A named type (see StagedSettings.qml) so the panes can declare a typed
    // `settings` property and qmllint verifies every staged member access.
    StagedSettings {
        id: staged
    }

    function _near(a, b) { return Math.abs(a - b) < 1e-9 }

    // staged differs from what is currently applied: gates Apply/OK.
    readonly property bool dirty:
           staged.bitPerfect !== audioController.bitPerfect
        || staged.outputDeviceId !== audioController.outputDeviceId
        || staged.mode !== audioController.replayGainMode
        || !_near(staged.preampDb, audioController.replayGainPreampDb)
        || !_near(staged.untaggedPreampDb, audioController.replayGainUntaggedPreampDb)
        || staged.clip !== audioController.replayGainClipPrevention
        || staged.skipExisting !== audioController.replayGainScanSkipExisting
        || staged.spectrumSource !== spectrumProvider.source
        || staged.id3v2Version !== settingsStore.id3v2Version
        || staged.id3v1Mode !== settingsStore.id3v1Mode
        || staged.apeMode !== settingsStore.apeMode
        || staged.id3v2Encoding !== settingsStore.id3v2Encoding

    // Copy applied -> staged (open and Cancel/revert).
    function reseed() {
        staged.bitPerfect       = audioController.bitPerfect
        staged.outputDeviceId   = audioController.outputDeviceId
        // The device list may have changed since the last open (hot-plug);
        // ask for a fresh enumeration so the Playback pane's picker is
        // current. Async: the combo repopulates when the reply lands.
        audioController.refreshOutputDevices()
        staged.mode             = audioController.replayGainMode
        staged.preampDb         = audioController.replayGainPreampDb
        staged.untaggedPreampDb = audioController.replayGainUntaggedPreampDb
        staged.clip             = audioController.replayGainClipPrevention
        staged.skipExisting     = audioController.replayGainScanSkipExisting
        staged.spectrumSource   = spectrumProvider.source
        staged.id3v2Version     = settingsStore.id3v2Version
        staged.id3v1Mode        = settingsStore.id3v1Mode
        staged.apeMode          = settingsStore.apeMode
        staged.id3v2Encoding    = settingsStore.id3v2Encoding
    }

    // Copy staged -> applied (Apply and OK). The controller's setters skip no-ops,
    // recompute the playing track's RG factor, and persist; the rate-policy
    // setter additionally pushes the engine's RateMode, which takes effect at
    // the next track start. The spectrum source commits
    // through the provider, which pushes the tap choice to the engine and
    // persists it. The tagging setters skip no-ops too and coalesce into a single
    // settings.yaml write per event-loop turn, so pushing all four is one write.
    function applyStaged() {
        audioController.setBitPerfect(staged.bitPerfect)
        audioController.setOutputDeviceId(staged.outputDeviceId)
        audioController.setReplayGainMode(staged.mode)
        audioController.setReplayGainPreampDb(staged.preampDb)
        audioController.setReplayGainUntaggedPreampDb(staged.untaggedPreampDb)
        audioController.setReplayGainClipPrevention(staged.clip)
        audioController.setReplayGainScanSkipExisting(staged.skipExisting)
        spectrumProvider.setSource(staged.spectrumSource)
        settingsStore.id3v2Version  = staged.id3v2Version
        settingsStore.id3v1Mode     = staged.id3v1Mode
        settingsStore.apeMode       = staged.apeMode
        settingsStore.id3v2Encoding = staged.id3v2Encoding
    }

    // Edit > Settings entry point: seed from the applied state, then show + raise.
    function openSettings() {
        reseed()
        show()
        raise()
        requestActivate()
    }

    // -----------------------------------------------------------------------
    // Body: rounded frame matching the main window.
    // -----------------------------------------------------------------------
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

            // ----- title bar: drag + close ---------------------------------
            Item {
                id: titleBar
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                // Drag the window from the title bar (frameless, so manual).
                MouseArea {
                    anchors.fill: parent
                    onPressed: settingsWindow.startSystemMove()
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Settings"
                    color: Theme.textPrimary
                    font.family: settingsWindow.uiFont
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }

                // Close == Cancel: revert staged and hide.
                ToolDialogCloseButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: { settingsWindow.reseed(); settingsWindow.dismiss() }
                }
            }

            // ----- nav + content -------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // Left navigation. A list of { label, depth } entries: depth 0
                // is a top-level page, depth 1 an indented sub-section of the
                // page above it. Index still maps 1:1 to the content
                // switch; extend navModel + the switch to add more panes.
                Rectangle {
                    Layout.preferredWidth: 160
                    Layout.fillHeight: true
                    color: Theme.surfaceSunken

                    property int currentIndex: 0
                    property var navModel: [
                        { label: "Playback",      depth: 0 },
                        { label: "ReplayGain",    depth: 1 },
                        { label: "Spectrum view", depth: 0 },
                        { label: "Tagging",       depth: 0 },
                        { label: "MP3",           depth: 1 }
                    ]
                    id: navPane

                    Column {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.topMargin: 8

                        Repeater {
                            model: navPane.navModel
                            delegate: Rectangle {
                                id: navDelegate
                                required property int index
                                required property var modelData
                                width: navPane.width
                                height: 34
                                color: navPane.currentIndex === index ? "#241F33"
                                     : navHover.hovered ? "#1E1E1E" : "transparent"

                                Rectangle {  // active accent bar
                                    visible: navPane.currentIndex === navDelegate.index
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 3
                                    color: Theme.accentSoft
                                }

                                Text {
                                    anchors.left: parent.left
                                    // Sub-sections indent one step per depth.
                                    anchors.leftMargin: 16 + navDelegate.modelData.depth * 14
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: navDelegate.modelData.label
                                    color: navPane.currentIndex === navDelegate.index ? Theme.textPrimary : "#B8B8B8"
                                    font.family: settingsWindow.uiFont
                                    font.pixelSize: 12
                                    font.weight: navPane.currentIndex === navDelegate.index ? Font.Bold : Font.Normal
                                }
                                HoverHandler { id: navHover }
                                TapHandler { onTapped: navPane.currentIndex = navDelegate.index }
                            }
                        }
                    }
                }

                // Right content. One pane per nav item, toggled by currentIndex.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: Theme.surfacePage

                    PlaybackSettingsPane {
                        anchors.fill: parent
                        anchors.margins: 20
                        visible: navPane.currentIndex === 0
                        settings: staged
                        controller: audioController
                    }

                    ReplayGainSettingsPane {
                        anchors.fill: parent
                        anchors.margins: 20
                        visible: navPane.currentIndex === 1
                        settings: staged
                        controller: audioController
                    }

                    SpectrumSettingsPane {
                        anchors.fill: parent
                        anchors.margins: 20
                        visible: navPane.currentIndex === 2
                        settings: staged
                        provider: spectrumProvider
                    }

                    TaggingSettingsPane {
                        anchors.fill: parent
                        anchors.margins: 20
                        visible: navPane.currentIndex === 3
                        settings: staged
                    }

                    Mp3TaggingSettingsPane {
                        anchors.fill: parent
                        anchors.margins: 20
                        visible: navPane.currentIndex === 4
                        settings: staged
                        store: settingsStore
                    }
                }
            }

            // ----- footer: Apply / OK / Cancel -----------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                bottomLeftRadius: 8
                bottomRightRadius: 8
                color: Theme.headerBand

                RowLayout {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10

                    FooterButton {
                        label: "Cancel"
                        onClicked: { settingsWindow.reseed(); settingsWindow.dismiss() }
                    }
                    FooterButton {
                        label: "Apply"
                        enabled: settingsWindow.dirty
                        onClicked: settingsWindow.applyStaged()
                    }
                    FooterButton {
                        label: "OK"
                        accent: true
                        onClicked: { settingsWindow.applyStaged(); settingsWindow.dismiss() }
                    }
                }
            }
        }
    }

    // Resize edges: this window is frameless on every platform, so the grips
    // are unconditional. Declared after windowBody so they sit above the
    // chrome. topInset keeps the top strip off the title bar's move zone.
    WindowResizeGrips {
        target: settingsWindow
        topInset: 40
    }

    // -----------------------------------------------------------------------
    // A small themed footer button. accent paints the primary (OK) variant.
    // -----------------------------------------------------------------------
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
            font.family: settingsWindow.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
        }
        HoverHandler { id: fbtnHover }
        TapHandler { onTapped: if (fbtn.enabled) fbtn.clicked() }
    }
}
