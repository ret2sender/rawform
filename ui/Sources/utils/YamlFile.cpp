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

// YamlFile.cpp
//
// See the header. This is the one definition of the read-all / atomic-write
// pair every YAML store shares.

#include "YamlFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>

namespace rawform::yamlfile {

std::optional<std::string> readAll(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;  // absent or unreadable: the first-run case
    }
    const QByteArray bytes = f.readAll();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

bool writeAtomically(const QString& path, const QByteArray& content) {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        qWarning("rawform: could not create config dir %s",
                 qUtf8Printable(info.absolutePath()));
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning("rawform: could not open %s for writing", qUtf8Printable(path));
        return false;
    }
    if (f.write(content) != content.size()) {
        f.cancelWriting();
        qWarning("rawform: short write to %s", qUtf8Printable(path));
        return false;
    }
    if (!f.commit()) {
        qWarning("rawform: could not commit %s", qUtf8Printable(path));
        return false;
    }
    return true;
}

}  // namespace rawform::yamlfile
