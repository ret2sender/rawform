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

// VolumeSlider.qml
//
// The master-volume control for the PlayerBar. It is a sibling in spirit to
// SeekBar: a pure, dumb, reusable bar that knows nothing about audioController.
// It takes a 0..1 `level` IN and emits moved(level) OUT, so the playback wiring
// stays up in PlayerBar and this bar stays reusable and testable, and it never
// reaches for an ancestor id (the font/color it needs are properties, the same
// discipline SeekBar adopted for exactly this reason).
//
// THE DIFFERENCE FROM SeekBar, and the whole reason this is its own file rather
// than a reused SeekBar: volume applies CONTINUOUSLY. SeekBar defers to release
// and commits exactly one seek because every drag tick would otherwise be a real
// decoder seek that flushes the ring buffer, which would thrash. Setting volume
// has none of that cost: it is an instant lock-free atomic store at the engine's
// pull chokepoint with no flush and no decoder work, so the natural, expected
// behavior is to hear every value as you drag. We therefore emit moved() on
// press, on every drag tick, and on each wheel notch, with no commit latch, no
// pending-ratio pin, and no async-landing machinery. There is nothing to wait
// for: the controller's level updates synchronously when we emit.
//
// We still borrow SeekBar's ONE structural trick: during a drag a local
// _dragRatio owns the fill, so the bar cannot fight the `level` input mid-gesture
// even though the round trip back through the controller is synchronous. Outside
// a drag the fill follows `level` directly.
//
// MUTE is shown, not controlled, here: the `muted` input dims the fill so the bar
// reads as inactive while keeping the stored position visible (mute remembers the
// level it will return to). The actual mute toggle lives on the PlayerBar's
// percentage label, since this design has no dedicated mute glyph.
//
// LOOK matches the SeekBar's lavender-on-gray pill. Every color and size is a
// property defaulted to the current value, so the look can be retuned without
// touching the logic below.

pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    // -----------------------------------------------------------------------
    // Inputs (driven by PlayerBar from the AudioController).
    // -----------------------------------------------------------------------

    // The displayed level, 0..1. This is the slider FRACTION (the percentage over
    // 100), not a gain: the perceptual taper that turns it into a gain lives in
    // the controller, so this bar is linear in position and that is correct (the
    // fill should track the number the user sees).
    property real level: 0

    // Muted state, for the dimmed look only; the bar still shows `level`.
    property bool muted: false

    // The perceptual taper exponent the controller applies (gain = ratio ^ exp).
    // Passed in so the dB tooltip reports the gain actually applied, from the
    // controller's one source of truth, rather than guessing a curve here.
    property real taperExponent: 2.0

    // -----------------------------------------------------------------------
    // Look. Defaults match the SeekBar's track/fill so the two bars read as a
    // pair; restyle freely without disturbing the logic.
    // -----------------------------------------------------------------------
    property color trackColor: Theme.border
    property color fillColor: Theme.accentSoft
    property color mutedFillColor: "#5A5668"  // the fill, grayed, while muted
    property color playheadColor: Theme.textPrimary   // the hover/drag pipe, as on the SeekBar
    property color bubbleColor: Theme.surfaceRaised
    property color bubbleTextColor: Theme.textPrimary
    property real  barHeight: 8
    property real  barRadius: 4

    // The monospace family for the dB bubble, passed in rather than read from an
    // ancestor id (the same reusability discipline SeekBar follows). Empty renders
    // in the default family, so the bubble still works if a host forgets to set it.
    property string bubbleFont: Theme.monoFont

    // How much one wheel notch moves the level (0..1). 0.05 == 5% per notch,
    // a comfortable nudge that matches the percentage readout's granularity.
    property real wheelStep: 0.05

    // -----------------------------------------------------------------------
    // Output. Emitted on every user change (press, drag tick, wheel notch) with
    // the new 0..1 level. The controller clamps and applies; we pre-clamp anyway
    // so a consumer that does not clamp still gets a valid value.
    // -----------------------------------------------------------------------
    signal moved(real level)

    // Match the SeekBar's comfortable hit target so the thin bar is easy to grab
    // and the two bars line up row-for-row in the PlayerBar.
    implicitHeight: barHeight + 12

    // -----------------------------------------------------------------------
    // Private drag state. _dragging gates the fill: while true the fill follows
    // _dragRatio so it cannot fight the incoming `level` for the one frame the
    // round trip takes. That single flag IS the own-the-fill-during-drag rule.
    // -----------------------------------------------------------------------
    property bool _dragging: false
    property real _dragRatio: 0

    // Hover state for the playhead pipe (the SeekBar shows the same thin marker on
    // hover and scrub). _hoverRatio tracks the bare-hover pointer; _cursorRatio is
    // what the pipe points at: the drag target while dragging, else the hover spot.
    property bool _hovering: false
    property real _hoverRatio: 0
    readonly property real _cursorRatio: _dragging ? _dragRatio : _hoverRatio

    // Show the pipe whenever the pointer is engaged with the bar.
    readonly property bool _showCursor: _hovering || _dragging

    // The level as a clamped 0..1 ratio (level is already 0..1, but guard anyway).
    readonly property real _levelRatio: Math.max(0, Math.min(1, level))

    // Pointer x to a clamped 0..1 ratio across the bar.
    function _ratioAt(x) {
        if (width <= 0)
            return 0
        return Math.max(0, Math.min(1, x / width))
    }

    // Emit a clamped level. The single exit point for every gesture.
    function _emit(r) {
        root.moved(Math.max(0, Math.min(1, r)))
    }

    // The dB the bubble shows for a 0..1 ratio: the gain the engine will apply at
    // that position, expressed in decibels. gain = ratio ^ taperExponent, so
    // dB = 20*log10(gain) = 20*taperExponent*log10(ratio). Ratio 1 is 0.0 dB
    // (unity), and ratio 0 is true silence, shown as -inf. One decimal, matching
    // the resolution a volume readout wants.
    function _db(ratio) {
        if (ratio <= 0)
            return "-inf dB"
        var db = 20 * root.taperExponent * Math.log(ratio) / Math.LN10
        var s = db.toFixed(1)
        if (s === "-0.0")
            s = "0.0"
        return s + " dB"
    }

    // -----------------------------------------------------------------------
    // The bar: track plus fill, the SeekBar look. The fill reads _dragRatio while
    // dragging, else the live level; its color grays out while muted.
    // -----------------------------------------------------------------------
    Rectangle {
        id: track
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width
        height: root.barHeight
        radius: root.barRadius
        color: root.trackColor

        Rectangle {
            id: fill
            height: parent.height
            radius: parent.radius
            color: root.muted ? root.mutedFillColor : root.fillColor
            width: parent.width * (root._dragging ? root._dragRatio
                                                   : root._levelRatio)
        }
    }

    // -----------------------------------------------------------------------
    // The hover/drag playhead: a thin vertical pipe at the cursor, the same marker
    // the SeekBar shows. No percentage bubble here, the readout above the bar
    // already shows the value; this is purely the positional pipe. Drawn above
    // the track and only while the pointer is engaged.
    // -----------------------------------------------------------------------
    Rectangle {
        id: playhead
        visible: root._showCursor
        width: 1
        height: root.barHeight + 8
        color: root.playheadColor
        opacity: 0.7
        anchors.verticalCenter: track.verticalCenter
        x: root._cursorRatio * root.width
    }

    // -----------------------------------------------------------------------
    // The dB bubble, the volume analogue of the SeekBar's time bubble: a small
    // mono readout above the bar showing the dB at the cursor, shown only while
    // the pointer is engaged (hover or drag). Clamped so it never runs off either
    // end of the bar. Drawn above the track; no ancestor clips it.
    // -----------------------------------------------------------------------
    Rectangle {
        id: bubble
        visible: root._showCursor
        color: root.bubbleColor
        radius: 4
        height: bubbleText.implicitHeight + 6
        width: bubbleText.implicitWidth + 12
        y: track.y - height - 6
        x: Math.max(0, Math.min(root.width - width,
                                root._cursorRatio * root.width - width / 2))

        Text {
            id: bubbleText
            anchors.centerIn: parent
            text: root._db(root._cursorRatio)
            color: root.bubbleTextColor
            font.family: root.bubbleFont
            font.pixelSize: 11
            font.weight: Font.Bold
        }
    }

    // -----------------------------------------------------------------------
    // Interaction. One MouseArea owns press / drag / release and the wheel. Every
    // path emits moved() immediately: there is no release-only commit because a
    // volume change is free to apply live (see the file header).
    // -----------------------------------------------------------------------
    MouseArea {
        id: hit
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        onEntered: root._hovering = true
        onExited:  root._hovering = false

        // Press jumps the level to the press point and begins the drag. Unlike the
        // SeekBar we DO act on press, because a click should set the volume at once
        // (and there is no flush penalty for doing so).
        onPressed: function (mouse) {
            root._dragging = true
            root._dragRatio = root._ratioAt(mouse.x)
            root._hoverRatio = root._dragRatio
            root._emit(root._dragRatio)
        }

        // Fires for both hover moves and drags. Keep the hover ratio current either
        // way so the pipe follows the pointer; while the button is down it is also
        // a drag, so move the level live.
        onPositionChanged: function (mouse) {
            var r = root._ratioAt(mouse.x)
            root._hoverRatio = r
            if (pressed) {
                root._dragRatio = r
                root._emit(r)
            }
        }

        // Release ends the drag and hands the fill back to the live `level`. No
        // commit is needed: the last drag tick already applied.
        onReleased: root._dragging = false

        // A grab stolen mid-drag (window deactivate, for instance) just ends the
        // drag; the last applied level stands.
        onCanceled: root._dragging = false

        // Wheel nudges by one step per notch, from the CURRENT level (not a drag
        // ratio), so a scroll without a prior click still steps from where the bar
        // is. angleDelta.y is positive scrolling up (louder), negative down. The
        // tab strip uses the same onWheel-on-the-consuming-MouseArea shape. We also
        // park the hover cursor on the new level so the pipe and dB bubble jump to
        // it and climb with each notch (the pointer x is irrelevant to a wheel
        // change); the next mouse move hands the cursor back to the pointer.
        onWheel: function (wheel) {
            var dir = wheel.angleDelta.y > 0 ? 1 : -1
            var next = Math.max(0, Math.min(1, root._levelRatio + dir * root.wheelStep))
            root._hoverRatio = next
            root._emit(next)
        }
    }
}
