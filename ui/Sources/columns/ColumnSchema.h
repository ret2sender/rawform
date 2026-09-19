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

// ColumnSchema.h
//
// The vocabulary of values a playlist column can display: the id a schema entry resolves
// to, and the key PlaylistModel dispatches on when rendering a cell.
//
// Most enumerators are 1:1 reads of a TrackData member. The two trailing
// COMPOSITE values are fixed built-in renderers (a switch in PatternEvaluator,
// not a format-string parser):
//
//   - TrackIndex -> "disc.track", e.g. "1.01"
//   - AlbumGroup -> "AlbumArtist - Album"
//
// The string spellings (fieldFromString) match MetadataModel::FieldId where they
// overlap; this enum is a superset (it adds the two composites).

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <Qt>  // Qt::Alignment

#include <optional>

namespace rawform {

enum class ColumnField {
    // Direct tag fields.
    Artist,
    AlbumArtist,
    Album,
    Title,
    Genre,
    Year,
    TrackNo,
    DiscNo,
    TrackTotal,
    DiscTotal,

    // Direct audio properties.
    Duration,
    Bitrate,
    SampleRate,
    Channels,
    Codec,

    // Direct filesystem fields.
    FileName,
    FolderName,
    FilePath,
    FileSize,
    Modified,
    Created,
    SubsongIndex,

    // Composite (built-in) renderers.
    TrackIndex, ///< "disc.track" from discNo + trackNo, e.g. "1.01"
    AlbumGroup, ///< "AlbumArtist - Album"
};

/// The catalog of playlist columns, built in code.
///
/// Pure data: a list of entries, each naming one field. It carries no rendering
/// logic; PlaylistModel turns (entry.field + a track) into a display string.
///
/// Built entirely from code (see load()): one entry per field. A field's title
/// and alignment are derived on demand from humanTitleFor() / defaultAlignmentFor()
/// (the single sources of truth), never stored on the entry. There is no config
/// file and no per-user override tier; to show a column under a different name (or
/// built from several fields), use a CUSTOM column (CustomColumnRegistry). Which
/// columns show by default, in what order and at what width, is
/// defaultColumnArrangement().
struct ColumnSchema {
    /// One catalog column: just a field. (Title/alignment are derived, not
    /// stored; width and order are arrangement concerns.)
    struct Entry {
        ColumnField field; ///< what this column shows
    };

    /// The full catalog: one entry per ColumnField, in enum order, so every
    /// field is offerable (the header right-click list). Not the display order;
    /// that is defaultColumnArrangement().
    QList<Entry> entries;

    /// Width fallback for any column with no arrangement seed.
    static constexpr int kDefaultColumnWidth = 120;

    /// Assemble the active schema (the code-built catalog). No I/O; always
    /// returns a usable, non-empty schema.
    [[nodiscard]] static ColumnSchema load();
};

/// Map a field-id string to a ColumnField. std::nullopt for unknown names.
[[nodiscard]] std::optional<ColumnField> fieldFromString(const QString& name);

/// The inverse of fieldFromString(): the canonical field-id spelling. This is
/// the stable identity used to persist column order per playlist (the .rwfpl /
/// .rwftp store field-id strings, never positional indices). Total over the enum.
[[nodiscard]] QString stringFromField(ColumnField field);

/// The header label for a field (code-only, the single source of truth; rename
/// via a custom column instead). Total over the enum.
[[nodiscard]] QString humanTitleFor(ColumnField field);

/// The code baseline alignment for a field (the analog of humanTitleFor). Only
/// track index and year are AlignRight; everything else, numeric fields
/// included, is AlignLeft. Total over the enum.
[[nodiscard]] Qt::Alignment defaultAlignmentFor(ColumnField field);

/// True for the COMPOSITE presentation fields: columns whose value is derived
/// from several tags rather than mirroring one ("Track" = disc.track from
/// discNo+trackNo; "Artist album / album"). The header menu lists these in
/// their own separator-segregated group; plain fields sort alphabetically
/// above them.
[[nodiscard]] bool isCompositeField(ColumnField field);

/// Every ColumnField, in enum order. The catalog and the header menu are built
/// over this. Total over the enum.
[[nodiscard]] QList<ColumnField> allColumnFields();

/// One column in the code-level default arrangement: a field plus its seed
/// pixel width. (Width is the only per-column number outside the catalog.)
struct DefaultColumn {
    ColumnField field;
    int         seedWidth;
};

/// The default arrangement: which columns a fresh playlist shows, in display
/// order, with seed widths; the single source of truth for the default layout.
/// A field not listed is still offerable, just not shown by default (it seeds at
/// kDefaultColumnWidth when toggled on). A runtime .rwftp preset overrides this
/// at startup when present.
[[nodiscard]] QList<DefaultColumn> defaultColumnArrangement();

// ===========================================================================
// Custom columns
// ===========================================================================
//
// A custom column is a user-defined record (not a catalog Entry, not a
// ColumnField): a label, an alignment, and a %token% pattern
// evaluated by PatternEvaluator. The records are owned by CustomColumnRegistry
// and persisted to playlist_custom_columns.yaml; this header declares only the
// plain record type plus the identity/encoding helpers the model and registry
// share.
//
// Identity is a stable generated id, not the name, so renames and pattern edits
// never break the per-playlist arrangement references (which store columns by
// field-id string). An arrangement encodes a custom column as "custom:<id>";
// native columns keep their bare field-id (stringFromField). fieldFromString
// stays strictly enum: "custom:<id>" is parsed only by the helpers below, at the
// one layer (the model) that owns the id.

/// A single user-defined custom column. Pure data; the pattern is interpreted by
/// PatternEvaluator::evaluatePattern, the alignment by the model + QML delegates.
struct CustomColumn {
    QString       id;                       ///< stable generated identity ("custom:<id>")
    QString       name;                     ///< header label (display only; freely renamable)
    Qt::Alignment alignment = Qt::AlignLeft; ///< Left / HCenter / Right
    QString       pattern;                  ///< %token% + literal expression
};

/// The field-id prefix marking a custom-column reference in an arrangement.
/// Public so callers can reason about it, but prefer the helpers below.
inline constexpr char kCustomFieldPrefix[] = "custom:";

/// True if @p fieldId names a custom column (starts with "custom:").
[[nodiscard]] bool isCustomFieldId(const QString& fieldId);

/// The id embedded in @p fieldId ("custom:<id>" -> "<id>"), or empty if not a
/// custom field id. Splits on the first prefix only, so an id may itself contain
/// a colon without ambiguity.
[[nodiscard]] QString customIdFromFieldId(const QString& fieldId);

/// The canonical field-id for a custom column ("<id>" -> "custom:<id>"). The
/// inverse of customIdFromFieldId.
[[nodiscard]] QString customFieldId(const QString& id);

/// Map an alignment keyword ("left"/"center"/"right") to a Qt::Alignment.
/// Unknown/empty -> Qt::AlignLeft. Case-insensitive.
[[nodiscard]] Qt::Alignment alignmentFromString(const QString& keyword);

/// The inverse: a Qt::Alignment to its keyword. Only the horizontal flag is
/// considered; anything but Right / HCenter -> "left".
[[nodiscard]] QString stringFromAlignment(Qt::Alignment alignment);

} // namespace rawform
