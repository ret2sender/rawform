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

// PlaylistSearchFilterTest.cpp
//
// Standalone unit test for the Find grammar, validation, and match test.
//
// Qt-Quick-FREE like its siblings: Qt6::Core plus PlaylistSearchFilter.cpp,
// PatternEvaluator.cpp, ColumnSchema.cpp and Formats.cpp. No model, no view,
// no TagLib. Build via the optional CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_search_test
//     ctest --test-dir build            # or run ./build/rawform_search_test
//
// Exits non-zero if any check fails (so it doubles as a CI gate).

#include "search/PlaylistSearchFilter.h"

#include "columns/ColumnSchema.h"
#include "media/TrackData.h"

#include <QString>
#include <QStringList>

#include <cstdio>

using namespace rawform;
using namespace rawform::search;

namespace {

int g_checks   = 0;
int g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

void expectStr(const QString& got, const QString& want, const char* label) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL %-28s got =\"%s\"\n"
                     "                             want=\"%s\"\n",
                     label, qUtf8Printable(got), qUtf8Printable(want));
    }
}

/// A track with accented tags (the fold cases), a multi-valued native field,
/// and one custom tag, so both token resolution paths are exercised.
TrackData sampleTrack() {
    TrackData t;
    t.artists      = { QStringLiteral("Beyonc\u00e9"), QStringLiteral("Jay-Z") };
    t.albumArtists = { QStringLiteral("Beyonc\u00e9") };
    t.album        = QStringLiteral("Renaissance");
    t.title        = QStringLiteral("Am\u00e9rica Has a Problem");
    t.year         = 2022;
    t.trackNo      = 9;
    t.discNo       = 1;
    t.fileName     = QStringLiteral("09 - America Has a Problem.flac");
    t.extraTags.insert(QStringLiteral("COMPOSER"), { QStringLiteral("The-Dream") });
    return t;
}

FilterContext sampleContext() {
    FilterContext ctx;
    CustomColumn combo;
    combo.id      = QStringLiteral("abc123");
    combo.name    = QStringLiteral("Artist / Title");
    combo.pattern = QStringLiteral("%artist% / %title%");
    ctx.customColumns.append(combo);
    ctx.extraTagKeys.insert(QStringLiteral("COMPOSER"));
    return ctx;
}

} // namespace

int main() {
    const TrackData     t   = sampleTrack();
    const FilterContext ctx = sampleContext();

    // --- foldForSearch -----------------------------------------------------
    expectStr(foldForSearch(QStringLiteral("Beyonc\u00e9")),
              QStringLiteral("beyonce"), "fold_accent_case");
    expectStr(foldForSearch(QStringLiteral("\u00c9\u00c0\u00dc\u00d1")),
              QStringLiteral("eaun"), "fold_uppercase_accents");
    expectStr(foldForSearch(QStringLiteral("plain 123")),
              QStringLiteral("plain 123"), "fold_identity");
    expectStr(foldForSearch(QString()), QString(), "fold_empty");
    // The separator survives the fold (it is a control character, not a mark).
    expectStr(foldForSearch(QString(QChar(kHaystackSeparator))),
              QString(QChar(kHaystackSeparator)), "fold_keeps_separator");

    // --- queryTerms --------------------------------------------------------
    check(queryTerms(QString()).isEmpty(), "terms_empty");
    check(queryTerms(QStringLiteral("   \t ")).isEmpty(), "terms_blank");
    check(queryTerms(QStringLiteral("  Foo\tBAR  baz ")) ==
              QStringList({ QStringLiteral("foo"), QStringLiteral("bar"),
                            QStringLiteral("baz") }),
          "terms_split_fold");

    // --- parseFilter: column entries ---------------------------------------
    {
        const ParsedFilter p = parseFilter(QStringLiteral("Artist"), ctx);
        check(p.entries.size() == 1, "col_title_count");
        check(p.entries.at(0).valid, "col_title_valid");
        check(p.entries.at(0).kind == FilterEntry::Kind::Column, "col_title_kind");
        check(!p.entries.at(0).resolved.isPattern, "col_title_native");
        check(p.entries.at(0).resolved.field == ColumnField::Artist,
              "col_title_field");
    }
    {
        // Titles resolve case-insensitively, including the ones that differ
        // from their field id ("Date" -> Year, "Length" -> Duration).
        const ParsedFilter p =
            parseFilter(QStringLiteral("date : LENGTH : track #"), ctx);
        check(p.entries.size() == 3, "col_titles_count");
        check(!p.hasInvalid(), "col_titles_all_valid");
        check(p.entries.at(0).resolved.field == ColumnField::Year, "col_title_date");
        check(p.entries.at(1).resolved.field == ColumnField::Duration,
              "col_title_length");
        check(p.entries.at(2).resolved.field == ColumnField::TrackNo,
              "col_title_track_no");
    }
    {
        // Custom column by name -> its pattern.
        const ParsedFilter p = parseFilter(QStringLiteral("artist / title"), ctx);
        check(p.entries.size() == 1 && p.entries.at(0).valid, "col_custom_valid");
        check(p.entries.at(0).resolved.isPattern, "col_custom_is_pattern");
        expectStr(p.entries.at(0).resolved.pattern,
                  QStringLiteral("%artist% / %title%"), "col_custom_pattern");
    }
    {
        // Field id, case-insensitive, as the last resort.
        const ParsedFilter p = parseFilter(QStringLiteral("Album_Artist"), ctx);
        check(p.entries.size() == 1 && p.entries.at(0).valid, "col_id_valid");
        check(p.entries.at(0).resolved.field == ColumnField::AlbumArtist,
              "col_id_field");
    }
    {
        const ParsedFilter p = parseFilter(QStringLiteral("Artists"), ctx);
        check(p.entries.size() == 1 && !p.entries.at(0).valid, "col_unknown_invalid");
        check(p.hasInvalid(), "col_unknown_has_invalid");
        check(p.searchables().isEmpty(), "col_unknown_excluded");
    }

    // --- parseFilter: pattern entries --------------------------------------
    {
        const ParsedFilter p = parseFilter(QStringLiteral("%artist% - %album%"), ctx);
        check(p.entries.size() == 1 && p.entries.at(0).valid, "pat_native_valid");
        check(p.entries.at(0).kind == FilterEntry::Kind::Pattern, "pat_native_kind");
        expectStr(p.entries.at(0).resolved.pattern,
                  QStringLiteral("%artist% - %album%"), "pat_native_text");
    }
    {
        // A custom tag key present in the playlist validates, either case.
        check(!parseFilter(QStringLiteral("%composer%"), ctx).hasInvalid(),
              "pat_tag_lower");
        check(!parseFilter(QStringLiteral("%COMPOSER%"), ctx).hasInvalid(),
              "pat_tag_upper");
    }
    {
        // Unknown token, unterminated token: invalid. "%%" alone is a literal
        // percent, valid.
        check(parseFilter(QStringLiteral("%conductor%"), ctx).hasInvalid(),
              "pat_unknown");
        check(parseFilter(QStringLiteral("%artist"), ctx).hasInvalid(),
              "pat_unterminated");
        check(parseFilter(QStringLiteral("%artist%%"), ctx).hasInvalid(),
              "pat_trailing_lone_percent");
        check(!parseFilter(QStringLiteral("%%"), ctx).hasInvalid(), "pat_escape_only");
        check(!parseFilter(QStringLiteral("100%% %title%"), ctx).hasInvalid(),
              "pat_escape_then_token");
    }
    {
        // Native ids are strict lowercase in patterns, as in the evaluator:
        // "%ARTIST%" is a tag lookup, and ARTIST is not in extraTagKeys.
        check(parseFilter(QStringLiteral("%ARTIST%"), ctx).hasInvalid(),
              "pat_strict_case");
    }

    // --- parseFilter: separators, spans, mixing ----------------------------
    {
        const ParsedFilter p =
            parseFilter(QStringLiteral(" Artist :: %title% : Bogus "), ctx);
        check(p.entries.size() == 3, "mix_count_skips_empty");
        check(p.entries.at(0).valid && p.entries.at(1).valid && !p.entries.at(2).valid,
              "mix_validity");
        check(p.searchables().size() == 2, "mix_searchables");
        // Spans cover the trimmed text, offset into the ORIGINAL string.
        check(p.entries.at(0).start == 1 && p.entries.at(0).length == 6, "mix_span_0");
        check(p.entries.at(1).start == 11 && p.entries.at(1).length == 7, "mix_span_1");
        check(p.entries.at(2).start == 21 && p.entries.at(2).length == 5, "mix_span_2");
        expectStr(p.entries.at(2).text, QStringLiteral("Bogus"), "mix_text_2");
    }
    check(parseFilter(QString(), ctx).entries.isEmpty(), "empty_filter");
    check(parseFilter(QStringLiteral(" : : "), ctx).entries.isEmpty(), "only_separators");

    // --- buildHaystack -----------------------------------------------------
    {
        const QList<Searchable> set = {
            Searchable::forField(ColumnField::Artist),
            Searchable::forField(ColumnField::Title),
        };
        // Brace-init: with parentheses this line is a function declaration.
        const QString sep{ QChar(kHaystackSeparator) };
        expectStr(buildHaystack(t, set),
                  QStringLiteral("beyonce; jay-z") + sep
                      + QStringLiteral("america has a problem"),
                  "haystack_two_fields");
        expectStr(buildHaystack(t, {}), QString(), "haystack_empty_set");
        const QList<Searchable> one = {
            Searchable::forPattern(QStringLiteral("%year% %composer%")),
        };
        expectStr(buildHaystack(t, one), QStringLiteral("2022 the-dream"),
                  "haystack_pattern");
    }

    // --- haystackMatches ---------------------------------------------------
    {
        const QList<Searchable> set = {
            Searchable::forField(ColumnField::Artist),
            Searchable::forField(ColumnField::Album),
            Searchable::forField(ColumnField::Title),
        };
        const QString hay = buildHaystack(t, set);

        check(haystackMatches(hay, {}), "match_no_terms");
        check(haystackMatches(hay, queryTerms(QStringLiteral("beyonce"))),
              "match_accent_fold");
        check(haystackMatches(hay, queryTerms(QStringLiteral("BEYONC\u00c9"))),
              "match_query_folded_too");
        // AND across terms, each term free to hit a different field.
        check(haystackMatches(hay, queryTerms(QStringLiteral("jay renaissance problem"))),
              "match_and_across_fields");
        check(!haystackMatches(hay, queryTerms(QStringLiteral("jay nope"))),
              "match_and_fails");
        // A term never spans two values: the end of Artist plus the start of
        // Album is not a match.
        check(!haystackMatches(hay, queryTerms(QStringLiteral("zrenaissance"))),
              "match_no_cross_field");
        // Substring, not word: "aissa" hits inside "Renaissance".
        check(haystackMatches(hay, queryTerms(QStringLiteral("aissa"))),
              "match_substring");
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
