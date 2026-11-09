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

#include <cmath>  // std::fabs

namespace rawform::audio {

bool RateLedger::ratesEqual(double a, double b) noexcept {
    return std::fabs(a - b) < 1.0;
}

void RateLedger::beginSession(double currentRate) noexcept {
    m_originalRate = currentRate;
    m_knownRate    = currentRate;
    m_ownChange    = false;
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

}  // namespace rawform::audio
