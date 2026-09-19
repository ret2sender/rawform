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

// PatternEvaluator.cpp
//
// Implementation of the per-field renderer and the %token% pattern evaluator.
// renderFieldValue is the single place a built-in field becomes display text;
// evaluatePattern resolves each token to a native field first, then a custom tag,
// then empty, with the optional per-value transform hook the rename pipeline uses.

#include "columns/PatternEvaluator.h"

#include "utils/Formats.h"

#include "columns/ColumnSchema.h" // fieldFromString, ColumnField

#include <QDateTime>
#include <QLatin1Char>
#include <QLatin1String>
#include <QLocale>
#include <QString>
#include <QStringList>

#include <optional>

namespace rawform {
namespace {

// ---------------------------------------------------------------------------
// Per-field display formatters.
//
// renderFieldValue() below is the single place a built-in field becomes text:
// the model forwards to it, and the pattern evaluator calls the same function,
// so a playlist cell and a %token% render a given field identically.
// ---------------------------------------------------------------------------

/// Empty string for an unset (<= 0) integer field; otherwise the number.
QString intOrEmpty(int v) {
    return v > 0 ? QString::number(v) : QString{};
}

QString dateText(const QDateTime& dt) {
    return dt.isValid() ? QLocale().toString(dt, QLocale::ShortFormat) : QString{};
}

/// Resolve ONE %token% name to its value (see evaluatePattern's doc for the
/// rules). NATIVE field first (strict, case-sensitive), then a custom tag from
/// extraTags (upper-cased key), then empty. renderFieldValue is declared in the
/// header included above, so calling it here (before its definition lower in the
/// file) is fine.
QString resolveToken(const TrackData& t, const QString& name) {
    // 1) Native field, matched strictly. The canonical ids are lowercase and
    //    fieldFromString is case-sensitive, so "%ARTIST%" does NOT match the
    //    Artist field; it falls through to the custom-tag lookup below.
    if (const std::optional<ColumnField> f = fieldFromString(name))
        return renderFieldValue(t, *f);

    // 2) Custom tag: TagLib PropertyMap keys are uppercase. Join multi-values
    //    the same way the native multi-value fields are joined ("; ").
    const auto it = t.extraTags.constFind(name.toUpper());
    if (it != t.extraTags.constEnd())
        return joinedValues(*it);

    // 3) Unknown token -> empty.
    return {};
}

} // namespace

QString renderFieldValue(const TrackData& t, ColumnField field) {
    switch (field) {
    // --- Direct tag fields -------------------------------------------------
    case ColumnField::Artist:       return joinedValues(t.artists);
    case ColumnField::AlbumArtist:  return joinedValues(t.albumArtists);
    case ColumnField::Album:        return t.album;
    case ColumnField::Title:        return t.title;
    case ColumnField::Genre:        return joinedValues(t.genres);
    case ColumnField::Year:         return intOrEmpty(t.year);
    case ColumnField::TrackNo:      return intOrEmpty(t.trackNo);
    case ColumnField::DiscNo:       return intOrEmpty(t.discNo);
    case ColumnField::TrackTotal:   return intOrEmpty(t.trackTotal);
    case ColumnField::DiscTotal:    return intOrEmpty(t.discTotal);

    // --- Direct audio properties -------------------------------------------
    case ColumnField::Duration:     return formats::durationText(t.durationMs);
    case ColumnField::Bitrate:      return t.bitrateKbps > 0
                                        ? QStringLiteral("%1 kbps").arg(t.bitrateKbps) : QString{};
    case ColumnField::SampleRate:   return t.sampleRateHz > 0
                                        ? QStringLiteral("%1 Hz").arg(t.sampleRateHz) : QString{};
    case ColumnField::Channels:     return intOrEmpty(t.channels);
    case ColumnField::Codec:        return t.codec;

    // --- Direct filesystem fields ------------------------------------------
    case ColumnField::FileName:     return t.fileName;
    case ColumnField::FolderName:   return t.folderName;
    case ColumnField::FilePath:     return t.filePath;
    case ColumnField::FileSize:     return formats::fileSizeText(t.fileSize);
    case ColumnField::Modified:     return dateText(t.modified);
    case ColumnField::Created:      return dateText(t.created);
    case ColumnField::SubsongIndex: return QString::number(t.subsongIndex);

    // --- Composite renderers -----------------------------------------------
    case ColumnField::TrackIndex:
        // "disc.track" with the track zero-padded to two digits, e.g. "1.01".
        // Fall back gracefully when fields are unknown (0).
        if (t.discNo > 0 && t.trackNo > 0)
            return QStringLiteral("%1.%2")
                .arg(t.discNo)
                .arg(t.trackNo, 2, 10, QLatin1Char('0'));
        if (t.trackNo > 0)
            return QString::number(t.trackNo);
        return {};

    case ColumnField::AlbumGroup: {
        // "Album Artist - Album", the album grouping column. Non-const so the
        // bare return can move it out instead of copying.
        QString who = !t.albumArtists.isEmpty() ? joinedValues(t.albumArtists)
                    : (!t.artists.isEmpty()     ? joinedValues(t.artists)
                                                : QString{});
        if (who.isEmpty())
            return t.album;
        if (t.album.isEmpty())
            return who;
        return who + QStringLiteral(" - ") + t.album;
    }
    }
    return {};
}

QString evaluatePattern(const TrackData& t, const QString& pattern,
                        const TokenValueTransform& transform) {
    QString out;
    out.reserve(pattern.size());

    const qsizetype n = pattern.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = pattern.at(i);

        if (c == QLatin1Char('%')) {
            // "%%" -> a single literal '%'. This is the ONLY way to emit a
            // literal percent; everything else starting with '%' is a token.
            if (i + 1 < n && pattern.at(i + 1) == QLatin1Char('%')) {
                out.append(QLatin1Char('%'));
                i += 2;
                continue;
            }

            // Open a token: collect the name up to the next '%'.
            const qsizetype nameStart = i + 1;
            qsizetype j = nameStart;
            while (j < n && pattern.at(j) != QLatin1Char('%'))
                ++j;

            if (j >= n) {
                // Unterminated token (lenient): emit the '%' and the rest of the
                // string as literal text, then stop. Never throws; a half-typed
                // pattern shows what was typed.
                out.append(QLatin1Char('%'));
                out.append(pattern.mid(nameStart));
                break;
            }

            // Closed token: resolve and substitute, then resume after the '%'.
            // The transform (when set) maps ONLY this resolved value; literal
            // text never goes through it, so a rename-context sanitizer can
            // rewrite characters inside a value while the same characters
            // typed in the pattern keep their structural meaning.
            const QString name = pattern.mid(nameStart, j - nameStart);
            const QString value = resolveToken(t, name);
            out.append(transform ? transform(name, value) : value);
            i = j + 1;
            continue;
        }

        // Plain literal character.
        out.append(c);
        ++i;
    }

    return out;
}

} // namespace rawform
