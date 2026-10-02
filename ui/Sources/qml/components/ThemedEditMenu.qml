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

// ThemedEditMenu.qml
//
// The app-themed replacement for Qt's stock text-edit context menu: Undo,
// Redo, Cut, Copy, Paste, Delete, Select All over one TextField, attached
// through `ContextMenu.menu: ThemedEditMenu { editor: theField }`. Typed
// against TextField so the enabled bindings (canUndo, canPaste, selectedText)
// are statically checked. Used by the Rename Files and Find dialogs' fields;
// the other text fields keep Qt's stock menu.
//
// Resolves its blur backdrop like every ThemedMenu: through the hosting
// window's menuBlurSource, which each dialog that uses this menu declares.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

ThemedMenu {
    id: root

    property TextField editor: null

    ThemedMenuItem {
        text: "Undo"
        enabled: root.editor !== null && root.editor.canUndo
        onTriggered: root.editor.undo()
    }
    ThemedMenuItem {
        text: "Redo"
        enabled: root.editor !== null && root.editor.canRedo
        onTriggered: root.editor.redo()
    }
    MenuSeparator {}
    ThemedMenuItem {
        text: "Cut"
        enabled: root.editor !== null && root.editor.selectedText.length > 0
        onTriggered: root.editor.cut()
    }
    ThemedMenuItem {
        text: "Copy"
        enabled: root.editor !== null && root.editor.selectedText.length > 0
        onTriggered: root.editor.copy()
    }
    ThemedMenuItem {
        text: "Paste"
        enabled: root.editor !== null && root.editor.canPaste
        onTriggered: root.editor.paste()
    }
    ThemedMenuItem {
        text: "Delete"
        enabled: root.editor !== null && root.editor.selectedText.length > 0
        onTriggered: root.editor.remove(root.editor.selectionStart,
                                        root.editor.selectionEnd)
    }
    MenuSeparator {}
    ThemedMenuItem {
        text: "Select All"
        enabled: root.editor !== null && root.editor.length > 0
        onTriggered: root.editor.selectAll()
    }
}
