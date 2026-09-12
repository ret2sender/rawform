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

// RingBufferTest.cpp
//
// Standalone, dependency-free unit test for RingBuffer. No GoogleTest, no
// Catch2: a tiny CHECK macro tallies failures and main() returns non-zero if
// any fired, which is all CTest needs. Build it with the lib via the audio
// CMakeLists, then run it directly or through `ctest`, and crucially run the
// concurrent case under ThreadSanitizer (configure with
// -DRAWFORM_SANITIZE=thread).
//
// What is covered, the acceptance criteria:
//   - construction and empty/full accounting
//   - exact counter tracking for writableFrames()/readableFrames()
//   - partial write near-full and partial read near-empty return the real count
//   - wrap-around correctness, exercised hard with a NON power-of-two capacity
//     so the (pos % capacity) path is precisely the thing under test
//   - reset() empties and leaves both indices coherent
//   - a concurrent producer/consumer soak that verifies every sample arrives
//     exactly once, in order, and uncorrupted (the ThreadSanitizer target)

#include "rawform/audio/Types.h"  // included to confirm it compiles standalone
#include "rawform/audio/RingBuffer.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

using rawform::audio::RingBuffer;

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

// Deterministic, bit-exact sample generator. We encode an absolute sample index
// as its low 24 bits cast to float. Any integer below 2^24 is exactly
// representable in float32, so the consumer can compare with operator== and a
// single mismatch proves corruption, a drop, a duplicate, or a reorder. Using a
// per-SAMPLE counter (not per-frame) also exercises channel interleaving.
inline float enc(std::uint64_t sampleIndex) noexcept {
    return static_cast<float>(sampleIndex & 0xFFFFFFu);
}

// ---------------------------------------------------------------------------
// Construction and empty/full accounting.
void testConstructionAndAccounting() {
    RingBuffer rb(8, 2);
    CHECK(rb.channels() == 2);
    CHECK(rb.readableFrames() == 0);
    CHECK(rb.writableFrames() == 8);
}

// ---------------------------------------------------------------------------
// A scripted sequence pinning writableFrames()/readableFrames() at each step.
void testScriptedCounters() {
    RingBuffer rb(8, 1);
    std::vector<float> buf(64, 0.0f);

    CHECK(rb.writableFrames() == 8 && rb.readableFrames() == 0);

    CHECK(rb.write(buf.data(), 3) == 3);
    CHECK(rb.writableFrames() == 5 && rb.readableFrames() == 3);

    CHECK(rb.read(buf.data(), 2) == 2);
    CHECK(rb.writableFrames() == 7 && rb.readableFrames() == 1);

    // Free space is 7, so a request for 6 is accepted in full.
    CHECK(rb.write(buf.data(), 6) == 6);
    CHECK(rb.writableFrames() == 1 && rb.readableFrames() == 7);

    // Only 1 frame of space remains; a request for 4 yields 1.
    CHECK(rb.write(buf.data(), 4) == 1);
    CHECK(rb.writableFrames() == 0 && rb.readableFrames() == 8);

    // Buffer full: a further write yields 0.
    CHECK(rb.write(buf.data(), 1) == 0);
}

// ---------------------------------------------------------------------------
// Partial write near full returns the real (clamped) count and never overruns.
void testPartialWriteNearFull() {
    RingBuffer rb(4, 2);
    std::vector<float> in(6 * 2, 1.0f);
    // Free space is 4 frames; a 6-frame write is clamped to 4.
    CHECK(rb.write(in.data(), 6) == 4);
    CHECK(rb.writableFrames() == 0 && rb.readableFrames() == 4);
}

// ---------------------------------------------------------------------------
// Partial read near empty returns the real count and leaves the buffer empty.
void testPartialReadNearEmpty() {
    RingBuffer rb(8, 2);
    std::vector<float> in(8 * 2, 0.0f);
    std::vector<float> out(8 * 2, 0.0f);

    CHECK(rb.write(in.data(), 3) == 3);
    // Only 3 frames available; asking for 10 returns 3 and empties the buffer.
    CHECK(rb.read(out.data(), 10) == 3);
    CHECK(rb.readableFrames() == 0);
    // Empty buffer: a further read returns 0.
    CHECK(rb.read(out.data(), 4) == 0);
}

// ---------------------------------------------------------------------------
// Wrap-around soak with a deliberately NON power-of-two capacity (5), so the
// modulo wrap path is exercised on essentially every operation. A long
// deterministic stream is pushed through in randomly sized chunks and every
// sample is verified on the way out.
void testWrapAroundNonPow2() {
    constexpr std::uint16_t kChannels   = 2;
    constexpr std::size_t   kCapacity   = 5;
    constexpr std::uint64_t kTotalFrames = 20000;

    RingBuffer rb(kCapacity, kChannels);

    std::mt19937 prng(12345);
    std::uniform_int_distribution<std::size_t> chunkDist(1, 7);

    std::vector<float> wbuf(7 * kChannels);
    std::vector<float> rbuf(7 * kChannels);

    std::uint64_t produced = 0;  // absolute SAMPLE index next to be written
    std::uint64_t consumed = 0;  // absolute SAMPLE index next expected on read
    std::uint64_t framesWritten = 0;
    std::uint64_t framesRead    = 0;

    while (framesRead < kTotalFrames) {
        // Producer step (single thread, so we interleave the two roles by hand).
        if (framesWritten < kTotalFrames) {
            std::size_t want = chunkDist(prng);
            if (want > kTotalFrames - framesWritten) {
                want = static_cast<std::size_t>(kTotalFrames - framesWritten);
            }
            for (std::size_t i = 0; i < want * kChannels; ++i) {
                wbuf[i] = enc(produced + i);
            }
            const std::size_t n = rb.write(wbuf.data(), want);
            produced += static_cast<std::uint64_t>(n) * kChannels;
            framesWritten += n;
        }

        // Consumer step.
        std::size_t want = chunkDist(prng);
        if (want > kTotalFrames - framesRead) {
            want = static_cast<std::size_t>(kTotalFrames - framesRead);
        }
        const std::size_t n = rb.read(rbuf.data(), want);
        for (std::size_t i = 0; i < n * kChannels; ++i) {
            CHECK(rbuf[i] == enc(consumed + i));
        }
        consumed += static_cast<std::uint64_t>(n) * kChannels;
        framesRead += n;
    }

    CHECK(framesWritten == kTotalFrames);
    CHECK(framesRead == kTotalFrames);
}

// ---------------------------------------------------------------------------
// reset() empties the buffer and leaves both indices coherent for reuse.
void testReset() {
    RingBuffer rb(8, 2);
    std::vector<float> in(8 * 2, 0.0f);
    std::vector<float> out(8 * 2, 0.0f);
    for (std::size_t i = 0; i < 5 * 2; ++i) {
        in[i] = enc(i);
    }

    CHECK(rb.write(in.data(), 5) == 5);
    CHECK(rb.readableFrames() == 5);

    rb.reset();
    CHECK(rb.readableFrames() == 0);
    CHECK(rb.writableFrames() == 8);

    // After reset the buffer must behave like new: write and read fresh data.
    for (std::size_t i = 0; i < 4 * 2; ++i) {
        in[i] = enc(1000 + i);
    }
    CHECK(rb.write(in.data(), 4) == 4);
    CHECK(rb.read(out.data(), 4) == 4);
    for (std::size_t i = 0; i < 4 * 2; ++i) {
        CHECK(out[i] == enc(1000 + i));
    }
}

// ---------------------------------------------------------------------------
// The real ThreadSanitizer target. One producer thread and one consumer thread
// run flat out, exchanging a long deterministic stream through a small buffer.
// Small is on purpose: it keeps both threads contending at the full/empty edges
// and wrapping constantly. The consumer verifies every sample, so any race that
// tears, drops, duplicates, or reorders data surfaces as a value mismatch in
// addition to whatever TSan reports on the access pattern itself.
void testConcurrentSpsc() {
    constexpr std::uint16_t kChannels    = 2;
    constexpr std::size_t   kCapacity    = 4096;
    constexpr std::uint64_t kTotalFrames = 2'000'000;  // 4M samples; tune freely
    constexpr std::size_t   kMaxChunk    = 1024;

    RingBuffer rb(kCapacity, kChannels);
    std::atomic<bool>          mismatch{false};
    std::atomic<std::uint64_t> firstBadIndex{0};

    std::thread producer([&] {
        std::mt19937 prng(0xC0FFEEu);
        std::uniform_int_distribution<std::size_t> chunkDist(1, kMaxChunk);
        std::vector<float> buf(kMaxChunk * kChannels);
        std::uint64_t produced      = 0;  // absolute sample index
        std::uint64_t framesWritten = 0;

        while (framesWritten < kTotalFrames) {
            std::size_t want = chunkDist(prng);
            if (want > kTotalFrames - framesWritten) {
                want = static_cast<std::size_t>(kTotalFrames - framesWritten);
            }
            // Fill the whole chunk once from the current sample base; partial
            // writes simply re-offset into this buffer, so the encoded value at
            // any position stays consistent with its absolute sample index.
            for (std::size_t i = 0; i < want * kChannels; ++i) {
                buf[i] = enc(produced + i);
            }
            std::size_t off = 0;
            while (off < want) {
                const std::size_t n =
                    rb.write(buf.data() + off * kChannels, want - off);
                if (n == 0) {
                    std::this_thread::yield();  // buffer full; let consumer run
                    continue;
                }
                off += n;
                produced += static_cast<std::uint64_t>(n) * kChannels;
                framesWritten += n;
            }
        }
    });

    std::thread consumer([&] {
        std::mt19937 prng(0xBEEFu);
        std::uniform_int_distribution<std::size_t> chunkDist(1, kMaxChunk);
        std::vector<float> buf(kMaxChunk * kChannels);
        std::uint64_t consumed   = 0;  // absolute sample index expected next
        std::uint64_t framesRead = 0;

        while (framesRead < kTotalFrames) {
            std::size_t want = chunkDist(prng);
            if (want > kTotalFrames - framesRead) {
                want = static_cast<std::size_t>(kTotalFrames - framesRead);
            }
            const std::size_t n = rb.read(buf.data(), want);
            if (n == 0) {
                std::this_thread::yield();  // buffer empty; let producer run
                continue;
            }
            for (std::size_t i = 0; i < n * kChannels; ++i) {
                if (buf[i] != enc(consumed + i)) {
                    if (!mismatch.exchange(true)) {
                        firstBadIndex.store(consumed + i);
                    }
                }
            }
            consumed += static_cast<std::uint64_t>(n) * kChannels;
            framesRead += n;
        }
    });

    producer.join();
    consumer.join();

    CHECK(!mismatch.load());
    if (mismatch.load()) {
        std::fprintf(stderr, "  first mismatch at sample index %llu\n",
                     static_cast<unsigned long long>(firstBadIndex.load()));
    }
}

}  // namespace

int main() {
    testConstructionAndAccounting();
    testScriptedCounters();
    testPartialWriteNearFull();
    testPartialReadNearEmpty();
    testWrapAroundNonPow2();
    testReset();
    testConcurrentSpsc();

    if (g_failures == 0) {
        std::puts("RingBufferTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "RingBufferTest: %d check(s) failed\n", g_failures);
    return 1;
}
