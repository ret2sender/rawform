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

// PlaylistTabBar.qml
//
// Playlist tab strip. Sits above the playlist header and is bound to the
// PlaylistTabs list model. Flat tabs: the active one is lighter
// with bright text, inactive ones dim; a muted close glyph at each tab's right
// closes it. The strip spans the full playlist-pane width (the status line is
// below the playlist pane; see StatusLogBar).
//
// Overflow / scrolling:
//   The tabs live in a CLIPPED viewport. When their total width exceeds the
//   room the strip can give them, a clustered pair of scroll arrows (chevrons)
//   appears just left of the status panel, and the off-screen tabs tuck behind
//   that cluster. The viewport itself is NOT an interactive Flickable: the tab
//   drag-reorder is a horizontal drag on each tab's MouseArea, and an
//   interactive Flickable would steal that gesture. Instead the Row is shifted
//   by `x: -scrollOffset`, and ALL scrolling (arrows + wheel) drives that one
//   property. Resize shrinks/grows the viewport, so tabs hide and re-appear as
//   the window (or the splitter) is dragged, with scrollOffset re-clamped each
//   time so a shrink never leaves a blank gutter.
//
//   - arrows           -> snap-to-tab step (reveal the next clipped tab fully),
//                         each dimmed and inert at its travel limit
//   - mouse wheel      -> one snap-to-tab step per notch (matches the arrows)
//   - trackpad swipe   -> continuous free scroll (pixelDelta), no snapping
//   The active tab is auto-revealed when it changes (so creating a playlist
//   while overflowing scrolls the new tab into view).
//
// Interaction (all pointer handling for a tab lives in ONE MouseArea, so the
// click / middle-close / close-hit / drag-reorder paths can't fight each other):
//   - left click           -> switch to the tab (stashing the outgoing tab's
//                             column widths first, so the switch can re-seed)
//   - left click on close   -> close the tab
//   - middle click on a tab -> close the tab
//   - left drag a tab       -> reorder; the model is moved once, on release
//   - middle click on empty -> new playlist
//   - File drag over the strip:
//       * dropped < 2 s      -> a new tab, smart-named (single .m3u / a folder's
//                               inner .m3u or its name), content scanned in
//       * hover >= 2 s        -> an empty "New Playlist" pops and becomes active;
//                               the drop then lands in it (generic name)
//       * a .rwfpl            -> opened in its own new tab (as a fresh live copy)
//
// `tabs` is the PlaylistTabs (the list model + the session API); `view` is the
// PlaylistView, read only to snapshot the active column widths before a switch.

pragma ComponentBehavior: Bound

import QtQuick

Rectangle {
    id: strip
    color: Theme.surfacePage
    // Strip height. Matched to ThemedMenuBar (also 24): the playlist column and
    // the left-hand menu/album-art column share the same top edge inside
    // mainSplit, so an equal-height top item on each side lands the playlist top
    // flush with the album-art top. (Was 30, which pushed the playlist 6 px low.)
    // Tabs are verticalCenter-anchored with a 12 px font, so 24 stays uncramped.
    implicitHeight: 24

    property var tabs
    property var view

    // The session LogStore (assigned from MainWindow). The strip DropArea logs
    // one line per drop (url count + handler time) to the status console; a
    // null log degrades to silent.
    property var log

    // Drag-reorder gesture state (model is reordered on release, not live).
    property int  _dragFrom: -1
    property int  _dragTo: -1
    property bool _dragging: false

    // Live cursor x (strip coords) during a reorder drag. Updated on every move
    // and read by the edge auto-scroll tick when the cursor is held still at an
    // edge (no move events fire then, so the tick needs the last known position).
    property real _dragSx: 0

    // Edge auto-scroll tunables. While dragging a tab, holding it within this
    // many px of either viewport edge scrolls the strip in that direction so a
    // tab can be dropped into a slot currently hidden behind the arrows. Speed
    // ramps with how deep into the zone the cursor sits, up to _edgeMaxStep px
    // per ~16 ms tick.
    readonly property real _edgeZone: 28
    readonly property real _edgeMaxStep: 14

    // True once the 2 s hover popped a proactive empty tab, so the drop lands in
    // it (generic name) rather than creating a second, smart-named tab.
    property bool _proactive: false

    // --- Scroll / overflow geometry ----------------------------------------
    //
    // Tunables (safe to nudge): the per-arrow width and a small right inset so
    // the last tab (or the arrow cluster) never butts flush against the pane's
    // right edge. The tabs own the full strip width; the status line is under
    // the playlist pane (StatusLogBar).
    readonly property int arrowWidth: 22
    readonly property int rightInset: 8

    // How wide the tabs could be if NO arrows were shown. This is deliberately
    // arrow-INDEPENDENT (it subtracts only the fixed right inset, never the
    // arrows), which is what keeps the overflow test below from oscillating:
    // showing the arrows shrinks the viewport but cannot flip `overflow` back.
    readonly property real availNoArrows: Math.max(0, width - rightInset)

    // Overflowing => arrows appear and the viewport shrinks by their combined
    // width; not overflowing => tabs get the full availNoArrows and no arrows.
    readonly property bool overflow: tabsRow.width > availNoArrows

    // The live scroll position (content px hidden off the left of the viewport).
    // The Row is translated by -scrollOffset; everything else derives from this.
    property real scrollOffset: 0

    // Furthest we can scroll: the slack between the row and the visible window.
    // Uses the REAL anchored viewport width (single source of truth), so it is
    // automatically correct whether or not the arrows are currently reserved.
    readonly property real maxScroll: Math.max(0, tabsRow.width - tabsViewport.width)

    // Arrow enable/dim state. A small epsilon keeps a tab snap that lands a hair
    // shy of the limit from leaving an arrow falsely "live".
    readonly property bool canScrollLeft:  scrollOffset > 0.5
    readonly property bool canScrollRight: scrollOffset < (maxScroll - 0.5)

    // Animator for the discrete (arrow / wheel-notch) snap steps. We drive it
    // explicitly rather than via a Behavior, because trackpad free-scroll sets
    // scrollOffset every frame and a Behavior would fight those continuous
    // updates. Snap steps animate; trackpad and clamps are immediate.
    NumberAnimation {
        id: scrollAnim
        target: strip
        property: "scrollOffset"
        duration: 120
        easing.type: Easing.OutCubic
    }

    function _clampValue(v) {
        return Math.max(0, Math.min(v, maxScroll))
    }

    // Animated move to a target offset (used by arrows and wheel notches).
    function _animateScrollTo(v) {
        var t = _clampValue(v)
        scrollAnim.stop()
        scrollAnim.from = scrollOffset
        scrollAnim.to = t
        scrollAnim.start()
    }

    // Immediate move (trackpad free-scroll: no animation, follow the fingers).
    function _setScrollImmediate(v) {
        scrollAnim.stop()
        scrollOffset = _clampValue(v)
    }

    // Wheel / trackpad scroll. This is driven from the MouseAreas that sit
    // directly under the cursor (each tab, and the empty strip), NOT from a
    // WheelHandler on the strip: every tab is covered by its own MouseArea, and
    // that MouseArea receives (and consumes) the wheel event before any handler
    // on an ancestor can see it, so a strip-level WheelHandler never fired here.
    // The metadata pane scrolls fine only because its cells are plain delegates
    // with no MouseArea, so the wheel reaches the TableView's Flickable.
    //
    // The full WheelEvent is available here (both axes, and no orientation gate
    // like WheelHandler had), so one path covers a vertical wheel, a vertical
    // swipe, and a horizontal swipe alike.
    //   pixelDelta present -> continuous free scroll (trackpad), no snapping
    //   pixelDelta zero     -> one snap-to-tab step per notch (mouse wheel)
    // If the trackpad direction feels inverted, flip the sign on `scrollOffset - d`.
    function _onWheel(wheel) {
        if (!overflow) {
            wheel.accepted = false
            return
        }
        var px = wheel.pixelDelta
        if (px.x !== 0 || px.y !== 0) {
            var d = (px.x !== 0) ? px.x : px.y
            _setScrollImmediate(scrollOffset - d)
        } else {
            var a = (wheel.angleDelta.x !== 0) ? wheel.angleDelta.x
                                               : wheel.angleDelta.y
            if (a !== 0)
                a > 0 ? _stepLeft() : _stepRight()
        }
        wheel.accepted = true
    }

    // Re-clamp after a geometry change (window/splitter resize, tab add/remove,
    // rename). Only acts when actually out of range, so a benign width change
    // mid-animation does not cut a snap short. When overflow drops away maxScroll
    // becomes 0 and this pulls the strip back to a full, unscrolled view, which
    // is the "tabs re-appear when expanded" behavior.
    function _clampScroll() {
        if (scrollOffset > maxScroll) {
            scrollAnim.stop()
            scrollOffset = maxScroll
        } else if (scrollOffset < 0) {
            scrollAnim.stop()
            scrollOffset = 0
        }
    }

    // Snap one tab to the right: find the first tab clipped on the right edge and
    // align its right edge to the viewport's right edge. itemAt(i).x is the tab's
    // content x (inside the Row); the visible window in content space is
    // [scrollOffset, scrollOffset + viewportWidth].
    function _stepRight() {
        var rightEdge = scrollOffset + tabsViewport.width
        for (var i = 0; i < tabsRepeater.count; ++i) {
            var it = tabsRepeater.itemAt(i)
            if (!it)
                continue
            if (it.x + it.width > rightEdge + 0.5) {
                _animateScrollTo(it.x + it.width - tabsViewport.width)
                return
            }
        }
        _animateScrollTo(maxScroll) // defensive: nothing clipped, go flush right
    }

    // Snap one tab to the left: find the rightmost tab clipped on the left edge
    // (the nearest one peeking out under the left side of the window) and align
    // its left edge to the viewport's left edge.
    function _stepLeft() {
        var target = -1
        for (var i = tabsRepeater.count - 1; i >= 0; --i) {
            var it = tabsRepeater.itemAt(i)
            if (!it)
                continue
            if (it.x < scrollOffset - 0.5) {
                target = it.x
                break
            }
        }
        _animateScrollTo(target >= 0 ? target : 0)
    }

    // Bring the active tab fully into view with the smallest possible move. Run
    // deferred (Qt.callLater) from currentIndexChanged: a just-created tab's
    // delegate may not be realized (and the Row not yet relaid) at the instant
    // the index changes, so we measure after the event loop settles.
    function _revealActive() {
        if (!tabs)
            return
        var i = tabs.currentIndex
        if (i < 0 || i >= tabsRepeater.count)
            return
        var it = tabsRepeater.itemAt(i)
        if (!it)
            return
        var left = it.x
        var right = it.x + it.width
        if (left < scrollOffset)
            _animateScrollTo(left)
        else if (right > scrollOffset + tabsViewport.width)
            _animateScrollTo(right - tabsViewport.width)
    }

    // Switch to tab i, parking the OUTGOING tab's live header widths first so the
    // incoming tab's widths can be re-seeded (PlaylistView.onModelChanged). The
    // resize hook keeps parked widths current too; this is the belt-and-suspenders.
    function _switchTo(i) {
        if (!tabs || i === tabs.currentIndex)
            return
        if (view)
            tabs.stashActiveWidths(view.currentColumnWidths())
        tabs.currentIndex = i
    }

    // Content x (within tabsRow) of reorder boundary b (0..count): the left edge
    // of tab b, or the right edge of the last tab for b === count.
    function _contentBoundary(b) {
        if (!tabsRepeater.count)
            return 0
        if (b < tabsRepeater.count) {
            var it = tabsRepeater.itemAt(b)
            return it ? it.x : 0
        }
        var last = tabsRepeater.itemAt(tabsRepeater.count - 1)
        return last ? last.x + last.width : 0
    }

    // Strip x of boundary b. Written explicitly in terms of scrollOffset (rather
    // than mapToItem) for two reasons: it stays correct while the Row is
    // translated and clipped, AND it makes scrollOffset a tracked dependency, so
    // the drop-marker binding below re-evaluates as the strip auto-scrolls mid
    // drag (a mapToItem call would not establish that dependency, leaving the
    // marker stuck until _dragTo next changed).
    function _boundaryX(b) {
        return tabsViewport.x - scrollOffset + _contentBoundary(b)
    }

    // The reorder boundary (gap) nearest strip-x `sx`: index 0..count.
    function _boundaryAt(sx) {
        var best = 0
        var bestD = Number.POSITIVE_INFINITY
        for (var b = 0; b <= tabsRepeater.count; ++b) {
            var d = Math.abs(_boundaryX(b) - sx)
            if (d < bestD) { bestD = d; best = b }
        }
        return best
    }

    // One tick of edge auto-scroll during a reorder drag. Reads the held cursor
    // position (_dragSx); if it is inside either edge zone and there is slack to
    // scroll that way, it nudges scrollOffset (immediately, so the tick owns the
    // pacing) and recomputes the drop target so the insertion marker tracks the
    // newly revealed tabs. Outside both zones it is a no-op; the timer keeps
    // running for the whole drag so a cursor re-entering a zone resumes scrolling.
    function _edgeScrollTick() {
        if (!_dragging || _dragTo < 0) {
            edgeScroll.stop()
            return
        }
        var vpLeft = tabsViewport.x
        var vpRight = tabsViewport.x + tabsViewport.width
        var step = 0
        if (_dragSx < vpLeft + _edgeZone && canScrollLeft) {
            var depthL = Math.min(1, (vpLeft + _edgeZone - _dragSx) / _edgeZone)
            step = -Math.max(2, _edgeMaxStep * depthL)
        } else if (_dragSx > vpRight - _edgeZone && canScrollRight) {
            var depthR = Math.min(1, (_dragSx - (vpRight - _edgeZone)) / _edgeZone)
            step = Math.max(2, _edgeMaxStep * depthR)
        }
        if (step === 0)
            return
        _setScrollImmediate(scrollOffset + step)
        _dragTo = _boundaryAt(_dragSx)
    }

    // 2 s hover -> proactive empty tab (the original "drop into a fresh
    // playlist" behavior). A quick drop before this fires takes the smart-name
    // path in the DropArea below.
    Timer {
        id: hoverTimer
        interval: 2000
        repeat: false
        onTriggered: {
            if (!strip.tabs)
                return
            strip._proactive = true
            strip.tabs.newPlaylist("")
        }
    }

    // Drives edge auto-scroll while a reorder drag is held near a viewport edge.
    // Runs only for the duration of a drag (started when the drag threshold is
    // crossed, stopped on release); each tick delegates to _edgeScrollTick.
    Timer {
        id: edgeScroll
        interval: 16
        repeat: true
        running: false
        onTriggered: strip._edgeScrollTick()
    }

    // Re-reveal the active tab whenever it changes (new playlist becomes active,
    // a tab is closed, a switch lands on an off-screen tab). Deferred so the
    // delegate is realized first.
    Connections {
        target: strip.tabs
        function onCurrentIndexChanged() { Qt.callLater(strip._revealActive) }
    }

    // --- Tabs viewport (clipped) -------------------------------------------
    //
    // Fills from the strip's left to the arrow cluster's left. When the cluster
    // collapses (no overflow) its left edge sits at the status panel's left, so
    // the viewport simply extends to the status panel. clip:true is what tucks
    // the off-screen tabs out of sight behind the cluster.
    Item {
        id: tabsViewport
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: arrowCluster.left
        clip: true

        // Re-clamp when the visible width changes (window/splitter resize, or
        // the arrows appearing/disappearing).
        onWidthChanged: strip._clampScroll()

        // Empty-region middle-click = new playlist. Sits UNDER the Row (declared
        // first), so a middle-click on a tab is taken by that tab's own MouseArea
        // on top, and only clicks landing on bare viewport reach here. While
        // overflowing the Row covers the viewport, so this naturally goes inert.
        MouseArea {
            id: emptyArea
            anchors.fill: parent
            acceptedButtons: Qt.MiddleButton
            onClicked: function (mouse) {
                if (mouse.button === Qt.MiddleButton && strip.tabs)
                    strip.tabs.newPlaylist("")
            }
            // Wheel over bare strip (and the gaps between tabs) scrolls too.
            onWheel: function (wheel) { strip._onWheel(wheel) }
        }

        Row {
            id: tabsRow
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            // Translate the whole strip of tabs by the scroll position. Negative
            // x slides content left, hiding earlier tabs behind the viewport's
            // left clip edge.
            x: -strip.scrollOffset
            leftPadding: 12
            spacing: 2

            // Re-clamp when the content width changes (tab added/removed/renamed),
            // and re-reveal the active tab in case a width shift pushed it out.
            onWidthChanged: {
                strip._clampScroll()
                Qt.callLater(strip._revealActive)
            }

            Repeater {
                id: tabsRepeater
                model: strip.tabs

                delegate: Rectangle {
                    id: tabItem

                    // Under Bound behavior the view injects nothing through
                    // context; the tab's position and its model item (for the
                    // title role) are declared, like every other delegate.
                    required property int index
                    required property var model

                    color: tabItem.index === strip.tabs.currentIndex ? Theme.surfaceSelected : Theme.surfaceTabIdle
                    height: tabsRow.height
                    // Dim the tab being dragged.
                    opacity: (strip._dragging && strip._dragFrom === tabItem.index) ? 0.4 : 1
                    radius: 2
                    width: tabLabel.width + closeBtn.width + 24

                    // Inline-rename state (double-click the title to edit).
                    property bool editing: false

                    Text {
                        id: tabLabel
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        color: tabItem.index === strip.tabs.currentIndex ? Theme.textPrimary : Theme.textDim
                        elide: Text.ElideRight
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        font.weight: Font.Bold
                        text: tabItem.model.title
                        visible: !tabItem.editing
                        width: Math.min(implicitWidth, 200)
                    }

                    // Rename editor: shown on double-click, commits on Enter / focus
                    // loss, cancels on Escape. Empty input is ignored by renameTab.
                    TextInput {
                        id: tabEdit
                        visible: tabItem.editing
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.right: closeBtn.left
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        clip: true
                        color: Theme.textPrimary
                        selectionColor: Theme.selectionFill
                        selectedTextColor: Theme.textPrimary
                        font.family: Theme.uiFont
                        font.pixelSize: 12

                        function begin() {
                            text = tabItem.model.title
                            tabItem.editing = true
                            forceActiveFocus()
                            selectAll()
                        }
                        function commit() {
                            if (!tabItem.editing)
                                return
                            tabItem.editing = false
                            strip.tabs.renameTab(tabItem.index, text)
                        }
                        onEditingFinished: commit() // Enter or focus loss
                        Keys.onEscapePressed: {
                            text = tabItem.model.title       // discard the edit
                            tabItem.editing = false
                        }
                    }

                    Text {
                        id: closeBtn
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !tabItem.editing
                        text: "\u00D7" // multiplication sign
                        color: closeHover.hovered ? Theme.textPrimary : Theme.textFaint
                        font.family: Theme.uiFont
                        font.pixelSize: 14
                        // Passive hover (color only); clicks are resolved by the
                        // single MouseArea below via a hit-test, so there is no
                        // competing tap grabber.
                        HoverHandler { id: closeHover }
                    }

                    Rectangle {
                        id: tabUnderlineActive
                        // The underline doubles as this tab's scan
                        // progress bar. Idle: exactly the progress-free look (full
                        // accent underline on the focused tab, invisible
                        // otherwise). Scanning: the width tracks progress,
                        // growing left-to-right from the same left edge the
                        // full underline occupies, accent while this tab is
                        // focused and Theme.accentSoft (the lighter purple)
                        // while it is not, switching live if focus moves
                        // mid-scan; at scanFinished the roles reset and the
                        // idle look returns on its own. The width Behavior
                        // smooths the batch increments and is enabled ONLY
                        // while scanning, so ordinary tab switches never
                        // animate the full underline.
                        readonly property bool active:
                            tabItem.index === strip.tabs.currentIndex
                        readonly property bool scanning:
                            tabItem.model.scanning === true
                        readonly property real fullW:
                            tabLabel.width + closeBtn.width + 12
                        anchors.bottom: parent.bottom
                        x: (tabItem.width - fullW) / 2
                        height: 2
                        width: scanning
                               ? fullW * Math.max(0, Math.min(1,
                                     tabItem.model.scanProgress))
                               : fullW
                        color: scanning
                               ? (active ? Theme.accent : Theme.accentSoft)
                               : (active ? Theme.accent : "transparent")
                        Behavior on width {
                            enabled: tabUnderlineActive.scanning
                            NumberAnimation {
                                duration: 120
                                easing.type: Easing.OutQuad
                            }
                        }
                    }

                    MouseArea {
                        id: tabMouse
                        anchors.fill: parent
                        enabled: !tabItem.editing
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        hoverEnabled: false
                        property real pressX: 0
                        property bool moved: false
                        property bool pressOnClose: false

                        // This MouseArea is what sits under the cursor over a tab,
                        // so it is where wheel events land. Forward them to the
                        // strip's scroll. (Wheel is independent of the press/drag
                        // paths below, so this does not disturb click/reorder.)
                        onWheel: function (wheel) { strip._onWheel(wheel) }

                        onDoubleClicked: function (mouse) {
                            if (mouse.button === Qt.LeftButton)
                                tabEdit.begin()
                        }

                        onPressed: function (mouse) {
                            if (mouse.button === Qt.MiddleButton)
                                return
                            edgeScroll.stop() // clear any stale edge-scroll
                            pressX = mapToItem(strip, mouse.x, mouse.y).x
                            moved = false
                            // Did the press land on the close glyph? (mouse.x is
                            // tab-local since this fills the tab.)
                            pressOnClose = mouse.x >= (tabItem.width - closeBtn.width - 12)
                            strip._dragFrom = tabItem.index
                            strip._dragging = false
                        }
                        onPositionChanged: function (mouse) {
                            if (!(mouse.buttons & Qt.LeftButton) || pressOnClose)
                                return
                            var sx = mapToItem(strip, mouse.x, mouse.y).x
                            if (!moved && Math.abs(sx - pressX) > 6) {
                                moved = true
                                strip._dragging = true
                                strip._dragSx = sx
                                edgeScroll.start() // watch for edge auto-scroll
                            }
                            if (moved) {
                                strip._dragSx = sx
                                strip._dragTo = strip._boundaryAt(sx)
                            }
                        }
                        onReleased: function (mouse) {
                            if (mouse.button === Qt.MiddleButton) {
                                strip.tabs.closeTab(tabItem.index)
                                return
                            }
                            if (moved && strip._dragFrom >= 0 && strip._dragTo >= 0) {
                                // Boundary -> final index (removing the source shifts
                                // everything after it left by one).
                                var to = strip._dragTo > strip._dragFrom
                                       ? strip._dragTo - 1 : strip._dragTo
                                if (to !== strip._dragFrom)
                                    strip.tabs.moveTab(strip._dragFrom, to)
                            } else if (pressOnClose) {
                                // closeTab destroys THIS delegate, so return before the
                                // cleanup below; touching `strip` afterwards would fail
                                // its id resolution on the torn-down context.
                                strip.tabs.closeTab(tabItem.index)
                                return
                            } else {
                                strip._switchTo(tabItem.index)
                            }
                            strip._dragging = false
                            strip._dragFrom = -1
                            strip._dragTo = -1
                            moved = false
                            pressOnClose = false
                            edgeScroll.stop()
                        }
                    }
                }
            }
        }
    }

    // Insertion indicator during a reorder drag. Lives on `strip` (NOT inside the
    // clipped viewport) so it draws cleanly, but its x is clamped to the viewport
    // bounds so it can never paint over the arrow cluster or the status panel.
    Rectangle {
        id: dropMarker
        width: 2
        color: Theme.accentSoft
        visible: strip._dragging && strip._dragTo >= 0
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        x: {
            if (!(strip._dragging && strip._dragTo >= 0))
                return 0
            var bx = strip._boundaryX(strip._dragTo)
            var lo = tabsViewport.x
            var hi = tabsViewport.x + tabsViewport.width - width
            return Math.max(lo, Math.min(bx, hi))
        }
    }

    // --- Scroll arrow cluster (left / right chevrons), by the status -------
    //
    // Both arrows appear together while overflowing and collapse to zero width
    // otherwise (so the viewport reclaims the space). Each dims to inert at its
    // travel limit, which holds the cluster width constant as you scroll so the
    // tabs slide without the strip reflowing under them.
    Row {
        id: arrowCluster
        anchors.right: parent.right
        anchors.rightMargin: strip.rightInset
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        spacing: 0

        // Left arrow: reveal earlier tabs.
        Rectangle {
            id: leftArrow
            visible: strip.overflow
            width: strip.arrowWidth
            height: parent.height
            color: (strip.canScrollLeft && leftArrowMouse.containsMouse)
                   ? Theme.surfaceRaised : "transparent"

            Text {
                anchors.centerIn: parent
                text: "\u2039" // single left angle quote
                font.family: Theme.uiFont
                font.pixelSize: 16
                color: !strip.canScrollLeft ? "#4A4A4A"
                     : (leftArrowMouse.containsMouse ? Theme.textPrimary : Theme.textInactive)
            }

            MouseArea {
                id: leftArrowMouse
                anchors.fill: parent
                hoverEnabled: true
                enabled: strip.canScrollLeft
                onClicked: strip._stepLeft()
            }
        }

        // Right arrow: reveal later tabs.
        Rectangle {
            id: rightArrow
            visible: strip.overflow
            width: strip.arrowWidth
            height: parent.height
            color: (strip.canScrollRight && rightArrowMouse.containsMouse)
                   ? Theme.surfaceRaised : "transparent"

            Text {
                anchors.centerIn: parent
                text: "\u203A" // single right angle quote
                font.family: Theme.uiFont
                font.pixelSize: 16
                color: !strip.canScrollRight ? "#4A4A4A"
                     : (rightArrowMouse.containsMouse ? Theme.textPrimary : Theme.textInactive)
            }

            MouseArea {
                id: rightArrowMouse
                anchors.fill: parent
                hoverEnabled: true
                enabled: strip.canScrollRight
                onClicked: strip._stepRight()
            }
        }
    }

    // --- Wheel scrolling ---------------------------------------------------
    //
    // Handled on the MouseAreas under the cursor (the tabs' tabMouse and the
    // emptyArea), forwarding to strip._onWheel above. See that function's note
    // for why a strip-level WheelHandler does not work in this layout.

    // --- File drag-and-drop onto the strip ---------------------------------
    DropArea {
        anchors.fill: parent
        onEntered: function (drag) {
            if (!drag.hasUrls) {
                drag.accepted = false
                return
            }
            strip._proactive = false
            hoverTimer.restart()
        }
        onExited: hoverTimer.stop()
        onDropped: function (drop) {
            hoverTimer.stop()
            if (!drop.hasUrls || !strip.tabs)
                return

            var t0 = Date.now()
            // Hand drop.urls to C++ in ONE property access; see the
            // PlaylistView body DropArea for the full note (the getter
            // re-decodes the payload per access and a QML sequence value is a
            // live reference, so per-element JS access is O(N^2)). The
            // .rwfpl-per-tab / one-tab split lives in
            // PlaylistTabs::dropUrlsIntoNewTab; _proactive tells it whether the
            // 2 s hover already popped the destination tab.
            var n = strip.tabs.dropUrlsIntoNewTab(drop.urls, strip._proactive)
            drop.accept(Qt.CopyAction)
            strip._proactive = false
            if (strip.log)
                strip.log.append("info", "drop: " + n
                                 + " url(s) dispatched in "
                                 + (Date.now() - t0) + " ms")
        }
    }
}
