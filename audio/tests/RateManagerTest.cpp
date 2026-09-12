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

// RateManagerTest.cpp
//
// The portable, deterministic truth table for the rate policy. No sink, no
// audio hardware, no threads: just RateManager::decide() over a matrix of
// {source rates} x {device capability sets} x {RateMode}, plus the gapless-gate
// dimension {what is currently open}, plus the nearest-best fallback ladder
// inside BitPerfectWhenAvailable: family descent, family
// ascent, the two cross-family last resorts, and the degradations back to the
// stay-parked behavior. Because decide() is a pure function, every
// assertion is exact, which is the point of having lifted the policy out of the
// sink. Same hand-rolled CHECK harness as the rest of the suite; main returns
// non-zero on any failure.
//
// The capability sets mirror real devices: a fixed 48000-only output (cheap USB
// DACs, many Bluetooth links), a 44100/48000 pair (the common consumer case),
// the built-in Mac headphone DAC's wider 44100/48000/88200/96000 set, and a
// continuous-range device (built-in outputs advertise spans, not points) to
// prove RateRange covers ranges, not just discrete points. A "cannot switch"
// variant exercises the device-refuses-a-rate-change branch.

#include "rawform/audio/RateManager.h"
#include "rawform/audio/Types.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <vector>

using rawform::audio::AudioFormat;
using rawform::audio::RateDecision;
using rawform::audio::RateManager;
using rawform::audio::RateMode;
using rawform::audio::RateRange;
using rawform::audio::SinkCapabilities;

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

// ---------------------------------------------------------------------------
// Builders, so each case reads as data rather than struct plumbing.

// Capabilities from a list of discrete advertised rates (min == max each).
SinkCapabilities caps(std::initializer_list<std::uint32_t> rates, std::uint32_t current,
                      bool canSwitch) {
    SinkCapabilities c;
    c.deviceName    = "test device";
    c.currentRate   = current;
    c.canSwitchRate = canSwitch;
    for (std::uint32_t r : rates) {
        c.rates.push_back(RateRange{.min = r, .max = r});
    }
    return c;
}

// Capabilities from a single continuous span [min, max].
SinkCapabilities capsRange(std::uint32_t min, std::uint32_t max,
                           std::uint32_t current, bool canSwitch) {
    SinkCapabilities c;
    c.deviceName    = "range device";
    c.currentRate   = current;
    c.canSwitchRate = canSwitch;
    c.rates.push_back(RateRange{.min = min, .max = max});
    return c;
}

AudioFormat fmt(std::uint32_t rate, std::uint16_t channels = 2) {
    return AudioFormat{rate, channels};
}

constexpr AudioFormat kClosed{};  // invalid: transitioning from a closed device

// ---------------------------------------------------------------------------
// BitPerfectWhenAvailable: switch only on an advertised, switchable match.

void testBitPerfectExactMatchSwitches() {
    constexpr RateManager rm;
    const RateDecision d =
        rm.decide(fmt(96000), caps({44100, 48000, 88200, 96000}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 96000);
    CHECK(d.switchDevice == true);      // 96000 advertised, device was at 48000
    CHECK(d.resampleNeeded == false);   // bit-perfect predicted
    CHECK(d.needsDeviceReconfigure == true);  // fresh start
    CHECK(d.keepSink == false);
}

void testBitPerfectAlreadyAtRateDoesNotSwitch() {
    constexpr RateManager rm;
    const RateDecision d =
        rm.decide(fmt(48000), caps({44100, 48000}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 48000);
    CHECK(d.switchDevice == false);     // already there, no-op switch avoided
    CHECK(d.resampleNeeded == false);   // bit-perfect (device clocks the source)
}

void testBitPerfectUnadvertisedRateResamples() {
    constexpr RateManager rm;
    // 96000 is not advertised, so the nearest-best fallback runs: family descent
    // finds 48000 (96000 / 2), which happens to BE the standing rate, so the
    // device is not touched. Same assertions as before the fallback existed, but
    // for a different reason: the policy now CHOSE 48000, it did not surrender
    // to it.
    const RateDecision d =
        rm.decide(fmt(96000), caps({44100, 48000}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 48000);       // family fallback == the standing rate
    CHECK(d.switchDevice == false);     // no-op switch skipped
    CHECK(d.resampleNeeded == true);    // OS path resamples 96000 -> 48000
}

// ---------------------------------------------------------------------------
// Nearest-best fallback: the ladder inside
// BitPerfectWhenAvailable when the exact rate is unreachable on a switchable
// device. Family descent, family ascent, highest-below, lowest-above, and the
// degradations (cannot switch, empty capability set), plus the gapless
// interaction.

void testFallbackFamilyDescentSwitchesUp() {
    constexpr RateManager rm;
    // THE motivating case: a 192000 track, a built-in output topping out at
    // 96000 and parked at 44100. Family descent picks 96000 (192000 / 2) and the
    // device SWITCHES UP from its standing 44100 instead of resampling into it.
    const RateDecision d =
        rm.decide(fmt(192000), caps({44100, 48000, 88200, 96000}, 44100, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 96000);
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == true);    // honest: 192000 -> 96000 is not bit-perfect
}

void testFallbackFamilyDescentPrefersOwnFamily() {
    constexpr RateManager rm;
    // A 96000 source on a device parked at 44100 that advertises both families:
    // the fallback must land on 48000 (96000 / 2), not stay at 44100.
    const RateDecision d =
        rm.decide(fmt(96000), caps({44100, 48000}, 44100, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 48000);       // family, not the standing 44100
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == true);
}

void testFallbackFamilyDescent441Family() {
    constexpr RateManager rm;
    // The 44100-family mirror: 176400 lands on 88200, not on the (higher-rate,
    // cross-family) 96000 and not on the standing 48000.
    const RateDecision d =
        rm.decide(fmt(176400), caps({44100, 48000, 88200, 96000}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 88200);
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == true);
}

void testFallbackFamilyDescentDeeperDivisor() {
    constexpr RateManager rm;
    // 192000 with no 96000 advertised: descent continues to 48000 (192000 / 4).
    const RateDecision d =
        rm.decide(fmt(192000), caps({44100, 48000}, 44100, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 48000);
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == true);
}

void testFallbackFamilyAscent() {
    constexpr RateManager rm;
    // A 22050 source on a device whose floor is 44100: ascent picks 44100
    // (22050 * 2), the family multiple, not the standing 48000.
    const RateDecision d =
        rm.decide(fmt(22050), caps({44100, 48000}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 44100);
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == true);
}

void testFallbackHighestBelowCrossFamily() {
    constexpr RateManager rm;
    // A 176400 source on a 48000-family-only device: no family rate exists at
    // any divisor or multiplier, so the cross-family step takes the highest
    // advertised rate below the source, 96000.
    const RateDecision d =
        rm.decide(fmt(176400), caps({48000, 96000}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 96000);
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == true);
}

void testFallbackLowestAbove() {
    constexpr RateManager rm;
    // An 8000 telephony-rate source on a device whose floor is 44100: nothing
    // below, no family multiple advertised (16000/32000/64000 absent), so the
    // last step takes the lowest advertised rate above, 44100.
    const RateDecision d =
        rm.decide(fmt(8000), caps({44100, 48000}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 44100);
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == true);
}

void testFallbackSpanBoundary() {
    constexpr RateManager rm;
    // A continuous span below the source: the highest-below step must use the
    // span MAXIMUM. A 192000 source over a 44100..96000 span lands on 96000
    // (here via family descent AND the span edge agreeing; the span device
    // proves covers() feeds the family probe, not just discrete points).
    const RateDecision d =
        rm.decide(fmt(192000), capsRange(44100, 96000, 44100, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 96000);
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == true);
}

void testFallbackCannotSwitchStillStays() {
    constexpr RateManager rm;
    // The fallback is gated on canSwitchRate exactly like the exact match: a
    // device that refuses a rate change is never asked to fall back anywhere.
    const RateDecision d =
        rm.decide(fmt(192000), caps({44100, 48000, 96000}, 44100, false),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 44100);       // parked; OS path resamples into it
    CHECK(d.switchDevice == false);
    CHECK(d.resampleNeeded == true);
}

void testFallbackEmptyCapabilitySetStays() {
    constexpr RateManager rm;
    // A degenerate capabilities read (no advertised spans at all): every ladder
    // step returns nothing, so the decision stays at the current rate, exactly
    // the pre-fallback behavior.
    const RateDecision d =
        rm.decide(fmt(96000), caps({}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 48000);
    CHECK(d.switchDevice == false);
    CHECK(d.resampleNeeded == true);
}

void testFallbackGaplessAcrossSameSourceRate() {
    constexpr RateManager rm;
    // The gapless interaction: two consecutive 192000 tracks both fall back to 96000,
    // so the second decision sees the device config unchanged and keeps the
    // sink. Fallback transitions stitch gaplessly like any other.
    const SinkCapabilities c = caps({44100, 48000, 96000}, 96000, true);
    const AudioFormat openAt = fmt(96000);  // device stream open at 96000/2
    const RateDecision d =
        rm.decide(fmt(192000), c, RateMode::BitPerfectWhenAvailable, openAt);
    CHECK(d.deviceRate == 96000);
    CHECK(d.switchDevice == false);           // already clocking the fallback
    CHECK(d.resampleNeeded == true);
    CHECK(d.needsDeviceReconfigure == false);
    CHECK(d.keepSink == true);
}

void testBitPerfectCannotSwitchResamples() {
    constexpr RateManager rm;
    // The rate IS advertised, but the device refuses a nominal-rate change.
    const RateDecision d =
        rm.decide(fmt(44100), caps({44100, 48000}, 48000, false),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 48000);
    CHECK(d.switchDevice == false);
    CHECK(d.resampleNeeded == true);
}

void testBitPerfectContinuousRangeCovers() {
    constexpr RateManager rm;
    // A device advertising a continuous 44100..192000 span must accept 96000.
    const RateDecision d =
        rm.decide(fmt(96000), capsRange(44100, 192000, 44100, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.deviceRate == 96000);
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == false);
}

// ---------------------------------------------------------------------------
// AlwaysResample: never switch, regardless of advertised support.

void testAlwaysResampleNeverSwitchesEvenWhenSupported() {
    constexpr RateManager rm;
    const RateDecision d =
        rm.decide(fmt(44100), caps({44100, 48000}, 48000, true),
                  RateMode::AlwaysResample, kClosed);
    CHECK(d.deviceRate == 48000);       // stays at the device's current rate
    CHECK(d.switchDevice == false);
    CHECK(d.resampleNeeded == true);    // 44100 source over a 48000 device
}

void testAlwaysResampleLuckyMatchIsBitPerfect() {
    constexpr RateManager rm;
    // Source already equals the device rate: no switch, and it happens to be
    // bit-perfect even though we never touched the device.
    const RateDecision d =
        rm.decide(fmt(48000), caps({44100, 48000}, 48000, true),
                  RateMode::AlwaysResample, kClosed);
    CHECK(d.deviceRate == 48000);
    CHECK(d.switchDevice == false);
    CHECK(d.resampleNeeded == false);
}

// ---------------------------------------------------------------------------
// ForceDeviceRate: switch to the source rate regardless of the advertised list.

void testForceSwitchesUnadvertisedRate() {
    constexpr RateManager rm;
    const RateDecision d =
        rm.decide(fmt(96000), caps({44100, 48000}, 48000, true),
                  RateMode::ForceDeviceRate, kClosed);
    CHECK(d.deviceRate == 96000);       // forced despite not being advertised
    CHECK(d.switchDevice == true);
    CHECK(d.resampleNeeded == false);   // predicted bit-perfect; sink measures truth
}

void testForceCannotSwitchFallsBackToResample() {
    constexpr RateManager rm;
    const RateDecision d =
        rm.decide(fmt(96000), caps({48000}, 48000, false),
                  RateMode::ForceDeviceRate, kClosed);
    CHECK(d.deviceRate == 48000);       // device refuses a switch; cannot force
    CHECK(d.switchDevice == false);
    CHECK(d.resampleNeeded == true);
}

// ---------------------------------------------------------------------------
// The gapless gate: needsDeviceReconfigure / keepSink versus what is currently open.

void testGateSameDeviceRateHolds() {
    constexpr RateManager rm;
    // Two 44100 sources, both bit-perfect at a device already running 44100: the
    // second transition keeps the sink.
    const SinkCapabilities c = caps({44100, 48000}, 44100, true);
    const AudioFormat openAt = fmt(44100);  // device currently open at 44100/2
    const RateDecision d =
        rm.decide(fmt(44100), c, RateMode::BitPerfectWhenAvailable, openAt);
    CHECK(d.deviceRate == 44100);
    CHECK(d.needsDeviceReconfigure == false);
    CHECK(d.keepSink == true);
}

void testGateDifferentDeviceRateReconfigures() {
    constexpr RateManager rm;
    const SinkCapabilities c = caps({44100, 48000}, 44100, true);
    const AudioFormat openAt = fmt(44100);
    const RateDecision d =
        rm.decide(fmt(48000), c, RateMode::BitPerfectWhenAvailable, openAt);
    CHECK(d.deviceRate == 48000);
    CHECK(d.needsDeviceReconfigure == true);
    CHECK(d.keepSink == false);
}

void testGateChannelChangeReconfigures() {
    constexpr RateManager rm;
    const SinkCapabilities c = caps({48000}, 48000, true);
    const AudioFormat openAt = fmt(48000, 2);  // open stereo
    const RateDecision d =
        rm.decide(fmt(48000, 1), c, RateMode::BitPerfectWhenAvailable, openAt);
    CHECK(d.deviceRate == 48000);             // same device rate
    CHECK(d.needsDeviceReconfigure == true);  // but mono vs stereo: reconfigure
    CHECK(d.keepSink == false);
}

void testGateResampleToSameDeviceRateHolds() {
    constexpr RateManager rm;
    // The nice gapless property: two DIFFERENT source rates that both resample to
    // the same fixed device rate keep the sink, because the device stream config
    // does not move. Device fixed at 48000, cannot switch.
    const SinkCapabilities c = caps({48000}, 48000, false);
    const AudioFormat openAt = fmt(48000);  // device stream running at 48000/2
    const RateDecision d =
        rm.decide(fmt(44100), c, RateMode::BitPerfectWhenAvailable, openAt);
    CHECK(d.deviceRate == 48000);
    CHECK(d.resampleNeeded == true);          // 44100 source resampled to 48000
    CHECK(d.needsDeviceReconfigure == false); // device config unchanged: hold
    CHECK(d.keepSink == true);
}

void testGateFreshStartAlwaysReconfigures() {
    constexpr RateManager rm;
    const RateDecision d =
        rm.decide(fmt(48000), caps({48000}, 48000, true),
                  RateMode::BitPerfectWhenAvailable, kClosed);
    CHECK(d.needsDeviceReconfigure == true);  // nothing open
    CHECK(d.keepSink == false);
}

}  // namespace

int main() {
    testBitPerfectExactMatchSwitches();
    testBitPerfectAlreadyAtRateDoesNotSwitch();
    testBitPerfectUnadvertisedRateResamples();
    testBitPerfectCannotSwitchResamples();
    testBitPerfectContinuousRangeCovers();

    testFallbackFamilyDescentSwitchesUp();
    testFallbackFamilyDescentPrefersOwnFamily();
    testFallbackFamilyDescent441Family();
    testFallbackFamilyDescentDeeperDivisor();
    testFallbackFamilyAscent();
    testFallbackHighestBelowCrossFamily();
    testFallbackLowestAbove();
    testFallbackSpanBoundary();
    testFallbackCannotSwitchStillStays();
    testFallbackEmptyCapabilitySetStays();
    testFallbackGaplessAcrossSameSourceRate();

    testAlwaysResampleNeverSwitchesEvenWhenSupported();
    testAlwaysResampleLuckyMatchIsBitPerfect();

    testForceSwitchesUnadvertisedRate();
    testForceCannotSwitchFallsBackToResample();

    testGateSameDeviceRateHolds();
    testGateDifferentDeviceRateReconfigures();
    testGateChannelChangeReconfigures();
    testGateResampleToSameDeviceRateHolds();
    testGateFreshStartAlwaysReconfigures();

    if (g_failures == 0) {
        std::puts("RateManagerTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "RateManagerTest: %d check(s) failed\n", g_failures);
    return 1;
}
