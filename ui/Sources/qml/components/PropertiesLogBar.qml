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

// PropertiesLogBar.qml
//
// The Properties window's single message surface, sitting between the tab
// strip and the tab content. Every transient message the window produces lands
// here and nowhere else: menu hover hints (the footer Tools menu and the
// panes' right-click menus), the busy label while a write or reload pass
// runs, and the status / failure messages. The host composes ONE text from
// those sources by priority and binds it in; this is a pure view that owns
// no message state, exactly like StatusLogBar under the playlist.
//
// It sits ABOVE the panes so no popup ever covers it: the Tools menu opens
// upward from the footer's left corner and the panes' context menus open
// downward from the cursor, so both stay clear of the top of the window.
// That is also why the text can read left-to-right like everything else.
//
// One line, 24 px, like StatusLogBar. The hints are worded to fit the bar at
// the window's minimum width (about 80 characters of 12 px Inter), so
// eliding is the exception, not the design. The frame mirrors StatusLogBar
// and the panes (2 px stroke at 10% lavender). Nothing in the bar is
// interactive, so the stroke is static: no hover brightening.

pragma ComponentBehavior: Bound

import QtQuick

Rectangle {
    id: bar
    implicitHeight: 24
    color: Theme.surfacePage
    radius: 4
    border.width: 2
    border.color: Qt.rgba(0xBD / 255, 0xB2 / 255, 0xFF / 255, 0.10)

    // What to display. `level` selects the message color; `text` is the message.
    property string level: "info"          // info | warning | error
    property string text: ""

    // Severity -> message color, the same mapping StatusLogBar uses so a
    // failure reads the same in both windows: info muted, warning amber, error
    // the warm salmon of the log surfaces (Theme.errorText).
    function _levelColor(l) {
        return l === "error"   ? Theme.errorText
             : l === "warning" ? Theme.warning
             :                    Theme.textDim
    }

    // The message. Elides on the right if it ever outgrows the bar.
    Text {
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: bar.text
        color: bar._levelColor(bar.level)
        elide: Text.ElideRight
        font.family: Theme.uiFont
        font.pixelSize: 12
    }
}
