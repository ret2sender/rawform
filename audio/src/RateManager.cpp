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

// RateManager.cpp
//
// The portable rate policy implementation. All of the decision lives in
// decide(), a pure function: same inputs, same RateDecision, no device access,
// no allocation, no global state. The device read happens once, in the Engine,
// through IAudioSink::capabilities(); the result is handed here as
// SinkCapabilities, so this file never knows whether it is reasoning about a
// CoreAudio device, a PipeWire node, or a NullSink fake. That is the whole point
// of the policy/mechanism split, and it is what lets RateManagerTest assert the full truth
// table with no audio hardware in sight.
//
// The decision is two questions answered in order:
//   1. What rate should the device run at, and does reaching it require a switch?
//      This is the mode-dependent part (the truth table proper).
//   2. Relative to what is currently open, does that amount to a device
//      reconfigure? This is the mode-independent gapless gate, computed from the
//      resolved device rate plus the source channel count.
//
// NEAREST-BEST FALLBACK.
// Under BitPerfectWhenAvailable the exact source rate is the goal, but
// "wherever the device is parked" is the wrong answer when the exact rate is
// out of reach: a 192000 track on a device that tops out at 96000 but is
// idling at 44100 would stay at 44100, discarding an octave of bandwidth the
// hardware could have kept. So when the exact rate is unreachable but the
// device can switch, the policy picks the best advertised rate instead of
// surrendering to the standing one. Selection order, first hit wins:
//
//   1. FAMILY DESCENT: the largest integer divisor of the source rate the
//      device advertises (divisors 2, 4, 8, 16, so 192000 -> 96000 -> 48000...,
//      176400 -> 88200 -> 44100...). Tried first because it stays inside the
//      source's clock family (the 44100 multiples vs the 48000 multiples) and
//      the OS resampler then works a clean 1/N ratio with Nyquist at exactly
//      half per step, and because it makes the outcome predictable: a hi-res
//      track always lands on its own family's ladder when the device offers it.
//   2. FAMILY ASCENT: the smallest integer multiple of the source rate the
//      device advertises (multipliers 2, 4, 8, so a 22050 source on a device
//      without 22050 lands on 44100, not 48000). Upsampling preserves the whole
//      source band, so any advertised multiple is lossless in bandwidth terms;
//      the smallest is preferred only to keep the device rate sane.
//   3. HIGHEST BELOW: the highest advertised rate strictly below the source,
//      family or not (the cross-family last resort: a 176400 source on a device
//      advertising only 48000-family rates falls to the top of that ladder).
//   4. LOWEST ABOVE: the lowest advertised rate strictly above the source, for
//      the inverse corner (a low-rate source on a device whose floor is above
//      it).
//   5. Nothing advertised at all, or the device cannot switch: stay at the
//      current rate and let the OS path resample.
//
// The known judgment call in this order: family descent CAN pick a lower rate
// than a cross-family alternative (a 176400 source on a device advertising
// {44100, 96000} lands on 44100, not 96000). Real devices advertise both
// families symmetrically, so the case is nearly theoretical, and predictability
// plus the clean-ratio property won the argument.
//
// A fallback is never reported as bit-perfect: deviceRate differs from the
// source rate, so resampleNeeded is true, honestly. And because the fallback is
// a pure function of (source rate, capabilities), two consecutive tracks at the
// same source rate resolve to the same device rate, so the keepSink gapless
// gate keeps working across fallback transitions unchanged.

#include "rawform/audio/RateManager.h"

#include "rawform/audio/Types.h"

#include <cstdint>

namespace rawform::audio {

namespace {

// ---------------------------------------------------------------------------
// Nearest-best fallback selection (see the file header for the order and its
// rationale). Each helper returns 0 for "no candidate", which is safe as a
// sentinel because 0 is never a valid nominal rate. All of them are pure,
// allocation-free scans over the capability spans, keeping decide() trivially
// unit-testable and safe on the engine thread.

// Step 1: family descent. Smallest divisor first, so the HIGHEST advertised
// family rate wins (192000 prefers 96000 over 48000). The srcRate % d guard
// keeps the candidate an exact family member; a rate that does not divide
// cleanly is not "the same music clock, halved", it is just a different rate,
// and those are the cross-family steps' business.
std::uint32_t familyDescent(std::uint32_t srcRate, const SinkCapabilities& caps) noexcept {
    for (std::uint32_t d : {2u, 4u, 8u, 16u}) {
        if (srcRate % d != 0) {
            continue;
        }
        const std::uint32_t cand = srcRate / d;
        if (caps.advertises(cand)) {
            return cand;
        }
    }
    return 0;
}

// Step 2: family ascent. Smallest multiplier first, so the LOWEST advertised
// family rate wins (22050 prefers 44100 over 88200); any multiple already
// preserves the full source band, so going higher buys nothing. The overflow
// guard is academic for real audio rates but keeps the arithmetic defined.
std::uint32_t familyAscent(std::uint32_t srcRate, const SinkCapabilities& caps) noexcept {
    for (std::uint32_t m : {2u, 4u, 8u}) {
        if (srcRate > (0xFFFFFFFFu / m)) {
            break;  // srcRate * m would overflow; larger multipliers only worse
        }
        const std::uint32_t cand = srcRate * m;
        if (caps.advertises(cand)) {
            return cand;
        }
    }
    return 0;
}

// Step 3: highest advertised rate strictly below the source. Span arithmetic
// note: this only runs after advertises(srcRate) came back false, so no span
// covers the source; every span therefore lies ENTIRELY below it (max < src) or
// entirely above it (min > src), and the highest rate below the source is
// simply the largest below-lying span maximum. No span is ever truncated at the
// source rate, because a span reaching the source rate would have covered it.
std::uint32_t highestBelow(std::uint32_t srcRate, const SinkCapabilities& caps) noexcept {
    std::uint32_t best = 0;
    for (const RateRange& r : caps.rates) {
        if (r.max < srcRate && r.max > best) {
            best = r.max;
        }
    }
    return best;
}

// Step 4: lowest advertised rate strictly above the source. The mirror of step
// 3, over the above-lying span minimums.
std::uint32_t lowestAbove(std::uint32_t srcRate, const SinkCapabilities& caps) noexcept {
    std::uint32_t best = 0;
    for (const RateRange& r : caps.rates) {
        if (r.min > srcRate && (best == 0 || r.min < best)) {
            best = r.min;
        }
    }
    return best;
}

// The full ladder, first hit wins; 0 means "no advertised rate at all", which
// sends the caller to the stay-at-current-rate branch.
std::uint32_t bestFallbackRate(std::uint32_t srcRate, const SinkCapabilities& caps) noexcept {
    if (const std::uint32_t r = familyDescent(srcRate, caps)) {
        return r;
    }
    if (const std::uint32_t r = familyAscent(srcRate, caps)) {
        return r;
    }
    if (const std::uint32_t r = highestBelow(srcRate, caps)) {
        return r;
    }
    if (const std::uint32_t r = lowestAbove(srcRate, caps)) {
        return r;
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Stateless by design; see the class comment in RateManager.h
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RateDecision RateManager::decide(const AudioFormat&      source,
                                 const SinkCapabilities& caps,
                                 RateMode                mode,
                                 const AudioFormat&      currentOpen) const noexcept {
    const std::uint32_t srcRate = source.sampleRate;
    const std::uint32_t curRate = caps.currentRate;

    // -----------------------------------------------------------------------
    // Question 1: target device rate and whether a switch is required.
    //
    // The default for every "do not switch" branch is to leave the device where
    // it is (deviceRate = curRate). The exception is a degenerate capabilities
    // read where the current rate came back as 0 (the sink could not read it):
    // rather than emit a nonsensical 0 Hz target, fall back to the source rate so
    // the decision is at least coherent; the sink's measured outcome corrects any
    // mismatch regardless.
    std::uint32_t deviceRate   = curRate;
    bool          switchDevice = false;

    switch (mode) {
        case RateMode::AlwaysResample:
            // Never touch the device. Speed is always correct because the OS path
            // matches the source to the device's standing rate.
            deviceRate   = curRate;
            switchDevice = false;
            break;

        case RateMode::BitPerfectWhenAvailable:
            // Exact match first: switch to the source rate if the device both
            // permits a switch and advertises that exact rate. Failing that, and
            // still only on a switchable device, take the nearest-best advertised
            // fallback (the ladder at the top of this file) rather than
            // surrendering to wherever the device is parked. Only a non-switchable
            // device, or one advertising nothing, stays at its current rate for
            // the OS path to resample against.
            if (caps.canSwitchRate && caps.advertises(srcRate)) {
                deviceRate   = srcRate;
                switchDevice = (srcRate != curRate);
            } else if (caps.canSwitchRate) {
                const std::uint32_t fallback = bestFallbackRate(srcRate, caps);
                if (fallback != 0) {
                    // The fallback may coincide with the standing rate (a 96000
                    // source on a device already parked at 48000); the
                    // switchDevice comparison then skips the no-op exactly as
                    // the exact-match branch does.
                    deviceRate   = fallback;
                    switchDevice = (fallback != curRate);
                } else {
                    deviceRate   = curRate;
                    switchDevice = false;
                }
            } else {
                deviceRate   = curRate;
                switchDevice = false;
            }
            break;

        case RateMode::ForceDeviceRate:
            // Switch to the source rate regardless of the advertised list, still
            // gated on the device permitting a switch at all (a device that
            // refuses nominal-rate changes cannot be forced; fall back to leaving
            // it and resampling). Diagnostic mode: the policy predicts no resample
            // here, and the sink's measured bitPerfect() reports whether the
            // hardware actually honored the forced rate.
            if (caps.canSwitchRate) {
                deviceRate   = srcRate;
                switchDevice = (srcRate != curRate);
            } else {
                deviceRate   = curRate;
                switchDevice = false;
            }
            break;
    }

    // Degenerate-read safety net described above: never report a 0 Hz target.
    if (deviceRate == 0) {
        deviceRate = srcRate;
    }

    // Predicted outcome: a device rate that differs from the source rate means
    // the OS path resamples (not bit-perfect, in the planning sense).
    const bool resampleNeeded = (deviceRate != srcRate);

    // -----------------------------------------------------------------------
    // Question 2: the gapless gate. Compare the resolved device config (the chosen
    // device rate plus the source channel count, which is what the sink's input
    // scope carries) against what the engine is transitioning from. An invalid
    // currentOpen means a fresh start from a closed device, which is always a
    // reconfigure. A channel-count change is always a reconfigure too, since the
    // device stream's channel layout cannot change underneath a running sink.
    bool needsReconfigure = true;
    if (currentOpen.isValid()) {
        needsReconfigure = (currentOpen.sampleRate != deviceRate) ||
                           (currentOpen.channels != source.channels);
    }

    RateDecision d;
    d.deviceRate             = deviceRate;
    d.switchDevice           = switchDevice;
    d.resampleNeeded         = resampleNeeded;
    d.needsDeviceReconfigure = needsReconfigure;
    d.keepSink               = !needsReconfigure;
    return d;
}

}  // namespace rawform::audio
