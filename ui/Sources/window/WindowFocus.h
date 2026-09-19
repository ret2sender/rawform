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

// WindowFocus.h
//
// A QML singleton exposing QGuiApplication::focusWindow() as a notifying
// property, so QML can gate on STRICT focus-window identity:
//
//     enabled: someWindow.visible && WindowFocus.focusWindow === someWindow
//
// Why this exists: the QML-visible activation surfaces are all GROUP
// semantics, not focus identity. Window.active maps to QWindow::isActive(),
// which for a window with a transient parent returns the transient parent's
// activation, and for the top level returns whether the focus window is
// anywhere in its transient family. Every secondary window in rawform is a
// transient child of the main window, so whenever ANY app window has focus,
// every visible secondary window reports active == true simultaneously. Qt's
// shortcut-context matching resolves through the same family logic. The
// consequence for same-sequence Shortcuts (all the Escape handlers): with two
// secondary windows open, both grabs match every press, the press is
// AMBIGUOUS, and Qt fires neither onActivated (it rotates
// activatedAmbiguously across the candidates), reading as app-wide dead
// Escape. focusWindow() is singular by definition, so identity against it
// restores the invariant the gates need: at most one window-scoped Escape
// grab exists at any instant.
//
// Header-only (the Clipboard.h pattern): one trivial getter and one signal
// relay, no state of its own. The notify rides
// QGuiApplication::focusWindowChanged directly; the argument is dropped by
// the connection, consumers re-read the property.

#pragma once

#include <QGuiApplication>
#include <QObject>
#include <QQmlEngine> // QML_ELEMENT / QML_SINGLETON
#include <QWindow>

namespace rawform {

class WindowFocus : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// The application's focus window (null when the app is inactive). Strict:
    /// exactly one window at most, never a transient-family aggregate.
    Q_PROPERTY(QWindow* focusWindow READ focusWindow NOTIFY focusWindowChanged)

public:
    explicit WindowFocus(QObject* parent = nullptr) : QObject(parent) {
        // Signal-to-signal relay; QGuiApplication outlives any QML engine, so
        // the connection's lifetime is governed by this object alone.
        connect(qGuiApp, &QGuiApplication::focusWindowChanged,
                this, &WindowFocus::focusWindowChanged);
    }

    [[nodiscard]] QWindow* focusWindow() const {
        return QGuiApplication::focusWindow();
    }

signals:
    void focusWindowChanged();
};

} // namespace rawform
