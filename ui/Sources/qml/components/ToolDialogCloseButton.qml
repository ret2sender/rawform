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

// ToolDialogCloseButton.qml
//
// Close "x" for the frameless tool dialogs.
//
// One fixed look shared by every dialog that carries a slim custom title
// bar (Settings, About, Properties, Custom Columns, Rename Files, Edit
// Value): a 22 x 22 cell, a 20 x 20 AppIcon glyph, a danger plate on
// hover, and a colorization chain that tracks the enabled/hovered state
// through Theme. The glyph goes through AppIcon so its logical size is
// pinned on every screen; a plain Image rasterizes the svg at 20 device
// pixels and shrinks on high-DPI displays.
//
// The instantiation site owns only what varies: anchors (kept out of
// here so the button also works inside a layout), `enabled` for dialogs
// that must not close mid-write, and `onClicked`.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import com.rawform.app

Button {
    id: root

    implicitHeight: 22
    implicitWidth: 22
    // 20 x 20 content box, matching the glyph's sourceSize, so the icon
    // never overflows the box the MultiEffect layer captures.
    padding: 1

    background: Rectangle {
        color: root.enabled && root.hovered ? Theme.dangerSurface : "transparent"
        radius: 4
    }

    contentItem: AppIcon {
        id: glyph
        iconSize: 20
        source: "../../icons/app/dialogs/tool_dialog_close_x.svg"
    }

    MultiEffect {
        anchors.fill: glyph
        colorization: 1.0
        colorizationColor: !root.enabled ? Theme.textDisabled
            : root.hovered ? Theme.danger : Theme.textInactive
        source: glyph
    }
}
