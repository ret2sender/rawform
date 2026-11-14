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

// FileOpenFilter.cpp
//
// Implementation of the file-open event filter declared in FileOpenFilter.h.

#include "utils/FileOpenFilter.h"

#include <QEvent>
#include <QFileOpenEvent>

namespace rawform {

FileOpenFilter::FileOpenFilter(QObject* parent)
    : QObject(parent) {
}

bool FileOpenFilter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::FileOpen) {
        return QObject::eventFilter(watched, event);
    }
    const QUrl url = static_cast<QFileOpenEvent*>(event)->url();
    if (url.isLocalFile()) {
        emit filesRequested({url});
    }
    return true;
}

} // namespace rawform
