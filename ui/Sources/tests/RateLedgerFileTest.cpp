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

// RateLedgerFileTest.cpp
//
// rate_ledger.yaml through real files in a QTemporaryDir: the per-device
// changes a sink publishes folded into the entry list, the list written and
// read back, the single top-level entry older builds wrote, malformed files,
// and the file deleted once the last debt is cleared. Same harness shape as
// the other ui tests (plain main, counted checks, non-zero exit on failure).

#include "playback/RateLedgerFile.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QString>
#include <QTemporaryDir>

#include <cstdint>
#include <cstdio>
#include <vector>

using rawform::rateledger::Entry;
using rawform::rateledger::ReadStatus;
namespace rateledger = rawform::rateledger;

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

Entry debt(const char* id, std::uint32_t original, std::uint32_t borrowed) {
    return Entry{.deviceId = id, .originalRateHz = original, .borrowedRateHz = borrowed};
}

bool writeRaw(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return f.write(bytes) == bytes.size();
}

// Changes from two devices fold into one entry each: a repeat is no change,
// a new borrowed rate replaces the entry, a clear removes only that device,
// and a clear for a device with no entry, or a change with no device, is
// ignored.
void testApplyIsPerDevice() {
    std::vector<Entry> entries;
    CHECK(rateledger::apply(entries, debt("dev-a", 96000, 44100)));
    CHECK(!rateledger::apply(entries, debt("dev-a", 96000, 44100)));
    CHECK(rateledger::apply(entries, debt("dev-b", 44100, 48000)));
    CHECK(entries.size() == 2);

    CHECK(rateledger::apply(entries, debt("dev-a", 96000, 48000)));
    CHECK(entries.size() == 2);
    CHECK(entries[0] == debt("dev-a", 96000, 48000));

    CHECK(rateledger::apply(entries, debt("dev-a", 0, 0)));
    CHECK(entries.size() == 1);
    CHECK(entries[0] == debt("dev-b", 44100, 48000));

    CHECK(!rateledger::apply(entries, debt("dev-a", 0, 0)));
    CHECK(!rateledger::apply(entries, debt("", 96000, 44100)));
    CHECK(!rateledger::apply(entries, debt("", 0, 0)));
    CHECK(entries.size() == 1);
}

// What is written reads back equal, in order; device ids keep characters
// YAML would otherwise interpret.
void testRoundTrip(const QString& dir) {
    const QString path = dir + QStringLiteral("/round/rate_ledger.yaml");
    const std::vector<Entry> written{
        debt("AppleUSBAudioEngine:Vendor:Headphones:1234:1", 96000, 44100),
        debt("BuiltInSpeakerDevice", 48000, 88200),
        debt("- [weird] #id", 44100, 192000),
    };
    CHECK(rateledger::write(path, written));  // creates the missing directory
    const rateledger::ReadResult back = rateledger::read(path);
    CHECK(back.status == ReadStatus::Ok);
    CHECK(back.entries == written);
}

// The last clear deletes the file; with no file, reading says Absent and
// writing nothing succeeds.
void testEmptyDeletesFile(const QString& dir) {
    const QString path = dir + QStringLiteral("/empty/rate_ledger.yaml");
    std::vector<Entry> entries;
    CHECK(rateledger::apply(entries, debt("dev-a", 96000, 44100)));
    CHECK(rateledger::write(path, entries));
    CHECK(QFile::exists(path));

    CHECK(rateledger::apply(entries, debt("dev-a", 0, 0)));
    CHECK(rateledger::write(path, entries));
    CHECK(!QFile::exists(path));
    CHECK(rateledger::read(path).status == ReadStatus::Absent);
    CHECK(rateledger::write(path, entries));
}

// The single top-level entry an older build wrote reads as one debt.
void testLegacySingleEntry(const QString& dir) {
    const QString path = dir + QStringLiteral("/legacy.yaml");
    CHECK(writeRaw(path, "# rawform rate ledger.\n"
                         "device_id: AppleUSBAudioEngine:Headphones\n"
                         "original_rate: 96000\n"
                         "borrowed_rate: 44100\n"));
    const rateledger::ReadResult back = rateledger::read(path);
    CHECK(back.status == ReadStatus::Ok);
    CHECK(back.entries.size() == 1);
    if (back.entries.size() == 1) {
        CHECK(back.entries[0] == debt("AppleUSBAudioEngine:Headphones", 96000, 44100));
    }
}

// Entries that name nothing repayable are skipped, a repeated device keeps
// its last entry, and a file that is not a ledger at all reads as Malformed
// with no entries.
void testUnusableAndMalformed(const QString& dir) {
    const QString partial = dir + QStringLiteral("/partial.yaml");
    CHECK(writeRaw(partial, "debts:\n"
                            "  - device_id: dev-a\n"
                            "    original_rate: 96000\n"
                            "    borrowed_rate: 44100\n"
                            "  - device_id: \"\"\n"
                            "    original_rate: 48000\n"
                            "  - device_id: dev-b\n"
                            "    original_rate: 0\n"
                            "  - just a string\n"
                            "  - device_id: dev-a\n"
                            "    original_rate: 96000\n"
                            "    borrowed_rate: 48000\n"));
    const rateledger::ReadResult back = rateledger::read(partial);
    CHECK(back.status == ReadStatus::Ok);
    CHECK(back.entries.size() == 1);
    if (back.entries.size() == 1) {
        CHECK(back.entries[0] == debt("dev-a", 96000, 48000));
    }

    const std::vector<QByteArray> malformed{
        "debts: 12\n",                                         // not a list
        "- device_id: dev-a\n",                                // root is a list
        "debts:\n  - device_id: dev-a\n    original_rate: fast\n",  // not a number
        "device_id: [unterminated\n",                          // not YAML
    };
    int n = 0;
    for (const QByteArray& bytes : malformed) {
        const QString path = dir + QStringLiteral("/malformed-%1.yaml").arg(n++);
        CHECK(writeRaw(path, bytes));
        const rateledger::ReadResult r = rateledger::read(path);
        CHECK(r.status == ReadStatus::Malformed);
        CHECK(r.entries.empty());
    }
}

}  // namespace

int main() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    if (!tmp.isValid()) {
        std::fprintf(stderr, "RateLedgerFileTest: no temporary directory\n");
        return 1;
    }

    testApplyIsPerDevice();
    testRoundTrip(tmp.path());
    testEmptyDeletesFile(tmp.path());
    testLegacySingleEntry(tmp.path());
    testUnusableAndMalformed(tmp.path());

    if (g_failures == 0) {
        std::puts("RateLedgerFileTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "RateLedgerFileTest: %d check(s) failed\n", g_failures);
    return 1;
}
