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

// ToolDialogButton.qml
//
// The footer button of the frameless tool dialogs (Settings, Properties,
// Custom Columns, Rename Files, Find, Edit Value, the ReplayGain scan
// overlay): one fixed look, a text label, and an `accent` variant for the
// primary action (OK, Rename). An AbstractButton drawn entirely here, so
// nothing from the style leaks in while the control still gives what a
// button owes the keyboard and assistive technology: Tab focus, Space and
// Return/Enter to activate, the button role with the label as its name, and
// the default-button flag on the accent variant (the dialog roots route an
// unclaimed Enter to it). The focus ring follows ThemedSwitch and
// ThemedComboBox: a 1 px Theme.textPrimary stroke, shown for keyboard focus
// only (visualFocus), never for a click.
//
// The instantiation site owns only what varies: `label`, `accent`, `enabled`,
// `onClicked`, and its layout attachment.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import com.rawform.app

AbstractButton {
    id: root

    property string label: ""
    property bool accent: false

    text: root.label
    padding: 0
    hoverEnabled: true
    implicitWidth: Math.max(72, btnText.implicitWidth + 28)
    implicitHeight: 30
    opacity: enabled ? 1.0 : 0.5

    Accessible.name: root.label
    Accessible.defaultButton: root.accent

    /// Activate as a click would, when the button can be clicked at all;
    /// returns whether it was. The dialog roots route an Enter that nothing
    /// else claimed to their accent button through this.
    function activate() {
        if (!root.enabled || !root.visible)
            return false
        root.clicked()
        return true
    }

    // Return and Enter activate the focused button itself, ahead of the
    // dialog root's default-button handler (a focused Cancel must cancel).
    // Space is the control's own.
    Keys.onReturnPressed: function (event) { root.activate(); event.accepted = true }
    Keys.onEnterPressed: function (event) { root.activate(); event.accepted = true }

    background: Rectangle {
        radius: 5
        color: !root.enabled ? Theme.surfaceControl
             : root.hovered ? (root.accent ? Theme.accentButtonHover
                                           : Theme.surfaceControlHover)
             : (root.accent ? Theme.accentSoft : Theme.buttonFace)
        border.color: root.visualFocus ? Theme.textPrimary
                    : root.accent ? "transparent" : Theme.border
        border.width: (root.accent && !root.visualFocus) ? 0 : 1
    }

    contentItem: Text {
        id: btnText
        text: root.label
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        // The disabled state overrides the accent text color: textOnAccent
        // over the disabled gray face is invisible (an OK button that went
        // blank while a write was in flight).
        color: !root.enabled ? Theme.textDisabled
             : root.accent ? Theme.textOnAccent : Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: Theme.fontSizeBody
        font.weight: Font.Bold
    }
}
