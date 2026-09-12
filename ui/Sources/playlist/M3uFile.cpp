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

#include "playlist/M3uFile.h"

#include "media/TrackData.h"
#include "playlist/PlaylistFile.h" // PlaylistDocument

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringConverter>
#include <QTextStream>

namespace rawform {

namespace {

/// The M3U line terminator. CRLF, written literally; see the encoding note in
/// M3uFile.h for why this is NOT delegated to QIODevice::Text.
constexpr QLatin1String kEol("\r\n");

/// The path line for @p absolute, relative to @p base (the playlist's own
/// directory) when the two can be related, absolute otherwise.
///
/// QDir::relativeFilePath does the whole decision in one call: it walks up with
/// ".." when a relative form exists, and returns the input ABSOLUTE path when
/// the two live on different roots (different Windows drives; on macOS and Linux
/// there is one root, so a relative form always exists). Its return value is
/// therefore already the settled answer in both cases, and the only thing worth
/// guarding is the degenerate empty result (path == base, which cannot happen
/// for a file but would otherwise emit a blank line).
QString pathLineFor(const QString& absolute, const QDir& base) {
    const QString rel = base.relativeFilePath(absolute);
    return rel.isEmpty() ? absolute : rel;
}

} // namespace

bool writeM3u(const QString& path, const PlaylistDocument& doc, QString* error) {
    const auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return false;
    };

    // Atomic by the same rule the RFW1 writer follows: write a temporary, rename
    // on commit, so an interrupted export cannot leave a half file where a good
    // one used to be.
    QSaveFile file(path);
    // Deliberately NOT QIODevice::Text: that mode rewrites '\n' to the PLATFORM
    // line ending, which on macOS and Linux would turn the chosen CRLF back
    // into LF. Writing the terminator ourselves is the only way to pin it.
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return fail(QStringLiteral("could not open %1 for writing: %2")
                        .arg(path, file.errorString()));

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    // No BOM: players that predate UTF-8 M3U read the BOM bytes as part of the
    // first path and then fail to find the file.
    out.setGenerateByteOrderMark(false);

    // The playlist file's own directory is the base every relative path resolves
    // against, both here and in TrackScanner::expandM3u on the way back in.
    const QDir base = QFileInfo(path).absoluteDir();

    // Plain M3U: paths only, in playlist order. No header line, no per-track
    // metadata line. See the CONTENT note in M3uFile.h.
    for (const TrackData& t : doc.tracks) {
        // A row with no path has nothing to reference; it cannot be expressed in
        // this format at all, so it is skipped rather than written as a blank
        // line (which a reader would silently drop anyway). Not an error: the
        // export of the remaining rows is still correct.
        if (t.filePath.isEmpty())
            continue;

        out << pathLineFor(t.filePath, base) << kEol;
    }

    // Flush before probing the device: QTextStream buffers, so a write error can
    // still be sitting in the stream at this point and would otherwise be
    // reported only as a commit failure with a less useful message.
    out.flush();
    if (file.error() != QFileDevice::NoError)
        return fail(QStringLiteral("write failed for %1: %2")
                        .arg(path, file.errorString()));

    if (!file.commit())
        return fail(QStringLiteral("could not commit %1: %2")
                        .arg(path, file.errorString()));

    if (error)
        error->clear();
    return true;
}

} // namespace rawform
