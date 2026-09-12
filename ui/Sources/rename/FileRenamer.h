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
// FileRenamer.h
//
// Renames audio files on disk off the GUI thread: the third sibling of
// MetadataEditor / ReplayGainEditor, same shape on purpose. QML_ELEMENT so the
// Rename dialog instantiates it directly, an off-thread QtConcurrent pass, and
// an applied(...) signal back on the GUI thread. It does NOT touch any model;
// the dialog forwards the success map to PlaylistTabs::applyPathRenames, which
// patches every open tab's rows in place.
//
// Job shape: a flat list of { path:string, newFileName:string } maps. The new
// name is a bare FILE NAME, never a path: a rename stays within the file's
// own directory (moving files is outside this class's contract), so the
// worker composes dir(path) + newFileName itself and a job cannot smuggle the
// file elsewhere (a newFileName containing a separator is rejected). Jobs are
// deduped by source path, later wins, mirroring the editors.
//
// Safety properties, in the order they matter:
//  - TagWriteLock on the SOURCE path for the whole rename, so a rename can
//    never interleave with a tag save of the same file from another
//    Properties window: the writer holding the lease finishes its
//    read-modify-write on the old path first, then we move it.
//  - renameFileNameProblem re-checked in the worker: the last gate before the
//    filesystem call must not trust preview-time validation (the dialog's
//    checks are UX; this one is the contract).
//  - QFile::rename never overwrites: an existing destination fails the job
//    (Qt's own exists() check), while a case-only rename of the SAME file on
//    a case-insensitive filesystem (macOS) is recognized by Qt as self and
//    allowed. This is exactly the no-clobber policy enforced at the bottom
//    layer, racing creations included; the dialog's exists-check is advisory.
//  - The playback interlock: playback is stopped first when it holds any
//    source file, same policy as the tag editors (renaming under a live
//    decoder is fine on POSIX fd semantics, but the controller's loaded-path
//    bookkeeping would go stale; stopping is the simple, uniform answer).
//
// The applied() payload carries the old->new ABSOLUTE path map of the
// successes so the caller can hand it straight to applyPathRenames; failures
// come back as source paths for the dialog's per-row error marks.
// =============================================================================

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>  // QML_ELEMENT
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "playback/AudioController.h"  // complete type: the audio Q_PROPERTY,
                                       // which moc must see fully defined to
                                       // build its meta-type (same reason as
                                       // MetadataEditor).

namespace rawform {

class FileRenamer : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /// As in MetadataEditor: optional, non-owning link to
    /// playback, set from QML (audio: audioController). When present, apply()
    /// stops playback if any job targets the loaded track; when null the guard
    /// is skipped and the renamer stays usable standalone.
    Q_PROPERTY(rawform::AudioController* audio READ audio WRITE setAudio NOTIFY audioChanged)

public:
    explicit FileRenamer(QObject* parent = nullptr);
    ~FileRenamer() override;

    [[nodiscard]] bool busy() const { return m_busy; }

    [[nodiscard]] AudioController* audio() const { return m_audio.data(); }
    void setAudio(AudioController* audio) {
        if (m_audio.data() == audio)
            return;
        m_audio = audio;
        emit audioChanged();
    }

    /// One worker verdict per job. In the header so the watcher can be a value
    /// member (QFutureWatcher needs the result type complete).
    struct RenameResult {
        QString oldPath;
        QString newPath;   ///< composed absolute destination (empty if never composed)
        bool    ok = false;
    };

    /// @p jobs: a flat list of { path, newFileName } maps (see the file
    /// comment). Malformed entries (empty path or name) are skipped; a job
    /// whose newFileName equals the current name is skipped as a no-op success
    /// upstream (the dialog grays those rows), and defensively treated as ok
    /// here without touching the disk. Always ends with an applied() emission,
    /// even when nothing was runnable, so the dialog's flow is uniform.
    Q_INVOKABLE void apply(const QVariantList& jobs);

signals:
    void busyChanged();
    void audioChanged();
    /// GUI thread, when the pass finishes. @p okCount jobs succeeded;
    /// @p failedPaths are the SOURCE paths that could not be renamed (bad name,
    /// destination exists, permission, gone); @p renames maps old absolute
    /// path -> new absolute path for every success that actually moved a file
    /// (no-op identity jobs are counted in okCount but excluded here), ready
    /// for PlaylistTabs::applyPathRenames.
    void applied(int okCount, const QStringList& failedPaths, const QVariantMap& renames);

private:
    void onFinished();
    void setBusy(bool on);

    QFutureWatcher<RenameResult> m_watcher;
    bool m_busy = false;
    QPointer<AudioController> m_audio;  ///< playback interlock; null skips it
};

}  // namespace rawform
