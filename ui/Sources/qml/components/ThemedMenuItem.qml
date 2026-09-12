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


MenuItem {
    id: root
    implicitHeight: 28

    property bool textItalic: false
    // Text gutter. 18 fits the 6 px checked-dot with 6 px margins either side,
    // so checkable and plain items share one gutter and stay aligned.
    property int textLeftPadding: 18

    // Intent flag for entries that should vanish from the menu (no row, no
    // gap), e.g. the custom-column header actions when the target column is
    // native. This exists because effective `visible` cannot carry intent: a
    // closed popup propagates visible:false into every item, so a call-site
    // idiom of height: visible ? implicitHeight : 0 reads 0 for every item
    // while closed, which makes intended visibility unrecoverable exactly
    // when ThemedMenu's fit binding needs it.
    // `collapsed` is a plain bool with no parent propagation, valid whether
    // the popup is open or closed; the item owns the visible/height
    // mechanics so call sites set one property instead of two coupled ones.
    property bool collapsed: false
    height: collapsed ? 0 : implicitHeight
    visible: !collapsed

    // Width this item needs, measured WITHOUT touching the control's deferred
    // internals: contentItem does not reliably exist before a popup first
    // shows, so a ThemedMenu measuring implicitWidth would read hollow items
    // at startup and the first aboutToShow would move the popup width on the
    // same pass as its first paint (a visible first-open blur hiccup).
    // TextMetrics is an ordinary, non-deferred child: valid from item creation, and the
    // Theme.uiFont binding is live, so the value self-corrects the moment the
    // FontLoader resolves during startup, never at first open.
    // Sum mirrors the content Text: control paddings, gutter, glyph advance,
    // then arrow clearance / trailing margin (must match rightPadding below).
    readonly property real fitWidth: root.leftPadding + root.rightPadding
                                     + root.textLeftPadding
                                     + Math.ceil(_metrics.advanceWidth)
                                     + (root.subMenu ? 26 : 12)

    // Font mirrors the content Text's font exactly; the pair is coupled, so a
    // font change below must be repeated here or measured and rendered widths
    // drift apart.
    TextMetrics {
        id: _metrics
        font.family: Theme.uiFont
        font.italic: root.textItalic
        font.pixelSize: 12
        text: root.text
    }

    arrow: Image {
        // Anchored, not fixed-coordinate: the svg keeps its intrinsic 12 px
        // size under Image.Pad, and the anchors hold it centered at any item
        // height and flush at any popup width.
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        asynchronous: true
        fillMode: Image.Pad
        source: "../../icons/app/dialogs/menu_arrow.svg"
        visible: root.subMenu
    }

    // Checked state: the modified-from-default accent dot used across the
    // settings panes, replacing the style's stock checkmark. A replaced
    // indicator is NOT positioned by the style, hence the explicit x, which
    // centers the dot in the text gutter.
    indicator: Rectangle {
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.accentSoft
        height: 6
        radius: 3
        visible: root.checkable && root.checked
        width: 6
        x: root.leftPadding + (root.textLeftPadding - width) / 2
    }

    background: Rectangle {
        color: root.highlighted ? Theme.selectionFill : "transparent"
        radius: 8
    }

    contentItem: Text {
        // Disabled items (e.g. Paste with an empty clipboard) read grayed; otherwise
        // the accent on highlight, default text color at rest.
        color: !root.enabled ? "#5E5E5E"
                             : (root.highlighted ? Theme.accentHover : Theme.textPrimary)
        font.family: Theme.uiFont
        font.italic: root.textItalic
        font.pixelSize: 12
        leftPadding: root.textLeftPadding
        // Arrow clearance when a submenu arrow is present (12 px arrow, 8 px
        // right margin, 6 px gap); a plain trailing margin otherwise. Must
        // stay in lockstep with the same term in fitWidth above, or a long
        // label can run under the arrow.
        rightPadding: root.subMenu ? 26 : 12
        text: root.text
        verticalAlignment: Text.AlignVCenter
    }
}