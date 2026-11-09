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

// RateLedgerFile.cpp
//
// See the header. The IO discipline (whole-file read, QSaveFile write) is the
// shared one from utils/YamlFile.h; this file owns only the schema.

#include "playback/RateLedgerFile.h"

#include "utils/YamlFile.h"

#include <QByteArray>
#include <QFile>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <optional>
#include <utility>

namespace rawform::rateledger {

namespace {

// One entry from a map node. False for a node that is not a usable debt; a
// value of the wrong type throws, and the caller treats the file as malformed.
bool toEntry(const YAML::Node& node, Entry& out) {
    if (!node.IsMap()) {
        return false;
    }
    const YAML::Node id       = node["device_id"];
    const YAML::Node original = node["original_rate"];
    const YAML::Node borrowed = node["borrowed_rate"];
    Entry entry;
    entry.deviceId       = id ? id.as<std::string>() : std::string{};
    entry.originalRateHz = original ? original.as<std::uint32_t>() : 0;
    entry.borrowedRateHz = borrowed ? borrowed.as<std::uint32_t>() : 0;
    if (entry.deviceId.empty() || entry.originalRateHz == 0) {
        return false;
    }
    out = std::move(entry);
    return true;
}

}  // namespace

ReadResult read(const QString& path) {
    ReadResult result;
    const std::optional<std::string> text = yamlfile::readAll(path);
    if (!text) {
        // readAll folds "unreadable" into "absent"; a file that exists but
        // cannot be read is not a ledger anyone can act on.
        result.status = QFile::exists(path) ? ReadStatus::Malformed : ReadStatus::Absent;
        return result;
    }
    try {
        const YAML::Node root = YAML::Load(*text);
        if (!root.IsMap()) {
            result.status = ReadStatus::Malformed;
            return result;
        }
        std::vector<Entry> entries;
        Entry              entry;
        if (const YAML::Node debts = root["debts"]) {
            if (!debts.IsSequence()) {
                result.status = ReadStatus::Malformed;
                return result;
            }
            for (const YAML::Node item : debts) {
                if (toEntry(item, entry)) {
                    // A repeated device keeps its last entry. Qualified because
                    // ADL through std::string would also find std::apply.
                    rateledger::apply(entries, entry);
                }
            }
        } else if (toEntry(root, entry)) {
            entries.push_back(entry);  // the single entry of an older build
        }
        result.status  = ReadStatus::Ok;
        result.entries = std::move(entries);
    } catch (const YAML::Exception&) {
        result.status = ReadStatus::Malformed;
        result.entries.clear();
    }
    return result;
}

bool apply(std::vector<Entry>& entries, const Entry& change) {
    if (change.deviceId.empty()) {
        return false;
    }
    const auto it = std::ranges::find(entries, change.deviceId, &Entry::deviceId);
    if (change.originalRateHz == 0) {
        if (it == entries.end()) {
            return false;
        }
        entries.erase(it);
        return true;
    }
    if (it == entries.end()) {
        entries.push_back(change);
        return true;
    }
    if (*it == change) {
        return false;
    }
    *it = change;
    return true;
}

bool write(const QString& path, const std::vector<Entry>& entries) {
    if (entries.empty()) {
        if (!QFile::exists(path) || QFile::remove(path)) {
            return true;
        }
        qWarning("rawform: could not remove %s", qUtf8Printable(path));
        return false;
    }

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "debts" << YAML::Value << YAML::BeginSeq;
    for (const Entry& e : entries) {
        out << YAML::BeginMap;
        out << YAML::Key << "device_id" << YAML::Value << e.deviceId;
        out << YAML::Key << "original_rate" << YAML::Value << e.originalRateHz;
        out << YAML::Key << "borrowed_rate" << YAML::Value << e.borrowedRateHz;
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;

    QByteArray content;
    content += "# rawform rate ledger. This file exists while an audio device sits\n";
    content += "# at a sample rate rawform borrowed, one entry per device; an entry\n";
    content += "# goes when its debt is repaid or forgiven, and the file goes with\n";
    content += "# the last one. Found at launch, it means a previous session ended\n";
    content += "# without repaying: rawform restores each original rate if the\n";
    content += "# device still sits at the borrowed one.\n";
    content += out.c_str();
    content += '\n';

    return yamlfile::writeAtomically(path, content);
}

}  // namespace rawform::rateledger
