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
// notification, close) and checks what the final close would restore. The
// second half covers the debts a close could not repay: kept per device,
// carried by a later session, settled, and published one device at a time.
// Same hand-rolled CHECK harness as the rest of the suite; main returns
// non-zero on any failure.

#include "sinks/RateLedger.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

using rawform::audio::RateDebtBook;
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

// ---------------------------------------------------------------------------
// Unpaid debts: what a close that missed its restore leaves behind, and how
// the debt is carried, settled and published per device.

using Entry = RateDebtBook::Entry;

Entry debt(const char* id, std::uint32_t original, std::uint32_t borrowed) {
    return Entry{.deviceId = id, .originalHz = original, .borrowedHz = borrowed};
}

// The session's debt as the sink publishes it: present only while it owes.
std::optional<Entry> sessionDebt(const RateLedger& ledger, const char* id) {
    if (!ledger.owes()) {
        return std::nullopt;
    }
    return debt(id, static_cast<std::uint32_t>(ledger.originalRate()),
                static_cast<std::uint32_t>(ledger.knownRate()));
}

// A failed restore moves the debt to the unpaid list: the published entry
// stands, so the move publishes nothing, and in particular no clear.
void testFailedRestoreKeepsDebt() {
    RateLedger   ledger;
    RateDebtBook book;
    ledger.beginSession(96000.0);
    ownChange(ledger, 44100.0);
    const std::vector<Entry> opened = book.publish(sessionDebt(ledger, "hp"));
    CHECK(opened.size() == 1);
    CHECK(!opened.empty() && opened[0] == debt("hp", 96000, 44100));

    // The close's restore did not reach the device: keep the debt, end the
    // session's own claim.
    book.keepUnpaid(debt("hp", 96000, 44100));
    ledger.settle();
    CHECK(book.publish(sessionDebt(ledger, "hp")).empty());
    CHECK(book.unpaid().size() == 1);
}

// A later session on the same device, which comes back at the borrowed rate,
// carries the debt: the original is 96000 again and the session owes it with
// no rate change, still publishing nothing new. A device that comes back at
// another rate makes the debt moot.
void testLaterSessionCarriesDebt() {
    RateLedger   ledger;
    RateDebtBook book;
    book.keepUnpaid(debt("hp", 96000, 44100));
    CHECK(book.publish(std::nullopt).size() == 1);  // the debt as kept

    const std::optional<Entry> carried = book.takeUnpaid("hp");
    CHECK(carried.has_value());
    CHECK(book.unpaid().empty());
    ledger.beginSession(44100.0);
    CHECK(carried && ledger.carry(carried->originalHz, carried->borrowedHz));
    CHECK(ledger.originalRate() == 96000.0);
    CHECK(ledger.owes());
    CHECK(book.publish(sessionDebt(ledger, "hp")).empty());  // same entry stands

    // The session's close repays it: the device's clear.
    ledger.settle();
    const std::vector<Entry> closed = book.publish(sessionDebt(ledger, "hp"));
    CHECK(closed.size() == 1);
    CHECK(!closed.empty() && closed[0] == debt("hp", 0, 0));

    // Back at another rate: the debt is moot, the session owes nothing, and
    // the next publication clears the device.
    RateLedger   moved;
    RateDebtBook book2;
    book2.keepUnpaid(debt("hp", 96000, 44100));
    CHECK(book2.publish(std::nullopt).size() == 1);
    const std::optional<Entry> stale = book2.takeUnpaid("hp");
    moved.beginSession(48000.0);
    CHECK(stale && !moved.carry(stale->originalHz, stale->borrowedHz));
    CHECK(moved.originalRate() == 48000.0);
    CHECK(!moved.owes());
    const std::vector<Entry> cleared = book2.publish(sessionDebt(moved, "hp"));
    CHECK(cleared.size() == 1);
    CHECK(!cleared.empty() && cleared[0] == debt("hp", 0, 0));
}

// The settle's verdicts, and a settle that restores publishes that device's
// clear.
void testSettleRestoresAndClears() {
    const Entry hp = debt("hp", 96000, 44100);
    CHECK(RateDebtBook::settlement(hp, 0.0) == RateDebtBook::Settlement::Keep);
    CHECK(RateDebtBook::settlement(hp, 44100.0) == RateDebtBook::Settlement::Restore);
    CHECK(RateDebtBook::settlement(hp, 48000.0) == RateDebtBook::Settlement::Moot);
    CHECK(RateDebtBook::settlement(hp, 96000.0) == RateDebtBook::Settlement::Moot);

    RateDebtBook book;
    book.keepUnpaid(hp);
    CHECK(book.publish(std::nullopt).size() == 1);
    CHECK(book.takeUnpaid("hp").has_value());  // the restore reached the device
    const std::vector<Entry> events = book.publish(std::nullopt);
    CHECK(events.size() == 1);
    CHECK(!events.empty() && events[0] == debt("hp", 0, 0));
    CHECK(book.publish(std::nullopt).empty());
}

// Two devices: the headphones' unpaid debt and the speakers' live session
// publish and clear independently.
void testTwoDevicesIndependent() {
    RateDebtBook book;
    book.keepUnpaid(debt("hp", 96000, 44100));
    const std::vector<Entry> kept = book.publish(std::nullopt);
    CHECK(kept.size() == 1 && kept[0] == debt("hp", 96000, 44100));

    RateLedger speakers;
    speakers.beginSession(48000.0);
    ownChange(speakers, 88200.0);
    const std::vector<Entry> opened = book.publish(sessionDebt(speakers, "spk"));
    CHECK(opened.size() == 1 && opened[0] == debt("spk", 48000, 88200));

    speakers.settle();  // the speakers' close repaid them
    const std::vector<Entry> closed = book.publish(sessionDebt(speakers, "spk"));
    CHECK(closed.size() == 1 && closed[0] == debt("spk", 0, 0));
    CHECK(book.unpaid().size() == 1);

    CHECK(book.takeUnpaid("hp").has_value());  // the headphones settled
    const std::vector<Entry> settled = book.publish(std::nullopt);
    CHECK(settled.size() == 1 && settled[0] == debt("hp", 0, 0));

    // A repeat keep for the same device replaces its entry.
    book.keepUnpaid(debt("hp", 96000, 44100));
    book.keepUnpaid(debt("hp", 96000, 48000));
    CHECK(book.unpaid().size() == 1);
    CHECK(book.unpaid()[0] == debt("hp", 96000, 48000));
    book.keepUnpaid(debt("", 96000, 48000));  // no device: ignored
    CHECK(book.unpaid().size() == 1);
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
    testFailedRestoreKeepsDebt();
    testLaterSessionCarriesDebt();
    testSettleRestoresAndClears();
    testTwoDevicesIndependent();

    if (g_failures == 0) {
        std::puts("RateLedgerTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "RateLedgerTest: %d check(s) failed\n", g_failures);
    return 1;
}
