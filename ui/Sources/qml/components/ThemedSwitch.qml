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

// ThemedSwitch.qml
//
// The app's pill switch: a Basic Switch with the indicator replaced and no label of its
// own (settings rows bring their label through SettingsRowLabel). It is a real Switch
// rather than a tappable Rectangle so it keeps what the control gives for free: tab
// focus, Space to toggle, drag across the track, and an accessible checked state.
//
// Callers bind `checked` to their source of truth and write `checked` back in
// onToggled:
//
//     checked: pane.settings ? pane.settings.clip : true
//     onToggled: if (pane.settings) pane.settings.clip = checked
//
// A click flips `checked` from the C++ side, which leaves the QML binding in place, so
// an outside change to the source (a right-click reset) still moves the switch. Writing
// `checked` rather than the negated source means a switch that ever disagrees with its
// source corrects itself on the next click instead of staying inverted.
//
// The implicit size is set explicitly: Basic sizes a Switch from its content item's
// padding around the indicator, and with an empty content item that collapses to zero.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import com.rawform.app

Switch {
    id: control
    padding: 0
    implicitWidth: 42
    implicitHeight: 22
    contentItem: Item {}

    indicator: Rectangle {
        width: 42
        height: 22
        radius: 11
        color: control.checked ? Theme.accentSoft : Theme.border
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.textPrimary

        Rectangle {
            width: 18
            height: 18
            radius: 9
            color: Theme.surfacePage
            anchors.verticalCenter: parent.verticalCenter
            x: 2 + control.visualPosition * (parent.width - width - 4)
            Behavior on x {
                // Off while pressed so a drag tracks the pointer instead of chasing it.
                enabled: !control.down
                NumberAnimation { duration: 90; easing.type: Easing.OutQuad }
            }
        }
    }
}