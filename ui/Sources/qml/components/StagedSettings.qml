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

import QtQml

/*
 * StagedSettings.qml
 *
 * The Settings window's staged edit object as a NAMED type. The scenario a
 * declared type prevents: an anonymous inline QtObject in SettingsWindow.qml
 * leaves the panes' `settings` properties typed as bare QtObject, qmllint
 * cannot check a single member access on the staged surface, and a renamed
 * property surfaces only at runtime. As a declared type, the panes write
 * `property StagedSettings settings` and every read and write below is
 * verified statically.
 *
 * Lifecycle contract (owned by SettingsWindow): seeded from the applied
 * values on every open via reseed(), read and written by the panes while the
 * window is up, and copied back on Apply/OK. The initializers below only
 * cover the pre-open state and mirror the factory defaults; do not
 * treat them as the source of truth, the stores' *Default constants are.
 */
QtObject {
    // Playback page: the output rate policy. True is
    // bit-perfect (switch the device to the track's rate, nearest-best fallback
    // when out of reach); false is always-resample (never touch the device).
    property bool bitPerfect: true

    // Playback page: the output device pin. The empty string follows
    // the system default; a non-empty value is a persistent device id from the
    // controller's enumeration.
    property string outputDeviceId: ""

    // ReplayGain page.
    property int  mode: 0
    property real preampDb: 0
    property real untaggedPreampDb: 0
    property bool clip: true
    property bool skipExisting: false

    // Spectrum view page: the analyzer source (0 Output / 1 Source).
    property int  spectrumSource: 0

    // Tagging > MP3 page: the MP3/ID3 write preferences (SettingsStore
    // enum ints).
    property int  id3v2Version: 0
    property int  id3v1Mode: 0
    property int  apeMode: 0
    property int  id3v2Encoding: 1
}
