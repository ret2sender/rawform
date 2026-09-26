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

// SettingsRow.qml
//
// One row of a Settings pane: the label with its modified-from-default dot, the
// right-click "Reset to default", and the pane's control, injected as the default
// property. The row emits reset(); the pane's handler writes the staged value
// back to its default, so the row never knows what it is resetting.
//
// The injected control lands in the row's RowLayout after the label (the default
// property aliases the layout's data), so its Layout attached properties work as
// for any layout child: a fillWidth control takes the remainder, anything else
// keeps its preferred width and the layout packs left. Under ComponentBehavior:
// Bound the control still resolves its pane's ids, since it is bound to the
// declaring file's scope, not to this one.
//
// The right-click MouseArea is the LOWEST child and accepts only the right
// button, so the control above handles its own left interaction and right clicks
// fall through to reset. It spans the full row, empty space included.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import com.rawform.app

Item {
    id: row

    property alias text: label.text
    property alias modified: label.modified

    // The label column. -1 is Layout's own "use the implicit width", for a pane
    // that does not align its controls into a column.
    property real labelWidth: -1

    // The gap between the label and the control.
    property int gutter: 24

    // The pane's one reset menu. The row points it at itself before popping it,
    // so the menu's action reaches back to this row's reset().
    required property SettingsResetMenu resetMenu

    // The label's natural width, for the pane's column maximum. Constant across
    // the modified state (the dot hides by opacity and keeps its slot).
    readonly property real labelImplicitWidth: label.implicitWidth

    // Emitted by the reset menu; the pane restores the default here.
    signal reset()

    default property alias control: layout.data

    implicitHeight: 46

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: {
            row.resetMenu.target = row
            row.resetMenu.popup()
        }
    }

    // Fills the row rather than taking only a width, so the layout's default
    // vertical centering works against the full row height whatever the
    // control's own height.
    RowLayout {
        id: layout
        anchors.fill: parent
        spacing: row.gutter

        SettingsRowLabel {
            id: label
            Layout.preferredWidth: row.labelWidth
        }
    }
}
