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

// SpectrumView.qml
//
// The spectrum bar display for the PlayerBar, a pure, dumb, reusable widget in the
// same spirit as SeekBar and VolumeSlider: it knows nothing about SpectrumProvider
// or AudioController. It takes the bar VALUES in (a 0..1 array, low frequency
// first) and a stable bar COUNT, and draws the dark rounded frame with the
// bottom-anchored lavender bars. Every color and size is a property defaulted
// to the current look, so it can be retuned without touching any host wiring,
// and it reaches for no ancestor id.
//
// The bars themselves are one SpectrumBars item (C++, playback/SpectrumBars.h):
// every bar is a quad in a single geometry node, so a tick costs the host one
// `values` write and the renderer one vertex upload, with no per-bar binding,
// item or rounded-corner texture in between. `count` is separate from `values`
// because the values list is republished on every tick while the count is
// effectively constant; the host binds count to SpectrumProvider.barCount and
// values to SpectrumProvider.bars.
//
// The provider already applies the attack/decay envelope, so the bars are smooth at
// the source; this file adds no animation of its own (a Behavior here would fight
// the per-tick updates rather than help).

pragma ComponentBehavior: Bound

import QtQuick
import com.rawform.app

Item {
    id: root

    // Default footprint, matching the player bar's spectrum slot. The host can still
    // override with width/height or Layout hints; these only give a layout a sane
    // implicit size to work from.
    implicitWidth: 120
    implicitHeight: 40

    // -----------------------------------------------------------------------
    // Inputs (driven by PlayerBar from the SpectrumProvider).
    // -----------------------------------------------------------------------

    // The bar magnitudes, each 0..1, low frequency first. Length is expected to
    // equal `count`; a shorter list simply leaves the trailing bars at zero, and a
    // longer one is truncated by `count`, so a transient mismatch never overruns.
    // A typed list, so the provider's QList<qreal> passes through to the item
    // as one value and never becomes a JavaScript array on the way.
    property list<real> values: []

    // The number of bars to draw. Stable for a given configuration; only the
    // values move per tick.
    property int count: 20

    // -----------------------------------------------------------------------
    // Look. The lavender token shared with the seek and volume fills, on the
    // dark frame. Restyle freely.
    // -----------------------------------------------------------------------
    property color frameColor: Theme.surfacePanel
    property color barColor: Theme.accentSoft
    property real  frameRadius: 4

    // Inner padding from the frame edge to the plot area, and the gap between bars.
    // The bars are whole pixels wide, all the same, and the row is centered in
    // the plot, so the side padding is chosen to make the split come out even:
    // in the player bar's 120 px slot, 1 px a side leaves 118 for the plot,
    // which is exactly twenty 4 px bars with 2 px gaps. The vertical padding
    // keeps the end bars clear of the frame's 4 px corner radius.
    property real  paddingX: 1
    property real  paddingTop: 6
    property real  paddingBottom: 6
    property real  barGap: 2

    // The smallest height a bar ever shows, so even a silent band keeps a visible
    // nub (the reference shows this) rather than vanishing.
    property real  minBarHeight: 2

    // -----------------------------------------------------------------------
    // The frame.
    // -----------------------------------------------------------------------
    Rectangle {
        id: frame
        anchors.fill: parent
        color: root.frameColor
        radius: root.frameRadius

        // The plot area: the frame inset by the padding. Bars are positioned and
        // sized against this, never the frame directly, so the padding is honored
        // on every edge.
        Item {
            id: plot
            anchors.fill: parent
            anchors.leftMargin: root.paddingX
            anchors.rightMargin: root.paddingX
            anchors.topMargin: root.paddingTop
            anchors.bottomMargin: root.paddingBottom

            // The bars: width is the leftover after the gaps, split evenly; x
            // steps by width plus gap; height is the min nub plus the value's
            // share of the remaining plot height, bottom-anchored. All of it is
            // laid out inside the item, from these inputs.
            SpectrumBars {
                anchors.fill: parent
                values: root.values
                count: root.count
                color: root.barColor
                gap: root.barGap
                minBarHeight: root.minBarHeight
            }
        }
    }
}
