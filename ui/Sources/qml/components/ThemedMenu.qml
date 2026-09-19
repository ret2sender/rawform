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

// ThemedMenu.qml
//
// The app's popup menu: a QtQuick.Controls Menu restyled to the rawform look
// and shared by the menu bar, the playlist header and row context menus, and
// the tool-window footer menus. The background is a frosted slice of the host
// window: a ShaderEffectSource grabs the pixels of `blurSource` behind the
// popup, a MultiEffect blurs them and clips them to the rounded rect, and a
// translucent Theme.surfacePage tint with a 2 px border is drawn sharp on top.
// The grab region follows the popup on open and on every move or resize.
//
// Width is fit-to-content: Qt's Menu never derives a width from its items (its
// contentItem is a ListView with no implicit width), so `_fitWidth` sums the
// widest visible item's fitWidth (see ThemedMenuItem) and the menu padding,
// floored at `menuWidth`. The binding is deliberately blind to `visible` so
// the popup never resizes across open/close; see the comment on `_fitWidth`.
//
// Items default to ThemedMenuItem through `delegate`, so call sites can use
// plain Action / MenuItem declarations and still get the themed rows.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects

Menu {
    id: root
    padding: 4

    // The item whose pixels show through (blurred) behind the menu.
    // MUST be a sibling subtree of the overlay; your app content root,
    // NOT window.contentItem (that contains the overlay -> recurses to black).
    property Item blurSource: {
        const contentItem = root.contentItem
        const w = contentItem ? contentItem.Window.window : null
        return w ? w.menuBlurSource : null  // qmllint disable missing-property
    }

    // Minimum popup width. The fitted width below can only grow past it, so
    // this is a floor, not the size; call sites do not set per-menu widths.
    property int menuWidth: 160

    // Fit-to-content width: the widest visible item's fitWidth plus the
    // menu's own horizontal padding. Qt's Menu never derives this itself (its
    // contentItem is a ListView, which reports no implicit width), so the
    // popup would otherwise collapse to the background's implicit width.
    //
    // DELIBERATELY a declarative binding, not an imperative recompute, and
    // DELIBERATELY blind to `visible`: QML dependency capture registers every
    // property read during evaluation, so reading effective visibility here
    // ties the width to the popup lifecycle (a closed popup propagates
    // visible:false into every item, collapsing the fit to zero on close and
    // re-expanding it at open, i.e. a resize and blur re-render on EVERY
    // open). The captured dependencies are instead: root.count (loop
    // condition), each item's collapsed intent flag, and each item's
    // fitWidth, which transitively depends on Theme.uiFont through the
    // item's TextMetrics. All of these change only when content genuinely
    // changes, which call sites do before popup(), so the width is a
    // constant across open/close and the popup never resizes on a paint
    // pass. The engine owns re-evaluation order, which is what rules out the
    // race an imperative recompute would have: one driven by uiFontChanged
    // can run before the items' metrics re-bind and snapshot stale
    // fallback-font values.
    //
    // fitWidth (TextMetrics-backed, see ThemedMenuItem) is used instead of
    // implicitWidth because the latter routes through the control's deferred
    // contentItem, which does not reliably exist before the popup first
    // shows. Separators (no fitWidth) are skipped by the typeof guard;
    // collapsed entries must not drive the width.
    readonly property real _fitWidth: {
        let widest = 0
        for (let i = 0; i < root.count; ++i) {
            const item = root.itemAt(i)
            // Deliberately duck-typed: itemAt returns plain QQuickItem, and
            // separators genuinely lack fitWidth/collapsed, which is exactly
            // what the typeof probe filters. qmllint cannot type this and
            // should not try.
            // qmllint disable missing-property
            if (item && typeof item.fitWidth === "number" && !item.collapsed)
                widest = Math.max(widest, item.fitWidth)
            // qmllint enable missing-property
        }
        return widest + root.leftPadding + root.rightPadding
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 110; easing.type: Easing.OutQuad }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 110; easing.type: Easing.InQuad }
    }

    background: Item {
        id: themedMenuBackground
        implicitWidth: Math.max(root.menuWidth, root._fitWidth)

        // Grab only the slice of blurSource sitting behind the menu.
        ShaderEffectSource {
            id: behind
            anchors.fill: parent
            live: true          // see snapshot note below
            recursive: false    // source is a sibling subtree, so no recursion
            sourceItem: root.blurSource
            visible: false      // used only as MultiEffect.source

            function refresh() {
                if (!root.blurSource)
                    return
                const p = root.blurSource.mapFromItem(themedMenuBackground, 0, 0)
                sourceRect = Qt.rect(p.x, p.y, themedMenuBackground.width, themedMenuBackground.height)
            }
        }

        // Blur the grabbed slice and clip it to the rounded rect.
        MultiEffect {
            anchors.fill: parent
            autoPaddingEnabled: false
            blur: 1.0
            blurEnabled: true
            blurMax: 32
            maskEnabled: true
            maskSource: maskSrc
            source: behind
        }

        // Translucent tint + border, drawn sharp on top of the blur.
        Rectangle {
            anchors.fill: parent
            border.color: Theme.border
            border.width: 2
            antialiasing: true
            color: Theme.surfacePage
            opacity: 0.80
            radius: 8
        }

        // Rounded-rect mask. layer.enabled + visible:false is the standard
        // texture-provider pattern; the layer FBO still updates because
        // MultiEffect references it.
        Item {
            id: maskSrc
            anchors.fill: parent
            layer.enabled: true
            visible: false

            Rectangle {
                anchors.fill: parent
                antialiasing: true
                radius: 8
            }
        }

        // Recompute the grab region whenever the menu appears or resizes.
        Connections {
            target: root
            function onOpened() { behind.refresh() }
            function onXChanged() { behind.refresh() }
            function onYChanged() { behind.refresh() }
        }

        onWidthChanged: behind.refresh()
        onHeightChanged: behind.refresh()

        Component.onCompleted: behind.refresh()
    }

    delegate: ThemedMenuItem { id: menuItem }
}
