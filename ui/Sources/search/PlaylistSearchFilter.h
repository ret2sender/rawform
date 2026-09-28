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

// PlaylistSearchFilter.h
//
// The pure half of Edit > Find: the Filter-box grammar, its validation, and
// the per-track match test the playlist filter proxy runs.
//
// Qt-Quick-free (QtCore + ColumnSchema + PatternEvaluator + TrackData only),
// for the same reason PatternEvaluator is: one place decides what a Filter
// entry means and what a query matches, and tests/PlaylistSearchFilterTest.cpp
// exercises it without a model or a window.
//
// GRAMMAR. The Filter box holds entries separated by ':'. An entry containing
// a '%' is a PATTERN, evaluated per track by evaluatePattern, so
// "%artist% - %album%" is one searchable string. Any other entry is a COLUMN
// reference, resolved case-insensitively after trimming, in this order: a
// native column title (humanTitleFor: "Date", "Track #", "Length"), a custom
// column's name, a native field id ("album_artist"). Empty entries ("a::b")
// are skipped, not errors. ':' is reserved by the grammar; no entry can hold
// one.
//
// VALIDITY. A column entry that resolves to nothing is invalid. A pattern
// entry is invalid when it has an unterminated '%' or a token that is neither
// a native field id nor a tag key present somewhere in the playlist
// (FilterContext::extraTagKeys, the only way "%COMPOSER%" can be judged at
// all). Invalid entries carry their character span so the dialog can
// underline them, and are left out of the search set. What happens when no
// valid entry remains is the caller's decision: the proxy shows every row, so
// a half-typed filter never blanks the playlist.
//
// MATCHING. The String box splits on whitespace into terms. Every term must
// be a substring of at least one searched value (AND across terms, OR across
// the set); comparison is case- and diacritic-insensitive (foldForSearch).
// The per-track values are joined into one HAYSTACK string on U+001F, a
// character no term can contain (terms are whitespace-split, and U+001F is a
// control character), so a term can never match across two values. The proxy
// caches one haystack per row, and a keystroke costs one contains() per row.

#pragma once

#include "columns/ColumnSchema.h" // ColumnField, CustomColumn
#include "media/TrackData.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal> // qsizetype

namespace rawform::search {

/// The value separator inside a haystack (see the MATCHING note above).
inline constexpr char16_t kHaystackSeparator = u'\x1F';

/// Case- and diacritic-insensitive normalization: NFD, non-spacing marks
/// dropped, then case-folded. Applied identically to haystacks and query
/// terms, which is what lets an unaccented query find an accented tag.
[[nodiscard]] QString foldForSearch(const QString& text);

/// The String box as folded, non-empty, whitespace-separated terms. Empty for
/// a blank query, which matches every row.
[[nodiscard]] QStringList queryTerms(const QString& query);

/// One source of searchable text for a track: a native field (rendered by
/// renderFieldValue) or a %token% pattern (rendered by evaluatePattern). A
/// custom column resolves to its pattern, so a shown custom column and a
/// "%...%" entry are the same thing here.
struct Searchable {
    bool        isPattern = false;
    ColumnField field{};    ///< meaningful iff !isPattern
    QString     pattern;    ///< meaningful iff isPattern

    [[nodiscard]] static Searchable forField(ColumnField field);
    [[nodiscard]] static Searchable forPattern(QString pattern);
};

/// What the parser needs from the playlist to resolve and validate entries.
/// Built by the caller from the custom-column registry and one pass over the
/// tracks; the parser itself never touches a model.
struct FilterContext {
    QList<CustomColumn> customColumns; ///< bare-name resolution (name -> pattern)
    QSet<QString>       extraTagKeys;  ///< upper-cased keys present in any track
};

/// One parsed Filter-box entry, with its span in the filter string so the
/// dialog can underline an invalid one in place.
struct FilterEntry {
    enum class Kind { Column, Pattern };

    Kind       kind = Kind::Column;
    QString    text;        ///< the entry as typed, trimmed
    qsizetype  start = 0;   ///< offset of text within the filter string
    qsizetype  length = 0;  ///< text.size()
    bool       valid = false;
    Searchable resolved;    ///< meaningful iff valid
};

/// The parse result: every non-empty entry in order, valid or not.
struct ParsedFilter {
    QList<FilterEntry> entries;

    /// The search set: the valid entries' Searchables, in entry order.
    [[nodiscard]] QList<Searchable> searchables() const;

    /// True when at least one entry is invalid (the dialog's danger tint).
    [[nodiscard]] bool hasInvalid() const;
};

/// Parse and validate the Filter box (see GRAMMAR / VALIDITY above). Pure.
[[nodiscard]] ParsedFilter parseFilter(const QString& filterText,
                                       const FilterContext& context);

/// The folded haystack for @p track over @p set: each Searchable rendered,
/// joined on kHaystackSeparator, then foldForSearch. Empty set -> "".
[[nodiscard]] QString buildHaystack(const TrackData& track, const QList<Searchable>& set);

/// True when every term is a substring of @p haystack. Both sides must already
/// be folded (buildHaystack / queryTerms). An empty term list matches.
[[nodiscard]] bool haystackMatches(const QString& haystack,
                                   const QStringList& foldedTerms);

} // namespace rawform::search
