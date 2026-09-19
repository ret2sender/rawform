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

// WindowResizeGrips.qml
//
// Resize affordances for a FRAMELESS window. A frameless window has no
// compositor-drawn frame, and the frame is where a window system puts its
// resize edges; without this, a Qt.FramelessWindowHint window on Wayland
// cannot be resized at all (X11 window managers drop the borders too). On
// macOS the main window is titled and AppKit supplies the edges itself, so
// the host gates this off there (a second set of grips would fight the
// native ones); the tool windows are frameless everywhere and use it on
// every platform.
//
// Anchor it to fill the window's root item and DECLARE IT LAST so it sits
// above the chrome in z-order; the grips are thin transparent strips along
// the edges plus small corner squares, so they steal nothing from the body
// except those few pixels.
//
// Each grip hands the drag to the window system through startSystemResize,
// which is the only correct route on Wayland (the client cannot set its own
// position) and is what X11 (_NET_WM_MOVERESIZE) and macOS honor as well.
// Where a platform declines (returns false), an incremental-delta fallback
// resizes manually, but ONLY for the right/bottom family: those grips ride
// the moving edge under a stationary cursor, so the delta is well defined.
// The left/top family cannot be emulated (it needs x/y writes, forbidden on
// Wayland and jittery on X11), so those grips are system-resize only.
//
// One implementation shared by the main window and every tool window.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window

Item {
    id: root

    // The window to resize. Defaults to the window this item lives in, so
    // hosts only need to set it when the root item belongs to a different
    // window than the one meant to resize (no host does). The `as`
    // assertion bridges a type split in the tooling: the attached
    // Window.window is declared as the C++ base (QQuickWindow), while the
    // QML Window type is its QML subclass, so the plain binding is an
    // incompatible-type lint error even though every window this app creates
    // comes from QML and is that subclass at runtime (the assertion yields
    // null otherwise, which the grips already guard).
    property Window target: root.Window.window as Window

    // Keeps the top-edge grip (and the top corners) off the title bar's
    // drag zone, so a press there starts a MOVE, not a resize.
    property real topInset: 0

    // Thickness of the edge strips and the side of the corner squares.
    property real edgeSize: 6
    property real cornerSize: 14

    anchors.fill: parent

    component Grip: MouseArea {
        id: grip

        property int edges: 0
        property real _px: 0
        property real _py: 0

        // Drop out of the picture entirely when disabled: an enabled:false
        // MouseArea still eats hover for its cursor shape.
        visible: root.enabled
        enabled: root.enabled

        // A system resize is the whole gesture as far as Qt is concerned:
        // once accepted, no further press/move events reach this area, so
        // the fallback below never runs in that case.
        onPressed: function (mouse) {
            grip._px = mouse.x
            grip._py = mouse.y
            if (root.target && root.target.startSystemResize(grip.edges))
                mouse.accepted = true
        }
        onPositionChanged: function (mouse) {
            if (!grip.pressed || !root.target)
                return
            var w = root.target
            if (grip.edges & Qt.RightEdge)
                w.width = Math.max(w.minimumWidth, w.width + (mouse.x - grip._px))
            if (grip.edges & Qt.BottomEdge)
                w.height = Math.max(w.minimumHeight, w.height + (mouse.y - grip._py))
        }
    }

    // ----- edges -----------------------------------------------------------
    Grip {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: root.topInset + root.cornerSize
        anchors.bottomMargin: root.cornerSize
        width: root.edgeSize
        cursorShape: Qt.SizeHorCursor
        edges: Qt.LeftEdge
    }
    Grip {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: root.topInset + root.cornerSize
        anchors.bottomMargin: root.cornerSize
        width: root.edgeSize
        cursorShape: Qt.SizeHorCursor
        edges: Qt.RightEdge
    }
    Grip {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: root.cornerSize
        anchors.rightMargin: root.cornerSize
        height: root.edgeSize
        cursorShape: Qt.SizeVerCursor
        edges: Qt.TopEdge
    }
    Grip {
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: root.cornerSize
        anchors.rightMargin: root.cornerSize
        height: root.edgeSize
        cursorShape: Qt.SizeVerCursor
        edges: Qt.BottomEdge
    }

    // ----- corners ---------------------------------------------------------
    // The top corners sit BELOW topInset: the title bar's own drag zone owns
    // the very top strip, and a corner square poking into it would turn the
    // outermost title bar pixels into a resize rather than a move.
    Grip {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: root.topInset
        width: root.cornerSize
        height: root.cornerSize
        cursorShape: Qt.SizeFDiagCursor
        edges: Qt.LeftEdge | Qt.TopEdge
    }
    Grip {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: root.topInset
        width: root.cornerSize
        height: root.cornerSize
        cursorShape: Qt.SizeBDiagCursor
        edges: Qt.RightEdge | Qt.TopEdge
    }
    Grip {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: root.cornerSize
        height: root.cornerSize
        cursorShape: Qt.SizeBDiagCursor
        edges: Qt.LeftEdge | Qt.BottomEdge
    }
    Grip {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: root.cornerSize
        height: root.cornerSize
        cursorShape: Qt.SizeFDiagCursor
        edges: Qt.RightEdge | Qt.BottomEdge
    }
}
