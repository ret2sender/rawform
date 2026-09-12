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

/*
 * Shared header cell for HorizontalHeaderView delegates (the metadata pane
 * and the playlist header). It carries the
 * foobar-style header look: a flat #181818 band with a bold, elided title.
 *
 * Deliberately small and host-agnostic so the reuse is real, not a copy:
 *  - It reads `display` as a required delegate property (the view injects it),
 *    never an ancestor id, so it is not tied to one host's scope.
 *  - `alignment`, `showDivider`, and `dividerColor` cover the per-column
 *    differences between the two headers (the playlist right-aligns some
 *    columns and draws a divider; the metadata pane wants neither by default).
 *  - It owns NO interaction. A host that needs a draggable/reorderable header
 *    composes this cell with its own MouseArea as a sibling in the delegate,
 *    rather than this component growing a content slot it cannot test in
 *    isolation. That keeps the load-bearing reorder logic out of here.
 *
 * Font: the Theme singleton's UI family, resolvable in any window; this
 * component has no reliance on ids in the surrounding scope.
 */
Rectangle {
    id: cell

    // Injected by the HorizontalHeaderView for each column (the header title).
    required property var display

    // Per-column presentation knobs. Defaults match the metadata header: a plain
    // left-aligned title with no divider.
    property int   alignment: Text.AlignLeft
    property bool  showDivider: false
    property color dividerColor: "#494949"

    // The header has no rowHeightProvider, so this implicitHeight is what sets
    // the header's row height (unlike body cells, which the rowHeightProvider
    // governs).
    implicitHeight: 26
    color: Theme.headerBand

    Text {
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.right: parent.right
        anchors.rightMargin: 8
        text: cell.display
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 12
        font.weight: Font.Bold
        elide: Text.ElideRight
        horizontalAlignment: cell.alignment
        verticalAlignment: Text.AlignVCenter
    }

    // Optional column divider on the right edge (off by default; the playlist
    // header will switch it on for its inter-column boundaries).
    Rectangle {
        visible: cell.showDivider
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 2
        height: parent.height - 3
        color: cell.dividerColor
    }
}
