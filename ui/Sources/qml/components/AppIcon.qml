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

/// DPI-safe SVG icon.
///
/// A QML Image without an explicit sourceSize rasterizes an SVG at its
/// intrinsic document size in *device* pixels, so on high-DPI screens the
/// icon shrinks to intrinsicSize / devicePixelRatio logical pixels and
/// renders blurry. Setting sourceSize declares the *logical* size instead:
/// Qt rasterizes at sourceSize x devicePixelRatio and re-rasterizes when
/// the window moves between screens with different scale factors.
///
/// Use iconSize for square icons, or iconWidth/iconHeight independently
/// for non-square assets (logo, minimize glyph).
Image {
    id: root

    /// Convenience for square icons; sets both dimensions.
    property int iconSize: 24
    /// Logical width in device-independent pixels.
    property int iconWidth: root.iconSize
    /// Logical height in device-independent pixels.
    property int iconHeight: root.iconSize

    asynchronous: true
    fillMode: Image.Pad
    sourceSize: Qt.size(root.iconWidth, root.iconHeight)
}
