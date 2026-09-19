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

// CustomColumnRegistry.cpp
//
// Implementation of the custom-column registry: load and write-through of
// playlist_custom_columns.yaml through the shared YAML idiom (utils/YamlFile.h),
// id generation, and the mutation API the manager window drives. Every mutation
// persists wholesale and emits the change signal the playlist models react to.

#include "columns/CustomColumnRegistry.h"

#include "utils/YamlFile.h"

#include "paths/Paths.h" // userConfigDir()

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QLatin1Char>
#include <QSet>
#include <QString>
#include <QUuid>
#include <QVariantMap>
#include <QtGlobal> // qWarning

#include <yaml-cpp/yaml.h>

#include <optional>
#include <string>

namespace rawform {
namespace {

/// Filename of the app-managed custom-columns file (under userConfigDir()).
constexpr auto kCustomColumnsFileName = "playlist_custom_columns.yaml";

/// Absolute path of the custom-columns file (may not exist yet).
QString customColumnsPath() {
    return userConfigDir()
           + QLatin1Char('/') + QLatin1String(kCustomColumnsFileName);
}

/// Generate a fresh, stable id. QUuid without braces: collision-free, safe in
/// both the YAML and the binary .rwftp/.rwfpl "custom:<id>" strings.
QString freshId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace

CustomColumnRegistry::CustomColumnRegistry(QObject* parent) : QObject(parent) {}

void CustomColumnRegistry::load() {
    m_columns.clear();

    const auto bytes = yamlfile::readAll(customColumnsPath());
    if (!bytes)
        return; // absent or unreadable -> no custom columns (not an error)

    try {
        const YAML::Node root = YAML::Load(*bytes);
        if (!root || !root.IsMap())
            return;
        const YAML::Node cols = root["columns"];
        if (!cols || !cols.IsSequence())
            return;

        QSet<QString> seen; // drop duplicate ids (keep first)
        for (const YAML::Node& node : cols) {
            if (!node.IsMap())
                continue;

            const YAML::Node idNode = node["id"];
            if (!idNode || !idNode.IsScalar())
                continue; // a record without an id is unusable -> skip
            const QString id = QString::fromStdString(idNode.as<std::string>());
            if (id.isEmpty() || seen.contains(id))
                continue;

            CustomColumn c;
            c.id = id;
            if (const YAML::Node n = node["name"]; n && n.IsScalar())
                c.name = QString::fromStdString(n.as<std::string>());
            if (const YAML::Node n = node["align"]; n && n.IsScalar())
                c.alignment = alignmentFromString(
                    QString::fromStdString(n.as<std::string>()));
            else
                c.alignment = Qt::AlignLeft;
            if (const YAML::Node n = node["pattern"]; n && n.IsScalar())
                c.pattern = QString::fromStdString(n.as<std::string>());

            seen.insert(id);
            m_columns.push_back(std::move(c));
        }
    } catch (const YAML::Exception& e) {
        qWarning("rawform: %s is invalid (%s); ignoring custom columns",
                 qUtf8Printable(customColumnsPath()), e.what());
        m_columns.clear();
    }
}

const CustomColumn* CustomColumnRegistry::byId(const QString& id) const {
    const int i = indexOf(id);
    return i >= 0 ? &m_columns.at(i) : nullptr;
}

bool CustomColumnRegistry::contains(const QString& id) const {
    return indexOf(id) >= 0;
}

QString CustomColumnRegistry::add() {
    CustomColumn c;
    c.id        = freshId();
    c.name      = QString();
    c.alignment = Qt::AlignLeft;
    c.pattern   = QString();
    m_columns.push_back(c);
    persist();
    emit columnsChanged();
    return c.id;
}

QVariantList CustomColumnRegistry::catalog() const {
    QVariantList out;
    out.reserve(static_cast<int>(m_columns.size()));
    for (const CustomColumn& c : m_columns) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), c.id);
        m.insert(QStringLiteral("fieldId"), customFieldId(c.id));
        m.insert(QStringLiteral("name"), c.name);
        m.insert(QStringLiteral("align"), stringFromAlignment(c.alignment));
        m.insert(QStringLiteral("pattern"), c.pattern);
        out.append(m);
    }
    return out;
}

void CustomColumnRegistry::remove(const QString& id) {
    const int i = indexOf(id);
    if (i < 0)
        return;
    m_columns.removeAt(i);
    persist();
    // columnRemoved only (not columnsChanged): the model drops the bound column
    // on this signal, and emitting both would double the work.
    emit columnRemoved(id);
}

void CustomColumnRegistry::setAlignment(const QString& id, int alignment) {
    const int i = indexOf(id);
    if (i < 0)
        return;
    // Keep only a recognized horizontal flag; anything else clamps to Left.
    const Qt::Alignment h =
        static_cast<Qt::Alignment>(alignment) & Qt::AlignHorizontal_Mask;
    const Qt::Alignment a = (h == Qt::AlignRight)   ? Qt::AlignRight
                          : (h == Qt::AlignHCenter) ? Qt::AlignHCenter
                                                    : Qt::AlignLeft;
    if (m_columns.at(i).alignment == a)
        return;
    m_columns[i].alignment = a;
    persist();
    emit columnsChanged();
}

void CustomColumnRegistry::setName(const QString& id, const QString& name) {
    const int i = indexOf(id);
    if (i < 0 || m_columns.at(i).name == name)
        return;
    m_columns[i].name = name;
    persist();
    emit columnsChanged();
}

void CustomColumnRegistry::setPattern(const QString& id, const QString& pattern) {
    const int i = indexOf(id);
    if (i < 0 || m_columns.at(i).pattern == pattern)
        return;
    m_columns[i].pattern = pattern;
    persist();
    emit columnsChanged();
}

int CustomColumnRegistry::indexOf(const QString& id) const {
    for (int i = 0; i < m_columns.size(); ++i)
        if (m_columns.at(i).id == id)
            return i;
    return -1;
}

void CustomColumnRegistry::persist() const {
    const QString path = customColumnsPath();

    // Emit a sequence under a top-level `columns:` key, preserving user order
    // (which is meaningful here).
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "columns" << YAML::Value << YAML::BeginSeq;
    for (const CustomColumn& c : m_columns) {
        out << YAML::BeginMap;
        out << YAML::Key << "id"      << YAML::Value << c.id.toStdString();
        out << YAML::Key << "name"    << YAML::Value << c.name.toStdString();
        out << YAML::Key << "align"   << YAML::Value << stringFromAlignment(c.alignment).toStdString();
        out << YAML::Key << "pattern" << YAML::Value << c.pattern.toStdString();
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;

    QByteArray content;
    content += "# rawform - user-defined custom playlist columns (generated by the app).\n";
    content += "# Each entry: a stable id, a header name, an alignment\n";
    content += "# (left|center|right), and a %token% pattern. Delete an entry (or the\n";
    content += "# whole file) to remove a column. Tokens resolve native fields first\n";
    content += "# (e.g. %artist%, %album_group%), then non-promoted tags (e.g. %barcode%).\n";
    content += out.c_str();
    content += '\n';

    yamlfile::writeAtomically(path, content);
}

} // namespace rawform
