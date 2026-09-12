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
// ReplayGainEditor.h
//
// Writes the REPLAYGAIN_* tags to disk off the GUI thread, the ReplayGain
// sibling of MetadataEditor, mirroring how TrackReader reads them
// (QFile::encodeName path -> TagLib FileRef -> PropertyMap). Registered into
// the com.rawform.app QML module via QML_ELEMENT, so the Properties window
// instantiates it directly with no main.cpp wiring.
//
// Contract: apply() takes a list of per-track edit maps, each carrying the
// target file PATH (never a playlist row: rows are positions, and the playlist
// can mutate between staging and Apply because the Properties window is not
// modal), and returns (via the applied() signal, on the GUI thread) the count
// of files written and the paths that failed. It does NOT reload; the caller
// refreshes the model through MetadataReloader so the display reflects
// exactly what landed on disk. Writes are deduplicated by path (subsongs share
// a file), so one file is never written from two pool threads at once.
// =============================================================================

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>  // QML_ELEMENT
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "playback/AudioController.h"  // complete type: the audio Q_PROPERTY,
                                       // which moc must see fully defined to
                                       // build its meta-type.

namespace rawform {

class ReplayGainEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /// Optional, non-owning link to playback, set from QML (audio:
    /// audioController); same shape and rationale as MetadataEditor's. When
    /// present, apply() asks it to stop playback if any deduped job targets the
    /// loaded track; when null the guard is skipped.
    Q_PROPERTY(rawform::AudioController* audio READ audio WRITE setAudio NOTIFY audioChanged)

public:
    explicit ReplayGainEditor(QObject* parent = nullptr);
    ~ReplayGainEditor() override;

    [[nodiscard]] bool busy() const { return m_busy; }

    [[nodiscard]] AudioController* audio() const { return m_audio.data(); }
    void setAudio(AudioController* audio) {
        if (m_audio.data() == audio)
            return;
        m_audio = audio;
        emit audioChanged();
    }

    /// One worker verdict per written file. Kept in the header so the watcher can
    /// be a value member (QFutureWatcher needs the result type complete).
    struct WriteResult {
        QString path;
        bool    ok = false;
    };

    /// @p edits: a list of maps, each { path:string, and any of trackGain/
    /// albumGain/trackPeak/albumPeak as strings }. A present field is changed: an
    /// empty string removes that tag, a non-empty string sets it (normalized to a
    /// canonical form). Absent fields are left untouched. Pathless entries are
    /// skipped. Dedupes by path (later edits to the same path win field by field)
    /// and writes on the global thread pool.
    Q_INVOKABLE void apply(const QVariantList& edits);

signals:
    void busyChanged();
    void audioChanged();
    /// GUI thread, when the write pass finishes. @p okCount files were written;
    /// @p failedPaths could not be (read-only, unsupported format, gone).
    void applied(int okCount, const QStringList& failedPaths);

private:
    void onFinished();
    void setBusy(bool on);

    QFutureWatcher<WriteResult> m_watcher;
    bool m_busy = false;
    QPointer<AudioController> m_audio;  ///< playback interlock; null skips it
};

}  // namespace rawform
