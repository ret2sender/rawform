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
// the right-click reset, exactly as the sibling panes do. Rows are SettingsRow
// instances sharing one SettingsResetMenu; the labels keep their natural width
// here (no column), since the device picker fills the row and the two labels
// differ too much in length for a shared column to read well. Output device
// selection goes through the shared ThemedComboBox; only the full-name tooltip
// is local, since it is about device names.

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

    SettingsResetMenu { id: resetMenu }

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
        SettingsRow {
            Layout.fillWidth: true
            text: "Output device"
            modified: pane.settings && pane.settings.outputDeviceId !== pane.controller.outputDeviceIdDefault
            resetMenu: resetMenu
            onReset: if (pane.settings) pane.settings.outputDeviceId = pane.controller.outputDeviceIdDefault

            ThemedComboBox {
                id: deviceCombo
                Layout.fillWidth: true
                textRole: "name"
                model: pane.deviceModelWithStaged
                currentIndex: pane.deviceIndexOf(
                    pane.settings ? pane.settings.outputDeviceId : "")
                onActivated: function (index) {
                    if (pane.settings)
                        pane.settings.outputDeviceId = pane.deviceModelWithStaged[index].id
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
                    if (hovered && deviceCombo.displayTruncated && !popup.visible) {
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
                        color: Theme.textPrimary
                        text: deviceTip.text
                        font.family: pane.uiFont
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    background: Rectangle {
                        border.color: Theme.border
                        border.width: 1
                        color: Theme.surfacePage
                        radius: 4
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
        SettingsRow {
            Layout.fillWidth: true
            text: "Bit-perfect output (switch device sample rate)"
            modified: pane.settings && pane.settings.bitPerfect !== pane.controller.bitPerfectDefault
            resetMenu: resetMenu
            onReset: if (pane.settings) pane.settings.bitPerfect = pane.controller.bitPerfectDefault

            ThemedSwitch {
                checked: pane.settings ? pane.settings.bitPerfect : true
                onToggled: if (pane.settings) pane.settings.bitPerfect = checked
            }
        }

        // A subdued explanation of the two choices, so the toggle is not cryptic.
        // Updates with the staged value; both texts name the fallback behavior
        // because "bit-perfect" alone does not say what happens to a rate the
        // device cannot clock. That the change lands at the next track
        // boundary (the engine reads its rate mode there) is stated here as
        // "next track" rather than discovered by surprise. Platform-neutral on
        // purpose: the same pane serves PipeWire and CoreAudio, and "the
        // device's own rate" is the one phrase true of both (the graph's clock
        // on Linux, the Audio MIDI Setup rate on macOS).
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 2
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: 11
            text: (pane.settings && !pane.settings.bitPerfect)
                  ? "Off: rawform leaves the device's sample rate alone. The device returns to its own rate (restoring one an earlier track had switched) and every track is resampled to it. Takes effect at the next track."
                  : "On: the device follows each track's sample rate when it can; rates beyond the hardware fall back to the nearest supported rate in the same family (192 kHz plays at 96 kHz on a 96 kHz-max device). The device's own rate is restored when playback stops or rawform quits. Takes effect at the next track."
        }

        Item { Layout.fillHeight: true }  // push rows to the top
    }
}
