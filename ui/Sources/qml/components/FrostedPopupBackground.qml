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

// FrostedPopupBackground.qml
//
// The frosted backdrop behind the app's popups (ThemedMenu, ThemedComboBox's
// dropdown): a ShaderEffectSource grabs the pixels of `blurSource` behind the
// popup, a MultiEffect blurs them and clips them to the rounded rect, and a
// translucent Theme.surfacePage tint with a border is drawn sharp on top. The
// grab region follows the popup on open and on every move or resize.
//
// A caller assigns this to a Popup's `background` and binds `popup` to that
// popup, whose opened/x/y signals drive the grab region. Width comes from the
// popup as for any background; a caller that needs a fit-to-content width sets
// implicitWidth on its instance (ThemedMenu does).
//
// The effect chain (the grab, the blur, the mask layer) exists only while
// the popup is shown: it sits behind a Loader gated on this item's own
// effective visibility, which a popup propagates to its background for
// exactly the span it is on screen, exit transition included. Every popup in
// the app is closed at startup, so none of them builds a blur chain before
// the first frame, and a menu that is never opened never builds one.
//
// The grab is a still, not a live feed: the slice is rendered once when the
// popup appears and again only when the region moves or resizes. Anything
// animating behind an open menu (the spectrum, the seek bar) would otherwise
// re-render the whole source subtree into the grab texture and re-run the
// blur every frame for as long as the menu stays open; a frozen backdrop is
// what desktop popups show anyway.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Effects
import QtQuick.Templates as T

Item {
    id: frost

    // The popup this backs. Its opened/x/y recompute the grab region. Typed
    // against the templates base rather than the Basic style's Popup, which
    // is a QML file of its own that Menu does not derive from; T.Popup is the
    // C++ base both Menu and Popup share.
    required property T.Popup popup

    property int radius: 8
    property int borderWidth: 2

    // The item whose pixels show through (blurred) behind the popup, resolved
    // from the hosting window's menuBlurSource unless the caller sets one.
    // MUST be a sibling subtree of the overlay; your app content root,
    // NOT window.contentItem (that contains the overlay -> recurses to black).
    property Item blurSource: {
        const w = frost.Window.window
        return w ? w.menuBlurSource : null  // qmllint disable missing-property
    }

    // The slice of blurSource sitting behind the popup, in blurSource
    // coordinates. Kept on this item rather than on the grab so it survives
    // the chain being built and torn down with each open.
    property rect grabRect: Qt.rect(0, 0, 0, 0)

    function refresh() {
        if (!frost.blurSource)
            return
        const p = frost.blurSource.mapFromItem(frost, 0, 0)
        frost.grabRect = Qt.rect(p.x, p.y, frost.width, frost.height)
    }

    // The grab, the blur and the mask: built when the popup shows, released
    // when it has gone (see the file comment).
    Loader {
        id: chain
        anchors.fill: parent
        active: frost.visible

        sourceComponent: Item {
            // Grab only the slice of blurSource sitting behind the popup.
            ShaderEffectSource {
                id: behind
                anchors.fill: parent
                live: false         // a still of the moment the popup appeared
                recursive: false    // source is a sibling subtree, so no recursion
                // The source is attached only while this item itself is in a
                // window. ShaderEffectSource holds a window reference on its
                // sourceItem, and that accounting is one-sided when the source
                // is assigned while the effect has no window of its own yet:
                // it then references the SOURCE's window, references it again
                // when the effect joins a scene, and releases only one of the
                // two. The surplus reference is permanent and keeps the
                // source's window pointer alive past that window's teardown,
                // which QQuickItemPrivate::derefWindow then dereferences on
                // exit. Gating on behind.Window.window makes every reference
                // the effect takes one it also releases (the chain is built
                // while its blur target is already on screen, so the ungated
                // binding hits exactly that case).
                sourceItem: behind.Window.window ? frost.blurSource : null
                sourceRect: frost.grabRect
                visible: false      // used only as MultiEffect.source

                // A still needs an explicit render request at every point
                // the slice can change: the first attach, and each move or
                // resize of the region. scheduleUpdate before the item has a
                // window is remembered and honored at the first sync.
                onSourceItemChanged: scheduleUpdate()
                onSourceRectChanged: scheduleUpdate()
                Component.onCompleted: {
                    frost.refresh()
                    scheduleUpdate()
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
                    radius: frost.radius
                }
            }
        }
    }

    // Translucent tint + border, drawn sharp on top of the blur.
    Rectangle {
        anchors.fill: parent
        border.color: Theme.border
        border.width: frost.borderWidth
        antialiasing: true
        color: Theme.surfacePage
        opacity: 0.80
        radius: frost.radius
    }

    // Recompute the grab region whenever the popup appears, moves, or resizes.
    Connections {
        target: frost.popup
        function onOpened() { frost.refresh() }
        function onXChanged() { frost.refresh() }
        function onYChanged() { frost.refresh() }
    }

    onWidthChanged: refresh()
    onHeightChanged: refresh()

    Component.onCompleted: refresh()
}
