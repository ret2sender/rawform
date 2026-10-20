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

// AlbumArtProvider.cpp
//
// Implementation of the "rawformart" image provider: the sidecar lookup
// (cover/folder/front/album/albumart with the usual image extensions), the
// embedded-picture extraction through TagLib's complex properties, and the
// requestImage entry point that tries them in that order.
//
// Both sources decode through one QImageReader path that honors the requested
// size at DECODE time (see decodeScaled). A cover is never materialized at its
// full pixel dimensions and then shrunk: a typical 3000x3000 embedded JPEG is
// 36 MB as ARGB32, and that allocation happens on the QML image-reader thread,
// whose malloc arena keeps the freed pages. Decoding straight to the bounded
// size is what keeps browsing across albums from accumulating tens of MB per
// cover.

#include "media/AlbumArtProvider.h"

#include "utils/PerfLog.h"  // the per-decode timing line

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QUrl>

#include <algorithm>  // std::clamp, std::max: the DCT step for the request
#include <cmath>      // std::ceil

#include <taglib/fileref.h>
#include <taglib/tbytevector.h>
#include <taglib/tlist.h>
#include <taglib/tstringlist.h>
#include <taglib/tvariant.h>

namespace rawform {
namespace {

/// What one decode cost, for the rawform.perf line: the source and decoded
/// sizes, the format, and the JPEG DCT step the request landed on (0 when the
/// request was not a DCT size).
struct DecodeTrace {
    QSize      full;
    QSize      decoded;
    QByteArray format;
    int        dctNum = 0;
};

/// Decode through @p reader, bounded by @p requestedSize. When the request is
/// valid and the source is larger than it on either axis, the reader is told
/// the aspect-preserving target BEFORE reading, so the handler decodes to it
/// directly (libjpeg's DCT scaling never allocates the full-size image; other
/// handlers decode in full and hand back only the reduced result). A source
/// already within the bound is returned as is: never upscaled, so a small
/// cover stays small and the view's PreserveAspectFit does the stretching.
/// An invalid request (no sourceSize on the QML side) means a full decode.
///
/// JPEG is asked for a size its decoder can produce outright. libjpeg scales
/// at the DCT in eighths (an output of ceil(W * M / 8) for M in 1..8), and
/// Qt's handler picks the smallest M whose output still covers the request,
/// then smooth-scales that output down to the exact request on the CPU; for
/// the common cover that resample costs more than the decode it follows. So
/// when the aspect-fit target sits between two DCT steps, the request is
/// rounded UP to the step above it, provided that step is a whole-pixel size
/// on both axes (W * M and H * M divisible by 8; otherwise Qt's own rounding
/// would land on M + 1 and resample anyway, and the plain request stands).
/// A step of 8 is the full image, so the request is dropped: a full decode
/// beats a full decode followed by a few-percent resample, for at most 8/7 of
/// the bound in memory. The view fits whatever size comes back on the GPU.
QImage decodeScaled(QImageReader& reader, const QSize& requestedSize, DecodeTrace& trace) {
    trace.format = reader.format();
    if (requestedSize.isValid() && !requestedSize.isEmpty()) {
        // size() reads the header only; an invalid size (a handler that
        // cannot report it) just falls through to a full decode.
        const QSize full = reader.size();
        trace.full = full;
        if (full.isValid()
            && (full.width() > requestedSize.width()
                || full.height() > requestedSize.height())) {
            QSize target = full.scaled(requestedSize, Qt::KeepAspectRatio);
            if (trace.format == "jpeg") {
                const double ratio =
                    std::max(static_cast<double>(target.width()) / full.width(),
                             static_cast<double>(target.height()) / full.height());
                const int num = std::clamp(static_cast<int>(std::ceil(8.0 * ratio)), 1, 8);
                if (num >= 8) {
                    target = full;  // the DCT cannot help; take the full decode
                } else if ((full.width() * num) % 8 == 0 && (full.height() * num) % 8 == 0) {
                    target = QSize(full.width() * num / 8, full.height() * num / 8);
                    trace.dctNum = num;
                }
            }
            if (target != full)
                reader.setScaledSize(target);
        }
    }
    QImage img = reader.read();
    trace.decoded = img.size();
    return img;
}

/// Try the conventional sidecar cover-art names in @p dir. First hit wins.
QImage loadSidecar(const QDir& dir, const QSize& requestedSize, DecodeTrace& trace) {
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
                QImageReader reader(candidate);
                const QImage img = decodeScaled(reader, requestedSize, trace);
                if (!img.isNull())
                    return img;
            }
        }
    }
    return {};
}

/// Extract the first embedded picture from @p path via TagLib's complex
/// properties (the bytes are read here, on demand).
QImage loadEmbedded(const QString& path, const QSize& requestedSize, DecodeTrace& trace) {
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

    // A non-owning view over the ByteVector (no copy of the encoded bytes);
    // `data` outlives the reader, which is all fromRawData requires. The reader
    // sniffs the format from the content, so no extension hint is needed.
    const QByteArray bytes = QByteArray::fromRawData(data.data(),
                                                     static_cast<qsizetype>(data.size()));
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};

    QImageReader reader(&buffer);
    return decodeScaled(reader, requestedSize, trace);
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

    // Sidecar takes priority over the embedded picture. Both decode to the
    // requested bound (decodeScaled), so no post-decode scaling happens here.
    QElapsedTimer clock;
    clock.start();
    DecodeTrace trace;
    QImage img = loadSidecar(fi.absoluteDir(), requestedSize, trace);
    if (img.isNull())
        img = loadEmbedded(path, requestedSize, trace);
    if (img.isNull())
        return {};

    // The timing line, on the image-reader thread: the source and decoded
    // sizes say what the request bought (a JPEG at a DCT step decodes
    // straight to the decoded size; "dct 0/8" means the handler resampled).
    qCDebug(lcPerf, "art: %dx%d -> %dx%d %s dct %d/8 in %lld ms",
            trace.full.width(), trace.full.height(),
            trace.decoded.width(), trace.decoded.height(),
            trace.format.constData(), trace.dctNum,
            static_cast<long long>(clock.elapsed()));

    if (size)
        *size = img.size();
    return img;
}

} // namespace rawform
