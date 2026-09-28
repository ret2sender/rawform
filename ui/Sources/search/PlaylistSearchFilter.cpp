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

// PlaylistSearchFilter.cpp
//
// Implementation of the Find grammar and match test. The pattern tokenizer
// here mirrors evaluatePattern's byte for byte ("%%" literal, a lone '%' opens
// a token the next '%' closes) so that an entry this file calls valid renders
// exactly the tokens the evaluator will resolve; the two must not drift.

#include "search/PlaylistSearchFilter.h"

#include "columns/ColumnSchema.h"    // fieldFromString, humanTitleFor, allColumnFields
#include "columns/PatternEvaluator.h" // renderFieldValue, evaluatePattern

#include <QChar>
#include <QLatin1Char>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <utility>

namespace rawform::search {
namespace {

/// Validate one pattern entry: every closed token must name a native field
/// (strict, case-sensitive ids, exactly as resolveToken matches them) or a tag
/// key present in the playlist (upper-cased, the extraTags convention); an
/// unterminated '%' is invalid here even though the evaluator tolerates it,
/// because in a search a half-typed token would silently match literal text.
bool patternIsValid(const QString& pattern, const FilterContext& context) {
    const qsizetype n = pattern.size();
    qsizetype i = 0;
    while (i < n) {
        if (pattern.at(i) != QLatin1Char('%')) {
            ++i;
            continue;
        }
        // "%%" -> a literal percent, never a token.
        if (i + 1 < n && pattern.at(i + 1) == QLatin1Char('%')) {
            i += 2;
            continue;
        }
        const qsizetype nameStart = i + 1;
        qsizetype j = nameStart;
        while (j < n && pattern.at(j) != QLatin1Char('%')) {
            ++j;
        }
        if (j >= n) {
            return false;
        }
        const QString name = pattern.mid(nameStart, j - nameStart);
        if (!fieldFromString(name) && !context.extraTagKeys.contains(name.toUpper())) {
            return false;
        }
        i = j + 1;
    }
    return true;
}

/// Resolve a bare column reference (see the header's GRAMMAR order). Titles
/// first because they are what the user reads in the header; the field id is
/// last so "album_artist" still works for someone who knows the ids.
std::optional<Searchable> resolveColumn(const QString& name,
                                        const FilterContext& context) {
    for (const ColumnField field : allColumnFields()) {
        if (humanTitleFor(field).compare(name, Qt::CaseInsensitive) == 0) {
            return Searchable::forField(field);
        }
    }
    for (const CustomColumn& column : context.customColumns) {
        if (column.name.trimmed().compare(name, Qt::CaseInsensitive) == 0) {
            return Searchable::forPattern(column.pattern);
        }
    }
    if (const std::optional<ColumnField> field = fieldFromString(name.toLower())) {
        return Searchable::forField(*field);
    }
    return std::nullopt;
}

/// Build one entry from its trimmed text and span.
FilterEntry makeEntry(QString text, qsizetype start, const FilterContext& context) {
    FilterEntry entry;
    entry.start  = start;
    entry.length = text.size();
    if (text.contains(QLatin1Char('%'))) {
        entry.kind  = FilterEntry::Kind::Pattern;
        entry.valid = patternIsValid(text, context);
        if (entry.valid) {
            entry.resolved = Searchable::forPattern(text);
        }
    } else {
        entry.kind = FilterEntry::Kind::Column;
        if (const std::optional<Searchable> resolved = resolveColumn(text, context)) {
            entry.valid    = true;
            entry.resolved = *resolved;
        }
    }
    entry.text = std::move(text);
    return entry;
}

} // namespace

QString foldForSearch(const QString& text) {
    // NFD splits a precomposed letter into base + combining mark; dropping the
    // non-spacing marks (Mn) is the standard accent strip. Only Mn: spacing
    // combining marks (Mc) carry meaning in some scripts and are kept.
    const QString decomposed = text.normalized(QString::NormalizationForm_D);
    QString out;
    out.reserve(decomposed.size());
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing) {
            continue;
        }
        out.append(c);
    }
    return out.toCaseFolded();
}

QStringList queryTerms(const QString& query) {
    // simplified() collapses every whitespace run (tabs included) to one
    // space, so a split on ' ' is a split on whitespace.
    return foldForSearch(query).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

Searchable Searchable::forField(ColumnField field) {
    Searchable s;
    s.field = field;
    return s;
}

Searchable Searchable::forPattern(QString pattern) {
    Searchable s;
    s.isPattern = true;
    s.pattern   = std::move(pattern);
    return s;
}

QList<Searchable> ParsedFilter::searchables() const {
    QList<Searchable> out;
    for (const FilterEntry& entry : entries) {
        if (entry.valid) {
            out.append(entry.resolved);
        }
    }
    return out;
}

bool ParsedFilter::hasInvalid() const {
    return std::ranges::any_of(entries, [](const FilterEntry& e) { return !e.valid; });
}

ParsedFilter parseFilter(const QString& filterText, const FilterContext& context) {
    ParsedFilter parsed;
    const qsizetype n = filterText.size();
    qsizetype segmentStart = 0;
    // One pass over the text; the sentinel step at i == n closes the last
    // segment without a trailing-separator special case.
    for (qsizetype i = 0; i <= n; ++i) {
        if (i < n && filterText.at(i) != QLatin1Char(':')) {
            continue;
        }
        qsizetype s = segmentStart;
        qsizetype e = i;
        while (s < e && filterText.at(s).isSpace()) {
            ++s;
        }
        while (e > s && filterText.at(e - 1).isSpace()) {
            --e;
        }
        if (e > s) {
            parsed.entries.append(makeEntry(filterText.mid(s, e - s), s, context));
        }
        segmentStart = i + 1;
    }
    return parsed;
}

QString buildHaystack(const TrackData& track, const QList<Searchable>& set) {
    QString joined;
    for (qsizetype k = 0; k < set.size(); ++k) {
        if (k > 0) {
            joined.append(QChar(kHaystackSeparator));
        }
        const Searchable& s = set.at(k);
        joined.append(s.isPattern ? evaluatePattern(track, s.pattern)
                                  : renderFieldValue(track, s.field));
    }
    return foldForSearch(joined);
}

bool haystackMatches(const QString& haystack, const QStringList& foldedTerms) {
    return std::ranges::all_of(foldedTerms, [&haystack](const QString& term) {
        return haystack.contains(term);
    });
}

} // namespace rawform::search
