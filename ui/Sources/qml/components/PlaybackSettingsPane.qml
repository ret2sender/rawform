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

// PlaybackSettingsPane.qml
//
// The "Playback" page inside the Settings window, the parent section
// ReplayGain nests under, built to the same rules as its siblings: it reads
// and writes the window's staged object (passed in as `settings`), never the
// controller directly, so nothing applies until Apply/OK. Two settings live
// here: the OUTPUT DEVICE (below) and the OUTPUT RATE POLICY,
// a boolean over the engine's two user-facing RateModes: checked (the default)
// is BitPerfectWhenAvailable, switching the output device to the track's sample
// rate when the hardware can clock it and taking the nearest-best advertised
// fallback when it cannot (a 192 kHz track on a 96 kHz-max device lands on
// 96 kHz, its own rate family, not on whatever the device was parked at);
// unchecked is AlwaysResample, never touching the device rate and letting the
// OS path match every track to the device's standing rate. The engine's third
// mode, ForceDeviceRate, is a CLI-only diagnostic with no UI surface by design.
//
// The factory default comes from the bound controller (one source of truth,
// bitPerfectDefault) and drives both the modified-from-default accent dot and
// the right-click reset, exactly as the sibling panes do. Output device
// selection lives here too, right below.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import com.rawform.app

Item {
    id: pane

    // The window's staged edit object; this page uses settings.bitPerfect and
    // settings.outputDeviceId.
    property StagedSettings settings: null

    // The AudioController whose bitPerfectDefault is the reset target and the
    // modified-from-default reference. Declared and bound by the host rather
    // than read as an ambient global.
    property AudioController controller: null
    property string uiFont: Theme.uiFont

    // The picker model: the FOLLOW entry first (the empty pin, which
    // tracks the system's output choice as it changes), naming what it
    // currently resolves to so nothing else needs a "(system default)" badge,
    // then the concrete devices under their clean names. Picking the concrete
    // device that happens to be the default is deliberately distinct from the
    // follow entry: a pin stays put when the system default later moves.
    // Re-evaluates when a fresh enumeration lands (outputDevices NOTIFYs).
    readonly property var deviceModel: {
        var list = pane.controller ? pane.controller.outputDevices : []
        var defaultName = ""
        for (var d = 0; d < list.length; ++d) {
            if (list[d].isDefault) {
                defaultName = list[d].name
                break
            }
        }
        var m = [{ id: "",
                   name: "Follow system default"
                         + (defaultName.length > 0 ? " (" + defaultName + ")" : "") }]
        for (var i = 0; i < list.length; ++i) {
            m.push({ id: list[i].id, name: list[i].name })
        }
        return m
    }

    // Staged id -> combo index. A remembered device that is not in the current
    // enumeration (unplugged) resolves to a synthetic trailing entry rather
    // than silently showing "System default" for a non-default intent.
    readonly property var deviceModelWithStaged: {
        var m = pane.deviceModel
        var id = pane.settings ? pane.settings.outputDeviceId : ""
        if (id === "")
            return m
        for (var i = 0; i < m.length; ++i) {
            if (m[i].id === id)
                return m
        }
        var friendly = (pane.controller && pane.controller.outputDeviceId === id
                        && pane.controller.outputDeviceName.length > 0)
                       ? pane.controller.outputDeviceName : id
        return m.concat([{ id: id, name: friendly + " (not connected)" }])
    }

    function deviceIndexOf(id) {
        var m = pane.deviceModelWithStaged
        for (var i = 0; i < m.length; ++i) {
            if (m[i].id === id)
                return i
        }
        return 0
    }

    // First show of the pane: seed the enumeration (the window's reseed also
    // refreshes on every open; this covers the very first construction).
    Component.onCompleted: if (pane.controller) pane.controller.refreshOutputDevices()

    // Shared right-click reset menu, same shape as the sibling panes.
    ThemedMenu {
        id: resetMenu
        property var doReset: null
        Action {
            text: "Reset to default"
            // The var slot is the mechanism: each row assigns its own reset
            // closure before popping the menu, and a declared function could
            // not be reassigned per row.
            onTriggered: if (resetMenu.doReset) resetMenu.doReset()  // qmllint disable use-proper-function
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Text {
            text: "Playback"
            color: Theme.textPrimary
            font.family: pane.uiFont
            font.pixelSize: 16
            font.weight: Font.Bold
        }

        Rectangle {  // divider under the title
            Layout.fillWidth: true
            Layout.topMargin: 10
            Layout.bottomMargin: 6
            Layout.preferredHeight: 1
            color: Theme.separatorStrong
        }

        // ----- Output device -----------------------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 46

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.outputDeviceId = pane.controller.outputDeviceIdDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLabel {
                text: "Output device"
                modified: pane.settings && pane.settings.outputDeviceId !== pane.controller.outputDeviceIdDefault
            }

            ComboBox {
                id: deviceCombo
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 260
                // The Basic style's implicit height is taller than the row;
                // explicit height plus zeroed insets, the CustomColumnsWindow
                // combo's hard-won sizing (see the notes there).
                height: 24
                topInset: 0
                bottomInset: 0
                model: pane.deviceModelWithStaged
                textRole: "name"
                font.family: pane.uiFont
                font.pixelSize: 12
                currentIndex: pane.deviceIndexOf(
                                  pane.settings ? pane.settings.outputDeviceId : "")
                onActivated: function (index) {
                    if (pane.settings)
                        pane.settings.outputDeviceId = pane.deviceModelWithStaged[index].id
                }
                // Basic's default contentItem reserves vertical padding a 24 px
                // control has no room for; a plain Text sits flush. Device
                // names can be long, so elide right, which the fixed-label
                // column combo deliberately avoided.
                contentItem: Text {
                    id: deviceComboLabel
                    leftPadding: 6
                    rightPadding: 4
                    text: deviceCombo.displayText
                    font: deviceCombo.font
                    color: Theme.textPrimary
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                // Full-name tooltip: device names routinely outrun the 260 px
                // box, and the elide can eat exactly the informative tail
                // ("(not connected)"). Shown only when the label is genuinely
                // truncated and the popup is closed; a short delay so casual
                // mouse travel does not flicker it. Hand-styled on the
                // ThemedMenu palette (Basic's default tooltip chrome would be
                // off-theme).
                hoverEnabled: true  // deterministic hovered, independent of style hints
                Timer {
                    id: deviceTipDelay
                    interval: 600
                    onTriggered: deviceTip.visible = true
                }
                onHoveredChanged: {
                    if (hovered && deviceComboLabel.truncated && !popup.visible) {
                        deviceTipDelay.start()
                    } else {
                        deviceTipDelay.stop()
                        deviceTip.visible = false
                    }
                }
                ToolTip {
                    id: deviceTip
                    visible: false
                    text: deviceCombo.displayText
                    // A Popup cannot leave the window's overlay, and a floating
                    // tooltip WINDOW could not be positioned under Wayland
                    // (a documented dead end), so the full name is shown by wrapping
                    // instead of escaping: cap the width to the pane and let
                    // the text run to as many lines as it needs.
                    width: Math.min(implicitWidth, pane.width - 16)
                    x: deviceCombo.width - width  // right-align to the box, staying in-window
                    y: deviceCombo.height + 4
                    contentItem: Text {
                        text: deviceTip.text
                        font.family: pane.uiFont
                        font.pixelSize: 12
                        color: Theme.textPrimary
                        wrapMode: Text.Wrap
                    }
                    background: Rectangle {
                        color: Theme.surfacePage
                        border.color: Theme.border
                        border.width: 1
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 2
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: 11
            text: "\"Follow system default\" tracks the system's output choice, moving with it when it changes; picking a device pins rawform's audio to it, even if the system default later moves (remembered across restarts, by stable device identity). Applies on Apply or OK; if something is playing, the audio moves immediately and keeps its position. A remembered device that is not connected is kept as the choice and rawform falls back to the default until it returns."
        }

        Rectangle {  // divider between the device and rate-policy sections
            Layout.fillWidth: true
            Layout.topMargin: 8
            Layout.bottomMargin: 6
            Layout.preferredHeight: 1
            color: Theme.separator
        }

        // ----- Output rate policy ------------------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 46

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.bitPerfect = pane.controller.bitPerfectDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLabel {
                text: "Bit-perfect output (switch device sample rate)"
                modified: pane.settings && pane.settings.bitPerfect !== pane.controller.bitPerfectDefault
            }

            Toggle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: pane.settings ? pane.settings.bitPerfect : true
                onToggled: if (pane.settings) pane.settings.bitPerfect = !pane.settings.bitPerfect
            }
        }

        // A subdued explanation of the two choices, so the toggle is not cryptic.
        // Updates with the staged value; both texts name the fallback behavior
        // because "bit-perfect" alone does not say what happens to a rate the
        // device cannot clock. That the change lands at the next track
        // boundary (the engine reads its rate mode there) is stated here as
        // "next track" rather than discovered by surprise.
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 2
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: 11
            text: (pane.settings && !pane.settings.bitPerfect)
                  ? "Off: rawform gives rate control back to the system. The device returns to the sample rate configured in Audio MIDI Setup (restoring it if a previous track had switched it) and every track is resampled to that rate. Takes effect at the next track."
                  : "On: the device follows each track's sample rate when it can; rates beyond the hardware fall back to the nearest supported rate in the same family (192 kHz plays at 96 kHz on a 96 kHz-max device). The system-configured rate is restored on stop or quit. Takes effect at the next track."
        }

        Item { Layout.fillHeight: true }  // push rows to the top
    }

    // -----------------------------------------------------------------------
    // Label plus modified-from-default dot, matching the sibling panes.
    // -----------------------------------------------------------------------
    component RowLabel: Row {
        property string text: ""
        property bool modified: false
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: parent.text
            color: Theme.textSecondary
            font.family: pane.uiFont
            font.pixelSize: 12
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            visible: parent.modified
            width: 6
            height: 6
            radius: 3
            color: Theme.accentSoft
        }
    }

    // -----------------------------------------------------------------------
    // A pill toggle, the same control the ReplayGain pane uses.
    // -----------------------------------------------------------------------
    component Toggle: Rectangle {
        id: tg
        property bool checked: false
        signal toggled()
        width: 42
        height: 22
        radius: 11
        color: checked ? Theme.accentSoft : Theme.border

        Rectangle {
            width: 18
            height: 18
            radius: 9
            color: Theme.surfacePage
            anchors.verticalCenter: parent.verticalCenter
            x: tg.checked ? tg.width - width - 2 : 2
            Behavior on x { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
        }
        TapHandler { onTapped: tg.toggled() }
    }
}
