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

// TitleBar.qml
//
// The main window's custom title bar: the rawform logo pinned at a
// platform-dependent left inset, a fill-width spacer, and the caption buttons
// (TitleBarControls) at the right. A full-bleed MouseArea emits moveRequested
// on press so the host can start a system move.
//
// On macOS the native traffic lights (see applyMacOSStyling) replace the
// themed controls, which are hidden and disabled there, and the logo inset
// animates toward the window edge in fullscreen, where the native title bar
// and the lights slide away. Linux hugs the edge unconditionally; Windows
// keeps the 80 px inset for cross-platform parity.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import com.rawform.app

Item {
    id: root
    implicitHeight: 48

    signal moveRequested()

    readonly property bool usesCustomControls: Qt.platform.os !== "osx"

    MouseArea {
        id: moveArea
        anchors.fill: parent

        onPressed: root.moveRequested()
    }

    RowLayout {
        id: titleBarControlsRow
        anchors.fill: parent
        spacing: 12

        Item {
            id: rawformLogoSpacer

            // In macOS fullscreen the traffic lights (and the whole native
            // title bar) slide away, so the inset that normally clears them
            // would leave the logo floating in empty space; tuck it toward
            // the window edge instead. All other platforms and window states
            // keep the standard inset for cross-platform visual parity.
            readonly property bool macFullScreen: Qt.platform.os === "osx"
                && root.Window.visibility === Window.FullScreen

            // Linux has no traffic lights to clear, so the logo hugs the window edge
            // there unconditionally; macOS gets the same treatment only in fullscreen,
            // where the native title bar (and the lights) slide away. macOS windowed and
            // Windows keep the 80px inset that clears the traffic lights / preserves
            // cross-platform parity.
            property real logoInset: macFullScreen
                || Qt.platform.os === "linux" ? 16 : 80

            Layout.leftMargin: rawformLogoSpacer.logoInset

            Behavior on logoInset {
                NumberAnimation {
                    duration: 200
                    easing.type: Easing.OutCubic
                }
            }
        }

        Image {
            id: rawformLogo
            source: "../../images/rawform_logo.png"
            Layout.topMargin: -10
        }

        // The trailing spacer absorbs all the slack between the logo and the
        // right edge, which is what keeps the logo pinned to its left inset.
        // That job is needed on EVERY platform, so this stays present always:
        // it is an empty Item with no visual, it only consumes layout space.
        // Only the controls themselves are platform-specific (below).
        //
        // Do NOT condition `visible` on the platform: Layouts skip invisible
        // items, and with no visible fillWidth item the layout engine
        // redistributes the surplus width across the remaining columns,
        // which shoves the logo toward the window center (the scenario this
        // prevents, on macOS where the controls column is empty).
        Item {
            id: titleBarControlsSpacer
            Layout.fillWidth: true
        }

        // On macOS the traffic-light controls (drawn natively by AppKit, see
        // applyMacOSStyling) take the place of these, so the themed QML
        // controls are hidden and disabled there. Linux and Windows keep
        // them. The logo inset above is left at the SAME
        // value on every platform, so the rawform logo lands in the same spot
        // regardless of OS; on macOS that same inset also clears the traffic
        // lights at the top-left.
        TitleBarControls {
            id: titleBarControls
            Layout.fillHeight: true
            enabled: root.usesCustomControls
            visible: root.usesCustomControls
        }
    }
}
