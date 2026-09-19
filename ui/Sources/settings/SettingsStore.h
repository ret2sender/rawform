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

// SettingsStore.h
//
// The application-wide settings store, backed by settings.yaml under
// userConfigDir(). Unlike the per-feature files (spectrum.yaml, playback.yaml,
// window.yaml), this is the general store: a preference that is not owned by
// a live runtime object (the way the RG knobs live on AudioController) lands
// here, grouped by a top-level YAML sub-map per concern. It holds one group,
// `tagging:`, the MP3/ID3 write preferences consumed by MetadataEditor and
// edited in the Settings window's Tagging > MP3 pane; a second concern is a
// second sub-map, emitted by the same single writer.
//
// SHAPE: a plain QObject constructed once in main.cpp and exposed to QML as the
// `settingsStore` context property, matching every other app-global service.
// A static instance() accessor additionally lets C++ that is instantiated FROM
// QML (MetadataEditor is a QML_ELEMENT created inside PropertiesWindow.qml)
// reach the same object without new plumbing; it is a convenience pointer to
// the main.cpp instance, not a self-creating singleton.
//
// THREADING: all setters and getters are GUI-thread only. The write path never
// touches this object from a worker; MetadataEditor snapshots the tagging
// values into each Job on the GUI thread before QtConcurrent spawns.
//
// PERSISTENCE: the shared YAML idiom (utils/YamlFile.h: an absent file is the
// first-run default, an atomic write so a crash mid-write cannot truncate the
// file, qWarning on failure) under a generated-by-the-app banner comment.
// Setters mutate in memory, emit their notify signal, and schedule ONE
// coalesced write through a zero-interval single-shot timer, so an Apply that
// pushes all four tagging knobs still produces a single settings.yaml write.
//
// ON-DISK FORMAT (hand-editable; unknown values fall back to defaults with a
// qWarning, absent keys keep their defaults silently):
//
//   version: 1
//   tagging:
//     id3v2_version: 3          # 3 or 4
//     id3v1_mode: write         # write | preserve | strip
//     ape_mode: preserve        # preserve | strip
//     id3v2_encoding: utf16     # latin1 | utf16 | utf8

#pragma once

#include <QObject>
#include <QQmlEngine>  // QML_ELEMENT / QML_UNCREATABLE
#include <QString>
#include <QTimer>

namespace rawform {

class SettingsStore : public QObject {
    Q_OBJECT

    /// Registered so QML can NAME the type (typed property declarations, and
    /// qmllint member checking on them); never instantiated from QML. The one
    /// instance is created in main.cpp and exposed as a context property.
    QML_ELEMENT
    QML_UNCREATABLE("SettingsStore is created in C++ and exposed as a context property")

    /// The MP3/ID3 tagging knobs. Ints crossing to QML, backed by the
    /// Q_ENUMs below so both sides share the named values. Each setter clamps
    /// unknown ints to the default rather than storing garbage.
    Q_PROPERTY(int id3v2Version READ id3v2Version WRITE setId3v2Version
                       NOTIFY id3v2VersionChanged)
    Q_PROPERTY(int id3v1Mode READ id3v1Mode WRITE setId3v1Mode
                       NOTIFY id3v1ModeChanged)
    Q_PROPERTY(int apeMode READ apeMode WRITE setApeMode NOTIFY apeModeChanged)
    Q_PROPERTY(int id3v2Encoding READ id3v2Encoding WRITE setId3v2Encoding
                       NOTIFY id3v2EncodingChanged)

    /// Factory defaults, exposed CONSTANT so the Settings window's Tagging
    /// pane can mark modified-from-default rows and drive its right-click reset,
    /// exactly as AudioController's replayGain*Default and SpectrumProvider's
    /// sourceDefault do for their panes. One source of truth: these return the
    /// same enum constants the member initializers below use.
    Q_PROPERTY(int id3v2VersionDefault READ id3v2VersionDefault CONSTANT)
    Q_PROPERTY(int id3v1ModeDefault READ id3v1ModeDefault CONSTANT)
    Q_PROPERTY(int apeModeDefault READ apeModeDefault CONSTANT)
    Q_PROPERTY(int id3v2EncodingDefault READ id3v2EncodingDefault CONSTANT)

public:
    // -----------------------------------------------------------------------
    // Enums. Values are stable API: they are what QML compares against and what
    // MetadataEditor snapshots into its jobs. The YAML spelling is a separate,
    // human-readable string mapping (see the .cpp), so reordering here would
    // NOT corrupt existing files, but do not reorder anyway; QML integers
    // written into staged state must keep meaning across versions.
    // -----------------------------------------------------------------------

    /// Which ID3v2 revision the MP3 writer emits. 2.3 is the compatibility
    /// default; 2.4 unlocks UTF-8 text frames.
    enum Id3v2Version { Id3v2_3 = 0, Id3v2_4 = 1 };
    Q_ENUM(Id3v2Version)

    /// What happens to the legacy ID3v1 block on save. Write always emits one (the
    /// default: maximum compatibility with legacy readers, and the block can never go
    /// stale against the v2 frames because every save rewrites it); Preserve updates an
    /// existing block but never adds one; Strip removes it.
    enum Id3v1Mode { Id3v1Write = 0, Id3v1Preserve = 1, Id3v1Strip = 2 };
    Q_ENUM(Id3v1Mode)

    /// What happens to an APEv2 block found on an MP3. rawform never WRITES APE
    /// on MP3; the only question is whether a pre-existing block survives the
    /// save (Preserve, the non-destructive default) or is removed (Strip).
    enum ApeMode { ApePreserve = 0, ApeStrip = 1 };
    Q_ENUM(ApeMode)

    /// Default text encoding for ID3v2 text frames. UTF-16 is the safe
    /// full-Unicode choice under 2.3; UTF-8 is only valid in 2.4 (TagLib
    /// downgrades it to UTF-16 when rendering 2.3 frames, so the mismatched
    /// combination degrades safely rather than corrupting).
    enum Id3v2Encoding { EncLatin1 = 0, EncUtf16 = 1, EncUtf8 = 2 };
    Q_ENUM(Id3v2Encoding)

    explicit SettingsStore(QObject* parent = nullptr);
    ~SettingsStore() override;

    /// A plain value snapshot of the tagging knobs for the write workers: MetadataEditor
    /// copies one of these into every Job on the GUI thread before QtConcurrent spawns,
    /// so no worker ever reads this QObject. The defaults match the documented policy, so
    /// a DEFAULT-CONSTRUCTED snapshot is the correct fallback when instance() is null (an
    /// editor running without the store).
    struct TaggingSnapshot {
        int id3v2Version  = Id3v2_3;
        int id3v1Mode     = Id3v1Write;
        int apeMode       = ApePreserve;
        int id3v2Encoding = EncUtf16;
    };

    [[nodiscard]] TaggingSnapshot taggingSnapshot() const {
        return TaggingSnapshot{ m_id3v2Version, m_id3v1Mode, m_apeMode,
                                m_id3v2Encoding };
    }

    /// The main.cpp instance, for C++ that cannot receive it by injection (the
    /// QML-instantiated MetadataEditor). Null before construction and after
    /// destruction; callers must handle null and fall back to defaults.
    [[nodiscard]] static SettingsStore* instance();

    [[nodiscard]] int id3v2Version() const { return m_id3v2Version; }
    [[nodiscard]] int id3v1Mode() const { return m_id3v1Mode; }
    [[nodiscard]] int apeMode() const { return m_apeMode; }
    [[nodiscard]] int id3v2Encoding() const { return m_id3v2Encoding; }

    [[nodiscard]] int id3v2VersionDefault() const { return Id3v2_3; }
    [[nodiscard]] int id3v1ModeDefault() const { return Id3v1Write; }
    [[nodiscard]] int apeModeDefault() const { return ApePreserve; }
    [[nodiscard]] int id3v2EncodingDefault() const { return EncUtf16; }

    void setId3v2Version(int v);
    void setId3v1Mode(int v);
    void setApeMode(int v);
    void setId3v2Encoding(int v);

signals:
    void id3v2VersionChanged();
    void id3v1ModeChanged();
    void apeModeChanged();
    void id3v2EncodingChanged();

private:
    void loadSettings();
    void schedulePersist();
    void persistSettings();

    /// Defaults: ID3v2.3 for compatibility, always write
    /// the legacy ID3v1 block, never add APE sidecar
    /// blocks unasked, UTF-16 as the safe encoding under 2.3. These are the
    /// values a fresh install (no settings.yaml) runs with, and the fallback for
    /// any unknown value found in a hand-edited file; an existing file with an
    /// explicit id3v1_mode key keeps its stored choice.
    int m_id3v2Version  = Id3v2_3;
    int m_id3v1Mode     = Id3v1Write;
    int m_apeMode       = ApePreserve;
    int m_id3v2Encoding = EncUtf16;

    /// Coalesces the per-setter persist requests into one write per event-loop
    /// turn, so a Settings Apply that pushes four properties writes the file
    /// once. Zero-interval single-shot; parked between changes.
    QTimer m_persistTimer;

    static SettingsStore* s_instance;
};

}  // namespace rawform
