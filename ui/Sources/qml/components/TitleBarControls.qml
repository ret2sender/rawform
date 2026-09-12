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
import QtQuick.Controls.Basic
import QtQuick.Layouts

import com.rawform.app


/*!
    Caption buttons for the frameless title bar.

    Minimize, maximize/restore, and close, in that visual order. The
    maximize button is profile-gated (TitleBarStyle.showMaximize): present
    on the Linux profiles, absent on the Windows profile (its icon set
    carries no maximize artwork). macOS is not handled here, as the parent supplies the native
    traffic lights instead.

    This is a Control so the whole caption chunk can be padded with the
    stock leftPadding/rightPadding/topPadding/bottomPadding contract: the
    Control derives its implicit size from contentItem plus padding, and
    the parent layout sees one box that already includes the gap.

    All per-platform values come from TitleBarStyle.
*/
Control {
    id: root

    leftPadding: TitleBarStyle.paddingLeft
    rightPadding: TitleBarStyle.paddingRight
    topPadding: TitleBarStyle.paddingTop
    bottomPadding: TitleBarStyle.paddingBottom

    component CaptionButton: Button {
        id: captionButton

        required property url iconSource
        // Glyph swapped in while hovered. Profiles that do not invert
        // simply pass the same url as iconSource, so the swap is a no-op
        // there; the Image source binding sees no change and does not
        // reload.
        required property url hoverIconSource
        required property real iconWidth
        required property real iconHeight
        required property color restColor
        required property color hoverColor

        // Applied to whichever top corner faces the window edge.
        property real cornerRadius: 0

        hoverEnabled: true
        implicitHeight: TitleBarStyle.buttonHeight
        implicitWidth: TitleBarStyle.buttonWidth

        // The Item is a transparent carrier sized to the button cell; the
        // Rectangle inside it is the actual fill. It is sized explicitly
        // rather than inset from the cell: the cell is not square, so only
        // an explicit square side can render a true circle. The whole cell
        // stays clickable and hoverable regardless of plate size.
        background: Item {
            Rectangle {
                id: backgroundPlate

                // Full-cell mode is where the plate reaches the window
                // edge, and therefore the only mode where the edge-corner
                // rounding (cornerRadius) can apply; a centered plate takes
                // the uniform backgroundRadius on every corner instead.
                readonly property bool fullCell: TitleBarStyle.backgroundSize === 0

                anchors.centerIn: parent
                width: backgroundPlate.fullCell ? parent.width : TitleBarStyle.backgroundSize
                height: backgroundPlate.fullCell ? parent.height : TitleBarStyle.backgroundSize

                color: captionButton.hovered
                    ? captionButton.hoverColor
                    : captionButton.restColor

                radius: TitleBarStyle.backgroundRadius
                topLeftRadius: backgroundPlate.fullCell && TitleBarStyle.controlsOnLeft
                    ? captionButton.cornerRadius
                    : TitleBarStyle.backgroundRadius
                topRightRadius: backgroundPlate.fullCell && !TitleBarStyle.controlsOnLeft
                    ? captionButton.cornerRadius
                    : TitleBarStyle.backgroundRadius

                Behavior on color {
                    ColorAnimation { duration: TitleBarStyle.hoverDuration }
                }
            }
        }

        contentItem: AppIcon {
            iconHeight: captionButton.iconHeight
            iconWidth: captionButton.iconWidth
            opacity: captionButton.Window.active ? 1.0 : TitleBarStyle.inactiveOpacity
            source: captionButton.hovered
                ? captionButton.hoverIconSource
                : captionButton.iconSource

            // Dim the caption glyphs while the window is unfocused.
            Behavior on opacity {
                NumberAnimation { duration: TitleBarStyle.dimDuration }
            }
        }
    }

    contentItem: RowLayout {
        id: controlsRow

        spacing: TitleBarStyle.spacing

        // Mirrors the row so close stays outboard on either side.
        layoutDirection: TitleBarStyle.controlsOnLeft ? Qt.RightToLeft : Qt.LeftToRight

        CaptionButton {
            iconSource: TitleBarStyle.minimizeIcon
            hoverIconSource: TitleBarStyle.minimizeHoverIcon
            iconWidth: TitleBarStyle.iconWidth
            iconHeight: TitleBarStyle.iconHeight
            restColor: TitleBarStyle.controlBackground
            hoverColor: TitleBarStyle.controlHover

            onClicked: root.Window.window.showMinimized()
        }

        CaptionButton {
            id: maximizeButton

            // Window.visibility is the ground truth for the toggle: it also
            // tracks maximization performed by the window manager itself
            // (title bar double-click, edge snapping, taskbar actions), so
            // the glyph and the click action never desynchronize from a
            // state change this button did not initiate.
            readonly property bool windowMaximized:
                root.Window.visibility === Window.Maximized

            visible: TitleBarStyle.showMaximize

            iconSource: maximizeButton.windowMaximized
                ? TitleBarStyle.restoreIcon
                : TitleBarStyle.maximizeIcon
            hoverIconSource: maximizeButton.windowMaximized
                ? TitleBarStyle.restoreHoverIcon
                : TitleBarStyle.maximizeHoverIcon
            iconWidth: TitleBarStyle.iconWidth
            iconHeight: TitleBarStyle.iconHeight
            restColor: TitleBarStyle.controlBackground
            hoverColor: TitleBarStyle.controlHover

            onClicked: maximizeButton.windowMaximized
                ? root.Window.window.showNormal()
                : root.Window.window.showMaximized()
        }

        CaptionButton {
            iconSource: TitleBarStyle.closeIcon
            hoverIconSource: TitleBarStyle.closeHoverIcon
            iconWidth: TitleBarStyle.iconWidth
            iconHeight: TitleBarStyle.iconHeight
            restColor: TitleBarStyle.closeBackground
            hoverColor: TitleBarStyle.closeHover
            cornerRadius: TitleBarStyle.closeCornerRadius

            onClicked: root.Window.window.close()
        }
    }
}
