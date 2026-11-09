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

// RateLedger.cpp
//
// The rules are in the header; this file only applies them.

#include "sinks/RateLedger.h"

#include <algorithm>
#include <cmath>  // std::fabs
#include <utility>

namespace rawform::audio {

bool RateLedger::ratesEqual(double a, double b) noexcept {
    return std::fabs(a - b) < 1.0;
}

void RateLedger::beginSession(double currentRate) noexcept {
    m_originalRate = currentRate;
    m_knownRate    = currentRate;
    m_ownChange    = false;
}

bool RateLedger::carry(double originalRate, double borrowedRate) noexcept {
    if (originalRate <= 0.0 || m_knownRate <= 0.0 ||
        !ratesEqual(m_knownRate, borrowedRate)) {
        return false;
    }
    m_originalRate = originalRate;
    return true;
}

void RateLedger::beginOwnChange() noexcept {
    m_ownChange = true;
}

void RateLedger::endOwnChange(double nowRate) noexcept {
    m_ownChange = false;
    if (nowRate > 0.0) {
        m_knownRate = nowRate;
    }
}

RateLedger::Notice RateLedger::notice(double rate) noexcept {
    if (rate <= 0.0 || m_ownChange || ratesEqual(rate, m_knownRate)) {
        return Notice::Own;
    }
    m_originalRate = rate;
    m_knownRate    = rate;
    return Notice::External;
}

void RateLedger::settle() noexcept {
    m_knownRate = m_originalRate;
}

bool RateLedger::owes() const noexcept {
    return m_originalRate > 0.0 && m_knownRate > 0.0 &&
           !ratesEqual(m_knownRate, m_originalRate);
}

// ---------------------------------------------------------------------------
RateDebtBook::Settlement RateDebtBook::settlement(const Entry& debt,
                                                  double currentRate) noexcept {
    if (currentRate <= 0.0) {
        return Settlement::Keep;
    }
    return RateLedger::ratesEqual(currentRate, static_cast<double>(debt.borrowedHz))
               ? Settlement::Restore
               : Settlement::Moot;
}

void RateDebtBook::keepUnpaid(const Entry& debt) {
    if (debt.deviceId.empty() || debt.originalHz == 0) {
        return;
    }
    const auto it = std::ranges::find(m_unpaid, debt.deviceId, &Entry::deviceId);
    if (it != m_unpaid.end()) {
        *it = debt;
    } else {
        m_unpaid.push_back(debt);
    }
}

std::optional<RateDebtBook::Entry> RateDebtBook::takeUnpaid(const std::string& deviceId) {
    const auto it = std::ranges::find(m_unpaid, deviceId, &Entry::deviceId);
    if (it == m_unpaid.end()) {
        return std::nullopt;
    }
    Entry debt = std::move(*it);
    m_unpaid.erase(it);
    return debt;
}

std::vector<RateDebtBook::Entry> RateDebtBook::publish(const std::optional<Entry>& session) {
    // The standing set: the unpaid debts, with the live session's debt taking
    // its device's place (a session never runs on a device with an unpaid
    // debt, since open() takes it, but the session is the fresher truth).
    std::vector<Entry> standing = m_unpaid;
    if (session && !session->deviceId.empty() && session->originalHz != 0) {
        const auto it = std::ranges::find(standing, session->deviceId, &Entry::deviceId);
        if (it != standing.end()) {
            *it = *session;
        } else {
            standing.push_back(*session);
        }
    }

    std::vector<Entry> events;
    for (const Entry& was : m_published) {
        if (std::ranges::find(standing, was.deviceId, &Entry::deviceId) == standing.end()) {
            events.push_back(Entry{.deviceId = was.deviceId, .originalHz = 0, .borrowedHz = 0});
        }
    }
    for (const Entry& now : standing) {
        const auto it = std::ranges::find(m_published, now.deviceId, &Entry::deviceId);
        if (it == m_published.end() || *it != now) {
            events.push_back(now);
        }
    }
    m_published = std::move(standing);
    return events;
}

}  // namespace rawform::audio
