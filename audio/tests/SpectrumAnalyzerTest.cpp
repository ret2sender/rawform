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

// SpectrumAnalyzerTest.cpp
//
// The codec-free, hardware-free test for the spectrum transform, in the spirit of
// ReplayGainScannerTest and EngineTest: the analyzer takes a raw float buffer, so
// the test simply synthesizes a known signal in-process (a sine of a chosen
// frequency and amplitude, or digital silence) and asserts the shape of the bars
// that come back. No libsndfile, no FFmpeg, no audio device.
//
// An FFT is exact arithmetic, but a BAR display is a lossy, calibrated summary, so
// the assertions are STRUCTURAL (the project's lossy tier): the strongest checks
// are relational and so independent of the exact dB calibration. A pure tone must
// light the log band that contains its frequency and leave distant bands dark; the
// lit band must be the loudest of all; raising or lowering the tone must move the
// lit band the right way; and digital silence must leave every bar at the floor.
// The expected band is recomputed here from the SAME log-edge formula the analyzer
// uses, so the test pins WHERE a frequency lands without re-deriving the FFT.

#include "rawform/audio/SpectrumAnalyzer.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

using rawform::audio::SpectrumAnalyzer;

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

constexpr double kPi = 3.14159265358979323846;

// Test configuration, matching the analyzer defaults so the band layout the test
// reasons about is the one the UI will use.
constexpr std::uint32_t kRate     = 44100;
constexpr std::size_t   kFftSize  = 2048;
constexpr std::size_t   kBarCount = 20;
constexpr double        kMinHz    = 40.0;
constexpr double        kMaxHz    = 18000.0;
constexpr double        kFloorDb  = -70.0;
constexpr double        kCeilDb   = -12.0;

// Generate `count` samples of a sine at `freqHz` and amplitude `amp`.
std::vector<float> makeSine(double freqHz, double amp, std::size_t count) {
    std::vector<float> v(count);
    const double w = 2.0 * kPi * freqHz / static_cast<double>(kRate);
    for (std::size_t i = 0; i < count; ++i) {
        v[i] = static_cast<float>(amp * std::sin(w * static_cast<double>(i)));
    }
    return v;
}

// The bar whose log band contains `freqHz`, the analytic inverse of the analyzer's
// geometric edge formula f_k = minHz * (maxHz/minHz)^(k/barCount). Clamped to the
// valid bar range.
std::size_t expectedBar(double freqHz) {
    const double frac = std::log(freqHz / kMinHz) / std::log(kMaxHz / kMinHz);
    long         k    = static_cast<long>(std::floor(frac * static_cast<double>(kBarCount)));
    if (k < 0) {
        k = 0;
    }
    if (k >= static_cast<long>(kBarCount)) {
        k = static_cast<long>(kBarCount) - 1;
    }
    return static_cast<std::size_t>(k);
}

// Index of the largest bar.
std::size_t argMax(const std::vector<float>& bars) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < bars.size(); ++i) {
        if (bars[i] > bars[best]) {
            best = i;
        }
    }
    return best;
}

void testToneLandsInItsBand() {
    SpectrumAnalyzer a;
    a.configure(kRate, kFftSize, kBarCount, kMinHz, kMaxHz, kFloorDb, kCeilDb);
    CHECK(a.barCount() == kBarCount);
    CHECK(a.fftSize() == kFftSize);

    // A 1 kHz tone, comfortably loud. It should light the band that contains
    // 1 kHz, and that band should be the loudest of all.
    constexpr double freq = 1000.0;
    const std::vector<float> sine = makeSine(freq, 0.5, kFftSize);
    const std::vector<float>& bars = a.analyze(sine.data(), sine.size());

    const std::size_t expect = expectedBar(freq);
    const std::size_t peak   = argMax(bars);

    // The loud band is near where the tone sits (allow one bar of slack for the
    // bin/edge rounding) and is strongly lit.
    CHECK(peak + 1 >= expect && peak <= expect + 1);
    CHECK(bars[peak] > 0.6f);

    // Distant bands stay dark. The lowest band (40..~54 Hz) carries no energy from
    // a 1 kHz tone, and neither does the top band.
    CHECK(bars[0] < 0.15f);
    CHECK(bars[kBarCount - 1] < 0.15f);
}

void testToneMovesWithFrequency() {
    SpectrumAnalyzer a;
    a.configure(kRate, kFftSize, kBarCount, kMinHz, kMaxHz, kFloorDb, kCeilDb);

    // A low tone and a high tone must light bars in the right ORDER: the low tone's
    // lit bar is to the left of the high tone's. This pins the log-frequency
    // ordering without depending on exact calibration.
    const std::vector<float> low  = makeSine(120.0, 0.5, kFftSize);
    const std::size_t lowBar = argMax(a.analyze(low.data(), low.size()));

    const std::vector<float> high = makeSine(6000.0, 0.5, kFftSize);
    const std::size_t highBar = argMax(a.analyze(high.data(), high.size()));

    CHECK(lowBar < highBar);
    CHECK(lowBar + 1 >= expectedBar(120.0) && lowBar <= expectedBar(120.0) + 1);
    CHECK(highBar + 1 >= expectedBar(6000.0) && highBar <= expectedBar(6000.0) + 1);
}

void testSilenceIsFlat() {
    SpectrumAnalyzer a;
    a.configure(kRate, kFftSize, kBarCount, kMinHz, kMaxHz, kFloorDb, kCeilDb);

    const std::vector<float> quiet(kFftSize, 0.0f);
    const std::vector<float>& bars = a.analyze(quiet.data(), quiet.size());
    for (std::size_t i = 0; i < bars.size(); ++i) {
        CHECK(bars[i] < 0.05f);
    }

    // A null / empty block is also a clean idle frame.
    const std::vector<float>& none = a.analyze(nullptr, 0);
    for (std::size_t i = 0; i < none.size(); ++i) {
        CHECK(none[i] < 0.05f);
    }
}

void testShortBlockIsHandled() {
    SpectrumAnalyzer a;
    a.configure(kRate, kFftSize, kBarCount, kMinHz, kMaxHz, kFloorDb, kCeilDb);

    // Fewer samples than a full window (the first instants of playback): the
    // analyzer must zero-pad and still produce a valid, bounded result without
    // reading out of range.
    const std::vector<float> partial = makeSine(1000.0, 0.5, kFftSize / 4);
    const std::vector<float>& bars = a.analyze(partial.data(), partial.size());
    CHECK(bars.size() == kBarCount);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        CHECK(bars[i] >= 0.0f && bars[i] <= 1.0f);
    }
}

void testReconfigureRebuildsBands() {
    SpectrumAnalyzer a;

    // At 44.1 kHz a 1 kHz tone lands in some band; after reconfiguring to a tiny
    // bar count the result resizes and the tone still lands in a valid, lit band.
    a.configure(kRate, kFftSize, kBarCount, kMinHz, kMaxHz, kFloorDb, kCeilDb);
    const std::vector<float> sine = makeSine(1000.0, 0.5, kFftSize);
    CHECK(a.analyze(sine.data(), sine.size()).size() == kBarCount);

    a.configure(kRate, kFftSize, 4, kMinHz, kMaxHz, kFloorDb, kCeilDb);
    const std::vector<float>& bars = a.analyze(sine.data(), sine.size());
    CHECK(bars.size() == 4);
    CHECK(bars[argMax(bars)] > 0.6f);
}

}  // namespace

int main() {
    testToneLandsInItsBand();
    testToneMovesWithFrequency();
    testSilenceIsFlat();
    testShortBlockIsHandled();
    testReconfigureRebuildsBands();

    if (g_failures == 0) {
        std::printf("SpectrumAnalyzerTest: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "SpectrumAnalyzerTest: %d check(s) failed\n", g_failures);
    return 1;
}
