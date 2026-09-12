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

// Bound component behavior: nested components and delegates resolve outer
// document ids statically instead of through dynamic context lookup. The
// flip side is that views no longer inject model data into delegates via
// context; every delegate in this file declares what it consumes as
// `required property`, which is both the contract and the qmllint proof.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

SplitView {
    id: root

    handle: Rectangle {
        id: handleDelegate
        color: "transparent"
        implicitHeight: 6
        implicitWidth: 12

        property color midColor: SplitHandle.hovered ? "#555555" : "transparent"
        Behavior on midColor { ColorAnimation { duration: 250 } }

        Rectangle {
            anchors.centerIn: parent
            gradient: Gradient {
                orientation: root.orientation === Qt.Horizontal ? Qt.Vertical : Qt.Horizontal
                GradientStop { position: 0.0; color: "transparent" }
                GradientStop { position: 0.5; color: handleDelegate.midColor }
                GradientStop { position: 1.0; color: "transparent" }
            }
            height: root.orientation === Qt.Horizontal ? parent.height / 3 : 3
            width: root.orientation === Qt.Horizontal ? 3 : parent.width / 3
            x: root.orientation === Qt.Horizontal ? 0 : parent.width / 2
            y: root.orientation === Qt.Horizontal ? parent.height / 2 : 0
        }
    }
}