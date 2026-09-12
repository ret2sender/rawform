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

// tools/tsan_spsc_min.cpp
//
// DIAGNOSTIC, not part of the build. The RingBuffer's release/acquire SPSC
// handoff reduced to its axioms: two monotonic 64-bit counters, a plain
// payload array, one frame per operation. A correct ThreadSanitizer model
// must not warn on this program; the C++ memory model guarantees every
// payload access is ordered by the release store / acquire load pairing on
// the counters (and every producer store is itself a release, so no release
// sequence subtleties apply).
//
// Two build variants split the suspect stack:
//
//   default          : the counters are std::atomic<std::uint64_t>, i.e. the
//                      libstdc++ <atomic> header implements the operations.
//   -DUSE_BUILTINS=1 : the counters are plain std::uint64_t accessed ONLY via
//                      the __atomic_* builtins with explicit orders, i.e. the
//                      standard library headers are out of the picture.
//
// Interpretation matrix on a machine where the RingBuffer test warns:
//   default warns, builtins clean  -> the libstdc++ <atomic> implementation
//                                     mismaps an ordering (header bug).
//   both warn                      -> the sanitizer runtime's happens-before
//                                     model is at fault (runtime bug).
//   both clean                     -> neither; the RingBuffer difference lies
//                                     elsewhere and the hunt continues.
//
// Build (mirror the project's sanitizer profile):
//   g++     -std=c++20 -O0 -g -fsanitize=thread -fno-omit-frame-pointer \
//           tsan_spsc_min.cpp -o spsc_std && ./spsc_std
//   g++     -DUSE_BUILTINS=1 ... -o spsc_builtin && ./spsc_builtin
//   clang++ (same two lines)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#if !defined(USE_BUILTINS)
#define USE_BUILTINS 0
#endif

#if !USE_BUILTINS
#include <atomic>
#endif

namespace {

constexpr std::size_t   kCap   = 4096;
constexpr std::uint64_t kTotal = 2'000'000;

float payload[kCap];

#if USE_BUILTINS
std::uint64_t wpos = 0;
std::uint64_t rpos = 0;

std::uint64_t loadRelaxed(const std::uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}
std::uint64_t loadAcquire(const std::uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
void storeRelease(std::uint64_t* p, std::uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}
#else
std::atomic<std::uint64_t> wpos{0};
std::atomic<std::uint64_t> rpos{0};

std::uint64_t loadRelaxed(const std::atomic<std::uint64_t>* p) {
    return p->load(std::memory_order_relaxed);
}
std::uint64_t loadAcquire(const std::atomic<std::uint64_t>* p) {
    return p->load(std::memory_order_acquire);
}
void storeRelease(std::atomic<std::uint64_t>* p, std::uint64_t v) {
    p->store(v, std::memory_order_release);
}
#endif

}  // namespace

int main() {
    std::thread producer([] {
        std::uint64_t produced = 0;
        while (produced < kTotal) {
            const std::uint64_t w = loadRelaxed(&wpos);
            const std::uint64_t r = loadAcquire(&rpos);
            if (kCap - static_cast<std::size_t>(w - r) == 0) {
                std::this_thread::yield();
                continue;
            }
            const float v = static_cast<float>(w & 0xFFFFu);
            std::memcpy(&payload[w % kCap], &v, sizeof(float));
            storeRelease(&wpos, w + 1);
            ++produced;
        }
    });

    std::thread consumer([] {
        std::uint64_t consumed = 0;
        float         sink     = 0.0f;
        while (consumed < kTotal) {
            const std::uint64_t r = loadRelaxed(&rpos);
            const std::uint64_t w = loadAcquire(&wpos);
            if (w - r == 0) {
                std::this_thread::yield();
                continue;
            }
            float v = 0.0f;
            std::memcpy(&v, &payload[r % kCap], sizeof(float));
            sink += v;  // keep the read observable
            storeRelease(&rpos, r + 1);
            ++consumed;
        }
        if (sink < 0.0f) {
            std::puts("impossible");
        }
    });

    producer.join();
    consumer.join();
    std::puts("minimal SPSC done");
    return 0;
}
