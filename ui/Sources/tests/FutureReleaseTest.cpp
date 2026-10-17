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

// FutureReleaseTest.cpp
//
// Standalone unit test for utils/FutureRelease.h: releasing a watcher's
// finished future from inside its own finished handler must drop the result
// store without the watcher ever firing again for the empty future it now
// holds, through any number of event-loop turns, and must leave the watcher
// ready for a real future afterwards.
//
// Same harness shape as the other tests (plain main, counted checks, non-zero
// exit on failure). A QCoreApplication exists because the watcher delivers
// its signals through posted events; the test spins the loop to let any
// stray delivery happen.
//
// Build via the CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_future_test
//     ctest --test-dir build -L ui       # or run ./build/rawform_future_test

#include "utils/FutureRelease.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFuture>
#include <QFutureWatcher>
#include <QList>
#include <QTimer>
#include <QtConcurrent>

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

void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// Run @p count squares through a watcher whose finished handler reads the
/// results and releases the future; count the handler's calls.
struct Harness {
    QFutureWatcher<int> watcher;
    int                 finishedCalls = 0;
    int                 startedCalls  = 0;
    int                 canceledCalls = 0;
    int                 lastSum       = -1;

    Harness() {
        QObject::connect(&watcher, &QFutureWatcher<int>::finished, &watcher, [this]() {
            ++finishedCalls;
            // The handler reads the results, then lets the store go. A second
            // delivery for the released (empty) future would read past the
            // end here, which is what the scanner's handler did.
            const QFuture<int> f = watcher.future();
            int sum = 0;
            for (int i = 0; i < f.resultCount(); ++i) {
                sum += f.resultAt(i);
            }
            lastSum = sum;
            releaseFuture(watcher);
        });
        QObject::connect(&watcher, &QFutureWatcher<int>::started, &watcher,
                         [this]() { ++startedCalls; });
        QObject::connect(&watcher, &QFutureWatcher<int>::canceled, &watcher,
                         [this]() { ++canceledCalls; });
    }

    void run(int count) {
        QList<int> input;
        for (int i = 1; i <= count; ++i) {
            input.append(i);
        }
        watcher.setFuture(QtConcurrent::mapped(input, [](int v) { return v * v; }));
        watcher.waitForFinished();
        spin(50); // deliver the posted signals, plus anything stray after them
    }
};

void testReleaseFromFinishedHandler() {
    Harness h;
    h.run(10);
    check(h.finishedCalls == 1, "release: finished delivered exactly once");
    check(h.lastSum == 385, "release: the handler saw every result before the release");
    check(h.watcher.future().resultCount() == 0, "release: the store is gone afterwards");
    check(h.canceledCalls == 0,
          "release: the empty future's canceled state is not replayed");

    spin(50);
    check(h.finishedCalls == 1, "release: still once after further loop turns");

    // The watcher is reusable: a second pass is delivered exactly once too.
    h.run(4);
    check(h.finishedCalls == 2 && h.lastSum == 30,
          "release: a second pass runs and finishes once");
    check(h.startedCalls == 2,
          "release: started once per real future, never for the empty one");
}

void testReleaseOutsideHandler() {
    // Releasing an idle watcher (the scanner's empty-enumeration route reaches
    // its finish with no scan future at all) is a no-op that fires nothing.
    QFutureWatcher<int> watcher;
    int fired = 0;
    QObject::connect(&watcher, &QFutureWatcher<int>::finished, &watcher,
                     [&]() { ++fired; });
    QObject::connect(&watcher, &QFutureWatcher<int>::started, &watcher,
                     [&]() { ++fired; });
    QObject::connect(&watcher, &QFutureWatcher<int>::canceled, &watcher,
                     [&]() { ++fired; });
    releaseFuture(watcher);
    releaseFuture(watcher);
    spin(50);
    check(fired == 0, "release: an idle watcher released twice fires nothing");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    testReleaseFromFinishedHandler();
    testReleaseOutsideHandler();

    std::printf("FutureReleaseTest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
