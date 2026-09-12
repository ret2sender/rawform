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

// FileRenamer.cpp
//
// The rename worker. See the header for the contract. Mirrors MetadataEditor's
// structure: build per-file jobs on the GUI thread, run renameOne across the
// global pool, collect verdicts back on the GUI thread in onFinished.

#include "rename/FileRenamer.h"

#include "media/TagWriteLock.h"       // cross-window same-file write lease
#include "rename/RenameSanitizer.h"   // renameFileNameProblem (the last gate)

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLatin1Char>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <QtConcurrent>

#include <utility> // std::move

namespace rawform {
namespace {

// One file's rename job, snapshotted on the GUI thread.
struct Job {
    QString path;         ///< absolute source path
    QString newFileName;  ///< bare target file name (no directory part)
};

// Rename one file. Off the GUI thread. Never throws; failure is r.ok == false
// with r.newPath carrying the composed destination when composition got that
// far (the dialog surfaces the source path either way).
FileRenamer::RenameResult renameOne(const Job& j) {
    FileRenamer::RenameResult r;
    r.oldPath = j.path;

    // The last gate (defensive; the dialog validated already). A name with a
    // path separator is a directory escape and is refused outright: a rename
    // stays within the file's own directory. Checked for both separators
    // regardless of platform, since '\\' in a POSIX file name is legal but
    // never something this feature should produce.
    if (renameFileNameProblem(j.newFileName) != QString()
        || j.newFileName.contains(QLatin1Char('/'))
        || j.newFileName.contains(QLatin1Char('\\')))
        return r;

    // Serialize against any tag writer targeting this same file (per-pass
    // dedupe already rules out a collision within our own pass). A
    // MetadataEditor save in flight holds this lease across its whole
    // read-modify-write; taking it here means we move the file only between
    // whole tag operations, never inside one.
    const TagWriteLock lease(j.path);

    const QFileInfo src(j.path);
    r.newPath = src.absolutePath() + QLatin1Char('/') + j.newFileName;

    if (r.newPath == j.path) {
        // Identity after composition: nothing to do, and not a failure. The
        // dialog grays these rows out and does not submit them; this branch
        // is belt-and-suspenders. onFinished excludes it from the renames map by
        // the same comparison.
        r.ok = true;
        return r;
    }

    if (!src.exists())
        return r; // gone since preview; the reloader will gray the row

    // QFile::rename refuses an existing destination (no clobber, racing
    // creations included) while recognizing a case-only rename of the same
    // file on a case-insensitive filesystem as self and allowing it. Both
    // behaviors are exactly what the no-clobber policy wants, so no exists()
    // pre-check here: rolling our own would either race or wrongly refuse the
    // case-only rename on macOS.
    r.ok = QFile::rename(j.path, r.newPath);
    return r;
}

} // namespace

FileRenamer::FileRenamer(QObject* parent) : QObject(parent) {
    QObject::connect(&m_watcher, &QFutureWatcher<RenameResult>::finished,
                     this, &FileRenamer::onFinished);
}

FileRenamer::~FileRenamer() {
    // Let an in-flight pass settle so worker tasks do not outlive this object.
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
}

void FileRenamer::apply(const QVariantList& jobs) {
    if (m_busy)
        return;

    // Dedupe by source path, later wins (a duplicate playlist entry selected
    // twice submits the same file twice with the same target; one rename).
    QHash<QString, Job> byPath;
    byPath.reserve(jobs.size());
    for (const QVariant& jv : jobs) {
        const QVariantMap m = jv.toMap();
        const QString path = m.value(QStringLiteral("path")).toString();
        const QString name = m.value(QStringLiteral("newFileName")).toString();
        if (path.isEmpty() || name.isEmpty())
            continue;
        byPath[path] = Job{ path, name };
    }

    QList<Job> runnable = byPath.values();
    if (runnable.isEmpty()) {
        emit applied(0, {}, {});  // nothing to do; keep the caller's flow uniform
        return;
    }

    // If playback holds one of these files, stop it before the move.
    // POSIX fd semantics would survive the rename, but the controller's
    // loaded-path bookkeeping would not; stopping is the uniform policy the
    // tag editors already follow. Synchronous on the GUI thread, so the stop
    // command is posted to the engine before any worker below touches a file.
    if (m_audio) {
        QStringList paths;
        paths.reserve(runnable.size());
        for (const Job& j : runnable)
            paths.push_back(j.path);
        m_audio->stopIfPlayingAny(paths);
    }

    setBusy(true);
    m_watcher.setFuture(QtConcurrent::mapped(std::move(runnable), renameOne));
}

void FileRenamer::onFinished() {
    const QFuture<RenameResult> f = m_watcher.future();
    int okCount = 0;
    QStringList failed;
    QVariantMap renames;
    for (int i = 0; i < f.resultCount(); ++i) {
        const RenameResult r = f.resultAt(i);
        if (!r.ok) {
            failed << r.oldPath;
            continue;
        }
        ++okCount;
        if (r.newPath != r.oldPath)
            renames.insert(r.oldPath, r.newPath);
    }
    setBusy(false);
    emit applied(okCount, failed, renames);
}

void FileRenamer::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

}  // namespace rawform
