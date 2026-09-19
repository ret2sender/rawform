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

// AppInfo.h
//
// Read-only application metadata exposed to QML.

#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

// Defined by the build system from PROJECT_VERSION (see
// target_compile_definitions in ui/CMakeLists.txt). The fallback keeps
// out-of-tree builds compiling while making the misconfiguration obvious
// wherever the version is displayed.
#ifndef RAWFORM_VERSION_STR
#define RAWFORM_VERSION_STR "0.0.0-unversioned"
#endif

/// Exposes application metadata to QML as the `AppInfo` singleton.
///
/// The version string has one source of truth, ui/VERSION.txt:
/// ui/CMakeLists.txt reads that file into project()'s VERSION and bakes the
/// result into this translation unit as the RAWFORM_VERSION_STR compile
/// definition. The Qt version is the runtime library version (qVersion()),
/// which is what actually loaded, not the headers the binary was compiled
/// against.
///
/// Stateless and configuration-free, so the QML engine constructs and owns
/// the singleton instance itself (constructor-mode registration). This is
/// deliberate: the constructor-mode trap only bites classes whose create()
/// factory would inject configuration; there is none here.
///
/// Note: Deliberately not `final`: constructor-mode QML singletons are
/// instantiated by the engine through QQmlPrivate::QQmlElement<T>, which
/// derives from T. Marking the class final makes registration ill-formed
/// (MSVC diagnoses this as C3246; clang happens not to instantiate the
/// offending specialization, but the code is invalid either way).
class AppInfo : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(AppInfo)
    QML_SINGLETON

    /// Application version, e.g. "1.0.0".
    Q_PROPERTY(QString version READ version CONSTANT)

    /// Qt runtime library version, e.g. "6.11.1".
    Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)

    /// Name of the active system icon theme, e.g. "breeze". Empty where
    /// the platform provides no icon theme.
    Q_PROPERTY(QString systemIconTheme READ systemIconTheme CONSTANT)

public:
    /// Constructs an AppInfo; the optional QObject parent is for ownership only.
    explicit AppInfo(QObject* parent = nullptr);

    /// Application version from the build system: the semantic version string,
    /// e.g. "1.0.0".
    [[nodiscard]] QString version() const;

    /// Qt library version loaded at runtime, as reported by qVersion().
    [[nodiscard]] QString qtVersion() const;

    /// Name of the icon theme the user has configured.
    ///
    /// Read from QIcon::themeName(), which the QPA platform theme
    /// populates during QGuiApplication construction, so it is available
    /// by the time the QML engine instantiates this singleton.
    ///
    /// Consumed by TitleBarStyle to select caption button artwork. The
    /// icon theme is used in preference to XDG_CURRENT_DESKTOP because it
    /// reflects what the user actually configured: it stays correct on
    /// tiling compositors, on desktops this build has never heard of, and
    /// on a Plasma session running a third-party theme such as Papirus.
    ///
    /// Returns the theme name, e.g. "breeze"; empty on Windows and macOS, and
    /// on Linux sessions that expose no icon theme setting. An empty
    /// result is the expected fallback path, not an error - callers
    /// resolve it to their default styling.
    [[nodiscard]] QString systemIconTheme() const;
};
