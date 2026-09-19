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

// LogConsole.qml
//
// Expanded log console. Pops upward from the StatusLogBar over the bottom of the
// playlist pane: non-modal and overlaid, so the track list does not reflow when
// it opens. Renders the session log as a scrollable, read-only, selectable text
// block in JetBrains Mono, one colored line per event, newest at the bottom.
//
// It is a text log, not a ListView: it binds a RichText TextEdit straight to
// LogStore.logText (HTML with a colored span per line). The caller owns
// placement (anchors) and the toggle; this component owns the open animation,
// the auto-scroll, and Escape-to-close.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Rectangle {
    id: consolePanel

    // Open state and the height it expands to. The caller flips `open`; the panel
    // animates its own height between 0 and panelHeight, growing upward (its
    // bottom edge is anchored by the caller just above the status line).
    property bool open: false
    property int panelHeight: 220

    // The history to print (HTML), supplied by the caller from LogStore.
    property string logText: ""

    // Tracks whether the view is parked at the bottom, so new lines only yank the
    // scroll down when the reader was already there (not while they read history).
    property bool _stick: true

    height: open ? panelHeight : 0
    visible: height > 0
    clip: true
    color: Theme.surfaceInset
    radius: 4
    border.width: 2
    border.color: Qt.rgba(0xBD / 255, 0xB2 / 255, 0xFF / 255, 0.18)

    Behavior on height {
        NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
    }

    function scrollToBottom() {
        flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }

    // On open, stick and jump to the newest line (deferred so the grown height is
    // settled before we measure).
    onOpenChanged: if (open) { _stick = true; Qt.callLater(scrollToBottom) }

    // Escape closes, but only while open, so it does not swallow Escape elsewhere
    // (tab rename, dialogs) the rest of the time. onActivatedAmbiguously:
    // the playlist's Escape-deselect shortcut can be enabled at the same time
    // (console open, table focused); Qt then activates both AMBIGUOUSLY instead
    // of firing onActivated on either, so both surfaces handle that signal too
    // and the overlap closes the console and clears the selection together.
    Shortcut {
        sequence: "Escape"
        enabled: consolePanel.open
        onActivated: consolePanel.open = false
        onActivatedAmbiguously: consolePanel.open = false
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 10
        contentWidth: width
        contentHeight: logEdit.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        // Re-evaluate the stick state whenever the position changes (user drag,
        // wheel, or our own programmatic jump): stuck only when at the bottom.
        onContentYChanged:
            consolePanel._stick = (contentY >= contentHeight - height - 4)

        TextEdit {
            id: logEdit
            width: flick.width
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            textFormat: TextEdit.RichText
            color: "#C8C8C8"                 // default; per-line spans override it
            selectionColor: Theme.selectionFill
            selectedTextColor: Theme.textPrimary
            font.family: Theme.monoFont
            font.pixelSize: 12
            text: consolePanel.logText

            // Content grew (a line was appended): follow to the bottom only if the
            // reader was parked there.
            onHeightChanged: if (consolePanel._stick) consolePanel.scrollToBottom()
        }
    }

    // Faint hint while nothing has been logged yet.
    Text {
        anchors.centerIn: parent
        visible: consolePanel.logText.length === 0
        text: "No events yet."
        color: Theme.textDisabled
        font.family: Theme.monoFont
        font.pixelSize: 12
    }
}
