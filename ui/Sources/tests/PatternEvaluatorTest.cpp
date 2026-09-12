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

// ---------------------------------------------------------------------------
// rawform: standalone unit test for the custom-column pattern evaluator.
//
// Deliberately Qt-Quick-FREE: it links only Qt6::Core + yaml-cpp (the latter is
// pulled in transitively by ColumnSchema.cpp, whose fieldFromString the
// evaluator calls) plus PatternEvaluator.cpp + ColumnSchema.cpp. No model, no
// view, no TagLib.
//
// Build it via the optional CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_pattern_test
//     ctest --test-dir build            # or run ./build/rawform_pattern_test
//
// or by hand (macOS/Homebrew Qt example, from ui/):
//     c++ -std=c++20 -fPIC \
//         Sources/tests/PatternEvaluatorTest.cpp \
//         Sources/columns/PatternEvaluator.cpp \
//         Sources/columns/ColumnSchema.cpp \
//         Sources/utils/Formats.cpp \
//         -I Sources -I Sources/columns -I Sources/media \
//         $(pkg-config --cflags --libs Qt6Core yaml-cpp) \
//         -o pattern_test && ./pattern_test
//
// Exits non-zero if any check fails (so it doubles as a CI gate).
// ---------------------------------------------------------------------------

#include "columns/PatternEvaluator.h"
#include "media/TrackData.h"

#include <QString>
#include <QStringList>

#include <cstdio>

using namespace rawform;

namespace {

int g_checks = 0;
int g_failures = 0;

/// Compare evaluatePattern(track, pattern) against an expected string.
void expect(const TrackData& t, const QString& pattern,
            const QString& want, const char* label) {
    ++g_checks;
    const QString got = evaluatePattern(t, pattern);
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL %-26s pattern=\"%s\"\n                           got =\"%s\"\n                           want=\"%s\"\n",
                     label,
                     qUtf8Printable(pattern),
                     qUtf8Printable(got),
                     qUtf8Printable(want));
    }
}

/// A representative track: a couple of promoted fields, a disc/track for the
/// composite, and two custom (non-promoted) tags: one single-valued, one
/// multi-valued.
TrackData sampleTrack() {
    TrackData t;
    t.artists      = { QStringLiteral("Kool & The Gang") };
    t.albumArtists = { QStringLiteral("Kool & The Gang") };
    t.album        = QStringLiteral("Celebrate!");
    t.title        = QStringLiteral("Celebration");
    t.year         = 1980;
    t.trackNo      = 1;
    t.discNo       = 1;
    // extraTags keys are uppercase (TagLib PropertyMap convention).
    t.extraTags.insert(QStringLiteral("BARCODE"),
                       { QStringLiteral("042282656328") });
    t.extraTags.insert(QStringLiteral("COMPOSER"),
                       { QStringLiteral("R. Bell"), QStringLiteral("Kool & The Gang") });
    return t;
}

} // namespace

int main() {
    const TrackData t = sampleTrack();

    // --- literals & empties --------------------------------------------------
    expect(t, QStringLiteral("hello world"), QStringLiteral("hello world"), "literal");
    expect(t, QString(),                     QString(),                     "empty_pattern");

    // --- native field tokens (strict lowercase ids) --------------------------
    expect(t, QStringLiteral("%artist%"), QStringLiteral("Kool & The Gang"), "field_artist");
    expect(t, QStringLiteral("%title%"),  QStringLiteral("Celebration"),     "field_title");
    expect(t, QStringLiteral("%year%"),   QStringLiteral("1980"),            "field_year");

    // --- composites go through renderFieldValue too --------------------------
    expect(t, QStringLiteral("%track_index%"),
           QStringLiteral("1.01"), "composite_track_index");
    expect(t, QStringLiteral("%album_group%"),
           QStringLiteral("Kool & The Gang - Celebrate!"), "composite_album_group");

    // --- custom tags from extraTags (token upper-cased for the lookup) -------
    expect(t, QStringLiteral("%barcode%"), QStringLiteral("042282656328"), "tag_barcode_lower");
    expect(t, QStringLiteral("%BARCODE%"), QStringLiteral("042282656328"), "tag_barcode_upper");
    expect(t, QStringLiteral("%composer%"),
           QStringLiteral("R. Bell; Kool & The Gang"), "tag_multi_join");

    // --- STRICT lowercase: an upper-case native id is NOT the field ----------
    // "%ARTIST%" does not match the Artist field; it falls through to
    // extraTags["ARTIST"], which is absent (ARTIST was promoted out) -> empty.
    expect(t, QStringLiteral("%ARTIST%"), QString(), "strict_lowercase_artist");

    // --- literal + token mixing (the YAML banner's example tokens) ------------
    expect(t, QStringLiteral("%artist% - %barcode%"),
           QStringLiteral("Kool & The Gang - 042282656328"), "mixed_literal");

    // --- adjacency: %a%%b% concatenates, no phantom percent ------------------
    expect(t, QStringLiteral("%artist%%title%"),
           QStringLiteral("Kool & The GangCelebration"), "adjacent_tokens");

    // --- %% escape -> a single literal percent -------------------------------
    expect(t, QStringLiteral("100%% off"), QStringLiteral("100% off"), "escape_percent");
    expect(t, QStringLiteral("%%"),        QStringLiteral("%"),        "escape_only");

    // --- unterminated '%' is lenient (literal tail) --------------------------
    expect(t, QStringLiteral("%artist"), QStringLiteral("%artist"), "unterminated_head");
    expect(t, QStringLiteral("a %b"),    QStringLiteral("a %b"),     "unterminated_tail");

    // --- unknown token -> empty ----------------------------------------------
    expect(t, QStringLiteral("%nope%"), QString(), "unknown_token");

    // --- unset native field -> empty (no "0") --------------------------------
    {
        const TrackData blank;
        expect(blank, QStringLiteral("%year%"),        QString(), "unset_year_empty");
        expect(blank, QStringLiteral("[%title%]"),     QStringLiteral("[]"), "unset_title_brackets");
    }

    std::printf("pattern evaluator: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
