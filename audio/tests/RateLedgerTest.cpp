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

// RateLedgerTest.cpp
//
// The restore ledger's rules, driven in the orders a platform can deliver its
// nominal-rate notifications, with no device: a notification for the sink's
// own change delivered after the change returned, delivered during it, and
// delivered twice, against a change made by someone else. Each case plays the
// calls the CoreAudio sink makes (session open, own change begun and ended,
// notification, close) and checks what the final close would restore. Same
// hand-rolled CHECK harness as the rest of the suite; main returns non-zero on
// any failure.

#include "sinks/RateLedger.h"

#include <cstdio>

using rawform::audio::RateLedger;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,         \
                         __FILE__, __LINE__);                                  \
        }                                                                      \
    } while (0)

// One own change from start to readback, with no notification in between.
void ownChange(RateLedger& ledger, double reached) {
    ledger.beginOwnChange();
    ledger.endOwnChange(reached);
}

// A mixed-rate playlist on a device that opened at 96000: the switch to 48000,
// then to 44100, whose notification arrives only after the change returned.
// It is the sink's own, so the original stays 96000 and the close restores it.
void testLateOwnNotificationIsOwn() {
    RateLedger ledger;
    ledger.beginSession(96000.0);
    ownChange(ledger, 48000.0);
    ownChange(ledger, 44100.0);

    CHECK(ledger.notice(44100.0) == RateLedger::Notice::Own);
    CHECK(ledger.originalRate() == 96000.0);
    CHECK(ledger.knownRate() == 44100.0);
    CHECK(ledger.owes());

    // The next track's switch is recognized the same way.
    ownChange(ledger, 96000.0);
    CHECK(ledger.notice(96000.0) == RateLedger::Notice::Own);
    CHECK(ledger.originalRate() == 96000.0);
    CHECK(!ledger.owes());  // back on the original: nothing to restore
}

// Notifications delivered while the change is still in progress, reading the
// old rate, an intermediate one, and the target: all the sink's own. The
// readback at the end decides where the device is.
void testNotificationDuringOwnChangeIsOwn() {
    RateLedger ledger;
    ledger.beginSession(96000.0);
    ledger.beginOwnChange();
    CHECK(ledger.ownChangeInProgress());
    CHECK(ledger.notice(96000.0) == RateLedger::Notice::Own);
    CHECK(ledger.notice(88200.0) == RateLedger::Notice::Own);
    CHECK(ledger.notice(44100.0) == RateLedger::Notice::Own);
    ledger.endOwnChange(44100.0);
    CHECK(!ledger.ownChangeInProgress());
    CHECK(ledger.originalRate() == 96000.0);
    CHECK(ledger.knownRate() == 44100.0);
    CHECK(ledger.owes());
}

// Someone else moves the device with no change of the sink's in progress: the
// new rate becomes the original and the debt is forgiven, so the close
// restores nothing.
void testForeignRateIsExternalAndForgives() {
    RateLedger ledger;
    ledger.beginSession(96000.0);
    ownChange(ledger, 44100.0);
    CHECK(ledger.owes());

    CHECK(ledger.notice(48000.0) == RateLedger::Notice::External);
    CHECK(ledger.originalRate() == 48000.0);
    CHECK(ledger.knownRate() == 48000.0);
    CHECK(!ledger.owes());

    // A later own switch is a debt against the adopted rate.
    ownChange(ledger, 44100.0);
    CHECK(ledger.originalRate() == 48000.0);
    CHECK(ledger.owes());
}

// The same notification delivered twice after the change: both are the
// sink's own, and neither moves the original.
void testDuplicateOwnNotificationIsOwn() {
    RateLedger ledger;
    ledger.beginSession(96000.0);
    ownChange(ledger, 48000.0);
    ownChange(ledger, 44100.0);
    CHECK(ledger.notice(44100.0) == RateLedger::Notice::Own);
    CHECK(ledger.notice(44100.0) == RateLedger::Notice::Own);
    CHECK(ledger.originalRate() == 96000.0);
    CHECK(ledger.owes());
}

// A change that lands on a third rate is a debt too, and a failed readback
// keeps the last known rate instead of dropping the debt.
void testThirdRateAndFailedReadback() {
    RateLedger ledger;
    ledger.beginSession(96000.0);
    ownChange(ledger, 48000.0);  // asked for another rate, the device chose 48000
    CHECK(ledger.owes());
    ownChange(ledger, 0.0);      // the readback failed
    CHECK(ledger.knownRate() == 48000.0);
    CHECK(ledger.owes());
    CHECK(ledger.notice(0.0) == RateLedger::Notice::Own);
    CHECK(ledger.originalRate() == 96000.0);
}

// No switch at all: nothing owed. A repaid debt: nothing owed, and the device
// reads its original again.
void testNoDebtAndSettle() {
    RateLedger ledger;
    ledger.beginSession(44100.0);
    CHECK(!ledger.owes());

    ownChange(ledger, 96000.0);
    CHECK(ledger.owes());
    ledger.settle();
    CHECK(!ledger.owes());
    CHECK(ledger.knownRate() == 44100.0);

    // An unknown starting rate never produces a debt to restore.
    ledger.beginSession(0.0);
    ownChange(ledger, 48000.0);
    CHECK(!ledger.owes());
}

// Sub-hertz differences are the same rate.
void testRatesEqualTolerance() {
    CHECK(RateLedger::ratesEqual(44100.0, 44100.4));
    CHECK(!RateLedger::ratesEqual(44100.0, 44101.0));
}

}  // namespace

int main() {
    testLateOwnNotificationIsOwn();
    testNotificationDuringOwnChangeIsOwn();
    testForeignRateIsExternalAndForgives();
    testDuplicateOwnNotificationIsOwn();
    testThirdRateAndFailedReadback();
    testNoDebtAndSettle();
    testRatesEqualTolerance();

    if (g_failures == 0) {
        std::puts("RateLedgerTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "RateLedgerTest: %d check(s) failed\n", g_failures);
    return 1;
}
