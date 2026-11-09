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

// RateLedger.h
//
// The restore ledger of a sink that borrows a device's nominal rate: the rate
// the device had when the session opened (what the final close restores), the
// rate the sink last left the device at, and whether one of the sink's own
// rate changes is in progress. Pure bookkeeping, with no platform call and no
// lock, so the rules can be tested without a device; the owning sink makes
// every call under its own state mutex, from its engine-thread entry points
// and from its notification threads alike.
//
// Telling the sink's own rate changes from someone else's. A platform reports
// a nominal-rate change through a notification whose timing it does not
// promise: the call that changed the rate can return before the notification
// is delivered, and one change can be delivered more than once. So a
// notification is judged by the rate the device reads when it arrives, never
// by when it arrives:
//
//   - while one of the sink's own changes is in progress, every notification
//     belongs to it; the change's own readback settles where the device ended
//     up. An external change racing it is taken as the sink's, so the final
//     close restores over it: the safe side, since a borrowed rate left behind
//     degrades every other application's audio;
//   - otherwise a notification reading the rate the sink last left the device
//     at is the sink's own, however late it is delivered;
//   - any other rate is someone else asserting ownership of the device: it
//     becomes the session's original and the debt is forgiven.
//
// rawform's CoreAudio sink is the one user; the class itself is portable.

#pragma once

namespace rawform::audio {

class RateLedger {
public:
    /// What a nominal-rate notification turned out to be.
    enum class Notice {
        Own,       ///< the sink's own change, landing late or delivered twice
        External,  ///< someone else moved the device: the debt is forgiven
    };

    /// A fresh session on a device sitting at currentRate (0 when unknown):
    /// no debt, and nothing in progress.
    void beginSession(double currentRate) noexcept;

    /// One of the sink's own rate changes starts. Call before the platform
    /// set, so a notification delivered during the set is already covered.
    void beginOwnChange() noexcept;

    /// The change ended with the device reading nowRate. A failed readback (0)
    /// keeps the last known rate rather than guessing.
    void endOwnChange(double nowRate) noexcept;

    /// Classify a notification whose readback is rate. External adopts the
    /// rate as the new original, which forgives the debt. A rate of 0 (a failed
    /// readback) says nothing and counts as Own.
    [[nodiscard]] Notice notice(double rate) noexcept;

    /// The device is back at the original: the debt is repaid.
    void settle() noexcept;

    /// Whether the device sits off the rate the session opened at.
    [[nodiscard]] bool owes() const noexcept;

    [[nodiscard]] double originalRate() const noexcept { return m_originalRate; }
    [[nodiscard]] double knownRate() const noexcept { return m_knownRate; }
    [[nodiscard]] bool ownChangeInProgress() const noexcept { return m_ownChange; }

    /// Nominal rates are integers in practice; a sub-hertz difference is the
    /// platform's double representation, not a different rate.
    [[nodiscard]] static bool ratesEqual(double a, double b) noexcept;

private:
    double m_originalRate = 0.0;
    double m_knownRate    = 0.0;
    bool   m_ownChange    = false;
};

}  // namespace rawform::audio
