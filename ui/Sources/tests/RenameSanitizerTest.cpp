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
// rawform: standalone unit test for the rename-name pipeline.
//
// Same shape and constraints as PatternEvaluatorTest.cpp: Qt-Quick-FREE,
// linking Qt6::Core + yaml-cpp (transitively via ColumnSchema.cpp) plus the
// three sources under test. No model, no view, no TagLib, NO FILESYSTEM: the
// pipeline is pure strings, and the on-disk half (FileRenamer) has its own
// concerns.
//
// Build via the CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_rename_test
//     ctest --test-dir build            # or run ./build/rawform_rename_test
//
// Exits non-zero if any check fails (so it doubles as a CI gate).
// ---------------------------------------------------------------------------

#include "rename/RenameSanitizer.h"
#include "media/TrackData.h"

#include <QString>
#include <QStringList>

#include <cstdio>

using namespace rawform;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(const QString& got, const QString& want, const char* label) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::fprintf(stderr,
                     "FAIL %-30s got =\"%s\"\n                               want=\"%s\"\n",
                     label, qUtf8Printable(got), qUtf8Printable(want));
    }
}

/// The contract's worked example plus enough spread to exercise padding and folding.
TrackData sampleTrack() {
    TrackData t;
    t.artists = { QStringLiteral("Great Artist") };
    t.title   = QStringLiteral("My Super & Duper Song");
    t.album   = QStringLiteral("Celebrate!");
    t.trackNo = 1;
    t.discNo  = 1;
    t.year    = 1980;
    return t;
}

} // namespace

int main() {
    // --- sceneSanitizeValue: the per-value normalizer ------------------------
    check(sceneSanitizeValue(QStringLiteral("My Super & Duper Song")),
          QStringLiteral("my_super_and_duper_song"), "value_spec_example");
    check(sceneSanitizeValue(QStringLiteral("AC&DC")),
          QStringLiteral("ac_and_dc"), "value_tight_ampersand");
    // Apostrophes delete (straight and typographic), never underscore.
    check(sceneSanitizeValue(QStringLiteral("Don't Stop Believin'")),
          QStringLiteral("dont_stop_believin"), "value_apostrophe_ascii");
    check(sceneSanitizeValue(QStringLiteral("Don\u2019t Stop Me Now")),
          QStringLiteral("dont_stop_me_now"), "value_apostrophe_typographic");
    check(sceneSanitizeValue(QStringLiteral("\u2018Heroes\u2019")),
          QStringLiteral("heroes"), "value_apostrophe_quotes");
    check(sceneSanitizeValue(QStringLiteral("Hi De Hi, Hi De Ho")),
          QStringLiteral("hi_de_hi_hi_de_ho"), "value_punctuation");
    // Hyphens INSIDE a value fold to '_' (the field separator level is the
    // pattern's, never the value's).
    check(sceneSanitizeValue(QStringLiteral("Hi-De-Ho")),
          QStringLiteral("hi_de_ho"), "value_inner_hyphen");
    // Dots inside values fold too.
    check(sceneSanitizeValue(QStringLiteral("Mr. Blue Sky (feat. R. Bell)")),
          QStringLiteral("mr_blue_sky_feat_r_bell"), "value_dots_parens");
    // NFKD + mark strip; and the non-decomposing fold table.
    check(sceneSanitizeValue(QStringLiteral("Mot\u00f6rhead")),
          QStringLiteral("motorhead"), "value_nfkd_umlaut");
    check(sceneSanitizeValue(QStringLiteral("M\u00f8")),
          QStringLiteral("mo"), "value_fold_oslash");
    check(sceneSanitizeValue(QStringLiteral("Sigur R\u00f3s \u00c6g\u00e6tis")),
          QStringLiteral("sigur_ros_aegaetis"), "value_fold_aelig");
    // No ASCII rendering at all -> underscores -> collapses to empty.
    check(sceneSanitizeValue(QString::fromUtf8("\xe6\xad\x8c")),
          QString(), "value_cjk_empty");
    check(sceneSanitizeValue(QString()), QString(), "value_empty");

    const TrackData t = sampleTrack();

    // --- buildRenameStem: the contract's worked example -----------------------
    check(buildRenameStem(t, QStringLiteral("%track_no%-%artist%-%title%")),
          QStringLiteral("01-great_artist-my_super_and_duper_song"), "stem_spec_example");

    // Padding applies to track_no/disc_no only; year is untouched.
    check(buildRenameStem(t, QStringLiteral("%disc_no%.%track_no% %year%")),
          QStringLiteral("01.01_1980"), "stem_padding_and_literals");

    // Literal cleanup: spaces and case in the PATTERN are folded, but the
    // literal '-' keeps its separator role.
    check(buildRenameStem(t, QStringLiteral("%artist% - %title%")),
          QStringLiteral("great_artist-my_super_and_duper_song"), "stem_literal_spaces");

    // Empty-token seams: a missing album must not leave "--" or a dangling
    // separator at either end.
    {
        TrackData blank = t;
        blank.album = QString();
        check(buildRenameStem(blank, QStringLiteral("%artist%-%album%-%title%")),
              QStringLiteral("great_artist-my_super_and_duper_song"), "stem_empty_middle");
        blank.trackNo = 0;
        check(buildRenameStem(blank, QStringLiteral("%track_no%-%artist%")),
              QStringLiteral("great_artist"), "stem_empty_leading");
        check(buildRenameStem(blank, QStringLiteral("%artist%-%album%")),
              QStringLiteral("great_artist"), "stem_empty_trailing");
    }

    // Mixed separator runs collapse to '-' (field separator outranks '_').
    check(buildRenameStem(t, QStringLiteral("%artist%_-_%title%")),
          QStringLiteral("great_artist-my_super_and_duper_song"), "stem_mixed_run");

    // "%%" still escapes a literal percent, which then folds to '_' as a
    // non-portable literal char, and trims when terminal.
    check(buildRenameStem(t, QStringLiteral("100%% %title%")),
          QStringLiteral("100_my_super_and_duper_song"), "stem_percent_literal");

    // A value's '-' is protected from separator meaning end to end.
    {
        TrackData hy = t;
        hy.title = QStringLiteral("Hi-De-Ho");
        check(buildRenameStem(hy, QStringLiteral("%artist%-%title%")),
              QStringLiteral("great_artist-hi_de_ho"), "stem_value_hyphen_guard");
    }

    // Whole pattern resolving to nothing -> empty stem (caller flags it).
    {
        const TrackData none;
        check(buildRenameStem(none, QStringLiteral("%album%-%title%")),
              QString(), "stem_all_empty");
    }

    // --- composeRenameFileName ----------------------------------------------
    check(composeRenameFileName(QStringLiteral("01-a-b"), QStringLiteral("01 A b.FLAC")),
          QStringLiteral("01-a-b.flac"), "compose_ext_lowercase");
    check(composeRenameFileName(QStringLiteral("x"), QStringLiteral("noext")),
          QStringLiteral("x"), "compose_no_ext");
    check(composeRenameFileName(QStringLiteral("x"), QStringLiteral(".hidden")),
          QStringLiteral("x"), "compose_dotfile");
    check(composeRenameFileName(QStringLiteral("x"), QStringLiteral("weird.")),
          QStringLiteral("x"), "compose_trailing_dot");
    check(composeRenameFileName(QString(), QStringLiteral("a.flac")),
          QString(), "compose_empty_stem");

    // --- renameFileNameProblem ----------------------------------------------
    check(renameFileNameProblem(QStringLiteral("01-a-b.flac")), QString(), "problem_ok");
    check(renameFileNameProblem(QString()).isEmpty() ? QString() : QStringLiteral("flagged"),
          QStringLiteral("flagged"), "problem_empty_flagged");
    check(renameFileNameProblem(QStringLiteral(".")).isEmpty() ? QString() : QStringLiteral("flagged"),
          QStringLiteral("flagged"), "problem_dot_flagged");
    check(renameFileNameProblem(QStringLiteral("-x")).isEmpty() ? QString() : QStringLiteral("flagged"),
          QStringLiteral("flagged"), "problem_leading_dash_flagged");
    check(renameFileNameProblem(QStringLiteral("a b")).isEmpty() ? QString() : QStringLiteral("flagged"),
          QStringLiteral("flagged"), "problem_space_flagged");
    check(renameFileNameProblem(QString(300, QLatin1Char('a'))).isEmpty()
              ? QString() : QStringLiteral("flagged"),
          QStringLiteral("flagged"), "problem_length_flagged");

    std::printf("rename sanitizer: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
