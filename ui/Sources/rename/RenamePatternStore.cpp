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

// RenamePatternStore.cpp
//
// See the header for the contract. The file I/O is the shared
// utils/YamlFile.h pair; what is local is the schema and the tolerant
// YAML::Load shape it shares with CustomColumnRegistry.cpp (skip unusable
// records, dedupe on the key, warn-and-empty on a parse throw). Keep the two
// load shapes in step.

#include "rename/RenamePatternStore.h"

#include "utils/YamlFile.h"

#include "paths/Paths.h" // userConfigDir()

#include <QByteArray>
#include <QIODevice>
#include <QLatin1Char>
#include <QLatin1String>
#include <QSet>
#include <QString>
#include <QVariantMap>
#include <QtGlobal> // qWarning

#include <yaml-cpp/yaml.h>

#include <optional>
#include <string>

namespace rawform {
namespace {

/// Filename of the app-managed rename-presets file (under userConfigDir()).
constexpr auto kRenamePatternsFileName = "rename_patterns.yaml";

/// Absolute path of the rename-presets file (may not exist yet).
QString renamePatternsPath() {
    return userConfigDir()
           + QLatin1Char('/') + QLatin1String(kRenamePatternsFileName);
}

} // namespace

RenamePatternStore::RenamePatternStore(QObject* parent) : QObject(parent) {
    load();
}

void RenamePatternStore::load() {
    m_presets.clear();
    m_lastUsed.clear();

    const auto bytes = yamlfile::readAll(renamePatternsPath());
    if (bytes) {
        try {
            const YAML::Node root = YAML::Load(*bytes);
            if (root && root.IsMap()) {
                if (const YAML::Node lu = root["last_used"]; lu && lu.IsScalar())
                    m_lastUsed = QString::fromStdString(lu.as<std::string>()).trimmed();
                const YAML::Node pats = root["patterns"];
                if (pats && pats.IsSequence()) {
                    QSet<QString> seen; // drop duplicate names (keep first)
                    for (const YAML::Node& node : pats) {
                        if (!node.IsMap())
                            continue;
                        const YAML::Node nameNode = node["name"];
                        if (!nameNode || !nameNode.IsScalar())
                            continue; // unusable without a name -> skip
                        const QString name = QString::fromStdString(
                            nameNode.as<std::string>()).trimmed();
                        if (name.isEmpty() || seen.contains(name))
                            continue;

                        Preset p;
                        p.name = name;
                        if (const YAML::Node n = node["pattern"]; n && n.IsScalar())
                            p.pattern = QString::fromStdString(n.as<std::string>());
                        if (p.pattern.isEmpty())
                            continue; // a preset with no pattern loads nothing

                        seen.insert(name);
                        m_presets.push_back(std::move(p));
                    }
                }
            }
        } catch (const YAML::Exception& e) {
            qWarning("rawform: %s is invalid (%s); ignoring rename presets",
                     qUtf8Printable(renamePatternsPath()), e.what());
            m_presets.clear();
            m_lastUsed.clear();
        }
    }
    // Absent or unreadable file: zero presets, not an error (first run).
    emit patternsChanged();
}

QVariantList RenamePatternStore::catalog() const {
    QVariantList out;
    out.reserve(static_cast<int>(m_presets.size()));
    for (const Preset& p : m_presets) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), p.name);
        m.insert(QStringLiteral("pattern"), p.pattern);
        out.append(m);
    }
    return out;
}

QString RenamePatternStore::patternFor(const QString& name) const {
    const int i = indexOf(name);
    return i >= 0 ? m_presets.at(i).pattern : QString();
}

void RenamePatternStore::save(const QString& name, const QString& pattern) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || pattern.isEmpty())
        return; // the dialog gates these; the store does not rely on it
    const int i = indexOf(trimmed);
    if (i >= 0) {
        if (m_presets.at(i).pattern == pattern)
            return; // saving the identical preset is a no-op, no churn
        m_presets[i].pattern = pattern; // upsert in place, position kept
    } else {
        m_presets.push_back(Preset{ trimmed, pattern });
    }
    persist();
    emit patternsChanged();
}

void RenamePatternStore::remove(const QString& name) {
    const int i = indexOf(name);
    if (i < 0)
        return;
    m_presets.removeAt(i);
    if (m_lastUsed == name)
        m_lastUsed.clear(); // a fresh dialog must not chase a deleted preset
    persist();
    emit patternsChanged();
}

void RenamePatternStore::setLastUsed(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == m_lastUsed)
        return;
    m_lastUsed = trimmed;
    persist();
}

int RenamePatternStore::indexOf(const QString& name) const {
    for (int i = 0; i < m_presets.size(); ++i)
        if (m_presets.at(i).name == name)
            return i;
    return -1;
}

void RenamePatternStore::persist() const {
    const QString path = renamePatternsPath();

    // A sequence under a top-level `patterns:` key, preserving user order.
    YAML::Emitter out;
    out << YAML::BeginMap;
    if (!m_lastUsed.isEmpty())
        out << YAML::Key << "last_used" << YAML::Value << m_lastUsed.toStdString();
    out << YAML::Key << "patterns" << YAML::Value << YAML::BeginSeq;
    for (const Preset& p : m_presets) {
        out << YAML::BeginMap;
        out << YAML::Key << "name"    << YAML::Value << p.name.toStdString();
        out << YAML::Key << "pattern" << YAML::Value << p.pattern.toStdString();
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;

    QByteArray content;
    content += "# rawform - saved file-rename patterns (generated by the app).\n";
    content += "# Each entry: a preset name and a %token% pattern (the same token\n";
    content += "# language as custom columns; e.g. %track_no%-%artist%-%title%).\n";
    content += "# Values are scene-sanitized at rename time, so patterns hold the\n";
    content += "# raw tokens, not sanitized text. Delete an entry (or the whole\n";
    content += "# file) to remove a preset.\n";
    content += out.c_str();
    content += '\n';

    yamlfile::writeAtomically(path, content);
}

}  // namespace rawform
