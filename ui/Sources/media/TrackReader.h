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

#include "media/TrackData.h"

#include <QString>

namespace rawform {

/**
 * @brief Read one media file's tags, audio properties, and filesystem fields
 *        into a TrackData. The single tag-read boundary in the app: every
 *        TrackData is filled here (the editors write, AlbumArtProvider fetches
 *        one picture; nothing else reads tags).
 *
 * Pure and stateless (each call owns its own TagLib::FileRef), so it is safe
 * both across the thread pool (TrackScanner) and directly on the GUI thread
 * (MetadataReloader, for a single stale file). Sharing one function keeps the
 * parsing identical between the "added" and "reloaded" paths.
 *
 * A file that cannot be opened or parsed comes back with valid=false (its
 * filesystem fields still filled); callers skip it rather than clobber good
 * cached data with an empty record.
 */
[[nodiscard]] TrackData readTrack(const QString& path);

} // namespace rawform
