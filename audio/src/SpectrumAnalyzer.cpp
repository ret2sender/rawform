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

// SpectrumAnalyzer.cpp
//
// Implementation of the spectrum transform. The whole file is plain,
// single-threaded DSP: a Hann window, a hand-rolled iterative radix-2 FFT, a
// per-bin magnitude, a fold into log-spaced bands, and a fixed dB-to-0..1 mapping.
// See the header for why each stage is here and why the FFT is hand-rolled rather
// than pulled from a third-party library.

#include "rawform/audio/SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace rawform::audio {

namespace {

// A pi constant local to this translation unit, matching the style the other
// engine tests and DSP use rather than relying on a non-standard M_PI.
constexpr double kPi = 3.14159265358979323846;

// The magnitude a full-scale (amplitude 1.0) sine concentrates into its main lobe
// once the Hann window's coherent gain of 0.5 is folded in: about fftSize/4. We
// normalize every band's peak bin against this so that magnitude maps to dBFS,
// 0 dB being a full-scale tone. The exact figure is a calibration choice, not a
// correctness one (it only shifts where [floorDb, ceilDb] sits relative to real
// signals), so a single clean expression is enough.
inline double fullScaleReference(std::size_t fftSize) {
    return static_cast<double>(fftSize) / 4.0;
}

}  // namespace

// ---------------------------------------------------------------------------
SpectrumAnalyzer::SpectrumAnalyzer() {
    // Construct in a valid, all-zero state so analyze() before configure() returns
    // a clean idle frame rather than touching empty buffers. A real configure()
    // (with a sample rate) replaces all of this.
    configure(44100);
}

// ---------------------------------------------------------------------------
std::size_t SpectrumAnalyzer::roundUpPow2(std::size_t n) noexcept {
    std::size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

// ---------------------------------------------------------------------------
void SpectrumAnalyzer::configure(std::uint32_t sampleRate,
                                 std::size_t   fftSize,
                                 std::size_t   barCount,
                                 double        minHz,
                                 double        maxHz,
                                 double        floorDb,
                                 double        ceilDb) {
    // Clamp the structural inputs to valid ranges so a careless caller cannot
    // produce a degenerate transform. fftSize is forced to a power of two (the
    // radix-2 FFT requires it) and to a floor of 64; barCount to at least one.
    m_sampleRate = sampleRate == 0 ? 44100 : sampleRate;
    m_fftSize    = roundUpPow2(fftSize < 64 ? 64 : fftSize);
    if (barCount < 1) {
        barCount = 1;
    }

    // Frequency window. Keep minHz positive (log spacing needs it), keep the top
    // edge below Nyquist (no band should reach past the real spectrum), and keep
    // maxHz strictly above minHz so the log ratio is well formed.
    const double nyquist = static_cast<double>(m_sampleRate) / 2.0;
    m_minHz = minHz < 1.0 ? 1.0 : minHz;
    m_maxHz = std::min(maxHz, nyquist);
    if (m_maxHz <= m_minHz) {
        m_maxHz = std::min(m_minHz * 2.0, nyquist);
        if (m_maxHz <= m_minHz) {
            m_maxHz = m_minHz + 1.0;  // last-resort guard for absurd rates
        }
    }

    m_floorDb = floorDb;
    // Keep the ceiling strictly above the floor so the normalize never divides by
    // zero or inverts.
    m_ceilDb = ceilDb > floorDb ? ceilDb : floorDb + 1.0;

    // (Re)size the scratch buffers once here, never in analyze().
    m_re.assign(m_fftSize, 0.0f);
    m_im.assign(m_fftSize, 0.0f);
    m_mag.assign(m_fftSize / 2 + 1, 0.0f);
    m_bars.assign(barCount, 0.0f);

    buildWindow();
    buildBands();
}

// ---------------------------------------------------------------------------
// Hann window. The classic 0.5 - 0.5*cos form over the whole frame. Built once
// per configure(); analyze() only multiplies.
void SpectrumAnalyzer::buildWindow() {
    m_window.assign(m_fftSize, 0.0f);
    if (m_fftSize < 2) {
        m_window[0] = 1.0f;
        return;
    }
    const auto denom = static_cast<double>(m_fftSize - 1);
    for (std::size_t i = 0; i < m_fftSize; ++i) {
        m_window[i] = static_cast<float>(
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / denom));
    }
}

// ---------------------------------------------------------------------------
// Map each bar to a half-open range of FFT bins from the log-spaced band edges.
// The k-th edge is minHz * (maxHz/minHz)^(k/barCount), so the edges are
// geometrically spaced and each bar spans an equal RATIO of frequency, which is
// how music and pitch perception are organized. Bins are indexed by
// freq / binWidth; we clamp to [1, N/2] (skipping the DC bin) and guarantee every
// band covers at least one bin so a band narrower than the bin spacing (which
// happens at the low end, where the bins are wide relative to the log steps) still
// produces a value instead of collapsing to empty.
void SpectrumAnalyzer::buildBands() {
    const std::size_t bars   = m_bars.size();
    const std::size_t maxBin = m_fftSize / 2;  // the Nyquist bin index
    m_bandLo.assign(bars, 1);
    m_bandHi.assign(bars, 2);

    const double binWidth = static_cast<double>(m_sampleRate) /
                            static_cast<double>(m_fftSize);
    const double ratio = m_maxHz / m_minHz;

    auto freqToBin = [&](double hz) -> std::size_t {
        const long bin = std::lround(hz / binWidth);
        if (bin < 1) {
            return 1;
        }
        if (static_cast<std::size_t>(bin) > maxBin) {
            return maxBin;
        }
        return static_cast<std::size_t>(bin);
    };

    for (std::size_t k = 0; k < bars; ++k) {
        const double fLo = m_minHz * std::pow(ratio, static_cast<double>(k) /
                                                          static_cast<double>(bars));
        const double fHi = m_minHz * std::pow(ratio, static_cast<double>(k + 1) /
                                                          static_cast<double>(bars));
        std::size_t lo = freqToBin(fLo);
        std::size_t hi = freqToBin(fHi);
        if (hi <= lo) {
            hi = lo + 1;  // at least one bin per band
        }
        if (hi > maxBin + 1) {
            hi = maxBin + 1;  // keep the half-open top within m_mag
        }
        m_bandLo[k] = lo;
        m_bandHi[k] = hi;
    }
}

// ---------------------------------------------------------------------------
// In-place iterative radix-2 decimation-in-time FFT. The standard two phases: an
// up-front bit-reversal permutation, then log2(N) stages of butterflies with the
// twiddle accumulated across each stage. m_im is all zero on entry (real input),
// but this is the general complex routine, which keeps it short and obviously
// correct. Forward transform (negative exponent); we only ever read magnitudes, so
// the sign and any overall scale wash out of the normalized result.
void SpectrumAnalyzer::fftInPlace() {
    const std::size_t n = m_fftSize;

    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(m_re[i], m_re[j]);
            std::swap(m_im[i], m_im[j]);
        }
    }

    // Butterfly stages.
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang   = -2.0 * kPi / static_cast<double>(len);
        const auto wLenRe  = static_cast<float>(std::cos(ang));
        const auto wLenIm  = static_cast<float>(std::sin(ang));
        const std::size_t half = len >> 1;
        for (std::size_t i = 0; i < n; i += len) {
            float wRe = 1.0f;
            float wIm = 0.0f;
            for (std::size_t k = 0; k < half; ++k) {
                const std::size_t a = i + k;
                const std::size_t b = i + k + half;
                const float uRe = m_re[a];
                const float uIm = m_im[a];
                const float vRe = m_re[b] * wRe - m_im[b] * wIm;
                const float vIm = m_re[b] * wIm + m_im[b] * wRe;
                m_re[a] = uRe + vRe;
                m_im[a] = uIm + vIm;
                m_re[b] = uRe - vRe;
                m_im[b] = uIm - vIm;
                const float nwRe = wRe * wLenRe - wIm * wLenIm;
                const float nwIm = wRe * wLenIm + wIm * wLenRe;
                wRe = nwRe;
                wIm = nwIm;
            }
        }
    }
}

// ---------------------------------------------------------------------------
const std::vector<float>& SpectrumAnalyzer::analyze(const float* mono,
                                                    std::size_t  n) {
    // Idle frame: nothing to analyze, so report a flat spectrum. The provider's
    // attack/decay will already be walking the bars down in this case, but a clean
    // zero here keeps analyze() total.
    if (mono == nullptr || n == 0) {
        std::ranges::fill(m_bars, 0.0f);
        return m_bars;
    }

    // Window the most recent fftSize samples into m_re, zero the imaginary part.
    // When the caller supplies more than a window (a tap often hands over its whole
    // buffer), keep the LATEST fftSize; when it supplies fewer (the first instants
    // of playback), use what there is and leave the tail zero.
    const std::size_t take   = std::min(n, m_fftSize);
    const std::size_t offset = n - take;  // index of the first sample we keep
    for (std::size_t i = 0; i < take; ++i) {
        m_re[i] = mono[offset + i] * m_window[i];
        m_im[i] = 0.0f;
    }
    for (std::size_t i = take; i < m_fftSize; ++i) {
        m_re[i] = 0.0f;
        m_im[i] = 0.0f;
    }

    fftInPlace();

    // Per-bin magnitude up to Nyquist.
    const std::size_t bins = m_fftSize / 2;
    for (std::size_t k = 0; k <= bins; ++k) {
        m_mag[k] = std::sqrt(m_re[k] * m_re[k] + m_im[k] * m_im[k]);
    }

    // Fold into log bands and map to 0..1. Per band, take the loudest bin (so a
    // narrow tone lights its bar fully rather than being averaged down), express it
    // in dBFS against the full-scale reference, and place it on the fixed
    // [floorDb, ceilDb] scale.
    const double ref   = fullScaleReference(m_fftSize);
    const double range = m_ceilDb - m_floorDb;
    for (std::size_t k = 0; k < m_bars.size(); ++k) {
        float peak = 0.0f;
        for (std::size_t b = m_bandLo[k]; b < m_bandHi[k]; ++b) {
            peak = std::max(peak, m_mag[b]);
        }
        if (peak <= 0.0f) {
            m_bars[k] = 0.0f;
            continue;
        }
        const double db  = 20.0 * std::log10(static_cast<double>(peak) / ref);
        double       val = (db - m_floorDb) / range;
        if (val < 0.0) {
            val = 0.0;
        } else if (val > 1.0) {
            val = 1.0;
        }
        m_bars[k] = static_cast<float>(val);
    }

    return m_bars;
}

}  // namespace rawform::audio
