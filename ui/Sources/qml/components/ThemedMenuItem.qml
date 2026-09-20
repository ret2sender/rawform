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

// ThemedMenuItem.qml
//
// The themed row inside a ThemedMenu: a QtQuick.Controls MenuItem with the
// rawform look (accent highlight, a 6 px checked dot in a fixed text gutter,
// an AppIcon sub-menu arrow, optional italic text) and three additions the
// menu relies on.
//
// `collapsed` is an intent flag for entries that must vanish from the menu
// without leaving a gap. Effective `visible` cannot carry intent (a closed
// popup propagates visible:false into every item), so this plain bool owns
// the visible/height mechanics and call sites set one property.
//
// `shortcut` makes the item the single owner of its keyboard shortcut: one
// portable sequence string gives the window-scoped trigger AND the hint drawn
// at the row's right edge, in the platform's native spelling, so the two can
// never drift apart. `shortcutAliases` adds silent extra sequences that fire
// the same item (a shifted twin of the hinted key, say) without a hint.
//
// `fitWidth` is the width this row needs, measured through a TextMetrics
// child rather than the control's deferred contentItem, so ThemedMenu can fit
// its popup width before the first show and the value self-corrects when the
// UI font finishes loading.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

MenuItem {
    id: root
    implicitHeight: 28

    property bool textItalic: false
    // Text gutter. 18 fits the 6 px checked-dot with 6 px margins either side,
    // so checkable and plain items share one gutter and stay aligned.
    property int textLeftPadding: 18

    // Intent flag for entries that should vanish from the menu (no row, no gap), e.g. the
    // custom-column header actions when the target column is native. This exists because
    // effective `visible` cannot carry intent: a closed popup propagates visible:false
    // into every item, so a call-site idiom of height: visible ? implicitHeight : 0 reads
    // 0 for every item while closed, which makes intended visibility unrecoverable
    // exactly when ThemedMenu's fit binding needs it. `collapsed` is a plain bool with no
    // parent propagation, valid whether the popup is open or closed; the item owns the
    // visible/height mechanics so call sites set one property instead of two coupled
    // ones.
    property bool collapsed: false
    height: collapsed ? 0 : implicitHeight
    visible: !collapsed

    // The item's keyboard shortcut as a portable Qt sequence ("Ctrl+S"); empty
    // for none. Window context on purpose: the shortcut fires from anywhere in
    // the host window while the menu is closed, and never fires from a tool
    // window, which is a different focus window. Qt keeps a window shortcut
    // dormant while a popup it does not belong to is open (a context menu,
    // another menu), but not while its OWN menu is open, where Space and the
    // letters must keep navigating the rows; the enabled binding below closes
    // that gap, so every open popup silences every menu shortcut alike.
    property string shortcut: ""
    // Extra sequences that trigger the item without appearing in the hint. A
    // plain JS array: Shortcut.sequences is a QVariantList and takes one as is.
    property var shortcutAliases: []
    // Gap between the label's trailing margin and the hint. 24 keeps a long
    // label and its hint readable as two columns rather than one run of text.
    readonly property int shortcutGap: 24

    Shortcut {
        id: _shortcut
        context: Qt.WindowShortcut
        // `sequence` (singular) is what nativeText spells; `sequences` adds the
        // silent aliases. Both register; an empty sequence registers nothing.
        sequence: root.shortcut
        sequences: root.shortcutAliases
        enabled: root.shortcut !== "" && root.enabled && !root.collapsed
                 && !(root.menu && root.menu.visible)
        onActivated: root.triggered()
    }

    // Width this item needs, measured WITHOUT touching the control's deferred
    // internals: contentItem does not reliably exist before a popup first
    // shows, so a ThemedMenu measuring implicitWidth would read hollow items
    // at startup and the first aboutToShow would move the popup width on the
    // same pass as its first paint (a visible first-open blur hiccup).
    // TextMetrics is an ordinary, non-deferred child: valid from item creation, and the
    // Theme.uiFont binding is live, so the value self-corrects the moment the
    // FontLoader resolves during startup, never at first open.
    // Sum mirrors the content Text: control paddings, gutter, glyph advance,
    // the hint column when there is one, then arrow clearance / trailing
    // margin (must match rightPadding below).
    readonly property real fitWidth: root.leftPadding + root.rightPadding
                                     + root.textLeftPadding
                                     + Math.ceil(_metrics.advanceWidth)
                                     + root._hintColumnWidth
                                     + (root.subMenu ? 26 : 12)

    // The hint column's contribution to the row width: the hint's advance plus
    // the gap, or nothing for an item without a shortcut, which stays
    // pixel-identical to a row that predates hints.
    readonly property real _hintColumnWidth:
        root.shortcut !== "" ? Math.ceil(_hintMetrics.advanceWidth) + root.shortcutGap
                             : 0

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

    // Same coupling for the hint Text below.
    TextMetrics {
        id: _hintMetrics
        font.family: Theme.uiFont
        font.pixelSize: 12
        text: _shortcut.nativeText
    }

    // The shortcut hint, right-aligned so every hinted row in a menu shares one
    // column edge. A child of the control rather than of contentItem: the
    // content Text reserves the column through its rightPadding, and this
    // sits over the reserved space, anchored to the same trailing margin the
    // arrow clearance uses.
    Text {
        anchors.right: parent.right
        anchors.rightMargin: root.subMenu ? 26 : 12
        anchors.verticalCenter: parent.verticalCenter
        color: !root.enabled ? "#5E5E5E"
                             : (root.highlighted ? Theme.textPrimary : Theme.textFaint)
        font.family: Theme.uiFont
        font.pixelSize: 12
        text: _shortcut.nativeText
        visible: root.shortcut !== ""
    }

    arrow: AppIcon {
        // Anchored, not fixed-coordinate: AppIcon pins the glyph to 12
        // logical px on every screen, and the anchors hold it centered at
        // any item height and flush at any popup width.
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        iconSize: 12
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
        // right margin, 6 px gap); a plain trailing margin otherwise; plus
        // the hint column when the item has a shortcut. Must stay in lockstep
        // with the same terms in fitWidth above, or a long label can run
        // under the arrow or the hint.
        rightPadding: (root.subMenu ? 26 : 12) + root._hintColumnWidth
        text: root.text
        verticalAlignment: Text.AlignVCenter
    }
}
