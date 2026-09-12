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

pragma Singleton

import QtQuick

/*
 * Theme.qml
 *
 * Application-wide look constants as a module singleton. One instance exists
 * per QML engine, resolvable by type name from any document in
 * com.rawform.app, including documents instantiated inside separate top-level
 * Windows (Settings, Properties). That last point is the reason this exists:
 * FontLoaders living in MainWindow.qml and reached by id through QML's
 * dynamic scoping would be a dependency every component carries silently,
 * and one that does not hold in other windows. A singleton is scope-independent,
 * so a component reading Theme.uiFont is self-contained: it can be
 * instantiated anywhere (another window, a test harness, qml preview) and
 * still resolve.
 *
 * Contents: the two font families and the palette, in two tiers (see the
 * section comments below). Add new shared look constants here rather than in
 * MainWindow.
 *
 * Registered in ui/CMakeLists.txt via QT_QML_SINGLETON_TYPE on this file;
 * the pragma above is the QML-side half of that contract.
 */
QtObject {
    // The loaders are implementation detail; consumers read the family-name
    // strings below. Paths are relative to this file (Sources/qml/theme/),
    // pointing at the module RESOURCES fonts under Sources/fonts/.
    readonly property FontLoader _interLoader: FontLoader {
        source: "../../fonts/Inter-VariableFont_opsz,wght.ttf"
    }
    readonly property FontLoader _monoLoader: FontLoader {
        source: "../../fonts/JetBrainsMono-VariableFont_wght.ttf"
    }

    // UI family (Inter): all chrome, labels, menus, table text.
    readonly property string uiFont: _interLoader.name

    // Monospace family (JetBrains Mono): time readouts, dB bubbles, the log
    // console; anywhere digits must not jitter as values tick.
    readonly property string monoFont: _monoLoader.name

    // -----------------------------------------------------------------------
    // Palette, tier 1: values with one unambiguous role across the app.
    // -----------------------------------------------------------------------

    // Text ramp, brightest to dimmest.
    readonly property color textPrimary: "#E2E2E2"   // default UI text
    readonly property color textSecondary: "#D0D0D0" // values, field text
    readonly property color textMuted: "#9F9F9F"     // hints, placeholders
    readonly property color textDim: "#8A8A8A"       // inactive labels, deemphasis
    // String twin of textDim for HTML/rich-text contexts (see the tier-2
    // policy note on why color tokens must never be concatenated into HTML).
    // The pair must hold the same value; edit both lines together.
    readonly property string textDimHex: "#8A8A8A"

    // Accent (the app's purple family).
    readonly property color accent: "#9641FF"        // selection underlines, indicators
    readonly property color accentSoft: "#BDB2FF"    // lavender: focus strokes, highlights
    readonly property color accentHover: "#D4B8FF"   // menu-label hover tint

    // The playlist's current-row (focus) outline, the remembered-track
    // rectangle. Deliberately a quiet gray rather than the accent family: the
    // outline marks FOCUS, not selection, and must read a full tier below the
    // selection tint (matches foobar's focus rectangle). Its own token so
    // taste-tuning is one edit here, never a hunt through delegates.
    readonly property color focusOutline: "#8A8A8A"  // shares #8A8A8A with textDim

    // Status.
    readonly property color warning: "#F2C94C"       // amber: warning log level, encoding conflict
    // String twin of warning for HTML/rich-text contexts; same pairing rule
    // as textDimHex: the two lines must hold the same value.
    readonly property string warningHex: "#F2C94C"
    readonly property color danger: "#FF6B6B"        // errors
    readonly property color dangerSoft: "#FF8A8A"    // error log level, destructive hints
    readonly property color success: "#CAFFBF"       // positive confirmations
    // The warm salmon shared by the log surfaces for error text (LogStore's
    // HTML lines and StatusLogBar's message color). Distinct from danger and
    // dangerSoft on purpose: those are for controls and hints, this one is
    // tuned for body text on dark surfaces. errorTextHex is its string twin
    // for HTML contexts; the two lines must hold the same value.
    readonly property color errorText: "#FFB4A2"
    readonly property string errorTextHex: "#FFB4A2"

    // The outline gray: borders, strokes, slider grooves.
    readonly property color border: "#3A3A3A"

    // -----------------------------------------------------------------------
    // Palette, tier 2: the dark surface ladder and its satellites, named by
    // ROLE. Several tokens deliberately share a value (noted inline); that is
    // the point of role naming: two roles that happen to coincide today can
    // diverge later by editing one line here, with no call-site migration.
    // Never map a call site to a token whose role does not match, even when
    // the value does; add a token or leave the site literal instead.
    //
    // Inclusion policy: a value earns a token when it appears in three or
    // more files, or four or more sites, with a nameable role. One-off shades
    // local to a single component stay literal at the call site (they are
    // local design decisions, not palette), as do pure white/black and the
    // HTML color strings in LogStore (a QML color stringifies as #AARRGGBB,
    // which rich text does not reliably parse, so color-typed tokens must
    // never be concatenated into HTML strings). The title-bar caption-button
    // hovers below are an exception to the three-file rule: TitleBarStyle's
    // Profile system consumes them by role, and the profiles must restyle
    // when the palette does, so they live here rather than inline.
    // -----------------------------------------------------------------------

    // Surfaces, deepest to most raised.
    readonly property color surfaceSunken: "#141414"   // deep panels below page level
    readonly property color surfaceInset: "#161616"    // inset content wells (log console, RG inner panels)
    readonly property color headerBand: "#181818"      // table/section header strips
    readonly property color surfacePage: "#1B1B1B"     // window/pane/dialog body background
    readonly property color surfaceTabIdle: "#1F1F1F"  // unselected tab fill
    readonly property color surfacePanel: "#232323"    // framed panel surface; shares #232323 with rowEven
    readonly property color buttonFace: "#262626"      // pushbutton resting face
    readonly property color surfaceRaised: "#2A2A2A"   // bubbles, transport buttons, hover fills; shares #2A2A2A with separator
    readonly property color surfaceSelected: "#2E2E2E" // selected tab/nav surface; shares #2E2E2E with separatorStrong

    // Zebra stripes (playlist, metadata tables, edit dialog grid).
    readonly property color rowEven: "#232323"         // shares #232323 with surfacePanel
    readonly property color rowOdd: "#1D1D1D"

    // Controls (segment selectors, spin boxes, footer buttons).
    readonly property color surfaceControl: "#222222"      // resting surface
    readonly property color surfaceControlHover: "#2C2C2C" // hover state

    // Lines. Two weights exist in the wild; both are kept, not unified,
    // because unification would be a visible pixel change.
    readonly property color separator: "#2A2A2A"       // 1px rules, dividers, panel frame borders; shares #2A2A2A with surfaceRaised
    readonly property color separatorStrong: "#2E2E2E" // section rules, dialog frame borders; shares #2E2E2E with surfaceSelected
    readonly property color windowOutline: "#4C4E51"   // frameless MAIN window edge line (Linux); KDE Breeze's outline value, lighter than the separators on purpose: it must read against arbitrary desktop content behind the window, not against our own surfaces

    // Selection tints (the purple wash family).
    readonly property color selectionFill: "#3D2D5C"   // selected row, text selectionColor, menu highlight
    readonly property color selectionSoft: "#2E2A44"   // selected cell, primary button base, add-row hover

    // Accent satellites.
    readonly property color accentButtonHover: "#A99CF0" // accent footer-button hover (pairs with buttonFace cluster)
    readonly property color textAccent: "#B285FF"        // purple field-name/key text
    readonly property color textOnAccent: "#1B1B1B"      // dark text on accent-filled controls; shares #1B1B1B with surfacePage

    // Status satellite.
    readonly property color dangerSurface: "#3A2A2A"   // close-button hover wash

    // Title bar caption buttons (frameless-window minimize/close), consumed
    // through TitleBarStyle's Profile system. The default pair styles the
    // Linux profiles; the windows* pair matches the stock Win11 caption
    // treatment and applies only under the Windows profile.
    readonly property color controlHover: "#4E4E4E"        // minimize hover fill
    readonly property color closeHover: "#C42B1C"          // close hover fill; shares #C42B1C with windowsCloseHover
    readonly property color windowsControlHover: "#626262" // Windows minimize hover fill
    readonly property color windowsCloseHover: "#C42B1C"   // Windows close hover fill; shares #C42B1C with closeHover
    // Breeze inverts on hover: a near-white plate carrying a dark glyph, and
    // a pink plate for close (Breeze's negative color lightened, as sampled
    // from a Plasma 6 decoration on a dark color scheme). The dark glyph
    // is baked into the *_hover.svg assets, not colored here.
    readonly property color breezeControlHover: "#FCFCFC"  // Breeze minimize/maximize hover plate
    readonly property color breezeCloseHover: "#FF98A2"    // Breeze close hover plate

    // Text ramp extensions, below tier 1's textDim.
    readonly property color textInactive: "#9A9A9A"    // inactive icons, secondary window chrome text
    readonly property color textFaint: "#6E6E6E"       // muted glyphs, empty-state hints
    readonly property color textDisabled: "#5A5A5A"    // placeholders, disabled labels
}
