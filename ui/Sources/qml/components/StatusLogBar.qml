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

import QtQuick

/*
 * Status / log line, sitting directly under the playlist pane (added to
 * playlistLayout below PlaylistView in MainWindow). It is a single line:
 *
 *     [ > ]  <message>
 *
 * The `>` is a prompt that doubles as the affordance for the expanded log
 * console (LogConsole): clicking it emits consoleRequested(). The message is
 * whatever the
 * caller resolves and feeds in via `text`, colored by `level` (info/warning/
 * error). This is a pure view; it owns no log state. MainWindow decides what to
 * show, live activity (scanning/saving/reading tags) when busy, otherwise the
 * latest logged entry from LogStore, and binds text/level here.
 *
 * The frame mirrors the playlist and metadata panes (2 px Theme.accentSoft stroke, faint
 * by default, brighter when the prompt is hovered) so it reads as a sibling
 * surface rather than a stray bar. Height is fixed at 24.
 */
Rectangle {
    id: bar
    implicitHeight: 24
    color: Theme.surfacePage
    radius: 4
    border.width: 2

    // What to display. `level` selects the message color; `text` is the message.
    property string level: "info"          // info | warning | error
    property string text: ""

    // Whether the expanded console is open. Drives the prompt's state: the `>`
    // rotates a quarter-turn down (disclosure-open) and takes the accent color.
    property bool consoleOpen: false

    // Raised when the prompt is clicked; MainWindow toggles the pop console on it.
    signal consoleRequested()

    // Frame stroke, matched to the panes: 10% alpha idle, 25% when the prompt is
    // hovered (the only interactive element on the bar). An intermediate opacity
    // property is animated and fed into the border color, since a GradientStop or
    // border color cannot itself carry a Behavior cleanly.
    property real _strokeOpacity: promptHover.hovered ? 0.25 : 0.10
    Behavior on _strokeOpacity {
        NumberAnimation { duration: 150; easing.type: Easing.InOutQuad }
    }
    border.color: Qt.rgba(0xBD / 255, 0xB2 / 255, 0xFF / 255, _strokeOpacity)

    // Severity -> message color. info reads cool/muted; warning amber; error
    // the warm salmon the log surfaces share (Theme.errorText; LogStore's HTML
    // lines read its string twin), so the two surfaces agree by construction.
    // The prompt itself stays muted regardless of level.
    function _levelColor(l) {
        return l === "error"   ? Theme.errorText
             : l === "warning" ? Theme.warning
             :                    Theme.textDim
    }

    // The `>` prompt. Muted; brightens on hover; rotates a quarter-turn down and
    // turns accent while the console is open. Clicking toggles the console.
    Text {
        id: prompt
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        text: ">"
        font.family: Theme.uiFont
        font.pixelSize: 13
        font.weight: Font.Bold
        rotation: bar.consoleOpen ? 90 : 0
        color: bar.consoleOpen ? Theme.accentSoft
             : (promptHover.hovered ? Theme.textPrimary : Theme.textFaint)

        Behavior on rotation {
            NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
        }

        HoverHandler {
            id: promptHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: bar.consoleRequested()
        }
    }

    // The message. Truncates with an ellipsis on a narrow pane; the full text
    // will be readable in the expanded console.
    Text {
        anchors.left: prompt.right
        anchors.leftMargin: 8
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
