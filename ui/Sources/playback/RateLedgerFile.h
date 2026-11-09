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

// RateLedgerFile.h
//
// rate_ledger.yaml, the crash-recovery record of the output devices a sink
// left at a borrowed sample rate. The file exists exactly while at least one
// device owes a return to its original rate, one entry per device:
//
//   debts:
//     - device_id: <persistent device id>
//       original_rate: <Hz to restore>
//       borrowed_rate: <Hz the device was left at>
//
// A file found at launch means the previous session ended without repaying.
// The reader also accepts the single top-level entry (device_id,
// original_rate, borrowed_rate at the root) that older builds wrote.
//
// Free functions with no state and no thread affinity: the controller writes
// the file from the engine thread, where the sink publishes its debts, and
// reads it on the GUI thread before the engine has a listener, so the two
// never overlap.

#pragma once

#include <QString>

#include <cstdint>
#include <string>
#include <vector>

namespace rawform::rateledger {

/// One device's debt. An entry with an empty id or a zero original rate is
/// never stored: neither names something that can be repaid.
struct Entry {
    std::string   deviceId;
    std::uint32_t originalRateHz = 0;
    std::uint32_t borrowedRateHz = 0;

    friend bool operator==(const Entry&, const Entry&) = default;
};

enum class ReadStatus {
    Absent,     ///< no file: nothing is owed
    Ok,         ///< parsed; `entries` holds every usable debt, possibly none
    Malformed,  ///< present but not a ledger; `entries` is empty
};

struct ReadResult {
    ReadStatus         status = ReadStatus::Absent;
    std::vector<Entry> entries;
};

/// Read the ledger at `path`. Entries with an empty id or a zero original
/// rate are skipped rather than failing the read.
[[nodiscard]] ReadResult read(const QString& path);

/// Fold one published change into `entries`: a zero original rate removes the
/// entry for that device, anything else adds it or replaces it. A change with
/// an empty device id is ignored. Returns true when `entries` changed.
bool apply(std::vector<Entry>& entries, const Entry& change);

/// Make the file at `path` say `entries`: written whole and atomically while
/// any debt remains, deleted when none does. Returns false when the file
/// could not be written or deleted; the previous content then stands.
bool write(const QString& path, const std::vector<Entry>& entries);

}  // namespace rawform::rateledger
