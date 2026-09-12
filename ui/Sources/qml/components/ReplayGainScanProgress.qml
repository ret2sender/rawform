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


// =============================================================================
// ReplayGainScanProgress.qml
//
// The scan progress overlay, shown inside the Properties window while a
// ReplayGain scan runs. It is not a separate window: it fills the window body,
// dims and blocks the content beneath (a full-bleed MouseArea swallows clicks),
// and centers a small card with the current file, an overall progress bar, and a
// Cancel button. Visibility is bound straight to the controller's busy flag, so it
// appears when a scan starts and vanishes when it ends or is canceled.
//
// It is purely a view over the controller: currentName / doneTracks / totalTracks
// / overallProgress drive the readout, and Cancel calls controller.cancel(). It
// holds no state of its own.
// =============================================================================
Item {
    id: root

    property var controller: null   // ReplayGainScanController
    property string uiFont: Theme.uiFont

    visible: controller !== null && controller.busy

    // Dim + input block. The MouseArea catches every press so the pane and footer
    // beneath cannot be touched mid-scan.
    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: 0.55
        MouseArea { anchors.fill: parent }
    }

    // The card.
    Rectangle {
        anchors.centerIn: parent
        width: 400
        height: cardCol.implicitHeight + 36
        radius: 8
        color: Theme.surfacePage
        border.color: Theme.separatorStrong
        border.width: 1

        Column {
            id: cardCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            spacing: 14

            Text {
                width: parent.width
                text: "Scanning ReplayGain"
                color: Theme.textPrimary
                font.family: root.uiFont
                font.pixelSize: 13
                font.weight: Font.Bold
            }

            Text {
                width: parent.width
                elide: Text.ElideMiddle
                text: {
                    if (!root.controller || root.controller.totalTracks <= 0)
                        return ""
                    var pos = (root.controller.doneTracks + 1) + " of " + root.controller.totalTracks
                    var name = root.controller.currentName
                    return name.length > 0 ? (name + "   (" + pos + ")") : pos
                }
                color: Theme.textInactive
                font.family: root.uiFont
                font.pixelSize: 12
            }

            // Overall progress (track count plus within-track fraction).
            Rectangle {
                width: parent.width
                height: 6
                radius: 3
                color: Theme.surfaceRaised

                Rectangle {
                    height: parent.height
                    radius: 3
                    color: Theme.accent
                    width: parent.width *
                           (root.controller ? root.controller.overallProgress : 0)
                }
            }

            // Cancel, right-aligned.
            Item {
                width: parent.width
                height: 30

                Rectangle {
                    id: cancelBtn
                    anchors.right: parent.right
                    width: Math.max(72, cancelText.implicitWidth + 28)
                    height: 30
                    radius: 5
                    color: cancelHover.hovered ? Theme.surfaceControlHover : Theme.buttonFace
                    border.color: Theme.border
                    border.width: 1

                    Text {
                        id: cancelText
                        anchors.centerIn: parent
                        text: "Cancel"
                        color: Theme.textPrimary
                        font.family: root.uiFont
                        font.pixelSize: 12
                        font.weight: Font.Bold
                    }
                    HoverHandler { id: cancelHover }
                    TapHandler { onTapped: if (root.controller) root.controller.cancel() }
                }
            }
        }
    }
}
