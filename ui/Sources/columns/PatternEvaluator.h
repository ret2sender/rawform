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

#include "columns/ColumnSchema.h" // ColumnField
#include "media/TrackData.h"    // TrackData (+ joinedValues, used by the implementation)

#include <QString>

#include <functional>

namespace rawform {

/**
 * @file PatternEvaluator.h
 * @brief The per-field renderer and the custom-column %pattern% evaluator.
 *
 * A small, Qt-Quick-free translation unit (QtCore + ColumnSchema + TrackData
 * only), which buys two things: it is the single place a built-in field becomes
 * text (so playlist cells and custom-column patterns render identically), and
 * it is unit-testable on its own (see tests/PatternEvaluatorTest.cpp).
 *
 * Scope: %token% substitution + literal text only. The full foobar2000
 * title-formatting language ($if, $functions, [...] sections) is outside this
 * evaluator's contract.
 */

/**
 * @brief Render one built-in field of a track as a display string. Direct
 *        tag/audio/filesystem reads plus the two composites (TrackIndex ->
 *        "1.01", AlbumGroup -> "Album Artist - Album"). Pure.
 */
[[nodiscard]] QString renderFieldValue(const TrackData& track, ColumnField field);

/**
 * @brief Optional per-token value hook for evaluatePattern (file rename).
 *
 * Called once per RESOLVED token with the token name exactly as it appeared
 * between the percent signs and the value resolveToken produced (possibly
 * empty); the return value is what gets appended to the output. Pattern
 * LITERALS never pass through it, which is the whole point: the rename
 * pipeline must sanitize a hyphen INSIDE a title (it becomes '_') while a
 * hyphen the user typed in the pattern is the field separator and survives.
 * A whole-string post-pass cannot tell those apart; this hook can.
 *
 * A default-constructed (empty) transform is skipped entirely, so a caller
 * that passes none gets plain substitution.
 */
using TokenValueTransform =
    std::function<QString(const QString& tokenName, const QString& value)>;

/**
 * @brief Evaluate a custom-column pattern (literal text interleaved with
 *        %token% references) against a track.
 *
 *  - Tokenizing: "%%" emits a literal '%'; a lone '%' opens a token that the
 *    next '%' closes (so "%a%%b%" concatenates cleanly); an unterminated '%' is
 *    lenient, emitting the rest as literal so a half-typed pattern is harmless.
 *  - Resolution, native first: fieldFromString(name) matched strictly
 *    (canonical ids are lowercase, case-sensitive) renders via renderFieldValue;
 *    otherwise the upper-cased name is looked up in extraTags (multi-values
 *    joined with "; "); otherwise empty.
 *  - @p transform, when non-empty, maps each resolved token value before it is
 *    appended (literals bypass it; see TokenValueTransform). The unterminated
 *    tail is literal text and also bypasses it.
 */
[[nodiscard]] QString evaluatePattern(const TrackData& track, const QString& pattern,
                                      const TokenValueTransform& transform = {});

} // namespace rawform
