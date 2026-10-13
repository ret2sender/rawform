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

// WindowGeometryStore.h
//
// Persists the app window's WINDOWED size and position across launches, via
// window.yaml under userConfigDir(), through the shared YAML idiom
// (utils/YamlFile.h) like spectrum.yaml and playback.yaml. Deliberately
// minimal state:
//
//   - Windowed geometry only. Maximized/fullscreen state is never stored;
//     the QML side guards the save to Window.Windowed visibility, so the
//     last GOOD windowed geometry in the file survives a maximized or
//     fullscreen quit.
//   - Size is always restored, clamped to the primary screen's available area
//     (a 1800 x 1040 default or a saved size from a larger monitor must not
//     open bigger than the screen; MainWindow.qml's own minimums still apply
//     below that). Position only when it still lands on a connected screen.
//     A position saved on a since-disconnected monitor would open the window
//     off-screen, so load() validates the saved rect against
//     QGuiApplication::screens() and, on rejection, computes the startup
//     position itself: dead-center on the primary screen's available area.
//     startupX/startupY are therefore always a usable position, and the QML
//     binds them as plain constants (no live dependency on the window's own
//     size or on the Screen attached property, either of which would re-center
//     the window on every resize or monitor change on cocoa, xcb and windows).
//   - Position is PLATFORM-CONDITIONAL. Wayland's xdg-shell gives a
//     client no way to read or set a toplevel's global position: QWindow's
//     x/y there only echo what the client last wrote, and writes are ignored
//     (the compositor alone places windows). On such a platform the store
//     stops pretending: save() ignores its x/y arguments, hasSavedPosition
//     reports false (the startup position is then the inert but honest
//     centered one), and any x/y already in the file are PRESERVED verbatim
//     on rewrite instead of clobbered with echo values, so a setup that also
//     runs X11 sessions keeps its real coordinates. Size is client-side on
//     every platform and stays unconditional.
//
//   - Keyed TOOL-WINDOW sizes: per-window `width`/`height` sub-maps
//     under a name key (`properties:`, `rename:`, `settings:`,
//     `customColumns:`), for the frameless tool windows (they resize
//     through WindowResizeGrips). SIZE ONLY, never position: the
//     fresh-instance windows cascade from the host at open, and a stored
//     position would fight the cascade; the hide()-reused ones keep the
//     spot the window system last gave them. Saved on each window's close
//     or hide (windowed visibility only, the same guard as the main
//     window); several instances closing just means last write wins,
//     matching the windows' own last-write-wins staging semantics.
//
// Lifecycle: constructed in main() AFTER the QGuiApplication (screens() needs
// the app object) and registered as the WindowGeometryStore singleton BEFORE
// the root component is created, so load() has run in the constructor and
// every startup property below is already valid when MainWindow.qml binds
// width/height/x/y.
// The properties never change after construction (the NOTIFY signals exist to
// satisfy QML's binding machinery, not because anything fires them), and the
// window binds them once at creation; save() is a plain invokable called from
// onClosing with the final windowed frame. The keyed accessors are plain
// invokables too: a tool window reads its size once as an initial value at
// creation (invokable calls are not tracked as binding dependencies, which is
// exactly right for a startup-only read).

#pragma once

#include "utils/QmlSingletonInstance.h" // the singleton wrapper at the end

#include <QMap>
#include <QObject>
#include <QSize>
#include <QString>

namespace rawform {

class WindowGeometryStore : public QObject {
    Q_OBJECT
    /// QML reaches the one instance as the WindowGeometryStore singleton: see
    /// the WindowGeometryStoreSingleton wrapper at the end of this file.

    /// Startup size: the saved windowed size, else the built-in defaults
    /// (1800 x 1040; MainWindow.qml carries no size literal of its own, it
    /// binds these).
    Q_PROPERTY(int startupWidth READ startupWidth NOTIFY geometryLoaded)
    Q_PROPERTY(int startupHeight READ startupHeight NOTIFY geometryLoaded)
    /// Startup position: the validated saved position, else the centered one
    /// the store computed, so first run (no file), a rejected off-screen
    /// position, and a position-less platform all open dead-center.
    Q_PROPERTY(int startupX READ startupX NOTIFY geometryLoaded)
    Q_PROPERTY(int startupY READ startupY NOTIFY geometryLoaded)
    /// Whether that position came from the file (see hasSavedPosition()).
    Q_PROPERTY(bool hasSavedPosition READ hasSavedPosition NOTIFY geometryLoaded)

public:
    explicit WindowGeometryStore(QObject* parent = nullptr);


    [[nodiscard]] int  startupWidth() const { return m_width; }
    [[nodiscard]] int  startupHeight() const { return m_height; }
    [[nodiscard]] int  startupX() const { return m_x; }
    [[nodiscard]] int  startupY() const { return m_y; }
    /// Whether startupX/startupY come from the file (validated against the
    /// connected screens) rather than from the centering fallback; false too on
    /// a platform that cannot apply a position at all.
    [[nodiscard]] bool hasSavedPosition() const {
        return m_fileHasPosition && m_platformCanPosition;
    }

    /// Persist the final windowed frame. Called from MainWindow.qml's
    /// onClosing, which guards to Window.Windowed visibility, so these are
    /// always plain windowed coordinates by the time they arrive here. The one
    /// policy beyond the atomic-write discipline: on a platform without
    /// client positioning the x/y arguments are echoes, not coordinates, and
    /// are dropped in favor of whatever the file already held.
    Q_INVOKABLE void save(int x, int y, int width, int height);

    /// Keyed tool-window sizes. savedWidth/savedHeight return the stored
    /// value for `key`, else `fallback` (the window's built-in default); the
    /// caller clamps against its own minimums, which the store does not know.
    /// saveSize records the size and rewrites the file; call it from the
    /// window's onClosing, guarded to windowed visibility.
    [[nodiscard]] Q_INVOKABLE int savedWidth(const QString& key, int fallback) const;
    [[nodiscard]] Q_INVOKABLE int savedHeight(const QString& key, int fallback) const;
    Q_INVOKABLE void saveSize(const QString& key, int width, int height);

signals:
    /// Fired once at the end of the constructor's load. Exists so the read-only
    /// Q_PROPERTYs above are NOTIFY-backed (QML warns on bindings to properties
    /// without one); no consumer needs to react to it.
    void geometryLoaded();

private:
    void load();
    /// Single emit point for window.yaml: both save() and saveSize() funnel
    /// through here, so the main rect and the keyed tool sizes can never
    /// clobber each other (each write re-emits everything the store holds).
    void writeFile() const;

    /// Clamp the size to the primary screen's available area and, with no
    /// validated position, center the rect on it. Runs at the end of load()
    /// on every path, the malformed-file one included.
    void placeOnPrimaryScreen();

    /// Defaults, used when there is no file, the file is malformed, or an
    /// individual key is absent.
    int  m_width            = 1800;
    int  m_height           = 1040;
    int  m_x                = 0;
    int  m_y                = 0;
    /// Two facts, not one: the FILE carrying a validated position (drives
    /// what writeFile emits and what a rewrite preserves) is a different fact
    /// from this PLATFORM being able to use it (folded into the
    /// hasSavedPosition property). On Wayland the first can be true, from an
    /// X11 session's save, while the second must read false.
    bool m_fileHasPosition    = false;
    bool m_platformCanPosition = true;

    /// Keyed tool-window sizes, by name (`properties`, `rename`, `settings`,
    /// `customColumns`). QMap so the emit order (and therefore the file) is
    /// stable across runs.
    QMap<QString, QSize> m_toolSizes;
};

/// The WindowGeometryStore QML singleton: the one instance main.cpp builds, handed to the
/// engine through create(). A QML_FOREIGN wrapper rather than QML_SINGLETON on
/// the class itself, because the engine prefers a type's default constructor
/// over its create() and would build a second, empty instance (see
/// utils/QmlSingletonInstance.h); with a wrapper the factory always wins.
struct WindowGeometryStoreSingleton {
    Q_GADGET
    QML_FOREIGN(rawform::WindowGeometryStore)
    QML_NAMED_ELEMENT(WindowGeometryStore)
    QML_SINGLETON

public:
    // Q_GADGET leaves the struct in a private section; the factory must be
    // public for the engine's detection to see it.
    static WindowGeometryStore* create(QQmlEngine* engine, QJSEngine* /*jsEngine*/) {
        return QmlSingletonInstance<WindowGeometryStore>::take(engine);
    }
};

}  // namespace rawform
