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

// PlaylistStore.cpp
//
// Implementation of the playlist saver: format resolution from the chosen path
// and the dialog's filter hint, the off-thread .rwfpl / M3U write with its
// completion signal, and the column-layout preset load and save.

#include "playlist/PlaylistStore.h"

#include "paths/Paths.h"
#include "playlist/M3uFile.h"
#include "playlist/PlaylistModel.h"
#include "playlist/PlaylistFile.h"
#include "playlist/PlaylistFormat.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QtConcurrent>
#include <QtGlobal>

#include <utility> // std::move

namespace rawform {

PlaylistStore::PlaylistStore(PlaylistModel* model, QObject* parent)
    : QObject(parent), m_model(model) {
    connect(&m_saveWatcher, &QFutureWatcher<WriteOutcome>::finished,
            this, &PlaylistStore::onSaveFinished);
}

void PlaylistStore::save(const QUrl& fileUrl,
                         const QStringList& fieldIds,
                         const QVariantList& widths,
                         const QString& formatHint,
                         int currentRow,
                         int scrollRow) {
    if (m_busy) {
        emit saved(false, QStringLiteral("another playlist operation is in progress"));
        return;
    }
    const QString chosen = fileUrl.toLocalFile();
    if (chosen.isEmpty()) {
        emit saved(false, QStringLiteral("invalid file path"));
        return;
    }
    if (!m_model) {
        emit saved(false, QStringLiteral("no playlist to save"));
        return;
    }

    // Format first, path second: the resolved format is what decides whether an
    // extension has to be appended, so completing the path before knowing the
    // format would be the wrong order. See PlaylistFormat.h for the precedence
    // (a recognized typed suffix beats the dialog's filter hint).
    const PlaylistFormat fmt =
        formatForPath(chosen, playlistFormatFromId(formatHint));
    const QString path = ensurePlaylistSuffix(chosen, fmt);

    // Snapshot everything on the GUI thread so the worker owns a private copy.
    PlaylistDocument doc;
    doc.tracks     = m_model->snapshotTracks();
    doc.fieldIds   = fieldIds;
    doc.widths.reserve(widths.size());
    for (const QVariant& v : widths)
        doc.widths << v.toInt();
    doc.savedAtUtc = QDateTime::currentDateTimeUtc();
    // The position snapshot, caller-supplied (the store owns neither
    // the selection nor the view). The .rwfpl writer embeds them as the CURR
    // and SCRL chunks; the M3U writer drops them like it drops the layout.
    // Without them a Save As writes -1/-1 and the opened .rwfpl restores
    // neither its scroll position nor its focus row.
    doc.currentRow = currentRow;
    doc.scrollRow  = scrollRow;

    m_savePath   = path;
    m_saveFormat = fmt;
    setBusy(true);
    m_saveWatcher.setFuture(QtConcurrent::run(
        [path, fmt, doc = std::move(doc)]() -> WriteOutcome {
            QString err;
            // Both writers are pure and atomic (QSaveFile), so the branch is the
            // only difference between the two paths; everything the worker owns
            // is a private copy either way.
            const bool ok = (fmt == PlaylistFormat::Rwfpl)
                                ? writePlaylist(path, doc, &err)
                                : writeM3u(path, doc, &err);
            return WriteOutcome{ ok, err };
        }));
}

void PlaylistStore::onSaveFinished() {
    const WriteOutcome out = m_saveWatcher.result();
    setBusy(false);
    if (out.ok) {
        // An M3U export carries file references only; the column layout, the
        // playlist title and the current row did not travel. Stated once here
        // rather than raised as a dialog: it is information, not a decision the
        // user still has to make.
        const QString note = (m_saveFormat == PlaylistFormat::Rwfpl)
                                 ? QString()
                                 : QStringLiteral(" (tracks only)");
        emit saved(true, QStringLiteral("Saved %1%2").arg(m_savePath, note));
    } else {
        emit saved(false, out.message.isEmpty() ? QStringLiteral("save failed")
                                                : out.message);
    }
    m_savePath.clear();
}

bool PlaylistStore::saveColumnPreset(const QStringList& fieldIds,
                                     const QVariantList& widths) {
    // A preset is layout-only: an otherwise-empty document carrying just the
    // column order + widths, written with the normal atomic .rwfpl writer.
    PlaylistDocument doc;
    doc.fieldIds = fieldIds;
    doc.widths.reserve(static_cast<int>(widths.size()));
    for (const QVariant& v : widths) {
        bool ok = false;
        const int w = v.toInt(&ok);
        doc.widths.push_back(ok ? w : 0);
    }
    doc.savedAtUtc = QDateTime::currentDateTimeUtc();

    const QString dir = userConfigDir();
    if (!QDir().mkpath(dir)) {
        qWarning("rawform: could not create config dir %s for the column preset",
                 qUtf8Printable(dir));
        return false;
    }
    const QString path = dir + QStringLiteral("/columns.rwftp");
    QString err;
    if (!writePlaylist(path, doc, &err)) {
        qWarning("rawform: failed to save column preset: %s", qUtf8Printable(err));
        return false;
    }
    return true;
}

QVariantMap PlaylistStore::loadColumnPreset() {
    const QString path =
        userConfigDir() + QStringLiteral("/columns.rwftp");
    if (!QFile::exists(path))
        return {}; // no preset yet -> caller keeps the schema defaults

    const PlaylistReadResult res = readPlaylist(path);
    if (!res.ok()) {
        qWarning("rawform: ignoring unreadable column preset %s (%s)",
                 qUtf8Printable(path), qUtf8Printable(res.message));
        return {};
    }

    QVariantList widths;
    widths.reserve(static_cast<int>(res.doc.widths.size()));
    for (const int w : res.doc.widths)
        widths << w;

    QVariantMap out;
    out.insert(QStringLiteral("fieldIds"), res.doc.fieldIds);
    out.insert(QStringLiteral("widths"), widths);
    return out;
}

void PlaylistStore::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

} // namespace rawform
