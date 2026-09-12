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

// ScopeTap.h
//
// The visualization feed: a tiny, lock-free, single-producer / single-consumer
// overwrite ring of MONO float samples, sized to hold a little more than one FFT
// window. It is the engine's contribution to the spectrum analyzer, and it is
// deliberately its own small header (in the spirit of LiveBitrateMeter) so the
// one object the real-time pull touches for visualization is auditable in
// isolation, separate from the position counters and the end-of-stream handshake.
//
// WHO CALLS WHAT, and why it is safe.
//   - The PRODUCER is the sink's real-time thread, inside EngineRingSource::pull().
//     It calls push() once per delivered block: it downmixes the interleaved
//     float32 frames to a single mono channel and appends them, then advances a
//     monotonic write counter with a release store. push() never locks, never
//     allocates (the ring is allocated once at construction), and makes no system
//     call, so it honors the strict pull() contract.
//   - The CONSUMER is the GUI thread, by way of a 60 Hz visualizer timer. It calls
//     copyLatest() to take a consistent snapshot of the most recent N mono
//     samples. It reads the write counter with an acquire load, copies the window
//     ending at it, then re-reads the counter; if the producer could have lapped
//     the copied window in between, it retries a bounded number of times. With the
//     capacity comfortably larger than the window this never actually fires (the
//     producer would have to write capacity-minus-window samples inside a
//     microsecond-scale memcpy), and a visualizer tolerates a torn frame anyway,
//     so the retry is pure belt-and-suspenders.
//
// WHY AN OVERWRITE RING (and not the SPSC RingBuffer the engine already has). The
// engine's RingBuffer is a back-pressured FIFO: the producer must not overrun the
// consumer, because every sample matters to playback. The visualizer is the
// opposite contract: only the MOST RECENT window matters, the consumer samples far
// slower than the producer, and dropped history is correct, not a fault. An
// overwrite ring expresses exactly that: the producer never blocks or checks for
// space, it just writes and wraps, and the consumer always reads the freshest
// window. Mixing this lossy contract into the FIFO would muddy both, hence a
// separate, purpose-built structure.
//
// MONO, by design. The reference display is a single spectrum, not an L/R split,
// so the tap downmixes at the source. That halves both the stored data and the
// analyzer's FFT work, and it makes the buffer channel-count agnostic: a track
// that changes channel count between songs needs no reconfiguration here.
//
// RESET. reset() zeroes the write counter only; it does not scrub the storage,
// because copyLatest() reads no further back than the counter allows, so stale
// samples below it are never observed. The engine calls it while the RT thread is
// parked (a fresh track or a seek), so the next analyzed window starts clean
// instead of briefly showing the tail of the previous track or the pre-seek audio.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rawform::audio {

class ScopeTap {
public:
    /// Allocate the ring once. The requested capacity is rounded UP to a power of
    /// two so the index wrap is a single mask, and floored to a sane minimum so a
    /// pathological request cannot produce a zero-length ring. The default holds
    /// 8192 mono samples, roughly 4x a 2048-sample FFT window, which is the
    /// headroom that makes copyLatest's lap check unreachable in practice.
    explicit ScopeTap(std::size_t requestedCapacity = 8192)
        : m_capacity(roundUpPow2(requestedCapacity < 64 ? 64 : requestedCapacity)),
          m_mask(m_capacity - 1),
          m_ring(m_capacity, 0.0f) {}

    /// PRODUCER (RT thread). Downmix `frames` interleaved float32 frames of
    /// `channels` samples each to mono and append them to the ring, then publish
    /// the new write position with a release store so the consumer's matching
    /// acquire load sees the samples. Single-producer: it snapshots the counter
    /// with a relaxed load (no other thread writes it), fills, then release-stores
    /// the advanced value. A zero channel count or null pointer is a no-op guard;
    /// it never happens on the live path but keeps the function total. Real-time
    /// safe: no lock, no allocation, no system call.
    void push(const float* interleaved, std::size_t frames,
              std::uint16_t channels) noexcept {
        if (interleaved == nullptr || frames == 0 || channels == 0) {
            return;
        }
        const std::uint64_t base = m_written.load(std::memory_order_relaxed);
        const float invCh = 1.0f / static_cast<float>(channels);
        for (std::size_t f = 0; f < frames; ++f) {
            // Downmix one frame: the mean of its channel samples. The mean (not
            // the sum) keeps a mono source and a dual-mono stereo source at the
            // same level, which is what a level-calibrated display wants.
            const float* frame = interleaved + f * static_cast<std::size_t>(channels);
            float acc = 0.0f;
            for (std::uint16_t c = 0; c < channels; ++c) {
                acc += frame[c];
            }
            m_ring[static_cast<std::size_t>((base + f)) & m_mask] = acc * invCh;
        }
        m_written.store(base + frames, std::memory_order_release);
    }

    /// CONSUMER (GUI thread). Copy the `count` most recent mono samples into `out`,
    /// oldest first, so out[count-1] is the very latest sample. When fewer than
    /// `count` samples have ever been produced (the first moments of playback or
    /// just after a reset), the leading shortfall is zero-filled, so `out` is always
    /// fully written with exactly `count` samples and the analyzer can window it
    /// unconditionally. `count` is clamped to the capacity. Returns `count` (after
    /// the clamp). Lock-free; safe to call concurrently with push().
    std::size_t copyLatest(float* out, std::size_t count) const noexcept {
        if (out == nullptr || count == 0) {
            return 0;
        }
        if (count > m_capacity) {
            count = m_capacity;
        }

        // A bounded seqlock-style snapshot: read the publish counter, copy, then
        // confirm the producer did not lap the window we just read. Two retries is
        // ample given the capacity headroom; on the (unreachable in practice) final
        // miss we accept the slightly torn window, which is invisible in a 60 Hz
        // bar display.
        for (int attempt = 0; attempt < 3; ++attempt) {
            const std::uint64_t w = m_written.load(std::memory_order_acquire);

            const std::uint64_t avail = (w < count) ? w : count;
            const auto lead = static_cast<std::size_t>(count - avail);

            for (std::size_t i = 0; i < lead; ++i) {
                out[i] = 0.0f;
            }
            const std::uint64_t start = w - avail;
            for (std::uint64_t i = 0; i < avail; ++i) {
                out[lead + static_cast<std::size_t>(i)] =
                    m_ring[static_cast<std::size_t>(start + i) & m_mask];
            }

            // If the producer has advanced by no more than the slack between the
            // capacity and the window, none of the samples we copied were
            // overwritten mid-copy, so the snapshot is coherent.
            const std::uint64_t w2 = m_written.load(std::memory_order_acquire);
            if (w2 - w <= (m_capacity - count)) {
                return count;
            }
        }
        return count;
    }

    /// Discard the history: the next copyLatest() zero-pads until fresh samples
    /// arrive. The engine calls this while the RT thread is parked (a new track or a
    /// seek), so the relaxed store races no PRODUCER; the GUI reader, however, is
    /// NOT parked and can run copyLatest() concurrently with this store. That is
    /// also safe, for two stacked reasons: a reset observed between a reader's two
    /// counter loads makes the w2 - w consistency check fail (unsigned wrap yields
    /// a huge value) and the copy retries, and after the restart avail = min(w,
    /// count) means only slots written since the reset are ever read, so the
    /// un-scrubbed stale storage below the new counter is never observed. Cheap:
    /// one atomic store, no scrub of the storage.
    void reset() noexcept { m_written.store(0, std::memory_order_relaxed); }

    /// Storage size in mono samples (a power of two). The visualizer reads this to
    /// clamp its window to something the ring can actually hold.
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }

private:
    /// Smallest power of two >= n (n >= 1). Used once at construction.
    static std::size_t roundUpPow2(std::size_t n) noexcept {
        std::size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    const std::size_t  m_capacity;  ///< power of two; storage length in mono samples
    const std::size_t  m_mask;      ///< m_capacity - 1, for the wrap
    std::vector<float> m_ring;      ///< allocated once; never resized after construction

    /// Monotonic count of mono samples ever written. The producer (RT thread) is
    /// its sole writer; the consumer (GUI thread) only reads it. On its own cache
    /// line so the consumer's frequent reads do not falsely share with anything the
    /// producer writes nearby, matching the false-sharing guards elsewhere in the
    /// engine. 64 is hardcoded for the same toolchain-portability reason the rest of
    /// the engine hardcodes it.
    alignas(64) std::atomic<std::uint64_t> m_written{0};  ///< W: RT  R: GUI
};

}  // namespace rawform::audio
