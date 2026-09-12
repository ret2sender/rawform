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

// RingBuffer.h
//
// Lock-free single-producer / single-consumer (SPSC) ring buffer holding
// interleaved float32 frames. This is the buffering boundary between the
// engine/decode thread (the single producer, thread 2) and the sink RT thread
// (the single consumer, thread 3). It is the only data structure those two
// threads share, and the entire point of it is to let them hand off audio with
// no mutex and no allocation on either side.
//
// Concurrency contract:
//   - Exactly one thread may call write()/writableFrames() (the producer).
//   - Exactly one thread may call read()/readableFrames() (the consumer).
//   - Those two roles may run fully concurrently; the handoff is lock-free.
//   - read() is real-time safe: no allocation, no lock, no system call, no I/O.
//   - reset() is NOT concurrent with anything. It is a coordinated flush, only
//     ever called while both the producer and consumer are parked (during a
//     stop or seek), so it needs no synchronization of its own.
//
// Sizing: capacity is fixed at construction, expressed in frames. The Engine
// computes that frame count from a target latency and the track's format (that
// sizing logic lives in the Engine); here we simply honor whatever
// capacityFrames we are handed, exactly, with no rounding.
//
// Wrap arithmetic: the write/read positions are monotonically increasing
// absolute frame counts that never wrap. The physical storage offset is
// (pos % capacity), computed at most twice per call (once per contiguous
// segment), not once per sample. We accept that single integer division per
// call deliberately: it lets capacity be exactly the requested value rather
// than a rounded-up power of two, and at block-transfer granularity its cost is
// negligible next to the memcpy beside it.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace rawform::audio {

class RingBuffer {
public:
    /// capacityFrames: usable size in frames; honored exactly, no rounding.
    /// channels: samples per frame; the stored float count is capacity*channels.
    RingBuffer(std::size_t capacityFrames, std::uint16_t channels);

    /// Not copyable or movable: it owns a heap block and two atomics, and a live
    /// buffer is shared by reference between two threads. Copying or moving it
    /// out from under them makes no sense.
    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&)                 = delete;
    RingBuffer& operator=(RingBuffer&&)      = delete;

    /// Producer. Copies up to `frames` frames from src and returns the number
    /// actually accepted, which is less than `frames` when the buffer fills. Call
    /// only from the single producer thread. noexcept for symmetry with the
    /// consumer pair: all four hot-path methods are equally non-throwing (index
    /// math and memcpy), and the contract reads cleaner stated on both sides.
    std::size_t write(const float* src, std::size_t frames) noexcept;

    /// Producer-side free space, in frames. Under concurrency this is a lower
    /// bound (the consumer can only free more), so it is always safe to act on.
    [[nodiscard]] std::size_t writableFrames() const noexcept;

    /// Consumer. Copies up to `frames` frames into dst and returns the number
    /// actually delivered, which is less than `frames` when the buffer empties.
    /// Real-time safe; call only from the single consumer thread.
    std::size_t read(float* dst, std::size_t frames) noexcept;

    /// Consumer-side available data, in frames. Under concurrency this is a lower
    /// bound (the producer can only add more), so it is always safe to act on.
    [[nodiscard]] std::size_t readableFrames() const noexcept;

    /// Coordinated flush: empties the buffer. Precondition: no concurrent
    /// write/read; both threads must be parked. See the contract note above.
    void reset();

    /// Channel count, fixed at construction.
    [[nodiscard]] std::uint16_t channels() const noexcept;

private:
    /// 64 is hardcoded rather than std::hardware_destructive_interference_size,
    /// whose support and value are uneven across the toolchains this builds on.
    static constexpr std::size_t kCacheLine = 64;

    /// Fixed configuration, written once in the constructor and only read
    /// afterwards, so both threads may read it without synchronization.
    const std::size_t        m_capacityFrames;
    const std::uint16_t      m_channels;
    std::unique_ptr<float[]> m_storage;  ///< capacityFrames * channels floats

    /// Absolute frame positions. Each is owned (written) by exactly one thread
    /// and read by both. They sit on separate cache lines so the producer storing
    /// m_writePos never dirties the line the consumer spins on for m_readPos, and
    /// vice versa: this is the false-sharing guard.
    alignas(kCacheLine) std::atomic<std::uint64_t> m_writePos{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> m_readPos{0};
};

}  // namespace rawform::audio
