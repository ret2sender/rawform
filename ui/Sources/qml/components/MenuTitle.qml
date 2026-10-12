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

// MenuTitle.qml
//
// One title of the main menu bar: the label (with its mnemonic letter
// underlined through the rich-text `label`), the hover tint, the Alt+letter
// shortcut that toggles the drop-down, and the click that opens or closes it.
// The drop-down itself is the `menu` the instantiation site declares; this
// item only positions and toggles it, through the host window's openMenu so
// the popup lands under the label.
//
// The instantiation site owns what varies: `label`, `mnemonic`, `menu`.

pragma ComponentBehavior: Bound

import QtQuick
import com.rawform.app

Text {
    id: root

    /// The title as rich text, the mnemonic letter underlined: "<u>F</u>ile".
    property string label: ""
    /// The Alt+letter sequence that toggles the menu: "Alt+F".
    property string mnemonic: ""
    /// The drop-down this title opens. Declared by the instantiation site, so
    /// its items and their signals stay in the menu bar's file.
    property ThemedMenu menu

    // The host window, resolved via the Window attached property. openMenu is
    // declared on MainWindow's ApplicationWindow root, and Window.window IS
    // that object, so the member resolves; it is the only host-window member
    // this file touches.
    readonly property var hostWindow: root.Window.window

    color: titleHover.hovered ? Theme.accentHover : Theme.textPrimary
    font.family: Theme.uiFont
    font.pixelSize: 12
    font.weight: Font.Bold
    text: root.label

    Accessible.role: Accessible.MenuItem
    Accessible.name: root.label.replace(/<\/?u>/g, "")

    function toggle() {
        if (root.menu.visible)
            root.menu.close()
        else
            root.hostWindow.openMenu(root, root.menu)
    }

    HoverHandler { id: titleHover }

    Shortcut {
        // Application context, not the default window context: while the
        // menu is open it holds the keyboard focus scope, and a window-context
        // shortcut would go dormant there, so the second Alt+letter would
        // never fire and the menu would not toggle closed.
        context: Qt.ApplicationShortcut
        sequence: root.mnemonic
        onActivated: root.toggle()
    }

    TapHandler {
        // The menu closes itself on a press outside it (including a press on
        // this label), so by the time onTapped fires it may already be
        // closing. Capture whether it was up at press time, before that
        // reaction, so a click on the title while open closes it rather than
        // immediately reopening.
        property bool wasOpenOnPress: false

        onPressedChanged: {
            if (pressed)
                wasOpenOnPress = root.menu.visible
        }

        onTapped: {
            if (wasOpenOnPress)
                root.menu.close()
            else
                root.hostWindow.openMenu(root, root.menu)
        }
    }
}
