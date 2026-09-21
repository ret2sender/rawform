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

// AlbumArtProvider.h
//
// Supplies album art to QML on demand, keyed by a track's file path.
//
// Registered as the "rawformart" image provider. PlaylistModel::artUrlForRow
// builds the URL, `image://rawformart/<percent-encoded-absolute-path>?v=<tag>`,
// and QML binds it to an Image source. Resolution order:
//
//   1. SIDECAR image in the track's folder (cover/folder/front/album/albumart.*),
//      which takes priority over the embedded picture.
//   2. EMBEDDED picture, extracted from the file via TagLib's complex-properties
//      API (the bytes are read here, lazily, only for the displayed selection).
//
// Returns a null QImage when neither is found (QML shows its placeholder). Runs
// on the asynchronous QML image-loading path, so the decode never blocks the UI.
//
// The requested size is a DECODE bound, not a post-decode resize: the Image
// binding this provider must set sourceSize (the art frame in MainWindow.qml
// does), and the provider decodes straight to that bound, aspect preserved,
// never upscaling. Without a sourceSize the request arrives invalid and the
// cover decodes at its full pixel dimensions, which for a large embedded JPEG
// is tens of MB retained per album browsed.

#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QString>

namespace rawform {

class AlbumArtProvider : public QQuickImageProvider {
public:
    AlbumArtProvider();

    QImage requestImage(const QString& id, QSize* size,
                        const QSize& requestedSize) override;
};

} // namespace rawform
