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

// MacOSStyling.h
//
// Native macOS window styling for the rawform application.
//
// rawform draws its own title bar in QML (see TitleBar.qml). On macOS we want
// the genuine traffic-light controls (close, minimize, zoom) rather than the
// themed QML controls used on Linux and Windows, while still keeping
// rawform's QML title bar content (the logo) painted underneath. Achieving
// that means reaching past Qt into AppKit, which is what the matching .mm
// does.
//
// The single entry point below is intentionally tiny so the only file that has
// to be Objective-C++ is the .mm; everything else in the app stays plain C++.

#pragma once

/// Forward declaration of QQmlApplicationEngine.
///
/// Only a pointer to this class appears in the function signature below, so the
/// compiler needs just the name, not the full definition. The heavy
/// `<QQmlApplicationEngine>` header is pulled in only by the .mm
/// implementation, where the class is actually used. Keeping it out of this
/// header avoids dragging a large Qt include into every translation unit that
/// includes us.
class QQmlApplicationEngine;

/// Applies native macOS window styling to the application's root window.
///
/// Configures the title bar to be transparent with a full-size content view so
/// the QML content extends behind it, hides the native title text, disables
/// AppKit's own window dragging (QML's TitleBar MouseArea drives moves via
/// `startSystemMove()` instead), and measures the native title bar height so
/// QML can offset its content by exactly that amount.
///
/// This function is a no-op on non-macOS platforms. In practice the build only
/// compiles the implementation translation unit on macOS (see CMakeLists.txt),
/// and main.cpp only calls it under `Q_OS_MAC`, so the no-op branch is a
/// belt-and-suspenders guard rather than a path the app relies on.
///
/// engine: The QQmlApplicationEngine whose root window will be styled.
void applyMacOSStyling(QQmlApplicationEngine* engine);
