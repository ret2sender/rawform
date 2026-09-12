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

// SpectrumAnalyzer.h
//
// The DSP half of the spectrum viewer: a pure, zero-Qt transform that turns a
// block of mono float samples into a small set of bar magnitudes ready to draw.
// It is the visual analogue of ReplayGainScanner: a self-contained measurement
// class in the rawform_audio engine library, the same tier as the decoders and
// the Engine, with no Qt above it. It is placed in the PUBLIC include (and built
// into the library) so the Qt UI that drives it on a timer can include it, and so
// it is exercisable in the engine's own zero-dependency CHECK harness.
//
// What it does, in order, per analyze() call:
//   1. Copy up to fftSize of the most recent samples (zero-padding a short block),
//      so the caller can hand over whatever the tap returned without pre-sizing.
//   2. Apply a Hann window, to trade a little resolution for far lower spectral
//      leakage, which is what makes the bars read as clean tones rather than a
//      smeared mess.
//   3. Run an in-place radix-2 Cooley-Tukey FFT, coded from scratch rather than
//      pulled from FFTW or kissfft. A bar display needs a few thousand points
//      sixty times a second, which a textbook iterative FFT delivers with cycles
//      to spare, so a third-party DSP dependency would buy nothing and cost a
//      build dependency the engine otherwise avoids; the hand-rolled CHECK test
//      harness and the LiveBitrateMeter follow the same rule of not importing a
//      library for a few dozen lines of arithmetic.
//   4. Take the per-bin magnitude up to Nyquist.
//   5. Fold the bins into barCount LOG-SPACED frequency bands (music and hearing
//      are both logarithmic in frequency, so linear bands would crush the whole
//      melodic range into the leftmost bar), taking the loudest bin in each band
//      so a narrow tone still lights its bar fully.
//   6. Convert to dB and normalize each band onto 0..1 against [floorDb, ceilDb],
//      a fixed scale (no auto-gain) for a steady readout.
//
// What it deliberately does NOT do: temporal smoothing. Attack/decay is state
// carried across UI frames at the visualizer's cadence, not a property of a single
// transform, so it lives in the Qt provider that calls this. Keeping analyze()
// stateless (its only state is the configuration and the scratch buffers) is what
// makes it a clean, deterministic unit to test: the same samples in always give
// the same bars out.
//
// Threading. Single-threaded by contract, exactly like ReplayGainScanner: the Qt
// provider owns one instance and calls configure()/analyze() only from its timer
// slot on the GUI thread, never concurrently, so the class needs no locking. The
// scratch buffers are reused across calls to avoid per-frame allocation.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rawform::audio {

class SpectrumAnalyzer {
public:
    /// Sensible defaults, all overridable through configure(). They are public so a
    /// caller (and the test) can build a matching configuration and reason about the
    /// band layout without duplicating the numbers.
    static constexpr std::size_t kDefaultFftSize  = 2048;   ///< power of two
    static constexpr std::size_t kDefaultBarCount = 20;     ///< matches the UI frame
    static constexpr double      kDefaultMinHz    = 40.0;   ///< low edge of the lowest bar
    static constexpr double      kDefaultMaxHz    = 18000.0;  ///< high edge of the top bar
    static constexpr double      kDefaultFloorDb  = -70.0;  ///< maps to 0.0 (silence)
    static constexpr double      kDefaultCeilDb   = -12.0;  ///< maps to 1.0 (full bar)

    SpectrumAnalyzer();

    /// (Re)configure the transform. Call once up front and again whenever the
    /// sample rate changes (the band-to-bin mapping depends on it), which in this
    /// player happens on a track change to a different-rate file. fftSize is forced
    /// to a power of two (rounded up) and to a sane minimum; barCount to at least 1;
    /// maxHz is clamped below Nyquist so the top band never reaches past the real
    /// spectrum. The other arguments are taken as given (the caller owns the visual
    /// tuning). Reallocates the scratch buffers and rebuilds the window and the band
    /// table; cheap, and off any hot path.
    void configure(std::uint32_t sampleRate,
                   std::size_t   fftSize  = kDefaultFftSize,
                   std::size_t   barCount = kDefaultBarCount,
                   double        minHz    = kDefaultMinHz,
                   double        maxHz    = kDefaultMaxHz,
                   double        floorDb  = kDefaultFloorDb,
                   double        ceilDb   = kDefaultCeilDb);

    /// Analyze the most recent mono block. Reads up to fftSize samples from `mono`
    /// (the LATEST ones when n > fftSize, since a tap hands over its newest window;
    /// zero-padded when n < fftSize), and returns a reference to the internal bar
    /// vector: barCount values, each clamped to 0..1, low frequency first. The
    /// reference is valid until the next analyze()/configure() call. A null pointer
    /// or n == 0 yields an all-zero result (the idle frame). Allocation-free after
    /// configure().
    const std::vector<float>& analyze(const float* mono, std::size_t n);

    /// The number of bars the current configuration produces (== the result size).
    [[nodiscard]] std::size_t barCount() const noexcept { return m_bars.size(); }

    /// The FFT size in samples, which is also the window length analyze() consumes.
    /// The caller sizes its tap copy to at least this.
    [[nodiscard]] std::size_t fftSize() const noexcept { return m_fftSize; }

private:
    /// Rebuild the Hann window (length fftSize) and the per-bar bin ranges from the
    /// log-spaced band edges and the current sample rate. Called by configure().
    void buildWindow();
    void buildBands();

    /// In-place iterative radix-2 FFT over m_re / m_im (length fftSize, a power of
    /// two). Decimation-in-time (DIT) with an up-front bit-reversal permutation; the
    /// twiddles are computed inline. Real input means m_im is all zero on entry, but
    /// the routine is the general complex FFT so it stays simple and correct.
    void fftInPlace();

    static std::size_t roundUpPow2(std::size_t n) noexcept;

    std::uint32_t m_sampleRate = 0;
    std::size_t   m_fftSize    = 0;
    double        m_minHz   = 0.0;
    double        m_maxHz   = 0.0;
    double        m_floorDb = 0.0;
    double        m_ceilDb  = 0.0;

    std::vector<float> m_window;  ///< Hann coefficients, length fftSize
    std::vector<float> m_re;      ///< FFT real part / windowed input, length fftSize
    std::vector<float> m_im;      ///< FFT imaginary part, length fftSize
    std::vector<float> m_mag;     ///< per-bin magnitude, length fftSize/2 + 1

    /// Per-bar half-open bin range [lo, hi) into m_mag. Each band covers at least
    /// one bin, so a band narrower than the bin spacing (possible at the low end)
    /// still has a value rather than collapsing to empty.
    std::vector<std::size_t> m_bandLo;
    std::vector<std::size_t> m_bandHi;

    std::vector<float> m_bars;  ///< the result, length barCount, each 0..1
};

}  // namespace rawform::audio
