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
// MetadataEditor.h
//
// Writes arbitrary tag fields to disk off the GUI thread, the general-purpose
// sibling of ReplayGainEditor. Same shape: QML_ELEMENT so the Properties window
// instantiates it directly, an off-thread QtConcurrent write pass, and an
// applied(okCount, failedPaths) signal back on the GUI thread. It does NOT
// reload; the window drives MetadataReloader afterwards, exactly as the RG path
// does.
//
// The write mirrors the read in TrackReader: QFile::encodeName -> TagLib FileRef
// -> the file's live PropertyMap. Only the keys named in the edits are touched
// (set or removed); every other tag in the map is left exactly as it was, so
// unknown tags, multi-value tags, and tags owned by other tabs (ReplayGain)
// survive a save untouched.
//
// SAVE POLICY: the save step branches on file type. MP3 files go through the
// typed TagLib::MPEG::File save so the user's tagging settings (Settings >
// Tagging > MP3, owned by SettingsStore) control the ID3v2 revision, the
// ID3v1 block (write / preserve / strip), a pre-existing APEv2 block (preserve
// / strip), and the ID3v2 text encoding, the last stamped per text frame just
// before the save, since TagLib offers no create-time encoding control
// for programmatic edits. Every other format uses the format-blind
// FileRef::save(). The settings are snapshotted into each Job on the GUI
// thread inside apply(), so the workers never touch the SettingsStore
// QObject and no process-global TagLib state is involved.
//
// Edit shape: a flat list of { path:string, key:string, values:stringlist } maps.
//  - A non-empty values list SETS the key, writing each entry as a distinct value
//    (for FLAC/Vorbis, one comment field per value). The list is authoritative:
//    the writer does NOT split or interpret strings, so the list/scalar policy
//    lives entirely in the model. Empty entries are skipped.
//  - An empty (or absent) values list REMOVES the key.
// Edits are grouped by path so each file is opened, mutated, and saved exactly
// once; within a path, a later edit for the same key wins.
//
// STRIP MODE (Tools > Remove tags): apply() takes an optional second list
// of paths whose KNOWN TAG TYPES are removed wholesale before any of that
// path's mutations are written. MP3 gets a true structural strip of the ID3v1,
// ID3v2, and APEv2 blocks (deliberately overriding the Preserve settings; the
// feature's contract is "removes known tag types"); if staged values survive,
// they rebuild a fresh ID3v2 under the normal settings-honoring save. Every
// other format starts from an empty PropertyMap (setProperties erases what is
// not in it), plus removeUnsupportedProperties for binary/unknown entries and
// complexProperties clearing for embedded pictures. Documented limitation:
// non-MP3 containers keep their emptied tag structures where the format or
// TagLib requires them (e.g. FLAC's vendor string).
//
// The caller (PropertiesMetadataModel::collectEdits) owns the higher-level
// semantics: list-vs-scalar per field, the TRACKNUMBER/TOTALTRACKS and
// DISCNUMBER/TOTALDISCS coupling, alias clearing (TRACKTOTAL/DISCTOTAL/YEAR), and
// one edit per affected track. This class just executes the value lists.
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
                                       // build its meta-type (ReplayGainEditor
                                       // includes it for the same reason).

namespace rawform {

class MetadataEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /// Optional, non-owning link to playback, set from QML (audio:
    /// audioController). When present, apply() asks it to stop playback if any
    /// job targets the loaded track (stopIfPlayingAny; policy, state test, and
    /// log message all live in the controller). When null the guard is skipped,
    /// so the editor stays usable standalone; it remains otherwise
    /// playback-agnostic and only reports which paths it is about to write.
    Q_PROPERTY(rawform::AudioController* audio READ audio WRITE setAudio NOTIFY audioChanged)

public:
    explicit MetadataEditor(QObject* parent = nullptr);
    ~MetadataEditor() override;

    [[nodiscard]] bool busy() const { return m_busy; }

    [[nodiscard]] AudioController* audio() const { return m_audio.data(); }
    void setAudio(AudioController* audio) {
        if (m_audio.data() == audio)
            return;
        m_audio = audio;
        emit audioChanged();
    }

    /// One worker verdict per written file. In the header so the watcher can be a
    /// value member (QFutureWatcher needs the result type complete).
    struct WriteResult {
        QString path;
        bool    ok = false;
    };

    /// @p edits: a flat list of { path, key, values } maps (see the file comment).
    /// @p stripAllPaths: files whose known tag types are removed wholesale
    /// before that path's mutations (if any) are written; a path may appear in
    /// both lists or in the strip list alone. Defaulted so a plain apply(edits)
    /// from QML needs no second argument. Grouped by path,
    /// written on the global thread pool. Empty / malformed entries are skipped.
    /// Always ends with an applied() emission so the caller's sequencer can chain
    /// uniformly even when nothing was writable.
    Q_INVOKABLE void apply(const QVariantList& edits,
                           const QStringList& stripAllPaths = {});

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
