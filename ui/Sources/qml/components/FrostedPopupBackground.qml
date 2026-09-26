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

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
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

    // Grab only the slice of blurSource sitting behind the popup.
    ShaderEffectSource {
        id: behind
        anchors.fill: parent
        live: true          // the slice tracks whatever moves behind the popup
        recursive: false    // source is a sibling subtree, so no recursion
        sourceItem: frost.blurSource
        visible: false      // used only as MultiEffect.source

        function refresh() {
            if (!frost.blurSource)
                return
            const p = frost.blurSource.mapFromItem(frost, 0, 0)
            sourceRect = Qt.rect(p.x, p.y, frost.width, frost.height)
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
        border.width: frost.borderWidth
        antialiasing: true
        color: Theme.surfacePage
        opacity: 0.80
        radius: frost.radius
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

    // Recompute the grab region whenever the popup appears, moves, or resizes.
    Connections {
        target: frost.popup
        function onOpened() { behind.refresh() }
        function onXChanged() { behind.refresh() }
        function onYChanged() { behind.refresh() }
    }

    onWidthChanged: behind.refresh()
    onHeightChanged: behind.refresh()

    Component.onCompleted: behind.refresh()
}
