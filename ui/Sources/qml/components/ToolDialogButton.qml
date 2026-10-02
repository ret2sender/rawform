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
// Custom Columns, Rename Files, Find): one fixed look, a text label, and an
// `accent` variant for the primary action (OK, Rename). A Rectangle with
// handlers rather than a Controls Button, so nothing from the style leaks in;
// the dialogs draw everything else themselves for the same reason.
//
// The instantiation site owns only what varies: `label`, `accent`, `enabled`,
// `onClicked`, and its layout attachment.

pragma ComponentBehavior: Bound

import QtQuick
import com.rawform.app

Rectangle {
    id: root

    property string label: ""
    property bool accent: false
    signal clicked()

    implicitWidth: Math.max(72, btnText.implicitWidth + 28)
    implicitHeight: 30
    radius: 5
    color: !enabled ? Theme.surfaceControl
         : hover.hovered ? (accent ? Theme.accentButtonHover : Theme.surfaceControlHover)
         : (accent ? Theme.accentSoft : Theme.buttonFace)
    border.color: accent ? "transparent" : Theme.border
    border.width: accent ? 0 : 1
    opacity: enabled ? 1.0 : 0.5

    Text {
        id: btnText
        anchors.centerIn: parent
        text: root.label
        // The disabled state overrides the accent text color: textOnAccent
        // over the disabled gray face is invisible (an OK button that went
        // blank while a write was in flight).
        color: !root.enabled ? Theme.textDisabled
             : root.accent ? Theme.textOnAccent : Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 12
        font.weight: Font.Bold
    }
    HoverHandler { id: hover }
    TapHandler { onTapped: if (root.enabled) root.clicked() }
}
