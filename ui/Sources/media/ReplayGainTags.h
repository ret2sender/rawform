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
// ReplayGainTags.h
//
// One source of truth for reading and presenting the REPLAYGAIN_* tags that ride
// verbatim in TrackData::extraTags. The playback path (AudioController, which
// turns gain + peak into a linear factor) and the Properties view (the
// PlaylistModel accessor that feeds the foobar-style table) parse through here,
// so a quirk like a comma decimal or a missing dB unit is handled once; the two
// producers of new values (ReplayGainEditor on write, ReplayGainScanController
// on scan) format through here, so a staged and a written gain agree to the
// character.
//
// Header-only and inline: the functions are tiny and pure, no Q_OBJECT, so they
// compile into whichever translation unit includes them without a separate .cpp.
//
// Two presentation styles live here on purpose:
//   - formatGainDisplay keeps the tag's own precision (so a "-4" tag shows "-4 dB"
//     next to a "-6.00 dB" tag, exactly as foobar renders the per-track cells) and
//     only normalizes the unit.
//   - formatGainValue is for COMPUTED summary numbers (lowest/highest/common gain),
//     always signed and two-decimal.
// =============================================================================

#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <optional>

namespace rawform::replaygain {

/// Parsed + verbatim fields for one track. The *Raw strings are what the cells
/// display (tag precision preserved); the optionals are the numbers the summary
/// aggregates over (nullopt when the tag is absent or unparseable).
struct Values {
    QString trackGainRaw;
    QString albumGainRaw;
    QString trackPeakRaw;
    QString albumPeakRaw;
    std::optional<double> trackGainDb;
    std::optional<double> albumGainDb;
    std::optional<double> trackPeak;
    std::optional<double> albumPeak;
};

/// The first value of a multi-value tag, trimmed; empty when the key is absent.
inline QString firstTag(const QMap<QString, QStringList>& tags, const QString& key) {
    const auto it = tags.find(key);
    if (it == tags.end() || it.value().isEmpty())
        return {};
    return it.value().first().trimmed();
}

/// Parse a gain string ("-7.89 dB", "+3.2", "-7,89 dB") to dB. Tolerates a trailing
/// dB unit in any case and a comma decimal separator. nullopt when unparseable.
inline std::optional<double> parseGainDb(const QString& raw) {
    QString s = raw.trimmed();
    if (s.isEmpty())
        return std::nullopt;
    static const QRegularExpression dbSuffix(
        QStringLiteral("\\s*dB\\s*$"), QRegularExpression::CaseInsensitiveOption);
    s.remove(dbSuffix);
    s.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    const double v = s.trimmed().toDouble(&ok);
    return ok ? std::optional<double>(v) : std::nullopt;
}

/// Parse a peak string (a plain linear magnitude). nullopt when unparseable or
/// non-positive (a 0 peak would make clip prevention divide by zero).
inline std::optional<double> parsePeak(const QString& raw) {
    QString s = raw.trimmed();
    if (s.isEmpty())
        return std::nullopt;
    s.replace(QLatin1Char(','), QLatin1Char('.'));
    bool ok = false;
    const double v = s.toDouble(&ok);
    return (ok && v > 0.0) ? std::optional<double>(v) : std::nullopt;
}

/// Read all four REPLAYGAIN_* tags (verbatim strings + parsed numbers).
inline Values read(const QMap<QString, QStringList>& tags) {
    Values v;
    v.trackGainRaw = firstTag(tags, QStringLiteral("REPLAYGAIN_TRACK_GAIN"));
    v.albumGainRaw = firstTag(tags, QStringLiteral("REPLAYGAIN_ALBUM_GAIN"));
    v.trackPeakRaw = firstTag(tags, QStringLiteral("REPLAYGAIN_TRACK_PEAK"));
    v.albumPeakRaw = firstTag(tags, QStringLiteral("REPLAYGAIN_ALBUM_PEAK"));
    v.trackGainDb = parseGainDb(v.trackGainRaw);
    v.albumGainDb = parseGainDb(v.albumGainRaw);
    v.trackPeak   = parsePeak(v.trackPeakRaw);
    v.albumPeak   = parsePeak(v.albumPeakRaw);
    return v;
}

/// Per-track cell: keep the tag's own number precision, normalize only the unit.
/// "-4" -> "-4 dB", "-6.00 dB" -> "-6.00 dB", "+10.00" -> "+10.00 dB". Empty in,
/// empty out (the cell stays blank).
inline QString formatGainDisplay(const QString& raw) {
    QString s = raw.trimmed();
    if (s.isEmpty())
        return {};
    static const QRegularExpression dbSuffix(
        QStringLiteral("\\s*dB\\s*$"), QRegularExpression::CaseInsensitiveOption);
    s.remove(dbSuffix);
    s = s.trimmed();
    if (s.isEmpty())
        return {};
    return s + QStringLiteral(" dB");
}

/// Computed summary number: always signed, two decimals, " dB". -0.0 is folded to
/// +0.00 so a near-zero average never prints a stray minus.
inline QString formatGainValue(double db) {
    double v = db;
    if (v == 0.0)
        v = 0.0;  // clears a negative zero
    return QString::asprintf("%+.2f dB", v);
}

/// Per-track peak cell: verbatim (peaks carry no unit). Empty in, empty out.
inline QString formatPeakDisplay(const QString& raw) {
    return raw.trimmed();
}

}  // namespace rawform::replaygain
