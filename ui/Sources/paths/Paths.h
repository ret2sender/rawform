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

#pragma once

#include <QString>

namespace rawform {

/// The rawform user config directory, using XDG semantics on every platform:
/// $XDG_CONFIG_HOME/rawform when set, else ~/.config/rawform. QStandardPaths is
/// deliberately avoided (it would land under ~/Library/Preferences on macOS),
/// so the location is the same on every platform and a config directory can
/// be copied between machines as-is. The single place this path is computed:
/// every app-managed file lives beneath it (settings.yaml, window.yaml,
/// playback.yaml, rate_ledger.yaml, spectrum.yaml, rename_patterns.yaml,
/// playlist_custom_columns.yaml, the columns.rwftp column preset, and the
/// live_playlist/ session directory). May not exist yet; each writer creates
/// it on its first save.
[[nodiscard]] QString userConfigDir();

} // namespace rawform
