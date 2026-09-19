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

// ColumnSchema.cpp
//
// Implementation of the column vocabulary declared in ColumnSchema.h: the
// ColumnField <-> string spellings (the canonical field ids that round-trip
// through .rwfpl headers and the column preset), the built-in catalog the
// schema is seeded from, the custom-column identity encoding ("custom:<id>"),
// and the alignment keyword mapping.

#include "columns/ColumnSchema.h"

#include <QHash>

#include <utility>   // std::move

namespace rawform {

std::optional<ColumnField> fieldFromString(const QString& name) {
    // Built once; the spellings are the canonical field ids (round-trip with
    // stringFromField) and align with MetadataModel::FieldId where they overlap.
    static const QHash<QString, ColumnField> kTable = {
        { QStringLiteral("artist"),        ColumnField::Artist },
        { QStringLiteral("album_artist"),  ColumnField::AlbumArtist },
        { QStringLiteral("album"),         ColumnField::Album },
        { QStringLiteral("title"),         ColumnField::Title },
        { QStringLiteral("genre"),         ColumnField::Genre },
        { QStringLiteral("year"),          ColumnField::Year },
        { QStringLiteral("track_no"),      ColumnField::TrackNo },
        { QStringLiteral("disc_no"),       ColumnField::DiscNo },
        { QStringLiteral("track_total"),   ColumnField::TrackTotal },
        { QStringLiteral("disc_total"),    ColumnField::DiscTotal },
        { QStringLiteral("duration"),      ColumnField::Duration },
        { QStringLiteral("bitrate"),       ColumnField::Bitrate },
        { QStringLiteral("sample_rate"),   ColumnField::SampleRate },
        { QStringLiteral("channels"),      ColumnField::Channels },
        { QStringLiteral("codec"),         ColumnField::Codec },
        { QStringLiteral("file_name"),     ColumnField::FileName },
        { QStringLiteral("folder_name"),   ColumnField::FolderName },
        { QStringLiteral("file_path"),     ColumnField::FilePath },
        { QStringLiteral("file_size"),     ColumnField::FileSize },
        { QStringLiteral("modified"),      ColumnField::Modified },
        { QStringLiteral("created"),       ColumnField::Created },
        { QStringLiteral("subsong_index"), ColumnField::SubsongIndex },
        { QStringLiteral("track_index"),   ColumnField::TrackIndex },
        { QStringLiteral("album_group"),   ColumnField::AlbumGroup },
    };
    const auto it = kTable.constFind(name);
    if (it == kTable.constEnd())
        return std::nullopt;
    return *it;
}

QString stringFromField(ColumnField field) {
    // Canonical field-id spellings; must round-trip with fieldFromString().
    switch (field) {
    case ColumnField::Artist:       return QStringLiteral("artist");
    case ColumnField::AlbumArtist:  return QStringLiteral("album_artist");
    case ColumnField::Album:        return QStringLiteral("album");
    case ColumnField::Title:        return QStringLiteral("title");
    case ColumnField::Genre:        return QStringLiteral("genre");
    case ColumnField::Year:         return QStringLiteral("year");
    case ColumnField::TrackNo:      return QStringLiteral("track_no");
    case ColumnField::DiscNo:       return QStringLiteral("disc_no");
    case ColumnField::TrackTotal:   return QStringLiteral("track_total");
    case ColumnField::DiscTotal:    return QStringLiteral("disc_total");
    case ColumnField::Duration:     return QStringLiteral("duration");
    case ColumnField::Bitrate:      return QStringLiteral("bitrate");
    case ColumnField::SampleRate:   return QStringLiteral("sample_rate");
    case ColumnField::Channels:     return QStringLiteral("channels");
    case ColumnField::Codec:        return QStringLiteral("codec");
    case ColumnField::FileName:     return QStringLiteral("file_name");
    case ColumnField::FolderName:   return QStringLiteral("folder_name");
    case ColumnField::FilePath:     return QStringLiteral("file_path");
    case ColumnField::FileSize:     return QStringLiteral("file_size");
    case ColumnField::Modified:     return QStringLiteral("modified");
    case ColumnField::Created:      return QStringLiteral("created");
    case ColumnField::SubsongIndex: return QStringLiteral("subsong_index");
    case ColumnField::TrackIndex:   return QStringLiteral("track_index");
    case ColumnField::AlbumGroup:   return QStringLiteral("album_group");
    }
    return {};
}

QList<ColumnField> allColumnFields() {
    // Enum order. Keep in sync with the ColumnField definition.
    return {
        ColumnField::Artist,       ColumnField::AlbumArtist, ColumnField::Album,
        ColumnField::Title,        ColumnField::Genre,       ColumnField::Year,
        ColumnField::TrackNo,      ColumnField::DiscNo,      ColumnField::TrackTotal,
        ColumnField::DiscTotal,    ColumnField::Duration,    ColumnField::Bitrate,
        ColumnField::SampleRate,   ColumnField::Channels,    ColumnField::Codec,
        ColumnField::FileName,     ColumnField::FolderName,  ColumnField::FilePath,
        ColumnField::FileSize,     ColumnField::Modified,    ColumnField::Created,
        ColumnField::SubsongIndex, ColumnField::TrackIndex,  ColumnField::AlbumGroup,
    };
}

QString humanTitleFor(ColumnField field) {
    switch (field) {
    case ColumnField::Artist:       return QStringLiteral("Artist");
    case ColumnField::AlbumArtist:  return QStringLiteral("Album Artist");
    case ColumnField::Album:        return QStringLiteral("Album");
    case ColumnField::Title:        return QStringLiteral("Title");
    case ColumnField::Genre:        return QStringLiteral("Genre");
    case ColumnField::Year:         return QStringLiteral("Date");
    case ColumnField::TrackNo:      return QStringLiteral("Track #");
    case ColumnField::DiscNo:       return QStringLiteral("Disc #");
    case ColumnField::TrackTotal:   return QStringLiteral("Total Tracks");
    case ColumnField::DiscTotal:    return QStringLiteral("Total Discs");
    case ColumnField::Duration:     return QStringLiteral("Length");
    case ColumnField::Bitrate:      return QStringLiteral("Bitrate");
    case ColumnField::SampleRate:   return QStringLiteral("Sample Rate");
    case ColumnField::Channels:     return QStringLiteral("Channels");
    case ColumnField::Codec:        return QStringLiteral("Codec");
    case ColumnField::FileName:     return QStringLiteral("File name");
    case ColumnField::FolderName:   return QStringLiteral("Folder name");
    case ColumnField::FilePath:     return QStringLiteral("File path");
    case ColumnField::FileSize:     return QStringLiteral("File size");
    case ColumnField::Modified:     return QStringLiteral("Last modified");
    case ColumnField::Created:      return QStringLiteral("Created");
    case ColumnField::SubsongIndex: return QStringLiteral("Subsong index");
    case ColumnField::TrackIndex:   return QStringLiteral("Track");
    case ColumnField::AlbumGroup:   return QStringLiteral("Artist album / album");
    }
    return {};
}

Qt::Alignment defaultAlignmentFor(ColumnField field) {
    // Code baseline alignment for every field (the analog of humanTitleFor).
    // Right alignment is reserved for the compact numeric columns (track
    // index, length, bitrate); Date/Year reads LEFT like the other text-ish
    // fields even though it is a number (a taste call, matches the default
    // layout). Listed exhaustively (no default:) so -Wswitch flags a
    // newly-added enumerator.
    switch (field) {
    case ColumnField::Duration:
    case ColumnField::TrackIndex:
    case ColumnField::Bitrate:
        return Qt::AlignRight;

    case ColumnField::Channels:
        return Qt::AlignHCenter;

    case ColumnField::Artist:
    case ColumnField::AlbumArtist:
    case ColumnField::Album:
    case ColumnField::Title:
    case ColumnField::Genre:
    case ColumnField::Year:
    case ColumnField::TrackNo:
    case ColumnField::DiscNo:
    case ColumnField::TrackTotal:
    case ColumnField::DiscTotal:
    case ColumnField::SampleRate:
    case ColumnField::Codec:
    case ColumnField::FileName:
    case ColumnField::FolderName:
    case ColumnField::FilePath:
    case ColumnField::FileSize:
    case ColumnField::Modified:
    case ColumnField::Created:
    case ColumnField::SubsongIndex:
    case ColumnField::AlbumGroup:
        return Qt::AlignLeft;
    }
    return Qt::AlignLeft; // unreachable (switch is total); keeps the compiler happy
}

bool isCompositeField(ColumnField field) {
    // Exhaustive like defaultAlignmentFor, so -Wswitch flags a new enumerator
    // and forces the composite/plain decision to be made for it.
    switch (field) {
    case ColumnField::TrackIndex:  // "Track": disc.track from discNo + trackNo
    case ColumnField::AlbumGroup:  // "Artist album / album"
        return true;

    case ColumnField::Artist:
    case ColumnField::AlbumArtist:
    case ColumnField::Album:
    case ColumnField::Title:
    case ColumnField::Genre:
    case ColumnField::Year:
    case ColumnField::TrackNo:
    case ColumnField::DiscNo:
    case ColumnField::TrackTotal:
    case ColumnField::DiscTotal:
    case ColumnField::Duration:
    case ColumnField::Bitrate:
    case ColumnField::SampleRate:
    case ColumnField::Channels:
    case ColumnField::Codec:
    case ColumnField::FileName:
    case ColumnField::FolderName:
    case ColumnField::FilePath:
    case ColumnField::FileSize:
    case ColumnField::Modified:
    case ColumnField::Created:
    case ColumnField::SubsongIndex:
        return false;
    }
    return false; // unreachable (switch is total); keeps the compiler happy
}

QList<DefaultColumn> defaultColumnArrangement() {
    // The default playlist columns, in display order, with their seed widths;
    // the single source of truth for the default layout. Any field not listed is
    // still offerable, just not shown by default (it seeds at
    // ColumnSchema::kDefaultColumnWidth when toggled on).
    return {
        { ColumnField::TrackIndex,  70 },
        { ColumnField::AlbumGroup, 300 },
        { ColumnField::Artist,     200 },
        { ColumnField::Title,      400 },
        { ColumnField::Year,        60 },
        { ColumnField::Genre,      120 },
        { ColumnField::Duration,    64 }, // "Length"; right-aligned m:ss
        { ColumnField::Codec,       70 },
    };
}

ColumnSchema ColumnSchema::load() {
    // Built entirely from code: one entry per field, in enum order, recording
    // only the field. Title and alignment are derived on demand from
    // humanTitleFor() / defaultAlignmentFor(). No config I/O (a column you want
    // renamed becomes a custom column instead).
    ColumnSchema schema;
    const QList<ColumnField> all = allColumnFields();
    schema.entries.reserve(all.size());
    for (const ColumnField f : all) {
        schema.entries.push_back(Entry{ .field = f });
    }
    return schema;
}

// --- Custom-column identity / encoding -------------------------------------

bool isCustomFieldId(const QString& fieldId) {
    return fieldId.startsWith(QLatin1String(kCustomFieldPrefix));
}

QString customIdFromFieldId(const QString& fieldId) {
    if (!isCustomFieldId(fieldId))
        return {};
    // Strip exactly the prefix (sizeof includes the NUL, hence -1); the
    // remainder, which may itself contain ':', is the id verbatim.
    constexpr int kPrefixLen = static_cast<int>(sizeof(kCustomFieldPrefix) - 1);
    return fieldId.mid(kPrefixLen);
}

QString customFieldId(const QString& id) {
    return QLatin1String(kCustomFieldPrefix) + id;
}

// --- Alignment <-> keyword -------------------------------------------------

Qt::Alignment alignmentFromString(const QString& keyword) {
    const QString k = keyword.trimmed().toLower();
    if (k == QLatin1String("right"))
        return Qt::AlignRight;
    if (k == QLatin1String("center") || k == QLatin1String("centre"))
        return Qt::AlignHCenter;
    return Qt::AlignLeft; // "left", empty, or anything unrecognized
}

QString stringFromAlignment(Qt::Alignment alignment) {
    // Only the horizontal component matters for a column.
    const Qt::Alignment h = alignment & Qt::AlignHorizontal_Mask;
    if (h == Qt::AlignRight)
        return QStringLiteral("right");
    if (h == Qt::AlignHCenter)
        return QStringLiteral("center");
    return QStringLiteral("left");
}

} // namespace rawform
