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

// SettingsResetMenu.qml
//
// The right-click "Reset to default" menu of a Settings pane. One instance per
// pane, shared by every SettingsRow on it: a row sets `target` to itself before
// popping the menu, and the action calls that row's reset(). A typed target
// rather than a stored closure, so the wiring is checked statically.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import com.rawform.app

ThemedMenu {
    id: menu

    // The row whose right-click opened the menu. Set by the row before popup().
    property SettingsRow target: null

    Action {
        text: "Reset to default"
        onTriggered: if (menu.target) menu.target.reset()
    }
}
