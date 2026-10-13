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

// Formats.cpp
//
// See the header. fileSizeText delegates to QLocale::formattedDataSize with
// the traditional 1024-based units, the one spelling both surfaces show.

#include "Formats.h"

#include <QLatin1Char>
#include <QLocale>

namespace rawform::formats {

QString durationText(qint64 ms) {
    if (ms <= 0)
        return {};
    const qint64 totalSec = ms / 1000;
    const qint64 hours    = totalSec / 3600;
    const qint64 minutes  = (totalSec / 60) % 60;
    const qint64 seconds  = totalSec % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString dateTimeText(const QDateTime& dt) {
    return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString{};
}

QString fileSizeText(qint64 bytes) {
    if (bytes <= 0)
        return {};
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

}  // namespace rawform::formats
