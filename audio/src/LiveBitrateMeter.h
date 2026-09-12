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

// LiveBitrateMeter.h
//
// A small trailing-time-window bitrate averager for the live VBR readout. It is
// the shared engine-room behind every variable-rate decoder's
// currentBitrateKbps(): the decoder converts its native facts into a stream of
// (bits, seconds) pairs, one per decoded frame/packet, and the meter reports the
// average kbps over roughly the last `kWindowSeconds` of decoded audio.
//
// WHY A WINDOW, NOT A RAW PER-FRAME VALUE. A single frame's bitrate is jittery
// (VBR frame sizes swing hard frame to frame), so a raw readout would flicker. A
// trailing window of fixed DURATION (not a fixed frame count) gives a stable,
// codec-independent "current bitrate" with the same feel as foobar2000's display,
// and reads correctly whether a codec emits ~10 frames/sec (FLAC) or ~38 (MP3).
//
// THREADING. This is plain, lock-free, single-thread state. Each decoder owns one
// in its Impl and touches it only from read()/seek(), which the engine calls on
// the engine thread and never concurrently, so no synchronization is needed here.
//
// SEEDING. reset(nominal) primes the meter with the decoder's nominal/average
// bitrate so value() reads sensibly before any audio has been fed and immediately
// after a seek (where the pre-seek window is stale and must be discarded).

#pragma once

#include <cmath>
#include <cstdint>
#include <deque>

namespace rawform::audio {

class LiveBitrateMeter {
public:
    /// Discard the window and seed the fallback with the nominal bitrate. Call
    /// once at open with sourceInfo().bitrateKbps, and again on every seek so the
    /// pre-seek samples do not bleed into the post-seek readout.
    void reset(std::uint32_t nominalKbps) {
        m_samples.clear();
        m_bits        = 0.0;
        m_seconds     = 0.0;
        m_nominalKbps = nominalKbps;
    }

    /// Feed one decoded unit: `bits` compressed bits that produced `seconds` of
    /// audio. A decoder builds this pair from whatever it has natively, e.g.
    /// bytesConsumed*8 over framesProduced/sampleRate (FLAC, FFmpeg), or
    /// frameBitrateKbps*1000*seconds over seconds (MP3). A non-positive duration
    /// or a negative bit count is ignored so a stray zero-duration frame cannot
    /// corrupt the sums.
    void push(double bits, double seconds) {
        if (seconds <= 0.0 || bits < 0.0) {
            return;
        }
        m_samples.push_back(Sample{ bits, seconds });
        m_bits    += bits;
        m_seconds += seconds;

        // Evict from the front while dropping the oldest sample would still leave
        // at least kWindowSeconds of audio covered, so the window holds roughly
        // kWindowSeconds (a touch more, never less). One sample is always kept so
        // value() has something to average.
        while (m_samples.size() > 1 &&
               m_seconds - m_samples.front().seconds >= kWindowSeconds) {
            m_bits    -= m_samples.front().bits;
            m_seconds -= m_samples.front().seconds;
            m_samples.pop_front();
        }
    }

    /// Average kbps over the trailing window, or the seeded nominal when nothing
    /// has been fed yet. Rounded to the nearest kbps.
    [[nodiscard]] std::uint32_t value() const {
        if (m_seconds <= 0.0) {
            return m_nominalKbps;
        }
        return static_cast<std::uint32_t>(std::round(m_bits / m_seconds / 1000.0));
    }

private:
    struct Sample {
        double bits;
        double seconds;
    };

    /// Window length. ~1 s gives a steady, foobar-like readout; shorten for a more
    /// reactive display, lengthen for a calmer one. The single tuning knob here.
    static constexpr double kWindowSeconds = 1.0;

    std::deque<Sample> m_samples;
    double             m_bits        = 0.0;  ///< running sum of window bits
    double             m_seconds     = 0.0;  ///< running sum of window seconds
    std::uint32_t      m_nominalKbps = 0;    ///< fallback before the window fills
};

}  // namespace rawform::audio
