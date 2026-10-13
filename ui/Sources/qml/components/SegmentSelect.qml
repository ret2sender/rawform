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

// SegmentSelect.qml
//
// The settings panes' segmented selector: a row of labeled segments of which
// exactly one is picked (Off / Track / Album, Output / Source, the ID3
// choices). The picked index IS the value: callers pass the labels in enum
// order and map `picked` straight to the setting.
//
// Each segment is a checkable AbstractButton, so the strip behaves like a
// radio group for the keyboard and for assistive technology: Tab reaches it,
// Space picks the focused segment, Left/Right (and Home/End) move the pick
// and the focus together, and every segment reports as a radio button with
// its label as its name and its checked state. The focus ring follows
// ThemedSwitch: a 1 px Theme.textPrimary stroke for keyboard focus only.
//
// The instantiation site owns `labels`, `value`, `segWidth` and `onPicked`.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import com.rawform.app

Row {
    id: root

    /// Segment labels, in value order.
    property var labels: []
    /// Width of each segment.
    property int segWidth: 68
    /// The picked index.
    property int value: 0
    /// A segment was picked (click, Space, or an arrow key).
    signal picked(int value)

    spacing: 0

    // Pick `index` from the keyboard: the pick and the focus move together,
    // like a radio group's arrows, so the ring stays on the picked segment.
    function _pickFromKey(index) {
        var count = root.labels.length
        if (count === 0)
            return
        var next = Math.max(0, Math.min(count - 1, index))
        root.picked(next)
        var item = segments.itemAt(next)
        if (item)
            item.forceActiveFocus(Qt.TabFocusReason)
    }

    Repeater {
        id: segments
        model: root.labels
        delegate: AbstractButton {
            id: seg
            required property int index
            required property string modelData

            width: root.segWidth
            height: 26
            padding: 0
            hoverEnabled: true
            checkable: true
            // Exclusive so a click on the picked segment cannot uncheck it;
            // the strip owns the check through `value`, a click only reports,
            // and the C++-side flip of `checked` on click leaves the binding
            // in place (the ThemedSwitch rule).
            autoExclusive: true
            checked: root.value === seg.index
            onClicked: root.picked(seg.index)

            Accessible.role: Accessible.RadioButton
            Accessible.name: seg.modelData

            Keys.onLeftPressed: function (event) {
                root._pickFromKey(seg.index - 1); event.accepted = true
            }
            Keys.onRightPressed: function (event) {
                root._pickFromKey(seg.index + 1); event.accepted = true
            }
            Keys.onPressed: function (event) {
                if (event.key === Qt.Key_Home) {
                    root._pickFromKey(0); event.accepted = true
                } else if (event.key === Qt.Key_End) {
                    root._pickFromKey(root.labels.length - 1); event.accepted = true
                }
            }

            background: Rectangle {
                color: seg.checked ? Theme.accentSoft
                     : seg.hovered ? Theme.surfaceControlHover : Theme.surfaceControl
                border.color: seg.visualFocus ? Theme.textPrimary : Theme.border
                border.width: 1
            }

            contentItem: Text {
                text: seg.modelData
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                color: seg.checked ? Theme.textOnAccent : Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: Theme.fontSizeSmall
                font.weight: seg.checked ? Font.Bold : Font.Normal
            }
        }
    }
}
