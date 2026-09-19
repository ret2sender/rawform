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

// RenameSanitizer.cpp
//
// Implementation of the rename-name pipeline declared in RenameSanitizer.h: the
// per-value scene sanitization (Unicode folding, charset mapping, separator
// collapsing), the stem build over the pattern evaluator, and the final name
// composition and validation.

#include "rename/RenameSanitizer.h"

#include "columns/PatternEvaluator.h" // evaluatePattern, TokenValueTransform

#include <QChar>
#include <QLatin1Char>
#include <QLatin1String>
#include <QString>

namespace rawform {
namespace {

/// True for the classes NFKD leaves as separate combining code points; these
/// are dropped after normalization ("o" + COMBINING DIAERESIS -> "o").
bool isCombiningMark(char32_t cp) {
    switch (QChar::category(cp)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_SpacingCombining:
    case QChar::Mark_Enclosing:
        return true;
    default:
        return false;
    }
}

/// ASCII rendering for the handful of Latin letters NFKD does NOT decompose
/// (they are letters in their own right, not letter+mark). Both cases are
/// listed because this table runs before the lowercase step. Empty string =
/// not in the table (caller keeps the code point as-is for the next stage).
QString foldNonDecomposing(char32_t cp) {
    switch (cp) {
    case U'\u00e6': case U'\u00c6': return QStringLiteral("ae"); // ae ligature
    case U'\u0153': case U'\u0152': return QStringLiteral("oe"); // oe ligature
    case U'\u00f8': case U'\u00d8': return QStringLiteral("o");  // o with stroke
    case U'\u00df': case U'\u1e9e': return QStringLiteral("ss"); // sharp s
    case U'\u00f0': case U'\u00d0': return QStringLiteral("d");  // eth
    case U'\u00fe': case U'\u00de': return QStringLiteral("th"); // thorn
    case U'\u0142': case U'\u0141': return QStringLiteral("l");  // l with stroke
    case U'\u0111': case U'\u0110': return QStringLiteral("d");  // d with stroke
    default:                        return {};
    }
}

/// NFKD-normalize, drop combining marks, fold the non-decomposing stragglers,
/// lowercase. Shared by the value sanitizer and the literal finalize pass so
/// both agree on what "asciified" means. Operates on UCS-4 so astral code
/// points are one unit, not a surrogate pair split across iterations.
QString asciiFoldLower(const QString& in) {
    const QString norm = in.normalized(QString::NormalizationForm_KD);
    QString out;
    out.reserve(norm.size());
    for (const char32_t cp : norm.toUcs4()) {
        if (isCombiningMark(cp))
            continue;
        const QString folded = foldNonDecomposing(cp);
        if (!folded.isEmpty()) {
            out.append(folded);
            continue;
        }
        // Everything else passes through; the per-stage charset map decides
        // its fate. fromUcs4 (not QChar(cp)) so an astral cp re-encodes as a
        // proper surrogate pair instead of truncating.
        out.append(QString::fromUcs4(&cp, 1));
    }
    return out.toLower();
}

/// The value-level charset gate: keep [a-z0-9], everything else -> '_'.
/// Runs after asciiFoldLower, so a surviving non-ASCII code point is one with
/// no ASCII rendering (CJK etc); it folds to '_' like punctuation does.
QString valueCharsToUnderscores(const QString& in) {
    QString out;
    out.reserve(in.size());
    for (const QChar c : in) {
        const char16_t u = c.unicode();
        const bool keep = (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9');
        out.append(keep ? c : QChar(QLatin1Char('_')));
    }
    return out;
}

/// Collapse '_' runs and trim leading/trailing '_' (the per-value tail step).
QString collapseUnderscores(const QString& in) {
    QString out;
    out.reserve(in.size());
    bool pendingUnderscore = false;
    for (const QChar c : in) {
        if (c == QLatin1Char('_')) {
            pendingUnderscore = !out.isEmpty(); // leading run: drop outright
            continue;
        }
        if (pendingUnderscore) {
            out.append(QLatin1Char('_'));
            pendingUnderscore = false;
        }
        out.append(c);
    }
    // A trailing run left pendingUnderscore set with nothing after it: gone.
    return out;
}

/// '&' -> " and ", spaced on both sides so a tight "AC&DC" still word-splits
/// ("ac_and_dc" after the charset map + collapse, never "acanddc").
QString expandAmpersands(const QString& in) {
    QString out = in;
    out.replace(QLatin1Char('&'), QLatin1String(" and "));
    return out;
}

/// Apostrophes DELETE rather than fold to '_': "Don't Stop" must read "dont_stop", not
/// "don_t_stop"; no scene name ever underscored an elision. The set covers the straight
/// ASCII apostrophe and the typographic ones tag editors produce (right and left single
/// quotes, modifier letter apostrophe). Applied in BOTH the value sanitizer and the
/// finalize pass's literal handling, so a pattern- literal apostrophe behaves identically
/// to one inside a title.
QString removeApostrophes(const QString& in) {
    QString out = in;
    out.remove(QChar(0x0027)); // '
    out.remove(QChar(0x2019)); // right single quotation mark
    out.remove(QChar(0x2018)); // left single quotation mark
    out.remove(QChar(0x02bc)); // modifier letter apostrophe
    return out;
}

/// Zero-pad a rendered integer field to 2 digits. Empty stays empty (an
/// unset field must vanish, not become "00"); an already-wider value ("103")
/// is left alone. Non-numeric text passes through untouched (renderFieldValue
/// only emits digits for these fields, so this is purely defensive).
QString padTwo(const QString& rendered) {
    if (rendered.isEmpty())
        return rendered;
    bool ok = false;
    const int n = rendered.toInt(&ok);
    if (!ok || n < 0)
        return rendered;
    return QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0'));
}

/// The whole-stem finalize pass (stage 2 tail): literal cleanup + separator
/// collapse + end trim. Values arriving here are already in [a-z0-9_]; this
/// pass exists for the pattern LITERALS interleaved between them, and for the
/// seams empty tokens leave behind.
QString finalizeStem(const QString& assembled) {
    // Literal cleanup: same ascii-fold as values, then the STEM charset map,
    // which is wider than the value one: '.', '-', '_' are structural here
    // (the user typed them in the pattern) and survive.
    const QString folded =
        asciiFoldLower(expandAmpersands(removeApostrophes(assembled)));
    QString mapped;
    mapped.reserve(folded.size());
    for (const QChar c : folded) {
        const char16_t u = c.unicode();
        const bool keep = (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9')
                       || u == u'.' || u == u'-' || u == u'_';
        mapped.append(keep ? c : QChar(QLatin1Char('_')));
    }

    // Separator-run collapse. Each maximal run of [-_] becomes ONE char: '-'
    // when the run contains any '-' (the field separator outranks the word
    // separator, so "%artist%-%album%-%title%" with an empty album gives
    // "artist-title"), else '_'. Dot runs collapse to a single '.'.
    QString collapsed;
    collapsed.reserve(mapped.size());
    bool runHasDash = false;
    bool inSepRun = false;
    bool inDotRun = false;
    const auto flushSep = [&]() {
        if (inSepRun) {
            collapsed.append(runHasDash ? QLatin1Char('-') : QLatin1Char('_'));
            inSepRun = false;
            runHasDash = false;
        }
    };
    for (const QChar c : mapped) {
        if (c == QLatin1Char('-') || c == QLatin1Char('_')) {
            inDotRun = false;
            inSepRun = true;
            runHasDash = runHasDash || c == QLatin1Char('-');
            continue;
        }
        flushSep();
        if (c == QLatin1Char('.')) {
            if (inDotRun)
                continue;
            inDotRun = true;
            collapsed.append(c);
            continue;
        }
        inDotRun = false;
        collapsed.append(c);
    }
    flushSep();

    // Trim [-_.] from both ends: kills the leading '-' POSIX forbids, the
    // hidden-file leading '.', and the dangling separator an empty trailing
    // token leaves.
    int lo = 0;
    qsizetype hi = collapsed.size();
    const auto isTrimmable = [](QChar c) {
        return c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.');
    };
    while (lo < hi && isTrimmable(collapsed.at(lo)))
        ++lo;
    while (hi > lo && isTrimmable(collapsed.at(hi - 1)))
        --hi;
    return collapsed.mid(lo, hi - lo);
}

} // namespace

QString sceneSanitizeValue(const QString& value) {
    return collapseUnderscores(
        valueCharsToUnderscores(
            asciiFoldLower(
                expandAmpersands(
                    removeApostrophes(value)))));
}

QString buildRenameStem(const TrackData& track, const QString& pattern) {
    // The rename-context transform: pad the two numeric position fields,
    // then scene-sanitize every value. Names are matched against the CANONICAL
    // ids (strict lowercase, same rule the resolver applies), so "%TRACK_NO%"
    // is a custom-tag lookup and gets no padding, consistent with it not being
    // the native field either.
    const TokenValueTransform renameTransform =
        [](const QString& name, const QString& value) {
            QString v = value;
            if (name == QLatin1String("track_no") || name == QLatin1String("disc_no"))
                v = padTwo(v);
            return sceneSanitizeValue(v);
        };
    return finalizeStem(evaluatePattern(track, pattern, renameTransform));
}

QString composeRenameFileName(const QString& stem, const QString& sourceFileName) {
    if (stem.isEmpty())
        return {}; // never a bare ".flac"; the caller flags the empty stem

    // Extension: after the LAST dot, ignoring a dot at position 0 (a dotfile
    // has no extension) and a trailing dot (empty extension). Lowercased and
    // reduced to [a-z0-9]; an extension that reduces to nothing is dropped.
    const qsizetype dot = sourceFileName.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot == sourceFileName.size() - 1)
        return stem;
    QString ext;
    for (const QChar c : sourceFileName.mid(dot + 1).toLower()) {
        const char16_t u = c.unicode();
        if ((u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9'))
            ext.append(c);
    }
    if (ext.isEmpty())
        return stem;
    return stem + QLatin1Char('.') + ext;
}

QString renameFileNameProblem(const QString& fileName) {
    if (fileName.isEmpty())
        return QStringLiteral("pattern produces an empty name");
    if (fileName == QLatin1String(".") || fileName == QLatin1String(".."))
        return QStringLiteral("reserved name");
    // Defensive re-checks; unreachable through buildRenameStem +
    // composeRenameFileName, but this function is the last gate before a
    // QFile::rename and must not trust its callers.
    if (fileName.startsWith(QLatin1Char('-')))
        return QStringLiteral("name starts with '-'");
    for (const QChar c : fileName) {
        const char16_t u = c.unicode();
        const bool ok = (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9')
                     || u == u'.' || u == u'-' || u == u'_';
        if (!ok)
            return QStringLiteral("name contains a non-portable character");
    }
    // NAME_MAX = 255 BYTES on every target filesystem; the name is ASCII by
    // construction so bytes == chars, but count bytes anyway (defensive, and
    // free).
    if (fileName.toUtf8().size() > 255)
        return QStringLiteral("name longer than 255 bytes");
    return {};
}

} // namespace rawform
