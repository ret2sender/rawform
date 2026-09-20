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

// SeekBar.qml
//
// The interactive seek scrubber for the PlayerBar: a progress track you can click and
// drag, driving nothing but the controller's existing seek() through the host. This
// component is a pure, dumb interactive bar: it does NOT know about audioController. It
// takes position / duration / seekable IN and emits seekRequested(seconds) OUT, so the
// playback wiring stays up in PlayerBar and this bar stays reusable and testable. (That
// also keeps it off the ancestor-id chain; a genuinely reusable component should not
// depend on the ids of whatever happens to host it.)
//
// THE ONE HARD PART: the scrub state machine. The controller pushes a position
// tick at roughly 10 Hz. If the fill bound straight to that tick, the thumb would
// fight the incoming position the whole time you drag. So during a press-drag a
// local "scrubbing" state owns the fill (via _scrubRatio) and the position input
// is IGNORED for the fill width; on release we commit exactly one seekRequested
// and hand the fill back to the live position. A plain click is just a zero-length
// scrub: press parks the target, release commits it, so a single click seeks to
// the clicked point on its own release (perceptually immediate), with no mid-drag
// seeks. Why release-only: a continuous seek per drag tick would each be a
// real decoder seek that flushes the ring buffer, which would thrash and stutter.
//
// THE STEP BADGE. A keyboard seek (the host calls showSeekStep with each step's
// effective delta) puts a small "+5" / "-5" in the fill's own color just above the
// tip of the fill. Steps that land while it is showing add up ("+15") and restart
// its hold; when the hold runs out it puffs away, fading while lifting, and only
// then does the running total reset, so the next step starts over at its own
// value. The badge follows the fill's tip, which follows the live position, so it
// moves with the line the moment a seek lands.
//
// LOOK: a gray track with a lavender fill. Every color and size is a property defaulted
// to the current value, so the look can be retuned without touching any of the logic
// below. The hover playhead, the time bubble and the step badge are the only added
// visuals; the first two appear only on hover/scrub, the badge only on a keyboard
// step.

pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    // -----------------------------------------------------------------------
    // Inputs (driven by PlayerBar from the AudioController).
    // -----------------------------------------------------------------------
    property real positionSeconds: 0
    property real durationSeconds: 0
    property bool seekable: false

    // The monospace family for the hover time bubble. A property (overridable
    // per the reusability note above) defaulting to the Theme singleton's mono
    // family, so hosts need not pass anything.
    property string timeFont: Theme.monoFont

    // -----------------------------------------------------------------------
    // Look. Restyle these freely without disturbing the state machine.
    // -----------------------------------------------------------------------
    property color trackColor: Theme.border
    property color fillColor: Theme.accentSoft
    property color playheadColor: Theme.textPrimary
    property color bubbleColor: Theme.surfaceRaised
    property color bubbleTextColor: Theme.textPrimary
    property real  barHeight: 8
    property real  barRadius: 4
    // Step badge timing and travel. Pop is the appear scale-in (first step of a
    // run only), hold is measured from the LAST step, fade is the puff-away,
    // and lift is how far it rises while fading.
    property int   stepBadgePopMs: 120
    property int   stepBadgeHoldMs: 700
    property int   stepBadgeFadeMs: 250
    property real  stepBadgeLift: 8
    property real  stepBadgePopFrom: 0.85

    // -----------------------------------------------------------------------
    // Output. Emitted exactly once, on release (click or drag end), with the
    // absolute target time in seconds.
    // -----------------------------------------------------------------------
    signal seekRequested(real seconds)

    // A comfortable hit target: the visible bar stays barHeight, but the row is a
    // little taller so the thin bar is easy to grab. Purely a usability gain; the
    // resting look is unchanged (the bar is vertically centered in this height).
    implicitHeight: barHeight + 12

    // -----------------------------------------------------------------------
    // Private state.
    //
    // _scrubbing gates the fill: while true the fill follows _scrubRatio and the
    // incoming positionSeconds is ignored. That single flag IS the suppress-during
    // -drag rule.
    // -----------------------------------------------------------------------
    property bool _scrubbing: false
    property real _scrubRatio: 0      // 0..1, the live drag target
    property bool _hovering: false
    property real _hoverRatio: 0      // 0..1, where the bare-hover playhead sits

    // The commit latch. seekSeconds is asynchronous: the controller only enqueues
    // the engine seek and returns, so positionSeconds keeps reporting the OLD spot
    // for the few ticks between release and the new position landing. If we dropped
    // straight back to the live binding on release, the fill would snap to that old
    // spot and then jump to the target, the brief revert-to-old flash. So on
    // release we PIN the fill at the committed target (_pendingRatio) and keep it
    // pinned through _pendingSeek until the live position actually reaches the
    // target, then unpin. A timer backstop guarantees the pin can never stick.
    property bool _pendingSeek: false
    property real _pendingRatio: 0

    // How close (in seconds) the live position must come to the committed target
    // before we call the seek landed and unpin. Seconds, not ratio, so the window
    // does not shrink on long tracks; a couple of 10 Hz ticks of slack.
    property real _seekLandTolerance: 0.5

    // The step badge's running total, in seconds, signed. Zero between runs;
    // the puff-away's completion is the only thing that zeroes it, so a step
    // arriving mid-fade still compounds.
    property real _stepTotal: 0

    // A keyboard step to show. Compounds into the running total, snaps the
    // badge back to fully visible wherever the run was (holding, fading), and
    // restarts the hold. Only the first step of a run pops in; later ones must
    // not, or a held key would flicker. Assignments rather than bindings so
    // the run's animations can drive the same properties.
    function showSeekStep(deltaSeconds) {
        var fresh = root._stepTotal === 0
        root._stepTotal += deltaSeconds
        stepBadgeRun.stop()
        stepBadge.opacity = 1
        stepBadge.lift = 0
        stepBadge.scale = fresh ? root.stepBadgePopFrom : 1
        stepBadgeRun.start()
    }

    // The live position as a 0..1 ratio, clamped and zero-guarded.
    readonly property real _progressRatio:
        durationSeconds > 0
            ? Math.max(0, Math.min(1, positionSeconds / durationSeconds))
            : 0

    // What the playhead/bubble point at: the drag target while scrubbing, else
    // the hovered position.
    readonly property real _cursorRatio: _scrubbing ? _scrubRatio : _hoverRatio

    // Show the hover affordance only when it can mean something: a seekable source
    // with a known duration, and either hovering or mid-scrub.
    readonly property bool _showCursor:
        seekable && durationSeconds > 0 && (_hovering || _scrubbing)

    // Pointer x to a clamped 0..1 ratio across the bar.
    function _ratioAt(x) {
        if (width <= 0)
            return 0
        return Math.max(0, Math.min(1, x / width))
    }

    // -----------------------------------------------------------------------
    // Latch release. While a seek is pending, each live position update is a
    // chance to notice the seek has landed: once the reported position reaches
    // the committed target (within tolerance), unpin and let the live binding
    // take back over. Stale old-position ticks that arrive first are far from the
    // target, so they neither unpin nor move the pinned fill.
    // -----------------------------------------------------------------------
    onPositionSecondsChanged: {
        if (root._pendingSeek) {
            var target = root._pendingRatio * root.durationSeconds
            if (Math.abs(root.positionSeconds - target) <= root._seekLandTolerance) {
                root._pendingSeek = false
                seekLatchTimer.stop()
            }
        }
    }

    // A new track invalidates any pending target outright (its duration, and so
    // the target time, no longer mean the same thing), so clear the latch.
    onDurationSecondsChanged: {
        root._pendingSeek = false
        seekLatchTimer.stop()
    }

    // Backstop: if the landing is never detected (an unusually slow seek, a report
    // that lands outside tolerance), unpin anyway after a short grace so the fill
    // can never get stuck off the live position. By the time this fires the seek
    // has effectively always completed, so the fall-back to the live binding is
    // seamless.
    Timer {
        id: seekLatchTimer
        interval: 600
        repeat: false
        onTriggered: root._pendingSeek = false
    }

    // -----------------------------------------------------------------------
    // The bar: track plus fill, same look as before.
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
            color: root.fillColor
            // The suppression in one line, now with the commit pin: while
            // scrubbing read the drag target; just after release read the pinned
            // target (_pendingRatio) until the seek lands; otherwise track the live
            // position. The pin is what keeps the fill from snapping back to the old
            // position during the async-seek window.
            width: parent.width * (root._scrubbing
                                   ? root._scrubRatio
                                   : (root._pendingSeek ? root._pendingRatio
                                                        : root._progressRatio))
        }
    }

    // -----------------------------------------------------------------------
    // Hover playhead plus time bubble. A thin line at the cursor and a small mono
    // bubble above it, transient, shown only while _showCursor holds. Drawn above
    // the bar; no ancestor clips, so it is free to sit over the readout row.
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

    Rectangle {
        id: bubble
        visible: root._showCursor
        color: root.bubbleColor
        radius: 4
        height: bubbleText.implicitHeight + 6
        width: bubbleText.implicitWidth + 12
        // Sit just above the bar, centered on the playhead, clamped so it never
        // runs off either end of the bar.
        y: track.y - height - 6
        x: Math.max(0, Math.min(root.width - width,
                                root._cursorRatio * root.width - width / 2))

        Text {
            id: bubbleText
            anchors.centerIn: parent
            text: Format.mmss(root._cursorRatio * root.durationSeconds)
            color: root.bubbleTextColor
            font.family: root.timeFont
            font.pixelSize: 11
            font.weight: Font.Bold
        }
    }

    // -----------------------------------------------------------------------
    // The step badge (see the header). Bare text in the fill color, centered on
    // the fill's tip just above the bar and clamped to the bar's ends, drawn in
    // the same free space above the bar as the bubble. Visibility rides on
    // opacity, which is 0 between runs, so a run that nets to zero ("+5" then
    // "-5") shows "0" and fades like any other rather than vanishing.
    // -----------------------------------------------------------------------
    Text {
        id: stepBadge
        // The fade's upward travel, animated separately from y so the position
        // binding below stays live while the badge lifts.
        property real lift: 0
        visible: opacity > 0
        opacity: 0
        text: (root._stepTotal > 0 ? "+" : "") + Math.round(root._stepTotal)
        color: root.fillColor
        font.family: root.timeFont
        font.pixelSize: 11
        font.weight: Font.Bold
        transformOrigin: Item.Bottom
        y: track.y - height - 2 - lift
        x: Math.max(0, Math.min(root.width - width, fill.width - width / 2))
    }

    // One run of the badge: pop in, hold, then puff away (fade while lifting),
    // and only at the very end zero the total. showSeekStep restarts this from
    // the top on every step, which is what makes the hold measure from the
    // last step and lets a mid-fade step revive the badge at full strength.
    SequentialAnimation {
        id: stepBadgeRun
        NumberAnimation {
            target: stepBadge
            property: "scale"
            to: 1
            duration: root.stepBadgePopMs
            easing.type: Easing.OutBack
        }
        PauseAnimation { duration: root.stepBadgeHoldMs }
        ParallelAnimation {
            NumberAnimation {
                target: stepBadge
                property: "opacity"
                to: 0
                duration: root.stepBadgeFadeMs
                easing.type: Easing.InQuad
            }
            NumberAnimation {
                target: stepBadge
                property: "lift"
                to: root.stepBadgeLift
                duration: root.stepBadgeFadeMs
                easing.type: Easing.OutQuad
            }
        }
        ScriptAction { script: root._stepTotal = 0 }
    }

    // -----------------------------------------------------------------------
    // Interaction. One MouseArea owns press / drag / release plus hover. It is
    // disabled wholesale when the source is not seekable, so a non-seekable stream
    // shows a live but inert bar: the fill still moves, nothing responds to the
    // pointer, and the cursor stays the default arrow.
    // -----------------------------------------------------------------------
    MouseArea {
        id: hit
        anchors.fill: parent
        enabled: root.seekable
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        onEntered: root._hovering = true
        onExited:  root._hovering = false

        // Press begins a scrub and parks the target at the press point. We do NOT
        // seek here: a click commits on its own release below, which keeps click
        // and drag on a single path and honors "no seeks mid-drag".
        onPressed: function (mouse) {
            root._scrubbing = true
            root._scrubRatio = root._ratioAt(mouse.x)
            root._hoverRatio = root._scrubRatio
        }

        // Fires for both hover moves and drags. While the button is down it is a
        // drag, so move the scrub target; either way keep the hover ratio current
        // so the playhead and bubble follow the pointer.
        onPositionChanged: function (mouse) {
            var r = root._ratioAt(mouse.x)
            root._hoverRatio = r
            if (pressed)
                root._scrubRatio = r
        }

        // Release commits exactly one seek at the final position (a click's point
        // or a drag's end). Before dropping _scrubbing we ARM the commit pin at the
        // same target, so the fill holds there through the async-seek window instead
        // of snapping to the stale live position. The pin releases itself once the
        // position catches up (or via the timer backstop). A click is the
        // zero-length case: press then release at the same point seeks straight
        // there.
        onReleased: {
            if (root._scrubbing && root.durationSeconds > 0) {
                root._pendingRatio = root._scrubRatio
                root._pendingSeek = true
                seekLatchTimer.restart()
                root.seekRequested(root._scrubRatio * root.durationSeconds)
            }
            root._scrubbing = false
        }

        // A grab stolen mid-drag (window deactivate, for instance) aborts the
        // scrub without committing a seek.
        onCanceled: root._scrubbing = false
    }
}
