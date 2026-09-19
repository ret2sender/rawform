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
// same spirit as SeekBar and VolumeSlider: it knows nothing about spectrumProvider
// or audioController. It takes the bar VALUES in (a 0..1 array, low frequency
// first) and a stable bar COUNT, and draws the dark rounded frame with the
// bottom-anchored lavender bars. Every color and size is a property defaulted
// to the current look, so it can be retuned without touching any host wiring,
// and it reaches for no ancestor id.
//
// WHY count IS SEPARATE FROM values. The values array is republished about sixty
// times a second. If the Repeater keyed off the array, every tick would rebuild the
// whole delegate set; keying off the stable `count` instead means the delegates are
// created once and only their height bindings re-evaluate per tick, which is what
// keeps a live 60 Hz display cheap. The host binds count to spectrumProvider.bar
// Count (effectively constant) and values to spectrumProvider.bars (per tick).
//
// The provider already applies the attack/decay envelope, so the bars are smooth at
// the source; this file adds no animation of its own (a Behavior here would fight
// the per-tick updates rather than help).

pragma ComponentBehavior: Bound

import QtQuick

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
    // equal `count`; a shorter array simply leaves the trailing bars at zero, and a
    // longer one is truncated by `count`, so a transient mismatch never overruns.
    property var values: []

    // The number of bars to draw. Stable for a given configuration, so the Repeater
    // delegate set does not churn on the per-tick values updates.
    property int count: 20

    // -----------------------------------------------------------------------
    // Look. The lavender token shared with the seek and volume fills, on the
    // dark frame. Restyle freely.
    // -----------------------------------------------------------------------
    property color frameColor: Theme.surfacePanel
    property color barColor: Theme.accentSoft
    property real  frameRadius: 4
    property real  barRadius: 1

    // Inner padding from the frame edge to the plot area, and the gap between bars.
    // The bar width then falls out of the remaining width divided by `count`.
    property real  paddingX: 7
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

            // One bar per index. Width is the leftover after the gaps, split evenly;
            // x steps by width+gap; height is the min nub plus the value's share of
            // the remaining plot height, bottom-anchored.
            readonly property real barWidth:
                root.count > 0
                    ? Math.max(1, (width - (root.count - 1) * root.barGap) / root.count)
                    : 0

            Repeater {
                model: root.count

                delegate: Rectangle {
                    id: bar
                    required property int index

                    // The bar's 0..1 magnitude, guarded for a values array that is
                    // momentarily shorter than count (or not yet populated).
                    readonly property real level:
                        (index < root.values.length)
                            ? Math.max(0, Math.min(1, root.values[index]))
                            : 0

                    width: plot.barWidth
                    x: index * (plot.barWidth + root.barGap)
                    anchors.bottom: parent.bottom
                    height: root.minBarHeight
                            + (plot.height - root.minBarHeight) * level
                    radius: root.barRadius
                    color: root.barColor
                    antialiasing: true
                }
            }
        }
    }
}
