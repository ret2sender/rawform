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

// RateManager.h
//
// The portable rate policy for the rawform audio engine: zero platform code, no
// codec headers, only the shared value types, the standard containers, and the
// decision logic. The split it enforces is policy versus mechanism. The Engine
// owns the policy and consults it at every track boundary; each platform sink
// is a mechanism that executes the resulting decision and measures the true
// hardware outcome; and because the policy touches no device, RateManagerTest
// can assert its full truth table with no audio hardware in the process.
// CoreAudioSink and PipeWireSink share this file verbatim and write no policy
// of their own.
//
// The division of labor worth stating up front, because it is the source of two
// fields that look redundant but are not:
//   - This header PREDICTS the outcome (RateDecision::resampleNeeded is the
//     planned result of the policy: will the OS device path resample).
//   - The sink MEASURES the outcome after open (IAudioSink::measuredDeviceRateHz:
//     the AUHAL output scope on CoreAudio, the io position clock on PipeWire).
// They usually agree, but real hardware can accept a rate set, report success
// at every layer, and still misclock, so the measured value is the authority
// for "was this really bit-perfect" and the predicted value is the authority
// for "what did the policy choose to attempt". The Engine logs the prediction;
// the sink logs the measurement; the Engine publishes measurement-first.
//
// Pipeline invariant this policy serves, restated: the device follows the track
// at its native rate (bit-perfect) whenever the device can clock that rate, and
// audio is never resampled to fit the device on the primary path. There is no
// engine-owned sample-rate converter; when bit-perfect is not reachable the
// policy picks the best advertised rate the device can switch to, or leaves the
// device at its current rate when it cannot switch, and the OS path resamples,
// flagged here so the outcome is explicit rather than silent.

#pragma once

#include "rawform/audio/Types.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace rawform::audio {

// ---------------------------------------------------------------------------
/// How the policy reconciles a source rate with the output device. A portable
/// policy enum, not a platform concept: both platform sinks execute the
/// identical three modes, the Engine owns the choice, and the CLI maps its
/// flags to these. Nothing reads global or environment state.
enum class RateMode : std::uint8_t {
    /// Default. If the device advertises the source rate AND permits a switch,
    /// change the device to the source rate and play natively (bit-perfect),
    /// restoring the original rate when the listening session closes. When the
    /// exact rate is out of reach but the device can switch, fall back to the
    /// NEAREST-BEST advertised rate (selection order documented at the top of
    /// the .cpp): same rate family first (192000 falls to 96000, not to
    /// whatever the device happens to be parked at), then the closest
    /// cross-family neighbor. Only when the device cannot switch, or
    /// advertises nothing usable, is it left at its current rate for the OS
    /// path to resample against. The advertised-list gate is what makes all of
    /// this safe on every device: a rate the device does not list is never
    /// set, only fallen back FROM.
    BitPerfectWhenAvailable,
    /// Never change the device rate; always run at whatever the device is
    /// currently clocking and let the OS path match the source to it. For
    /// leaving the device untouched.
    AlwaysResample,
    /// Switch the device to the source rate regardless of the advertised list,
    /// restoring when the session closes. Diagnostic only: on a device that
    /// cannot truly clock the rate this produces stretched, garbled output,
    /// which is exactly what it exists to demonstrate. Still gated on the
    /// device permitting a switch at all, since a device that refuses a
    /// nominal-rate change cannot be forced.
    ForceDeviceRate,
};

// ---------------------------------------------------------------------------
/// One advertised nominal-rate span of an output device. CoreAudio reports rates
/// as ranges (AudioValueRange), and built-in Mac outputs really do advertise
/// continuous spans, so a bare list of discrete rates would silently lose that;
/// this portable pair preserves it without dragging any CoreAudio type into the
/// policy. For the common discrete-rate DAC, min == max. Trivially copyable.
struct RateRange {
    std::uint32_t min = 0;
    std::uint32_t max = 0;

    /// True if `rate` lies within [min, max]. Both endpoints count, so a
    /// discrete-rate device expressed as {r, r} covers r.
    [[nodiscard]] constexpr bool covers(std::uint32_t rate) const noexcept {
        return rate >= min && rate <= max;
    }
};

// ---------------------------------------------------------------------------
/// What the output device can do, read fresh per decision through
/// IAudioSink::capabilities(). Unlike the RT-path value types this is NOT
/// trivially copyable (it owns a vector and a string), which is fine: it never
/// travels the RT path, only the engine-thread query at each track boundary,
/// exactly like TrackInfo. The shape is a struct rather than a bare vector
/// because the decision needs three things, not one: the advertised set, the
/// device's CURRENT rate (so the resample branch can name the rate the device
/// will keep running at), and whether the device permits a nominal-rate change
/// at all. deviceName rides along only for diagnostics: the Engine's decision
/// log names the device so a logged outcome is attributable across a device
/// change; the policy itself ignores it.
struct SinkCapabilities {
    std::string            deviceName;          ///< for logging; ignored by the policy
    std::vector<RateRange> rates;               ///< advertised nominal-rate spans
    std::uint32_t          currentRate   = 0;   ///< the device's current nominal rate
    bool                   canSwitchRate = false;  ///< device permits a rate change at all

    /// Does any advertised span cover `rate`? A plain membership query: the
    /// exact-match test BitPerfectWhenAvailable tries first, and the probe the
    /// nearest-best fallback (in the .cpp) reuses for each of
    /// its family candidates. Deliberately not fuzzy itself; all ranking lives
    /// in the fallback selection, not here.
    [[nodiscard]] bool advertises(std::uint32_t rate) const noexcept {
        return std::ranges::any_of(rates, [rate](const RateRange& r)
        {
            return r.covers(rate);
        });
    }
};

// ---------------------------------------------------------------------------
/// The policy's output: what the device should do for one source format under one
/// mode, given the device's capabilities and what is currently open. Trivially
/// copyable POD, passed by value into IAudioSink::open so the sink executes it
/// without re-deriving anything (one decider, one device read; see the Engine).
///
/// keepSink is exactly !needsDeviceReconfigure; both are present because the two
/// call sites name different signals and read better for it (the Engine asks
/// "needsDeviceReconfigure" when deciding to tear down, and asks "keepSink"
/// when deciding to stitch). They are computed together so they cannot disagree.
struct RateDecision {
    std::uint32_t deviceRate = 0;  ///< the rate the device should run at after open

    /// Must the device's nominal rate actually change to reach deviceRate? False
    /// when deviceRate already equals the device's current rate, so the sink skips
    /// a no-op switch (and the close-time restore) when nothing moved.
    bool switchDevice = false;

    /// Predicted: the OS device path will resample because deviceRate differs from
    /// the source rate. The complement of "bit-perfect was achieved" in the
    /// planning sense; the sink's measured device rate is the authority on the
    /// realized sense. See the file header.
    bool resampleNeeded = false;

    /// The gapless gate. True when the resolved device config (deviceRate plus
    /// the source channel count) differs from what is currently open, or when
    /// nothing is open. The gapless path reads it to skip the teardown for a
    /// same-config transition and stitch tracks without a boundary.
    bool needsDeviceReconfigure = false;

    /// == !needsDeviceReconfigure. The same gate, named for the stitch decision.
    bool keepSink = false;
};

// ---------------------------------------------------------------------------
/// The decision object. Stateless by design: decide() is a const,
/// side-effect-free pure function of its arguments, including the nearest-best
/// fallback ladder, which needs no configuration. It is a class rather than a
/// free function so the Engine can hold one as a member and any policy that
/// does someday need configuration can gain it without changing call sites.
class RateManager {
public:
    /// Resolve the device action for `source` under `mode`, given `caps` (read
    /// fresh from the sink) and `currentOpen` (the DEVICE format the engine is
    /// transitioning from: an invalid/zeroed AudioFormat means it is transitioning
    /// from a closed device, i.e. a fresh start). Deterministic and allocation-free
    /// so it is trivially unit-testable as a truth table and safe to call from the
    /// engine thread at every track boundary.
    [[nodiscard]] RateDecision decide(const AudioFormat&      source,
                                      const SinkCapabilities& caps,
                                      RateMode                mode,
                                      const AudioFormat&      currentOpen) const noexcept;
};

}  // namespace rawform::audio
