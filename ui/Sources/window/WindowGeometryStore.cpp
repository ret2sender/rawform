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

// WindowGeometryStore.cpp
//
// See the header for the contract. Persistence goes through the shared YAML
// idiom (utils/YamlFile.h): read-all then YAML::Load inside a try, with absent
// or unreadable treated as the normal first-run case, and an atomic write so a
// crash mid-write can never truncate the file.

#include "window/WindowGeometryStore.h"

#include "utils/YamlFile.h"

#include "paths/Paths.h"  // userConfigDir() for window.yaml

#include <QByteArray>
#include <QGuiApplication>
#include <QIODevice>
#include <QRect>
#include <QScreen>

#include <yaml-cpp/yaml.h>

#include <optional>
#include <string>

namespace rawform {
namespace {

QString windowSettingsPath() {
    return userConfigDir() + QStringLiteral("/window.yaml");
}

// A saved position is only usable when the saved rect still touches a
// connected screen. Intersection (not containment) is the right test: a window
// half off one edge is recoverable by the user with a drag, whereas a window
// entirely on a since-disconnected monitor is unreachable without knowing the
// platform's rescue shortcuts, which is exactly the failure this prevents.
bool rectTouchesAnyScreen(const QRect& rect) {
    const auto screens = QGuiApplication::screens();
    for (const QScreen* screen : screens) {
        if (screen != nullptr && rect.intersects(screen->geometry())) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
WindowGeometryStore::WindowGeometryStore(QObject* parent) : QObject(parent) {
    // On Wayland a client can neither read nor set its toplevel's global
    // position (QWindow x/y just echo the client's own last write), so
    // position persistence is disabled there up front. platformName covers
    // the plugin variants ("wayland", "wayland-egl", ...); every other
    // platform rawform runs on (xcb, cocoa, windows) round-trips positions
    // for real.
    m_platformCanPosition =
        !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
    // Load eagerly so the startup properties are valid before QML binds them
    // (the store is registered as a context property before engine.load, so
    // construction strictly precedes the first binding evaluation).
    load();
    emit geometryLoaded();
}

// ---------------------------------------------------------------------------
void WindowGeometryStore::load() {
    // Absent or unreadable is the normal first-run case: the defaults stand
    // (default size, no saved position, the window centers). A malformed file is
    // logged and ignored the same way.
    const auto text = yamlfile::readAll(windowSettingsPath());
    if (!text) {
        return;
    }

    // Keys are read individually so a partial file degrades per-field: a file
    // carrying only width/height restores the size and still centers.
    bool haveX = false;
    bool haveY = false;
    int  x     = 0;
    int  y     = 0;
    try {
        const YAML::Node root = YAML::Load(*text);
        if (root["width"]) {
            const int w = root["width"].as<int>();
            if (w > 0) {
                m_width = w;
            }
        }
        if (root["height"]) {
            const int h = root["height"].as<int>();
            if (h > 0) {
                m_height = h;
            }
        }
        if (root["x"]) {
            x     = root["x"].as<int>();
            haveX = true;
        }
        if (root["y"]) {
            y     = root["y"].as<int>();
            haveY = true;
        }
        // Keyed tool-window sizes. Any map-valued entry carrying width
        // and height is accepted under its key; the scalar main keys above
        // are Scalar nodes, so they can never match this branch. Reading is
        // per-key and per-field tolerant like everything else in this file:
        // a malformed entry is skipped, the rest of the file stands.
        for (auto it = root.begin(); it != root.end(); ++it) {
            // COPY the element, never bind a reference to it->second.
            // yaml-cpp's map iterator has no persistent element: operator->
            // returns a proxy holding a temporary iterator_value, and a
            // reference to its .second outlives it at the end of that
            // statement (ASan: stack-use-after-scope on the first
            // sub["width"] probe). iterator_value and Node are cheap
            // refcounted handles, so the copies cost nothing and keep the
            // underlying nodes alive for the block. (auto, not YAML::Node:
            // naming Node would slice away the first/second members.)
            const auto pair = *it;
            if (!pair.second.IsMap()) {
                continue;
            }
            const YAML::Node sub = pair.second;
            if (!sub["width"] || !sub["height"]) {
                continue;
            }
            const int w = sub["width"].as<int>(0);
            const int h = sub["height"].as<int>(0);
            if (w > 0 && h > 0) {
                m_toolSizes.insert(
                    QString::fromStdString(pair.first.as<std::string>()),
                    QSize(w, h));
            }
        }
    } catch (const YAML::Exception& e) {
        qWarning("rawform: ignoring malformed %s (%s)",
                 qUtf8Printable(windowSettingsPath()), e.what());
        m_width           = 1800;
        m_height          = 1040;
        m_fileHasPosition = false;
        m_toolSizes.clear();
        return;
    }

    // Accept the position only when the full saved rect (at the possibly
    // just-loaded size) still touches a connected screen. Size restores
    // unconditionally; only the position is guarded, because a wrong size is a
    // cosmetic nuisance while a wrong position can put the window out of reach.
    // Deliberately NOT gated on m_platformCanPosition: this records what the
    // FILE holds, so a Wayland session preserves an X11 session's coordinates
    // through its rewrites; hasSavedPosition() applies the platform gate
    // on the way out to QML.
    if (haveX && haveY &&
        rectTouchesAnyScreen(QRect(x, y, m_width, m_height))) {
        m_x              = x;
        m_y              = y;
        m_fileHasPosition = true;
    }
}

// ---------------------------------------------------------------------------
void WindowGeometryStore::save(int x, int y, int width, int height) {
    // Fold the final main-window frame into the members FIRST: writeFile()
    // emits from the members only, and a later saveSize() (another window
    // closing after the main one) re-emits the whole file, so stale members
    // would silently roll the main rect back. And on a platform without
    // client positioning the x/y arguments are only echoes of the startup
    // centering computation, so they are dropped and the members keep what
    // the file held (a real position from an X11 session, or nothing).
    if (m_platformCanPosition) {
        m_x              = x;
        m_y              = y;
        m_fileHasPosition = true;
    }
    m_width  = width;
    m_height = height;
    writeFile();
}

// ---------------------------------------------------------------------------
int WindowGeometryStore::savedWidth(const QString& key, int fallback) const {
    const auto it = m_toolSizes.constFind(key);
    return it != m_toolSizes.constEnd() ? it->width() : fallback;
}

int WindowGeometryStore::savedHeight(const QString& key, int fallback) const {
    const auto it = m_toolSizes.constFind(key);
    return it != m_toolSizes.constEnd() ? it->height() : fallback;
}

// ---------------------------------------------------------------------------
void WindowGeometryStore::saveSize(const QString& key, int width, int height) {
    if (key.isEmpty() || width <= 0 || height <= 0) {
        return;  // never let a degenerate frame poison the file
    }
    m_toolSizes.insert(key, QSize(width, height));
    writeFile();
}

// ---------------------------------------------------------------------------
void WindowGeometryStore::writeFile() const {
    // Emit everything the store holds and hand it to the shared atomic
    // writer. No debounce: each caller runs once, from a window's onClosing.
    const QString   path = windowSettingsPath();

    YAML::Emitter out;
    out << YAML::BeginMap;
    // The position keys are emitted only when a real position is known: on a
    // first run a tool-window close lands here before the main window has
    // ever saved, and inventing x/y 0/0 would pass the on-screen test next
    // launch and open the window top-left instead of centered. The file
    // flag, not the platform-gated property: a Wayland rewrite must carry an
    // X11 session's coordinates through unchanged.
    if (m_fileHasPosition) {
        out << YAML::Key << "x" << YAML::Value << m_x;
        out << YAML::Key << "y" << YAML::Value << m_y;
    }
    out << YAML::Key << "width" << YAML::Value << m_width;
    out << YAML::Key << "height" << YAML::Value << m_height;
    // Keyed tool-window sizes, in QMap (sorted-key) order for a stable file.
    for (auto it = m_toolSizes.constBegin(); it != m_toolSizes.constEnd(); ++it) {
        out << YAML::Key << it.key().toStdString();
        out << YAML::Value << YAML::BeginMap;
        out << YAML::Key << "width" << YAML::Value << it->width();
        out << YAML::Key << "height" << YAML::Value << it->height();
        out << YAML::EndMap;
    }
    out << YAML::EndMap;

    QByteArray content;
    content += "# rawform - window geometry (generated by the app).\n";
    content += "# Windowed size and position only; maximized/fullscreen state is never stored.\n";
    content += "# Named sub-maps hold per-tool-window sizes (size only; those windows cascade).\n";
    content += out.c_str();
    content += '\n';

    yamlfile::writeAtomically(path, content);
}

}  // namespace rawform
