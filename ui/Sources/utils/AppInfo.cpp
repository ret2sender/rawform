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

/// @file AppInfo.cpp
/// @brief Implementation of the AppInfo QML singleton.

#include "AppInfo.h"

#include <QtGlobal>
#include <QIcon>


AppInfo::AppInfo(QObject* parent)
    : QObject(parent) {
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Q_PROPERTY READ accessor
QString AppInfo::version() const {
    return QStringLiteral(RAWFORM_VERSION_STR);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Q_PROPERTY READ accessor
QString AppInfo::qtVersion() const {
    return QString::fromLatin1(qVersion());
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): Q_PROPERTY READ accessor
QString AppInfo::systemIconTheme() const {
    return QIcon::themeName();
}
