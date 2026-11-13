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

// PositionNotifyGateTest.cpp
//
// Standalone unit test for playback/PositionNotifyGate.h: unheld, a position
// report is sent at once; held, it is owed until the clock's next flush, which
// sends it exactly once however many reports came in; the live bitrate is owed
// on its own; a release sends what is owed and nothing else; a flush with
// nothing owed sends nothing.
//
// Same harness shape as the other tests (plain main, counted checks, non-zero
// exit on failure). No Qt: the gate is plain C++.
//
// Build via the CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_position_gate_test
//     ctest --test-dir build -L ui       # or run ./build/rawform_position_gate_test

#include "playback/PositionNotifyGate.h"

#include <cstdio>

using rawform::PositionNotifyGate;

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

[[nodiscard]] bool is(PositionNotifyGate::Emit e, bool position, bool bitrate) {
    return e.position == position && e.bitrate == bitrate;
}

void unheldSendsAtOnce() {
    PositionNotifyGate gate;
    check(!gate.held(), "unheld: a new gate is not held");
    check(is(gate.report(false), true, false), "unheld: position goes at once");
    check(is(gate.report(true), true, true), "unheld: bitrate goes with it");
    check(!gate.flush().any(), "unheld: nothing is left owed");
}

void heldOwesUntilFlush() {
    PositionNotifyGate gate;
    gate.hold();
    check(gate.held(), "held: the gate reports held");
    check(!gate.report(false).any(), "held: a report sends nothing");
    check(is(gate.flush(), true, false), "held: the flush sends the position");
    check(!gate.flush().any(), "held: a second flush sends nothing");
    check(gate.held(), "held: a flush keeps the hold");
}

void reportsCoalesce() {
    PositionNotifyGate gate;
    gate.hold();
    check(!gate.report(false).any(), "coalesce: first report held");
    check(!gate.report(false).any(), "coalesce: second report held");
    check(is(gate.flush(), true, false), "coalesce: one flush sends one position");
    check(!gate.flush().any(), "coalesce: nothing left after it");
}

void bitrateOwedOnItsOwn() {
    PositionNotifyGate gate;
    gate.hold();
    check(!gate.report(true).any(), "bitrate: a moved bitrate is held");
    check(!gate.report(false).any(), "bitrate: a later steady report is held");
    check(is(gate.flush(), true, true),
          "bitrate: a steady report does not cancel the owed bitrate");
    check(!gate.report(false).any(), "bitrate: next interval, steady report held");
    check(is(gate.flush(), true, false), "bitrate: steady interval owes no bitrate");
}

void releaseSendsOwed() {
    PositionNotifyGate gate;
    gate.hold();
    check(!gate.report(true).any(), "release: report held");
    check(is(gate.release(), true, true), "release: what is owed goes");
    check(!gate.held(), "release: the gate is no longer held");
    check(is(gate.report(false), true, false), "release: reports go at once again");
}

void releaseWithNothingOwed() {
    PositionNotifyGate gate;
    gate.hold();
    check(!gate.release().any(), "release empty: nothing is sent");

    gate.hold();
    check(!gate.report(false).any(), "release empty: report held");
    check(is(gate.flush(), true, false), "release empty: flushed by the tick");
    check(!gate.release().any(), "release empty: a flushed gate sends nothing");
}

void strayFlushSendsNothing() {
    PositionNotifyGate gate;
    check(!gate.flush().any(), "stray flush: unheld, nothing owed");
    gate.hold();
    check(!gate.flush().any(), "stray flush: held, nothing owed");
}

}  // namespace

int main() {
    unheldSendsAtOnce();
    heldOwesUntilFlush();
    reportsCoalesce();
    bitrateOwedOnItsOwn();
    releaseSendsOwed();
    releaseWithNothingOwed();
    strayFlushSendsNothing();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
