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

// SettingsRowLabel.qml
//
// A settings row's label plus its modified-from-default dot. The dot hides by
// opacity, not visibility, so the label's width is the same in both states and
// a control laid out next to it does not shift when the dot appears.

pragma ComponentBehavior: Bound

import QtQuick
import com.rawform.app

Row {
    id: root

    property string text: ""
    property bool modified: false
    spacing: 7

    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: root.text
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 12
    }
    Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        opacity: root.modified ? 1 : 0
        width: 6
        height: 6
        radius: 3
        color: Theme.accentSoft
    }
}