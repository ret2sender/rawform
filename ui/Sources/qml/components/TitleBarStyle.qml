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

// TitleBarStyle.qml
//
// Per-platform styling for the frameless title bar caption buttons.
//
// Structure:
//
//   Profile          - every knob, with the default (Linux) values.
//   <name>Profile    - a variant, stating ONLY what it changes.
//   resolveVariant() - the single place that decides which one applies.
//
// Adding a variant is two edits: one Profile block, one case in
// resolveVariant(). Nothing downstream changes.
//
// macOS never reaches here; the parent uses the native traffic lights.

pragma Singleton

pragma ComponentBehavior: Bound

import QtQuick
import com.rawform.app

QtObject {
    id: style

    // =======================================================================
    // Profile definition
    //
    // These values are the default profile. Variants below inherit them
    // and override only what differs, so a new distro or desktop variant
    // is a handful of lines rather than a full copy.
    // =======================================================================
    component Profile: QtObject {
        property url minimizeIcon: Qt.resolvedUrl("../../icons/default/control_minimize.svg")
        property url maximizeIcon: Qt.resolvedUrl("../../icons/default/control_maximize.svg")
        property url restoreIcon: Qt.resolvedUrl("../../icons/default/control_restore.svg")
        property url closeIcon: Qt.resolvedUrl("../../icons/default/control_close.svg")

        // Glyph shown while the button is hovered. Defaults to the rest
        // glyph (bound, so a profile that only overrides the rest icons
        // gets matching hover icons for free). A profile whose hover plate
        // is light enough to swallow the rest glyph overrides these with
        // dark-glyph assets; the hover color of the glyph therefore lives
        // in the artwork, not in a color knob, which keeps AppIcon free of
        // any colorization stage.
        property url minimizeHoverIcon: minimizeIcon
        property url maximizeHoverIcon: maximizeIcon
        property url restoreHoverIcon: restoreIcon
        property url closeHoverIcon: closeIcon

        property real iconWidth: 10
        property real iconHeight: 10

        // Button geometry.
        property real buttonWidth: 32
        property real buttonHeight: 40
        property real spacing: 0

        // Padding of the whole caption chunk (TitleBarControls is a
        // Control; these feed its leftPadding/rightPadding/topPadding/
        // bottomPadding). All zero by default; a decoration that keeps
        // its buttons a few px off the frame edge (Breeze) sets the
        // outboard side.
        property real paddingLeft: 0
        property real paddingRight: 0
        property real paddingTop: 0
        property real paddingBottom: 0

        // Applied to whichever top corner faces the window edge. Left at 0
        // by default: a rounded corner stays rounded when the compositor
        // half-tiles the window, and a Qt client cannot detect that state.
        property real closeCornerRadius: 0

        // Fill colors, per state. The rest fills are transparent by default
        // (glyph-only buttons); a profile that wants an
        // always-visible plate behind the glyphs overrides them.
        property color controlBackground: "transparent"
        property color closeBackground: "transparent"
        property color controlHover: Theme.controlHover
        property color closeHover: Theme.closeHover

        // Background plate geometry. The plate is a square centered behind
        // the glyph (the button cell itself is not square, so an inset can
        // only ever produce an oval; an explicit side length is the only
        // way to get a true circle). backgroundSize is that side length,
        // with 0 as the sentinel for "fill the whole button cell"
        // (Windows-style). backgroundRadius defaults to half the side, so
        // a sized plate is a circle unless a profile overrides the radius
        // for a rounded-square chip; in full-cell mode the default
        // collapses to 0 and the fill stays square.
        //
        // closeCornerRadius (above) applies ONLY in full-cell mode: a
        // centered plate never touches the window corner, so the
        // edge-corner rounding is meaningless there and the control
        // enforces that rather than trusting the two knobs to be kept
        // consistent.
        property real backgroundSize: 28
        property real backgroundRadius: backgroundSize / 2

        // Inactive-window dimming and animation.
        property real inactiveOpacity: 0.5
        property int hoverDuration: 200
        property int dimDuration: 200

        // false -> right side, reading [minimize][maximize][close]
        // true  -> left side,  reading [close][maximize][minimize]
        property bool controlsOnLeft: false

        // Whether the maximize/restore button exists at all. On by default
        // (the Linux profiles); variants whose icon sets lack the artwork
        // switch it off rather than showing a blank glyph.
        property bool showMaximize: true
    }

    // =======================================================================
    // Variants
    // =======================================================================
    readonly property Profile defaultProfile: Profile {}

    readonly property Profile windowsProfile: Profile {
        // Drawn tight to the glyph, so minimize is a literal 10x1 rule.
        minimizeIcon: Qt.resolvedUrl("../../icons/windows/control_minimize.svg")
        maximizeIcon: Qt.resolvedUrl("../../icons/windows/control_maximize.svg")
        restoreIcon: Qt.resolvedUrl("../../icons/windows/control_restore.svg")
        closeIcon: Qt.resolvedUrl("../../icons/windows/control_close.svg")

        buttonWidth: 46
        buttonHeight: 30
        closeCornerRadius: 8

        controlHover: Theme.windowsControlHover
        closeHover: Theme.windowsCloseHover
        inactiveOpacity: 0.4

        paddingBottom: 18

        // Full-cell hover fill, the stock Win11 caption treatment; the
        // circular plate is a Linux affordance.
        backgroundSize: 0

        hoverDuration: 200
        dimDuration: 150
    }

    // Breeze: chevron artwork, and the Plasma decoration's inverted hover
    // (light plate, dark glyph; pink plate for close). The dark glyphs are
    // separate assets because the rest glyph would vanish on the plate.
    readonly property Profile breezeProfile: Profile {
        minimizeIcon: Qt.resolvedUrl("../../icons/breeze/control_minimize.svg")
        maximizeIcon: Qt.resolvedUrl("../../icons/breeze/control_maximize.svg")
        restoreIcon: Qt.resolvedUrl("../../icons/breeze/control_restore.svg")
        closeIcon: Qt.resolvedUrl("../../icons/breeze/control_close.svg")

        minimizeHoverIcon: Qt.resolvedUrl("../../icons/breeze/control_minimize_hover.svg")
        maximizeHoverIcon: Qt.resolvedUrl("../../icons/breeze/control_maximize_hover.svg")
        restoreHoverIcon: Qt.resolvedUrl("../../icons/breeze/control_restore_hover.svg")
        closeHoverIcon: Qt.resolvedUrl("../../icons/breeze/control_close_hover.svg")

        controlHover: Theme.breezeControlHover
        closeHover: Theme.breezeCloseHover

        // The Breeze SVGs are transcribed from the decoration's 18x18
        // button box; rendering them at exactly that size (which is also
        // the plate size) keeps every line where Breeze puts it.
        iconHeight: 18
        iconWidth: 18

        buttonWidth: 26
        buttonHeight: 30

        // Breeze holds its caption row a few px in from the frame edge.
        paddingBottom: 16
        paddingRight: 4

        backgroundSize: 18

        hoverDuration: 0
        dimDuration: 0
    }

    // =======================================================================
    // Resolution
    //
    // Explicit override wins, then platform, then the user's icon theme.
    // overrideVariant is a declared property, so binding it to a persisted
    // preference would expose the choice to the user and the whole title
    // bar would restyle live when it changes.
    // =======================================================================
    property string overrideVariant: ""

    readonly property string variantName: style.resolveVariant()

    function resolveVariant(): string {
        if (style.overrideVariant !== "")
            return style.overrideVariant

        if (Qt.platform.os === "windows")
            return "windows"

        return style.resolveDesktopVariant()
    }

    // Keyed off the active icon theme rather than XDG_CURRENT_DESKTOP.
    // The theme name is what the user actually configured, so it stays
    // correct on Plasma-with-Papirus, on tiling compositors, and on any
    // desktop not enumerated here. Unknown themes fall through to the
    // default, which is always a valid answer.
    //
    // Requires AppInfo.systemIconTheme; returns "default" harmlessly if
    // that property is absent.
    function resolveDesktopVariant(): string {
        const iconTheme = (AppInfo.systemIconTheme || "").toLowerCase()

        if (iconTheme.startsWith("breeze"))
            return "breeze"

        return "default"
    }

    readonly property Profile profile: {
        switch (style.variantName) {
        case "windows":
            return style.windowsProfile
        case "breeze":
            return style.breezeProfile
        default:
            return style.defaultProfile
        }
    }

    // =======================================================================
    // Public API
    //
    // Flat forwards so consumers stay unaware of profiles and
    // TitleBarControls.qml needs no changes.
    // =======================================================================
    readonly property url minimizeIcon: style.profile.minimizeIcon
    readonly property url maximizeIcon: style.profile.maximizeIcon
    readonly property url restoreIcon: style.profile.restoreIcon
    readonly property url closeIcon: style.profile.closeIcon

    readonly property url minimizeHoverIcon: style.profile.minimizeHoverIcon
    readonly property url maximizeHoverIcon: style.profile.maximizeHoverIcon
    readonly property url restoreHoverIcon: style.profile.restoreHoverIcon
    readonly property url closeHoverIcon: style.profile.closeHoverIcon

    readonly property real iconWidth: style.profile.iconWidth
    readonly property real iconHeight: style.profile.iconHeight

    readonly property real buttonWidth: style.profile.buttonWidth
    readonly property real buttonHeight: style.profile.buttonHeight
    readonly property real spacing: style.profile.spacing
    readonly property real closeCornerRadius: style.profile.closeCornerRadius

    readonly property real paddingLeft: style.profile.paddingLeft
    readonly property real paddingRight: style.profile.paddingRight
    readonly property real paddingTop: style.profile.paddingTop
    readonly property real paddingBottom: style.profile.paddingBottom

    readonly property color controlBackground: style.profile.controlBackground
    readonly property color closeBackground: style.profile.closeBackground
    readonly property color controlHover: style.profile.controlHover
    readonly property color closeHover: style.profile.closeHover

    readonly property real backgroundSize: style.profile.backgroundSize
    readonly property real backgroundRadius: style.profile.backgroundRadius

    readonly property real inactiveOpacity: style.profile.inactiveOpacity
    readonly property int hoverDuration: style.profile.hoverDuration
    readonly property int dimDuration: style.profile.dimDuration

    readonly property bool controlsOnLeft: style.profile.controlsOnLeft
    readonly property bool showMaximize: style.profile.showMaximize
}
