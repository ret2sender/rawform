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

// HeapTrim.h
//
// A QML singleton that hands the process heap's free pages back to the OS
// after a tool window goes away. The dialogs call it from their close paths:
//
//     onClosing: { ...; propertiesWindow.destroy(); HeapTrim.trimSoon() }
//
// Why this exists: every secondary window (Properties, Rename Files, Custom
// Columns) is its own QQuickWindow with its own render thread, scene graph,
// swapchain, and texture set, and the Properties window adds two delegate
// pools and several blur chains on top. Closing one frees all of that, but
// glibc keeps the freed pages on the arena free lists: nothing in the
// allocator returns mid-heap pages on its own, and the render thread each
// open spawns lands in its own arena, so the residue is spread across
// arenas as well. RSS then climbs with every open/close cycle even though
// nothing is live (measured on the 1.2.0 Flatpak, 40 cycles of a 50-track
// Properties window: +30 MB over the first 20, +16 MB over the next 20, with
// the QML heap ruled out by QV4_MM_AGGRESSIVE_GC and the per-track models
// ruled out by a 1-track control). malloc_trim walks every arena and
// releases its completely free pages, mid-heap included, in a few ms; the
// same call TrackScanner makes once per completed scan, for the same reason.
//
// Deferred, not inline: destroy() is a deferred delete, and the window's
// render thread is stopped and its scene graph freed inside that delete, so
// a trim issued from the close handler would run before the pages it is
// meant to release are free. The single-shot lands one event-loop pass
// later (the delay is slack, not a dependency: a trim that runs early is
// merely less effective, never wrong), and the pending flag coalesces a
// burst of closes into one trim. GUI thread only, like the scanner's call.
//
// The call compiles to a no-op where there is no glibc (macOS, Windows);
// the method keeps its real signature so the QML call sites never branch.
// Header-only (the Clipboard.h pattern): one invokable and one flag.

#pragma once

#include <QObject>
#include <QQmlEngine> // QML_ELEMENT / QML_SINGLETON
#include <QTimer>

// glibc-only (not ISO C, not POSIX): malloc_trim. Absent on macOS, where the
// branch compiles out.
#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace rawform {

class HeapTrim : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit HeapTrim(QObject* parent = nullptr) : QObject(parent) {}

    /// Release the heap's free pages to the OS shortly after the caller's
    /// window has been torn down. Coalescing: a second call while one is
    /// pending is absorbed by it.
    Q_INVOKABLE void trimSoon() {
#ifdef __GLIBC__
        if (m_pending) {
            return;
        }
        m_pending = true;
        QTimer::singleShot(kDelayMs, this, [this] {
            m_pending = false;
            malloc_trim(0);
        });
#endif
    }

private:
    // One event-loop pass is what the deferred delete needs; the rest is
    // slack for the platform window's own teardown behind it. Both members
    // are referenced only inside the glibc branch, hence maybe_unused: the
    // non-glibc build would otherwise warn on a private field it never reads.
    [[maybe_unused]] static constexpr int kDelayMs = 100;

    [[maybe_unused]] bool m_pending = false;
};

} // namespace rawform
