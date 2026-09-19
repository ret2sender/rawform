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

// LogStore.qml
//
// In-memory log of notable events for the session. Backs two surfaces:
//   - the one-line StatusLogBar under the playlist pane, which shows the most
//     recent entry (latestText / latestLevel), and
//   - the pop-up LogConsole, which prints the whole history as a text log.
//
// Per the console being a text block (not a ListView), the history is kept as a
// ready-to-render `logText`: an HTML string with one colored line per entry,
// which a RichText TextEdit renders directly. We therefore hold the entries as a
// small array of pre-built HTML lines (joined into logText) rather than a
// ListModel; nothing renders a list, so the model would be dead weight.
//
// This is intentionally pure QML and non-persistent: it captures playback errors from
// AudioController (via MainWindow's Connections; the StatusLogBar and console are their
// only surfaces) plus any warnings/info a caller appends. Live transient activity
// (scanning, saving, reading tags) is shown on the bar directly while busy; only its edge
// transitions are appended here, as info entries. Note the consequence for the
// single-line bar: it renders the LATEST entry, so an info edge that lands after an error
// supersedes it on the bar while the console keeps the full history.
//
// No C++: callers push entries via append(level, text).

pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: store

    // Hard cap on retained lines; the console shows the tail, older lines drop
    // past this. Tunable.
    readonly property int maxEntries: 500

    // Most-recent entry, for the single-line StatusLogBar. Plain text (the bar
    // colors it itself via its own level mapping).
    property string latestText: ""
    property string latestLevel: "info"   // info | warning | error

    // The whole history as RichText (HTML), one colored line per entry, newest
    // last. The console binds a TextEdit to this. Empty until the first event.
    property string logText: ""

    // Emitted on every successful append (the console can react if it wants).
    signal appended(string level, string text)

    // Pre-built HTML lines, newest last; joined into logText on each append.
    property var _lines: []

    // Per-level color, shared with StatusLogBar's mapping so the two agree.
    // These are HTML color strings embedded in rich text, NOT color values:
    // a QML color stringifies as #AARRGGBB, which the rich-text parser does
    // not reliably accept, so Theme's color-typed tokens cannot be
    // concatenated here. Theme therefore exposes string TWINS of the three
    // level colors (errorTextHex, warningHex, textDimHex), declared adjacent
    // to their color partners; reading those keeps this mapping and the
    // bar's color-typed mapping in lockstep with a single edit point.
    function _color(level) {
        return level === "error"   ? Theme.errorTextHex
             : level === "warning" ? Theme.warningHex
             :                       Theme.textDimHex
    }

    // Escape the message for safe embedding in the RichText HTML. Error strings
    // can contain &, <, > (paths, codec messages), which would otherwise break
    // or inject markup.
    function _escape(s) {
        return String(s).replace(/&/g, "&amp;")
                        .replace(/</g, "&lt;")
                        .replace(/>/g, "&gt;")
    }

    // Append one entry. Empty text is ignored. level is info | warning | error;
    // anything else is treated as info by the color mapping.
    function append(level, text) {
        if (!text || text.length === 0)
            return
        var stamp = Qt.formatTime(new Date(), "HH:mm:ss")
        var line = '<span style="color:' + _color(level) + '">'
                 + stamp + '&nbsp;&nbsp;' + _escape(text) + '</span><br>'
        _lines.push(line)
        if (_lines.length > maxEntries)
            _lines.splice(0, _lines.length - maxEntries)
        logText = _lines.join("")
        latestText = text
        latestLevel = level
        appended(level, text)
    }

    function clear() {
        _lines = []
        logText = ""
        latestText = ""
        latestLevel = "info"
    }
}
