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

// M3uFile.h
//
// The M3U export path: a playlist as a plain list of file references that other players
// can read.
//
// The counterpart to PlaylistFile.cpp's binary RFW1 writer, and deliberately
// NOT a second document format. M3U carries file references and nothing else,
// so a save here is lossy by construction: column order, column widths, the tab
// title and the current row have no representation and are dropped. The tab's
// own live .rwfpl autosave is untouched by an M3U export, so nothing the user
// can see is actually lost; only the exported file is thinner.
//
// There is no reader here on purpose. Reading M3U already exists, in
// TrackScanner::expandM3u: an M3U's contents mean "these files, in this order",
// which is precisely a scan request, and routing it through the scanner gets
// each referenced file read through readTrack exactly like any other ingestion
// path. A second, parallel M3U parser would be a drift risk for no gain.
//
// CONTENT. PLAIN M3U, matching byte-for-byte the shape foobar2000 emits: one
// path line per track, in playlist order, and nothing else. No "#EXTM3U"
// header and no "#EXTINF" lines.
//
// Why plain rather than extended, since extended looks richer: #EXTINF
// duplicates the artist, title and duration into the playlist file, where they
// immediately begin to rot: the tags in the audio files are the authority, they
// get edited, and an exported copy of them does not follow. rawform's own reader
// already refuses to trust #EXTINF for exactly that reason (see expandM3u: the
// m3u governs WHICH files and in WHAT order, never their metadata), so writing
// the fields we would then decline to read back is pure noise. The plain form
// says only the thing an M3U is actually authoritative about.
//
// A row whose filePath is empty is skipped: it cannot be expressed in this
// format at all, and a blank line would be dropped by any reader anyway. Rows
// whose file is currently missing (available == false) ARE written: the file may
// come back, and silently thinning an export is worse than exporting a reference
// that does not resolve today. Nothing is deduped.
//
// PATHS. Relative to the playlist file's own directory whenever the
// two share a filesystem root, absolute otherwise. ".." climbing is allowed, so
// a whole music tree can be moved without breaking the playlist. QDir's
// relativeFilePath already encodes exactly this rule: it returns an ABSOLUTE
// path when the two paths cannot be related (different Windows drives), which is
// the fallback we want, so one call covers both cases. Separators are always
// '/', never native, matching what expandM3u resolves.
//
// ENCODING. UTF-8 without a BOM, CRLF line endings, for BOTH
// .m3u and .m3u8.
//
// CRLF is what M3U has used in practice since it was a Windows format, and it is
// what the reference exports show. It is written LITERALLY, not by opening the
// file with QIODevice::Text: that mode translates '\n' to the PLATFORM ending,
// which would silently produce LF files on macOS and Linux and defeat the
// whole point. Readers are universally tolerant of the other ending (rawform's
// own trims each line, so a stray '\r' never reaches a path), so this costs
// nothing and matches convention.
//
// The legacy ".m3u-is-the-system-codepage" convention has no content to express
// on either target platform: the 8-bit codec is UTF-8 on macOS and on Linux
// alike, so the two extensions differ in label only. A BOM is omitted because
// players that predate UTF-8 M3U read its bytes as part of the first path.
//
// Pure, Qt-object-free and thread-safe, the same contract PlaylistFile.cpp
// carries: PlaylistStore runs this on the global thread pool so a big export
// never blocks the GUI.

#pragma once

#include <QString>

namespace rawform {

struct PlaylistDocument; ///< defined in PlaylistFile.h

/// Write @p doc to @p path as a plain M3U, atomically.
///
/// Uses QSaveFile (temporary plus atomic rename on commit), so a failure
/// mid-write never truncates an existing file at @p path. Returns false with a
/// reason in @p error on any failure. An empty playlist is not an error: it
/// writes an empty file.
[[nodiscard]] bool writeM3u(const QString& path,
                            const PlaylistDocument& doc,
                            QString* error = nullptr);

} // namespace rawform
