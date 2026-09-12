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

// =============================================================================
// RenameSanitizer.h
//
// The pure string pipeline behind File Operations > Rename To: from a
// %token% pattern plus a track's metadata to a POSIX-portable, scene-style
// file name. QtCore + PatternEvaluator only; no I/O, no Qt Quick, no TagLib,
// so the whole pipeline is unit-testable standalone (tests/RenameSanitizerTest
// .cpp) exactly like the pattern evaluator it builds on.
//
// The pipeline has three stages, split so each is testable and so the
// literal-vs-value distinction is structural rather than heuristic:
//
//   1. sceneSanitizeValue: per-VALUE normalization, applied to each resolved
//      %token% through evaluatePattern's TokenValueTransform hook. Pattern
//      literals never pass through it; that is what lets a '-' inside a title
//      become '_' while the '-' the user typed between tokens stays the field
//      separator.
//   2. buildRenameStem: evaluates the pattern with the rename transform
//      (per-value sanitize + 2-digit padding of track_no/disc_no),
//      then runs the whole-stem finalize pass: case-fold and charset-map any
//      pattern LITERALS, collapse separator runs (so an empty token never
//      leaves "artist--title" or a leading '-'), and trim the ends.
//   3. composeRenameFileName + renameFileNameProblem: staple the source
//      file's extension back on (lowercased) and give the final verdict a
//      caller can gate Apply on.
//
// TARGET SHAPE (the contract): the stem is drawn from [a-z0-9._-]; '_'
// separates words INSIDE a field, '-' separates FIELDS (so it can only come
// from pattern literals), '&' reads as "and". Worked example: ARTIST "Great
// Artist", TITLE "My Super & Duper Song", track 1, under the pattern
// "%track_no%-%artist%-%title%", yields the stem
// "01-great_artist-my_super_and_duper_song". The result is POSIX portable by
// construction: dots inside VALUES are folded to '_' (see DOTS below), so the
// only dots in a full name are pattern-literal dots plus the extension one.
//
// DOTS: the value charset is [a-z0-9] with everything else -> '_', '.'
// included; structural dots come from pattern literals only. The scenario
// this prevents: a '.' surviving inside a value gives "Mr. Blue Sky" ->
// "mr._blue_sky" and "(feat. X)" -> "feat._x", dot-underscore collisions no
// scene name has, plus fake-double-extension confusion. The gate is
// valueCharsToUnderscores.
// =============================================================================

#include "media/TrackData.h" // TrackData

#include <QString>

namespace rawform {

/**
 * @brief Normalize ONE resolved token value to scene form (stage 1).
 *
 * Steps, in order:
 *  - apostrophes (straight and typographic) are DELETED, not underscored
 *    ("Don't Stop" -> "dont_stop", never "don_t_stop");
 *  - '&' -> " and " (spaced, so "AC&DC" -> "ac_and_dc", not "acanddc");
 *  - Unicode NFKD, then combining marks stripped ("Motorhead" from
 *    "Mot\u00f6rhead");
 *  - a small fold table for the Latin letters NFKD cannot decompose
 *    (ae oe o a-ring ss d th l from their aeligature/oslash/eth/thorn/stroke
 *    forms), so "M\u00f8" -> "mo" rather than "m";
 *  - lowercase;
 *  - every char outside [a-z0-9] -> '_' ('.' and '-' included; see DOTS in
 *    the file top);
 *  - '_' runs collapsed, leading/trailing '_' trimmed.
 *
 * Pure; empty in -> empty out. Code points with no ASCII rendering at all
 * (e.g. CJK) fold to '_' and may collapse away entirely; the caller's empty
 * checks catch a name that ends up blank.
 */
[[nodiscard]] QString sceneSanitizeValue(const QString& value);

/**
 * @brief Evaluate @p pattern against @p track in RENAME context and finalize
 *        the stem (stage 2). No extension; may be empty (caller rejects).
 *
 * Token values go through sceneSanitizeValue, with track_no / disc_no
 * zero-padded to 2 digits first (the padding then survives sanitize
 * untouched). The assembled string then gets the finalize pass, which is what
 * makes half-empty metadata degrade gracefully instead of producing junk:
 *
 *  - literals are lowercased and mapped into [a-z0-9._-] ('&' -> "and",
 *    anything else foreign -> '_'), so "%artist% - %title%" is usable as
 *    typed;
 *  - each maximal run of [-_] collapses to ONE char: '-' if the run contains
 *    a '-' (field separator wins), else '_'. An empty %album% in
 *    "%artist%-%album%-%title%" thus yields "artist-title", not
 *    "artist--title";
 *  - '.' runs collapse to one '.';
 *  - leading/trailing [-_.] are trimmed (also kills the leading '-' POSIX
 *    forbids and the leading '.' that would hide the file).
 */
[[nodiscard]] QString buildRenameStem(const TrackData& track, const QString& pattern);

/**
 * @brief Stem + the SOURCE file's extension, lowercased (stage 3a).
 *
 * The extension is everything after the last '.' of @p sourceFileName,
 * lowercased and reduced to [a-z0-9]; the pattern never carries it. No
 * extension (no dot, a dotfile, or a trailing dot) -> the stem alone. An
 * empty stem returns empty (never a bare ".flac").
 */
[[nodiscard]] QString composeRenameFileName(const QString& stem,
                                            const QString& sourceFileName);

/**
 * @brief Final verdict on a complete candidate file name (stage 3b): an empty
 *        string when the name is usable, else a short human-readable reason
 *        (surfaced verbatim as the preview row's flag text).
 *
 * Rejects: empty; "." / ".."; anything outside [a-z0-9._-] or a leading '-'
 * (defensive; unreachable through the builder); over 255 UTF-8 bytes (the
 * classic NAME_MAX; ASCII by construction, so bytes == chars).
 *
 * Deliberately NOT here: duplicate-in-batch and exists-on-disk. Those
 * are relational / filesystem questions; this function stays pure.
 */
[[nodiscard]] QString renameFileNameProblem(const QString& fileName);

} // namespace rawform
