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

// RingBuffer.cpp
//
// Implementation of the lock-free SPSC ring buffer. The whole design rests on
// two monotonically increasing 64-bit counters and a release/acquire handoff:
//
//   - The producer owns m_writePos; the consumer owns m_readPos. Each side
//     loads its own index relaxed (no other thread writes it) and loads the
//     counterparty index acquire (to observe the other side's published
//     progress). Each side publishes its own advance with a release store.
//   - The release store of m_writePos pairs with the consumer's acquire load of
//     it: that pairing is what makes the freshly memcpy'd frames visible before
//     the consumer is permitted to read them. The release store of m_readPos
//     pairs with the producer's acquire load, so consumed slots are safe to
//     overwrite only after the read has completed.
//
// 64-bit counters never wrap in any realistic run: at 192 kHz, 2^64 frames is
// on the order of three million years, so the (pos % capacity) offset math can
// treat the counters as effectively unbounded.

#include "rawform/audio/RingBuffer.h"

#include <cassert>
#include <cstring>  // std::memcpy

namespace rawform::audio {

RingBuffer::RingBuffer(std::size_t capacityFrames, std::uint16_t channels)
    : m_capacityFrames(capacityFrames)
    , m_channels(channels)
    , m_storage(std::make_unique<float[]>(
          capacityFrames * static_cast<std::size_t>(channels))) {
    // A zero-frame or zero-channel buffer is a programming error: it can carry
    // no audio, and every size computation below would degenerate. We catch it
    // in debug builds; release builds simply hold an unusable empty block.
    assert(capacityFrames > 0 && "RingBuffer capacity must be non-zero");
    assert(channels > 0 && "RingBuffer channels must be non-zero");
}

std::size_t RingBuffer::write(const float* src, std::size_t frames) noexcept {
    // Producer side. Own index relaxed, counterparty acquire (see file header).
    const std::uint64_t w = m_writePos.load(std::memory_order_relaxed);
    const std::uint64_t r = m_readPos.load(std::memory_order_acquire);

    const auto used = static_cast<std::size_t>(w - r);
    const std::size_t freeFrames = m_capacityFrames - used;

    // Accept as much as fits. A short write is normal when the buffer is nearly
    // full; returning the real count lets the caller retry the remainder.
    const std::size_t n = frames < freeFrames ? frames : freeFrames;
    if (n == 0) {
        return 0;
    }

    // Split the copy at the physical end of storage. When the write straddles
    // the wrap point it becomes two contiguous memcpys; otherwise the second is
    // empty.
    const auto head = static_cast<std::size_t>(w % m_capacityFrames);
    const std::size_t framesToEnd = m_capacityFrames - head;
    const std::size_t firstFrames = n < framesToEnd ? n : framesToEnd;
    const std::size_t secondFrames = n - firstFrames;

    const std::size_t ch = m_channels;
    std::memcpy(m_storage.get() + head * ch,
                src,
                firstFrames * ch * sizeof(float));
    if (secondFrames != 0) {
        std::memcpy(m_storage.get(),
                    src + firstFrames * ch,
                    secondFrames * ch * sizeof(float));
    }

    // Publish. This release store is the single point that hands the new frames
    // to the consumer; everything written above happens-before its acquire load.
    m_writePos.store(w + n, std::memory_order_release);
    return n;
}

std::size_t RingBuffer::read(float* dst, std::size_t frames) noexcept {
    // Consumer side, real-time safe: atomic loads/stores, integer math, and
    // memcpy only; no allocation, lock, or system call. Own index relaxed,
    // counterparty acquire.
    const std::uint64_t r = m_readPos.load(std::memory_order_relaxed);
    const std::uint64_t w = m_writePos.load(std::memory_order_acquire);

    const auto avail = static_cast<std::size_t>(w - r);
    const std::size_t n = frames < avail ? frames : avail;
    if (n == 0) {
        return 0;
    }

    const auto tail = static_cast<std::size_t>(r % m_capacityFrames);
    const std::size_t framesToEnd = m_capacityFrames - tail;
    const std::size_t firstFrames = n < framesToEnd ? n : framesToEnd;
    const std::size_t secondFrames = n - firstFrames;

    const std::size_t ch = m_channels;
    std::memcpy(dst,
                m_storage.get() + tail * ch,
                firstFrames * ch * sizeof(float));
    if (secondFrames != 0) {
        std::memcpy(dst + firstFrames * ch,
                    m_storage.get(),
                    secondFrames * ch * sizeof(float));
    }

    // Release the consumed slots back to the producer; our reads above
    // happen-before the producer's acquire load can let it overwrite them.
    m_readPos.store(r + n, std::memory_order_release);
    return n;
}

std::size_t RingBuffer::writableFrames() const noexcept {
    // Own index relaxed, counterparty acquire. The consumer can only advance
    // m_readPos (freeing space), so the value returned is a safe lower bound and
    // is exact whenever the buffer is quiescent.
    const std::uint64_t w = m_writePos.load(std::memory_order_relaxed);
    const std::uint64_t r = m_readPos.load(std::memory_order_acquire);
    return m_capacityFrames - static_cast<std::size_t>(w - r);
}

std::size_t RingBuffer::readableFrames() const noexcept {
    // Symmetric to writableFrames: the producer can only advance m_writePos
    // (adding data), so this is a safe lower bound, exact when quiescent.
    const std::uint64_t w = m_writePos.load(std::memory_order_acquire);
    const std::uint64_t r = m_readPos.load(std::memory_order_relaxed);
    return static_cast<std::size_t>(w - r);
}

void RingBuffer::reset() {
    // Coordinated flush only. The contract guarantees no producer or consumer is
    // running here (both parked during a stop or seek), so plain relaxed stores
    // suffice: the handshake that parked the threads, and the one that later
    // resumes them, supplies the necessary happens-before. Setting both indices
    // to zero empties the buffer and keeps the counters small; equal indices
    // means readable == 0 and writable == capacity.
    m_readPos.store(0, std::memory_order_relaxed);
    m_writePos.store(0, std::memory_order_relaxed);
}

std::uint16_t RingBuffer::channels() const noexcept {
    return m_channels;
}

}  // namespace rawform::audio
