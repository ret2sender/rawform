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

// SettingsStore.cpp
//
// Implementation of the general settings store. See the header for the shape
// and ownership rationale; this file is the enum<->string mapping, the YAML
// load with per-key fallback, and the coalesced persist through the shared
// utils/YamlFile.h pair.

#include "settings/SettingsStore.h"

#include "utils/YamlFile.h"

#include "paths/Paths.h"  // userConfigDir() for settings.yaml

#include <QByteArray>
#include <QIODevice>

#include <yaml-cpp/yaml.h>

#include <optional>
#include <string>

namespace rawform {

SettingsStore* SettingsStore::s_instance = nullptr;

namespace {

// settings.yaml lives beside spectrum.yaml and playback.yaml under
// userConfigDir(). Unlike those, it is the general store: one top-level sub-map
// per concern, `tagging:` being the one it holds.
QString settingsPath() {
    return userConfigDir() + QStringLiteral("/settings.yaml");
}

// ---------------------------------------------------------------------------
// Enum <-> YAML string mapping. The on-disk spellings are human words so a
// hand-edited file reads naturally; the enums are the in-memory/QML API. Every
// from-string helper returns the documented default for an unknown spelling and lets
// the caller qWarning once, so a typo in a hand-edited file degrades loudly
// but safely. The to-string switches are exhaustive for -Wswitch.
// ---------------------------------------------------------------------------

const char* id3v1ModeToYaml(int v) {
    switch (static_cast<SettingsStore::Id3v1Mode>(v)) {
        case SettingsStore::Id3v1Write:    return "write";
        case SettingsStore::Id3v1Preserve: return "preserve";
        case SettingsStore::Id3v1Strip:    return "strip";
    }
    return "write";  // unreachable with a clamped member; keeps -Wreturn-type quiet
}

int id3v1ModeFromYaml(const std::string& s, bool* ok) {
    *ok = true;
    if (s == "write")    return SettingsStore::Id3v1Write;
    if (s == "preserve") return SettingsStore::Id3v1Preserve;
    if (s == "strip")    return SettingsStore::Id3v1Strip;
    *ok = false;
    return SettingsStore::Id3v1Write;
}

const char* apeModeToYaml(int v) {
    switch (static_cast<SettingsStore::ApeMode>(v)) {
        case SettingsStore::ApePreserve: return "preserve";
        case SettingsStore::ApeStrip:    return "strip";
    }
    return "preserve";
}

int apeModeFromYaml(const std::string& s, bool* ok) {
    *ok = true;
    if (s == "preserve") return SettingsStore::ApePreserve;
    if (s == "strip")    return SettingsStore::ApeStrip;
    *ok = false;
    return SettingsStore::ApePreserve;
}

const char* encodingToYaml(int v) {
    switch (static_cast<SettingsStore::Id3v2Encoding>(v)) {
        case SettingsStore::EncLatin1: return "latin1";
        case SettingsStore::EncUtf16:  return "utf16";
        case SettingsStore::EncUtf8:   return "utf8";
    }
    return "utf16";
}

int encodingFromYaml(const std::string& s, bool* ok) {
    *ok = true;
    if (s == "latin1") return SettingsStore::EncLatin1;
    if (s == "utf16")  return SettingsStore::EncUtf16;
    if (s == "utf8")   return SettingsStore::EncUtf8;
    *ok = false;
    return SettingsStore::EncUtf16;
}

// The ID3v2 version is stored as the human number (3 or 4) rather than the
// enum ordinal, for the same hand-editability reason as the strings above.
int id3v2VersionToYaml(int v) {
    return (v == SettingsStore::Id3v2_4) ? 4 : 3;
}

int id3v2VersionFromYaml(int n, bool* ok) {
    *ok = true;
    if (n == 3) return SettingsStore::Id3v2_3;
    if (n == 4) return SettingsStore::Id3v2_4;
    *ok = false;
    return SettingsStore::Id3v2_3;
}

}  // namespace

// ---------------------------------------------------------------------------
SettingsStore::SettingsStore(QObject* parent) : QObject(parent) {
    s_instance = this;

    m_persistTimer.setSingleShot(true);
    m_persistTimer.setInterval(0);
    connect(&m_persistTimer, &QTimer::timeout, this,
            &SettingsStore::persistSettings);

    loadSettings();
}

SettingsStore::~SettingsStore() {
    // Flush a pending coalesced write so an Apply immediately followed by app
    // exit still lands on disk. The timer cannot fire after this dtor runs, so
    // this is the last chance.
    if (m_persistTimer.isActive()) {
        m_persistTimer.stop();
        persistSettings();
    }
    s_instance = nullptr;
}

SettingsStore* SettingsStore::instance() {
    return s_instance;
}

// ---------------------------------------------------------------------------
// Setters. Clamp-to-default on an out-of-range int (a QML typo or a stale
// staged value must never store garbage), mutate, notify, and schedule the one
// coalesced write.
// ---------------------------------------------------------------------------

void SettingsStore::setId3v2Version(int v) {
    if (v != Id3v2_3 && v != Id3v2_4)
        v = Id3v2_3;
    if (m_id3v2Version == v)
        return;
    m_id3v2Version = v;
    emit id3v2VersionChanged();
    schedulePersist();
}

void SettingsStore::setId3v1Mode(int v) {
    if (v != Id3v1Write && v != Id3v1Preserve && v != Id3v1Strip)
        v = Id3v1Write;
    if (m_id3v1Mode == v)
        return;
    m_id3v1Mode = v;
    emit id3v1ModeChanged();
    schedulePersist();
}

void SettingsStore::setApeMode(int v) {
    if (v != ApePreserve && v != ApeStrip)
        v = ApePreserve;
    if (m_apeMode == v)
        return;
    m_apeMode = v;
    emit apeModeChanged();
    schedulePersist();
}

void SettingsStore::setId3v2Encoding(int v) {
    if (v != EncLatin1 && v != EncUtf16 && v != EncUtf8)
        v = EncUtf16;
    if (m_id3v2Encoding == v)
        return;
    m_id3v2Encoding = v;
    emit id3v2EncodingChanged();
    schedulePersist();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void SettingsStore::loadSettings() {
    // Absent or unreadable is the normal first-run case: the defaults stand.
    // A malformed file is logged and ignored wholesale; an individual unknown
    // VALUE inside a well-formed file is logged and falls back per key, so one
    // typo in a hand-edited file does not discard the other preferences.
    const auto text = yamlfile::readAll(settingsPath());
    if (!text) {
        return;
    }
    try {
        const YAML::Node root = YAML::Load(*text);
        const YAML::Node tagging = root["tagging"];
        if (!tagging || !tagging.IsMap()) {
            return;
        }

        bool ok = true;
        if (tagging["id3v2_version"]) {
            m_id3v2Version =
                    id3v2VersionFromYaml(tagging["id3v2_version"].as<int>(), &ok);
            if (!ok)
                qWarning("rawform: settings.yaml: unknown id3v2_version, using 3");
        }
        if (tagging["id3v1_mode"]) {
            m_id3v1Mode =
                    id3v1ModeFromYaml(tagging["id3v1_mode"].as<std::string>(), &ok);
            if (!ok)
                qWarning("rawform: settings.yaml: unknown id3v1_mode, using write");
        }
        if (tagging["ape_mode"]) {
            m_apeMode = apeModeFromYaml(tagging["ape_mode"].as<std::string>(), &ok);
            if (!ok)
                qWarning("rawform: settings.yaml: unknown ape_mode, using preserve");
        }
        if (tagging["id3v2_encoding"]) {
            m_id3v2Encoding =
                    encodingFromYaml(tagging["id3v2_encoding"].as<std::string>(), &ok);
            if (!ok)
                qWarning("rawform: settings.yaml: unknown id3v2_encoding, using utf16");
        }
    } catch (const YAML::Exception& e) {
        qWarning("rawform: ignoring malformed %s (%s)",
                 qUtf8Printable(settingsPath()), e.what());
        m_id3v2Version  = Id3v2_3;
        m_id3v1Mode     = Id3v1Write;
        m_apeMode       = ApePreserve;
        m_id3v2Encoding = EncUtf16;
    }
}

void SettingsStore::schedulePersist() {
    // (Re)arm the zero-interval single-shot: however many setters run in this
    // event-loop turn, persistSettings fires once, after all of them.
    m_persistTimer.start();
}

void SettingsStore::persistSettings() {
    // The whole file is rewritten from the in-memory state on every persist
    // and handed to the shared atomic writer, so this stays the single writer
    // of settings.yaml: a concern added beside `tagging:` is emitted here and
    // nowhere else.
    const QString   path = settingsPath();

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "version" << YAML::Value << 1;
    out << YAML::Key << "tagging" << YAML::Value;
    out << YAML::BeginMap;
    out << YAML::Key << "id3v2_version" << YAML::Value
        << id3v2VersionToYaml(m_id3v2Version);
    out << YAML::Key << "id3v1_mode" << YAML::Value
        << id3v1ModeToYaml(m_id3v1Mode);
    out << YAML::Key << "ape_mode" << YAML::Value << apeModeToYaml(m_apeMode);
    out << YAML::Key << "id3v2_encoding" << YAML::Value
        << encodingToYaml(m_id3v2Encoding);
    out << YAML::EndMap;
    out << YAML::EndMap;

    QByteArray content;
    content += "# rawform - application settings (generated by the app).\n";
    content += "# tagging: MP3/ID3 write preferences.\n";
    content += "#   id3v2_version: 3 or 4.\n";
    content += "#   id3v1_mode: write | preserve | strip.\n";
    content += "#   ape_mode: preserve | strip (APEv2 on MP3 is never written, only kept or removed).\n";
    content += "#   id3v2_encoding: latin1 | utf16 | utf8 (utf8 is only valid in ID3v2.4).\n";
    content += out.c_str();
    content += '\n';

    yamlfile::writeAtomically(path, content);
}

}  // namespace rawform
