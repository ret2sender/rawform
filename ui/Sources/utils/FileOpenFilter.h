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

// FileOpenFilter.h
//
// The macOS way in for files. Finder, the Dock and `open -a` never put a file
// on the command line: LaunchServices starts the application, or finds the
// one already running, and delivers each file as a QFileOpenEvent to the
// application object. This filter, installed on that object, turns the event
// into a signal.
//
// An event filter rather than a QGuiApplication subclass, so the application
// object keeps its type. Built on every platform so one compiler set checks
// it; no other platform sends the event.

#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

class QEvent;

namespace rawform {

class FileOpenFilter : public QObject {
    Q_OBJECT

public:
    explicit FileOpenFilter(QObject* parent = nullptr);

    bool eventFilter(QObject* watched, QEvent* event) override;

signals:
    /// One file the system asked the application to open. A list, so the
    /// receiver is the one every other way in uses.
    void filesRequested(const QList<QUrl>& files);
};

} // namespace rawform
