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

// ThemedComboBox.qml
//
// The app's combo box: a Basic ComboBox with every visual piece replaced (background,
// indicator, popup, popup rows) so callers get the rawform look without restating it,
// and so the popup, which Basic paints from the platform palette, carries the same
// frosted backdrop and fades as ThemedMenu (FrostedPopupBackground).
//
// The content item is the one part callers own, because it is where purpose differs:
// the default here is a read-only eliding label (a device picker), and an editable
// caller replaces it with a TextField wired to editText (a preset name that doubles
// as a save-as field). An override should carry the same rightPadding as the default
// so its text stops short of the indicator.
//
// Vertical padding is zero and the background's implicit height rules: Basic sizes a
// ComboBox from max(background, content + padding), and a TextField content item
// brings its own 6 px padding, which would otherwise make an editable instance taller
// than a read-only one.
//
// textRole is required: the popup row reads model[textRole], Basic's own expression,
// which resolves for a JS array of objects and for a C++ list model alike.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Controls.impl
import com.rawform.app

ComboBox {
    id: control

    // True while the default label is in place and eliding, for a caller that shows
    // the full text some other way (the device picker's tooltip). Always false under
    // an overridden content item, whose truncation is the caller's to observe.
    readonly property bool displayTruncated: control.contentItem === label && label.truncated

    font.family: Theme.uiFont
    font.pixelSize: 12
    spacing: 6
    leftPadding: 6
    rightPadding: 6
    topPadding: 0
    bottomPadding: 0

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 28
        radius: 4
        color: Theme.rowEven
        border.width: control.visualFocus ? 1 : 0
        border.color: Theme.accentSoft
    }

    contentItem: Text {
        id: label
        rightPadding: control.indicator.width + control.spacing
        text: control.displayText
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    // Basic's own arrow asset, recolored; the impl import exists for this and is the
    // one Basic itself uses.
    indicator: ColorImage {
        x: control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        color: Theme.textSecondary
        source: "qrc:/qt-project.org/imports/QtQuick/Controls/Basic/images/double-arrow.png"
        opacity: control.enabled ? 1 : 0.4
    }

    delegate: ItemDelegate {
        id: entry
        required property var model
        required property int index
        width: ListView.view.width
        height: 26
        highlighted: control.highlightedIndex === index
        hoverEnabled: control.hoverEnabled

        contentItem: Text {
            text: entry.model[control.textRole]
            font: control.font
            color: Theme.textPrimary
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: 3
            color: entry.highlighted ? Theme.surfaceControlHover : "transparent"
        }
    }

    popup: Popup {
        id: dropdown
        y: control.height + 2
        width: control.width
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        padding: 4

        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 110; easing.type: Easing.OutQuad }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 110; easing.type: Easing.InQuad }
        }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            // Bound only while open: Basic's own guard, which keeps the rows from
            // being built for a popup nobody has dropped.
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        background: FrostedPopupBackground { popup: dropdown }
    }
}
