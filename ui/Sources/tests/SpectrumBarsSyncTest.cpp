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

// SpectrumBarsSyncTest.cpp
//
// Standalone test for playback/SpectrumBars under a threaded render loop: the
// item's members are written by its setters on the GUI thread and read by
// updatePaintNode on the scene graph render thread, and every read must be one
// a ThreadSanitizer runtime can order after the write. The plain build checks
// that the item is synced off the GUI thread, frame after frame; the sanitizer
// profile is where a handoff the runtime cannot see fails the test (the
// runtime exits non-zero on a report).
//
// THE LOOP. The test selects its own platform and renderer, so it runs the
// same way from ctest, the gate and CI: the offscreen platform, the software
// scene graph backend, and QSG_RENDER_LOOP=threaded, which gives that backend
// a render thread with the sync choreography of the hardware loop (the GUI
// thread blocks while the render thread calls updatePaintNode).
//
// THE PACING. Each new list is handed over as soon as the previous sync
// returns, while the render thread is still busy with that frame (the software
// loop holds each frame for the screen's refresh interval). The render thread
// then picks the next sync up without having slept, which is the case where
// only Qt's own QMutex orders the two threads: the one a runtime that cannot
// see into an uninstrumented Qt reports.
//
// Same harness shape as the other tests (plain main, counted checks, non-zero
// exit on failure).
//
// Build via the CMake switch:
//     cmake -S ui -B build/app -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build/app --target rawform_spectrum_bars_sync_test
//     ctest --test-dir build/app -L ui

#include "playback/SpectrumBars.h"

#include <QGuiApplication>
#include <QList>
#include <QQuickWindow>
#include <QSizeF>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cmath>
#include <cstdio>

using namespace rawform;

namespace {

int g_checks   = 0;
int g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

constexpr int kFrames    = 600;
constexpr int kBars      = 20;
constexpr int kTimeoutMs = 120000;

/// SpectrumBars with its paint-node updates counted by the thread they ran on.
class CountingBars : public SpectrumBars {
public:
    using SpectrumBars::SpectrumBars;

    std::atomic<int> renderThreadUpdates{0};
    std::atomic<int> guiThreadUpdates{0};

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override {
        if (QThread::isMainThread()) {
            ++guiThreadUpdates;
        } else {
            ++renderThreadUpdates;
        }
        return SpectrumBars::updatePaintNode(oldNode, data);
    }
};

// ---------------------------------------------------------------------------
void testValuesCrossToTheRenderThread(QGuiApplication& app) {
    QQuickWindow window;
    window.resize(120, 40);

    auto* bars = new CountingBars(window.contentItem());
    bars->setSize(QSizeF(120.0, 40.0));
    bars->setCount(kBars);

    // The provider's pattern: one list, rewritten in place on every tick and
    // then handed to the item, which shares its buffer until the next rewrite
    // detaches it.
    QList<qreal> levels(kBars, 0.0);
    int          ticks    = 0;
    int          swapped  = 0;
    bool         timedOut = false;

    const auto tick = [&]() {
        if (swapped >= kFrames) {
            return;
        }
        for (int i = 0; i < levels.size(); ++i) {
            levels[i] = 0.5 + 0.5 * std::sin(0.1 * static_cast<double>(ticks + i));
        }
        ++ticks;
        bars->setValues(levels);
    };

    // Both signals are emitted on the render thread; the context object brings
    // the handlers back to the GUI thread. A sync that has just returned is
    // the moment the next list goes in.
    QObject::connect(&window, &QQuickWindow::afterSynchronizing, &app, tick);
    QObject::connect(&window, &QQuickWindow::frameSwapped, &app, [&]() {
        if (++swapped == kFrames) {
            app.quit();
        }
    });

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, &app, [&]() {
        timedOut = true;
        app.quit();
    });
    watchdog.start(kTimeoutMs);

    window.show();
    app.exec();

    check(!timedOut, "sync: the frames arrive before the watchdog");
    check(swapped >= kFrames, "sync: every frame is swapped");
    check(ticks >= kFrames, "sync: every frame follows a new list");
    check(bars->renderThreadUpdates.load() >= kFrames,
          "sync: updatePaintNode runs on the render thread for every frame");
    check(bars->guiThreadUpdates.load() == 0,
          "sync: updatePaintNode never runs on the GUI thread");
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QSG_RENDER_LOOP", "threaded");
    QQuickWindow::setSceneGraphBackend(QStringLiteral("software"));

    QGuiApplication app(argc, argv);

    testValuesCrossToTheRenderThread(app);

    std::printf("SpectrumBarsSyncTest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
