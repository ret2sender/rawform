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

#include "media/AlbumArtProvider.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include <taglib/fileref.h>
#include <taglib/tbytevector.h>
#include <taglib/tlist.h>
#include <taglib/tstringlist.h>
#include <taglib/tvariant.h>

namespace rawform {
namespace {

/// Try the conventional sidecar cover-art names in @p dir. First hit wins.
QImage loadSidecar(const QDir& dir) {
    static const QStringList kNames = {
        QStringLiteral("cover"),  QStringLiteral("folder"),
        QStringLiteral("front"),  QStringLiteral("album"),
        QStringLiteral("albumart"),
    };
    static const QStringList kExts = {
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
        QStringLiteral("webp"), QStringLiteral("bmp"),
    };
    for (const QString& name : kNames) {
        for (const QString& ext : kExts) {
            const QString candidate = dir.absoluteFilePath(name + QLatin1Char('.') + ext);
            if (QFile::exists(candidate)) {
                QImage img(candidate);
                if (!img.isNull())
                    return img;
            }
        }
    }
    return {};
}

/// Extract the first embedded picture from @p path via TagLib's complex
/// properties (the bytes are read here, on demand).
QImage loadEmbedded(const QString& path) {
    TagLib::FileRef f(QFile::encodeName(path).constData(),
                      /*readAudioProperties=*/false);
    if (f.isNull() || !f.file())
        return {};

    const TagLib::List<TagLib::VariantMap> pics = f.complexProperties("PICTURE");
    if (pics.isEmpty())
        return {};

    const TagLib::ByteVector data = pics.front()["data"].toByteVector();
    if (data.isEmpty())
        return {};

    QImage img;
    img.loadFromData(reinterpret_cast<const uchar*>(data.data()),
                     static_cast<int>(data.size()));
    return img;
}

} // namespace

AlbumArtProvider::AlbumArtProvider()
    : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage AlbumArtProvider::requestImage(const QString& id, QSize* size,
                                      const QSize& requestedSize) {
    // The id carries a "?v=<mtime-size>-e<artEpoch>" staleness tag appended by
    // PlaylistModel::artUrlForRow so a re-read cover (same path, new bytes)
    // produces a distinct URL. It is not part of the path; drop it before
    // decoding.
    QString raw = id;
    const qsizetype q = raw.indexOf(QLatin1Char('?'));
    if (q >= 0)
        raw = raw.left(q);

    // The path arrives percent-encoded; decode it back. (Idempotent for
    // ordinary paths, and correct for the rare ones containing reserved chars.)
    const QString path = QUrl::fromPercentEncoding(raw.toUtf8());
    if (path.isEmpty())
        return {};

    const QFileInfo fi(path);

    // Sidecar takes priority over the embedded picture.
    QImage img = loadSidecar(fi.absoluteDir());
    if (img.isNull())
        img = loadEmbedded(path);
    if (img.isNull())
        return {};

    if (requestedSize.isValid() && !requestedSize.isEmpty())
        img = img.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    if (size)
        *size = img.size();
    return img;
}

} // namespace rawform
