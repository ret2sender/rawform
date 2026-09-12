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

// Format.qml
//
// The QML-side display formatters shared by the transport surfaces, one
// definition instead of a copy per component (SeekBar's hover bubble and
// PlayerBar's timing readout render the same m:ss). Registered as a module
// singleton in ui/CMakeLists.txt via
// QT_QML_SINGLETON_TYPE, the same two-halves contract as Theme and
// TitleBarStyle; a plain `Format.mmss(x)` resolves from any document in the
// module.
//
// Scope rule, mirroring the C++ side (utils/Formats.h): only formats used by
// MORE THAN ONE document live here. The C++ singleton formats MILLISECONDS
// for the playlist column; this one formats SECONDS, the unit the transport
// bindings already carry. Keep the two shapes in step if either ever changes.
QtObject {
    // m:ss from SECONDS (seconds zero-padded, minutes not); NaN and negative
    // clamp to a clean "0:00" so a binding evaluating before the first
    // position tick never renders garbage.
    function mmss(s) {
        if (!s || s < 0 || isNaN(s))
            s = 0
        var total = Math.floor(s)
        var m = Math.floor(total / 60)
        var sec = total % 60
        return m + ":" + (sec < 10 ? "0" + sec : sec)
    }
}
