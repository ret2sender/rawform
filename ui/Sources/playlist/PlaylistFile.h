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

#include <QDataStream>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace rawform {

/**
 * @brief rawform's own on-disk playlist format (".rwfpl"): a single playlist's
 *        track list and its column layout, doubling as a tag cache (a cached
 *        record is a TrackData as readTrack produced it).
 *
 * A deliberately distinct format with its own extension and magic number; no
 * binary compatibility with foobar's ".fpl" is intended.
 *
 * Design:
 *  - QDataStream over a binary file. No SQLite, no text/JSON.
 *  - Byte order is pinned LittleEndian by convention (not stored: it is our
 *    format, read with the same fixed order it was written with).
 *  - The QDataStream version is stored in the header so the file is
 *    self-describing: the writer uses one pinned @ref kStreamVersion; the reader
 *    honors whatever the file declares, validated against a supported range. POD
 *    integers in the header encode identically across stream versions, so the
 *    header always parses first.
 *  - Chunk-based: after the header, a sequence of (id, byteLength, payload)
 *    chunks. The per-chunk length prefix lets an unknown/newer chunk be skipped
 *    wholesale and lets a reader bounds-check against truncation.
 *  - Each track record is also length-prefixed, so one corrupt record is bounded
 *    and a record that grows new trailing fields in a future version still reads
 *    (an older reader takes the fields it knows, ignores the rest): forward
 *    compatibility without a format bump.
 *  - Column layout is stored by field id (string) paired with width, never by
 *    positional index, so it survives the user editing the schema. This is the
 *    currentColumnOrder() / currentColumnWidths() pair, restored via
 *    applyColumnLayout().
 *
 * Not stored on disk (by design):
 *  - TrackData::valid:     transient scan status; a cached record is valid by
 *                          construction, and availability is recomputed against
 *                          the real file on load, so a stale `false` can't poison
 *                          the cache.
 *  - TrackData::available: runtime-only "present and readable" flag, recomputed
 *                          on every load.
 *  - embedded cover bytes: only the hasEmbeddedArt flag is stored; the bytes stay
 *                          lazy (AlbumArtProvider), so covers never bloat the file
 *                          or the in-RAM list.
 *
 * This translation unit is pure, Qt-object-free and thread-safe: PlaylistStore
 * runs readPlaylist()/writePlaylist() on the global thread pool so neither a big
 * load nor a save blocks the GUI.
 */

/// The document a .rwfpl carries: the playlist's tracks plus its saved column
/// layout. fieldIds and widths are parallel (index i is one column), in visual
/// order, exactly as currentColumnOrder()/currentColumnWidths() produce them.
struct PlaylistDocument {
    QList<TrackData> tracks;
    QStringList      fieldIds;   ///< column field ids, visual order
    QList<int>       widths;     ///< column widths, parallel to fieldIds
    QDateTime        savedAtUtc; ///< when this file was last written (UTC)
    QString          title;      ///< the tab/playlist label (empty in older files)
    /// The CURRENT (focus) row when the file was written; -1 for none (and in
    /// files that lack the chunk). Carried by its OWN chunk (CURR) within
    /// format version 1: the chunk framework skips unknown chunks wholesale,
    /// so a reader without it ignores it and a file without it reads as -1.
    /// Clamped against the row count at APPLY time, never trusted raw (the
    /// playlist may have been edited by a newer/other writer).
    int              currentRow = -1;
    /// The scroll position when the file was written, as the FIRST
    /// VISIBLE ROW; -1 for never-scrolled (and in files that lack the
    /// chunk). Same contract as currentRow in every respect: its own chunk
    /// (SCRL), best-effort parse, clamped only at apply.
    /// Row-quantized on purpose, matching the in-session parking: a row
    /// realigns exactly after any edit, a pixel offset would not.
    int              scrollRow = -1;
};

/// Why a read failed (None == success). Distinct cases so the UI can report a
/// precise reason rather than a generic "could not open".
enum class PlaylistIoError {
    None,
    Open,               ///< could not open the file for reading/writing
    Magic,              ///< not a rawform playlist (wrong magic number)
    UnsupportedVersion, ///< format/stream version this build can't read (refuse)
    Truncated,          ///< file/chunk shorter than its own length fields claim
    Corrupt,            ///< structurally invalid (e.g. a malformed track record)
    Write,              ///< a failure during writing/committing
};

/// The outcome of a read: an error code, a human-readable message, and (on
/// success) the parsed document.
struct PlaylistReadResult {
    PlaylistIoError  error = PlaylistIoError::None;
    QString          message;
    PlaylistDocument doc;

    [[nodiscard]] bool ok() const { return error == PlaylistIoError::None; }
};

// --- Format identity / versioning (exposed for tests + diagnostics) ---------

/// File magic. Bytes on disk (LittleEndian) read "1WFR"; the exact value is
/// arbitrary; it only has to match on read. Identity travels with the MAGIC,
/// not the file extension, so a renamed file is still recognized (or rejected).
inline constexpr quint32 kMagic = 0x52465731u; ///< 'R','F','W','1'

/// OUR format version. Bumped only for changes the chunk/record length-prefixing
/// cannot absorb. A file declaring a HIGHER version than this is refused.
/// v1's META carries savedAt plus the playlist title; it stores no advisory
/// track count (the TRKS chunk is the single authority on row count).
inline constexpr quint32 kFormatVersion = 1u;

/// The pinned QDataStream version the writer always uses. Pinned (not
/// Qt_DefaultCompiledVersion) so output is stable across Qt upgrades. May be
/// raised to a newer Qt_6_x to pick up newer type encodings; doing so widens the
/// accepted read range below automatically.
inline constexpr int kStreamVersion = QDataStream::Qt_6_6;

/// Oldest stream version this build will read. Any Qt 6 build encodes the types
/// we use (QString, QStringList, QDateTime, QMap, integers) compatibly from
/// Qt_6_0 onward, so we accept the whole 6.x range up to the pinned writer.
inline constexpr int kMinStreamVersion = QDataStream::Qt_6_0;

// --- API --------------------------------------------------------------------

/**
 * @brief Write @p doc to @p path atomically.
 *
 * Uses QSaveFile (write to a temporary, then atomic rename on commit), so a
 * crash or error mid-write never corrupts an existing file at @p path. Returns
 * false with a reason in @p error on any failure.
 */
[[nodiscard]] bool writePlaylist(const QString& path,
                                 const PlaylistDocument& doc,
                                 QString* error = nullptr);

/**
 * @brief Read a .rwfpl from @p path.
 *
 * Reads the whole file once (no per-track disk I/O, the cache win),
 * validates the header and every chunk/record length against the file size, and
 * returns the parsed document or a precise error. Unknown chunks are skipped;
 * a track chunk is required, a layout/meta chunk is optional. A higher
 * formatVersion than this build understands is refused (see PlaylistIoError).
 */
[[nodiscard]] PlaylistReadResult readPlaylist(const QString& path);

} // namespace rawform
