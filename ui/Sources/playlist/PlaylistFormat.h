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

// PlaylistFormat.h
//
// Which on-disk playlist format a path denotes, and the suffix rules that decide it.
//
// Header-only and pure (no QObject, no I/O): both the GUI thread (PlaylistStore
// deciding what to write, PlaylistTabs deciding what to read) and pool workers
// use it, so it must be free of shared state.
//
// WHY THE POLICY LIVES HERE, NOT IN QML. A save dialog gives two independent
// signals about the intended format: the suffix the user actually typed, and the
// name filter they had selected. Qt's FileDialog::defaultSuffix can only append
// ONE fixed extension, so it cannot follow a filter change; and the native
// macOS panel and the portal dialog on Wayland differ in whether they rewrite
// the extension for you. Resolving both signals in one C++ place is the only way
// the two platforms can be made to agree.
//
// PRECEDENCE: an explicit, recognized suffix on the path always wins
// over the filter hint. Typing "mix.m3u" while the M3U8 filter is selected
// yields M3U, because the typed extension is the more deliberate act. Only when
// the path carries no recognized suffix does the hint decide, and the matching
// extension is then appended.

#pragma once

#include <QChar>      // QLatin1Char (the dot ensurePlaylistSuffix appends)
#include <QFileInfo>
#include <QLatin1String>
#include <QString>

namespace rawform {

enum class PlaylistFormat {
    Rwfpl, ///< rawform's own binary RFW1 document (tracks + column layout + title)
    M3u,   ///< extended M3U, UTF-8 (see M3uFile.h)
    M3u8,  ///< extended M3U, UTF-8; identical bytes to M3u, honest extension
};

/// The canonical file extension for @p fmt, without the leading dot.
[[nodiscard]] inline QString playlistFormatSuffix(PlaylistFormat fmt) {
    switch (fmt) {
    case PlaylistFormat::M3u:
        return QStringLiteral("m3u");
    case PlaylistFormat::M3u8:
        return QStringLiteral("m3u8");
    case PlaylistFormat::Rwfpl:
        break;
    }
    return QStringLiteral("rwfpl");
}

/// Map a stable string id to a format. The id is what QML passes as the save
/// dialog's filter hint; an unknown or empty id falls back to M3U, which is the
/// chosen default format, so a dialog that reports no usable filter index
/// still lands somewhere sane rather than on an error path.
[[nodiscard]] inline PlaylistFormat playlistFormatFromId(const QString& id) {
    const QString lower = id.toLower();
    if (lower == QLatin1String("m3u8"))
        return PlaylistFormat::M3u8;
    if (lower == QLatin1String("rwfpl"))
        return PlaylistFormat::Rwfpl;
    return PlaylistFormat::M3u;
}

/// True when @p suffixLower (no dot, already lowercased) is one this build
/// recognizes as a playlist container.
[[nodiscard]] inline bool isPlaylistSuffix(const QString& suffixLower) {
    return suffixLower == QLatin1String("rwfpl")
        || suffixLower == QLatin1String("m3u")
        || suffixLower == QLatin1String("m3u8");
}

/// Resolve @p path to a format. A recognized suffix decides; anything else
/// (including no suffix at all) yields @p fallback. Note that QFileInfo::suffix
/// returns the text after the LAST dot, so "My Album Vol.2" resolves to the
/// unrecognized "2" and therefore to @p fallback, which is what we want.
[[nodiscard]] inline PlaylistFormat formatForPath(const QString& path,
                                                  PlaylistFormat fallback) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("rwfpl"))
        return PlaylistFormat::Rwfpl;
    if (suffix == QLatin1String("m3u"))
        return PlaylistFormat::M3u;
    if (suffix == QLatin1String("m3u8"))
        return PlaylistFormat::M3u8;
    return fallback;
}

/// Return @p path unchanged when it already carries a recognized playlist
/// suffix, otherwise @p path with @p fmt's extension appended. Never REPLACES a
/// suffix: "mix.m3u" saved under the M3U8 filter stays "mix.m3u" (the typed
/// extension is authoritative, and formatForPath above will have reported M3U
/// for it, so file name and written bytes agree).
[[nodiscard]] inline QString ensurePlaylistSuffix(const QString& path,
                                                  PlaylistFormat fmt) {
    if (path.isEmpty())
        return path;
    if (isPlaylistSuffix(QFileInfo(path).suffix().toLower()))
        return path;
    return path + QLatin1Char('.') + playlistFormatSuffix(fmt);
}

} // namespace rawform
