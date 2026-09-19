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

// TrackData.h
//
// One track's tag + filesystem data: the unit shared by the playlist view, the metadata
// pane, and the .rwfpl cache. Filled by TagLib + QFileInfo in TrackReader / TrackScanner.
//
// Stores raw fields, never a pre-formatted display string; each view composes
// its own presentation. The layout is frozen into the binary cache, so a few
// choices are shaped against real-world tag messiness:
//
//  - Fields a single file legitimately carries several of (artists, genres) are
//    QStringList. ("Mixed across a multi-selection" is a separate, edit-time
//    concept, never a stored sentinel.)
//  - Any tag not promoted to a named field is kept verbatim in `extraTags`,
//    so MetadataEditor writes back only the keys an edit names and every other
//    tag round-trips losslessly.
//  - Date keeps both a parsed `year` (sort/column) and the raw `dateRaw`
//    string (full fidelity). Track and disc numbers do the same: parsed ints for
//    sort, plus `trackNoRaw` / `discNoRaw` for verbatim display.
//  - Embedded art is a flag only; the bytes are fetched lazily by
//    AlbumArtProvider, so covers never bloat the in-RAM list or the cache.
//
// mtime + fileSize double as the cache-invalidation key.

#pragma once

#include <QDateTime>
#include <QMap>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace rawform {

struct TrackData {
    // Promoted multi-value text tags (TagLib StringList).
    QStringList artists;
    QStringList albumArtists;  ///< may differ from the track artist
    QStringList genres;
    QString     album;
    QString     title;

    // Structured numerics (fast sort / display); 0 = unknown.
    int     trackNo    = 0;
    int     discNo     = 0;
    int     trackTotal = 0;
    int     discTotal  = 0;
    // Raw tagged text for the two position numbers, beside the parsed ints, in
    // the same spirit as dateRaw beside year. Preserves leading zeros ("05") and
    // non-numeric designators ("A1") for full-fidelity display and for the
    // Properties editor to detect padding (raw all-digits and != number(int))
    // when it renumbers. Empty means "use the int" (no raw was tagged).
    QString trackNoRaw;     ///< original TRACKNUMBER text (pre-slash), e.g. "05"
    QString discNoRaw;      ///< original DISCNUMBER text (pre-slash), e.g. "01"
    int     year       = 0; ///< parsed 4-digit year; the "Date" column
    QString dateRaw;        ///< original date string, e.g. "2024-03-15"

    // Audio properties (TagLib; the decoder refines these later); 0 = unknown.
    int     durationMs   = 0;
    int     bitrateKbps  = 0;
    int     sampleRateHz = 0;
    int     channels     = 0;
    QString codec;          ///< e.g. "FLAC"

    // Tag-derived technical extras (the General section in the metadata pane),
    // promoted out of the raw PropertyMap so they can show as named fields.
    QString tool;                        ///< encoder, from ENCODER / ID3 ENCODING
    bool    hasEmbeddedCuesheet = false; ///< a CUESHEET tag is present
    int     bitsPerSample = 0;           ///< lossless/PCM bits per sample (0 = N/A)
    QString codecProfile;                ///< e.g. "MPEG-1 Layer III" (often empty)
    QString tagType;                     ///< tag container(s), e.g. "ID3v2.4"
    QString audioMd5;                    ///< FLAC STREAMINFO MD5 of decoded audio (hex)

    /// Every tag not promoted above, by uppercase PropertyMap key (COMPOSER,
    /// REPLAYGAIN_*, MUSICBRAINZ_*, LYRICS, ...). Carried verbatim for lossless
    /// round-trip.
    QMap<QString, QStringList> extraTags;

    bool hasEmbeddedArt = false; ///< bytes fetched lazily by AlbumArtProvider

    // Filesystem fields (QFileInfo) + scan status.
    QString   fileName;
    QString   folderName;
    QString   filePath;         ///< absolute path; the art-provider / editor key
    qint64    fileSize = 0;     ///< also a cache-invalidation key
    QDateTime modified;         ///< also a cache-invalidation key
    QDateTime created;
    int       subsongIndex = 0; ///< for multi-track containers (cue / embedded)
    bool      valid = true;     ///< false = unreadable/corrupt; TrackScanner drops it

    /// Runtime-only "file present and readable". Set false by
    /// MetadataReloader's validation pass so the view grays a missing row
    /// instead of dropping it. Never serialized; recomputed against disk on
    /// every load; always true for a freshly scanned track.
    bool      available = true;
};

/// Join a within-file multi-value field (e.g. several artists or genres on one
/// track) for display with "; ". The metadata pane's across-selection join uses
/// "; " as well, so a within-file value and a cross-track value share the
/// separator by design: a multi-genre track inside a multi-selection reads as
/// one flat "A; B; C" list.
[[nodiscard]] inline QString joinedValues(const QStringList& values) {
    return values.join(QStringLiteral("; "));
}

} // namespace rawform
