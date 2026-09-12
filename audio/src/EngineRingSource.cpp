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

// EngineRingSource.cpp
//
// Implementation of the engine's real-time surface. The consumer side (pull) is
// the part that runs on the sink's RT thread, so it is held to the strict
// contract: one ring read, one consumed-counter add, and on a short read either
// one relaxed counter add or one release store, with no lock, allocation, or
// system call. The producer side runs on the engine thread and is plain.
//
// The end-of-stream handshake:
//   - The producer writes its final frames into the ring (RingBuffer::write does
//     a release store of the write index) and ONLY THEN calls markInputExhausted,
//     a release store of m_inputExhausted. Any thread that later sees
//     m_inputExhausted == true via an acquire load has, by that same
//     synchronization, also seen every frame the producer wrote, the last block
//     included. There is no window where "exhausted" is visible but the tail is
//     not.
//   - pull() publishes m_finished only on a ZERO read while m_inputExhausted is
//     set. Exhausted implies the final write is visible, so a zero read under
//     exhausted means the ring is genuinely empty, not merely lagging. The final
//     partial block (a short, non-zero read) is therefore never mistaken for the
//     end; finished flips on the FOLLOWING pull, after that audio reached the
//     device.
//   - A short read while NOT exhausted is real mid-stream starvation, counted as
//     an underrun. End-of-stream drain is excluded by that same flag. The
//     gapless machinery leans on
//     this: during the brief overlap where a stitched track is opened at EOS and
//     the outgoing ring tail is still draining, the producer is momentarily not
//     writing; if the ring ran dry there it would be counted as an underrun (a
//     real, audible gap), NOT a false finished. On local files the open completes
//     well inside the tail runway, so this does not happen; slow-storage runway
//     is explicitly out of scope for the gapless design.
//
// The position. pull() advances m_totalConsumed by the
// real frames it just delivered, with a relaxed read-modify-write, because the
// RT thread is its only writer and the value only needs to be eventually visible
// to a progress readout and to the run loop's seam watch. The reported source
// frame is m_totalConsumed + m_playheadOffset, clamped at 0. The engine sets the
// offset (setPlayhead / reset / reconfigure while parked, rebasePositionAtSeam
// while running); the RT thread never touches it. Because the engine and the RT
// thread write different atomics, the seam rebase needs no parked window. See
// the header for the full rationale and the benign-skew note.

#include "EngineRingSource.h"

namespace rawform::audio {

// ---------------------------------------------------------------------------
EngineRingSource::EngineRingSource()  = default;
EngineRingSource::~EngineRingSource() = default;

// ---------------------------------------------------------------------------
// Consumer side, real-time safe. Guards a null ring defensively: in practice the
// sink is open (and therefore a ring exists) for the entire window pull() can be
// called, but a zero read on a null ring is the correct, cost-free degenerate
// answer if that ever changes.
std::size_t EngineRingSource::pull(float* out, std::size_t frames) noexcept {
    RingBuffer* ring = m_ring.get();
    if (ring == nullptr) {
        return 0;
    }

    const std::size_t got = ring->read(out, frames);

    // Advance the consumed counter by the REAL frames delivered, never the
    // zero-pad the sink will add. This is what makes the reported position
    // "frames heard" and what the run loop's seam watch counts against.
    if (got != 0) {
        m_totalConsumed.fetch_add(static_cast<std::uint64_t>(got),
                                  std::memory_order_relaxed);

        // Visualization tap (the spectrum feed). Read the source selector once,
        // relaxed (a one-block-stale choice is invisible, like the gain). The
        // PreGain tap captures the decoded block BEFORE the attenuation below, so
        // the display ignores the volume fader; it pushes here, untouched.
        const ScopeSource scope = m_scopeSource.load(std::memory_order_relaxed);
        const std::uint16_t ch  = ring->channels();
        if (scope == ScopeSource::PreGain) {
            m_scopeTap.push(out, got, ch);
        }

        // Master gain. One relaxed read of the gain, then scale the
        // frames just delivered. The unity fast path is the bit-perfect guarantee:
        // at exactly 1.0f we touch nothing, so 100%/unmuted output is byte-for-byte
        // the decoder's samples. Only an attenuating gain enters the multiply, and
        // it scales got*channels floats (the interleaved real frames); the sink's
        // later zero-pad is untouched. No branch on channels: ring->channels() is
        // an RT-safe noexcept read, and the loop bound is the exact float count.
        const float g = m_gain.load(std::memory_order_relaxed);
        if (g != 1.0f) {
            const std::size_t samples =
                got * static_cast<std::size_t>(ch);
            for (std::size_t i = 0; i < samples; ++i) {
                out[i] *= g;
            }
        }

        // The PostGain tap (the default) captures the block AFTER the multiply, so
        // the display follows the volume. At unity the multiply was skipped, so
        // `out` still holds the bit-perfect decoder samples and the tap sees them
        // unchanged, which is the whole point of the unity fast path extending to
        // the visualizer.
        if (scope == ScopeSource::PostGain) {
            m_scopeTap.push(out, got, ch);
        }
    }

    if (got < frames) {
        const bool exhausted = m_inputExhausted.load(std::memory_order_acquire);
        if (!exhausted) {
            // Producer still owes frames but the ring ran dry: genuine
            // starvation. Count the shortfall; the SINK zero-pads it.
            m_underrunFrames.fetch_add(static_cast<std::uint64_t>(frames - got),
                                       std::memory_order_relaxed);
        } else if (got == 0) {
            // Ring fully drained and input is done: playback is complete. This is
            // idempotent; every subsequent pull is also a zero read.
            m_finished.store(true, std::memory_order_release);
        }
        // The remaining case, exhausted and got > 0, is the final real block:
        // not an underrun, not yet finished. finished flips on the next pull.
    }
    return got;
}

// ---------------------------------------------------------------------------
// Producer side. reconfigure and reset both run only while the RT thread is
// parked (the sink closed or stopped), so the relaxed clears below race
// nothing: the RT thread will not read these flags, advance the consumed
// counter, or read the offset until the sink is started again, which
// happens-after these calls on the same engine thread.
void EngineRingSource::reconfigure(std::size_t  capacityFrames,
                                   std::uint16_t channels) {
    m_ring = std::make_unique<RingBuffer>(capacityFrames, channels);
    m_inputExhausted.store(false, std::memory_order_relaxed);
    m_finished.store(false, std::memory_order_relaxed);
    m_underrunFrames.store(0, std::memory_order_relaxed);
    m_totalWritten.store(0, std::memory_order_relaxed);
    m_totalConsumed.store(0, std::memory_order_relaxed);
    m_playheadOffset.store(0, std::memory_order_relaxed);  // new track begins at frame 0
    m_scopeTap.reset();  // a new track: drop the previous track's tail from the visualizer
}

void EngineRingSource::reset() noexcept {
    if (m_ring) {
        m_ring->reset();
    }
    m_inputExhausted.store(false, std::memory_order_relaxed);
    m_finished.store(false, std::memory_order_relaxed);
    m_underrunFrames.store(0, std::memory_order_relaxed);
    m_totalWritten.store(0, std::memory_order_relaxed);
    m_totalConsumed.store(0, std::memory_order_relaxed);
    m_playheadOffset.store(0, std::memory_order_relaxed);  // a fresh ring starts at frame 0
    m_scopeTap.reset();  // a seek or a fresh ring: drop stale pre-seek audio from the visualizer
}

void EngineRingSource::setPlayhead(std::uint64_t frame) noexcept {
    // Position-origin store so the reported frame reads `frame` now. Only valid
    // with the RT thread parked (the seek window), where m_totalConsumed is
    // stable; after the paired reset() it is 0, so the offset is just `frame`.
    // Release so a paired sink->start() publishes the new origin to the RT thread.
    const auto consumed =
        static_cast<std::int64_t>(m_totalConsumed.load(std::memory_order_relaxed));
    m_playheadOffset.store(static_cast<std::int64_t>(frame) - consumed,
                           std::memory_order_release);
}

void EngineRingSource::rebasePositionAtSeam(std::uint64_t thresholdFrame) noexcept {
    // offset = -threshold makes the reported frame equal (consumed - threshold),
    // which is 0 exactly when consumed reaches the boundary and climbs into the
    // new track. The run loop calls this only once consumed has already crossed
    // the threshold, so the sum is non-negative in practice; the reader clamps
    // regardless. Engine-written only; safe with the RT thread running because the
    // RT thread writes m_totalConsumed, never this offset.
    m_playheadOffset.store(-static_cast<std::int64_t>(thresholdFrame),
                           std::memory_order_release);
}

std::size_t EngineRingSource::write(const float* src, std::size_t frames) noexcept {
    if (!m_ring) {
        return 0;
    }
    const std::size_t n = m_ring->write(src, frames);
    m_totalWritten.fetch_add(static_cast<std::uint64_t>(n),
                             std::memory_order_relaxed);
    return n;
}

std::size_t EngineRingSource::writableFrames() const noexcept {
    return m_ring ? m_ring->writableFrames() : 0;
}

std::size_t EngineRingSource::bufferedFrames() const noexcept {
    return m_ring ? m_ring->readableFrames() : 0;
}

void EngineRingSource::markInputExhausted() noexcept {
    m_inputExhausted.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Master gain. A relaxed store from the controlling thread paired with the
// relaxed load in pull(); no ordering need beyond eventual visibility (a single
// quantum at the old gain is inaudible). Independent of the ring: setting a gain
// before any track is open simply parks the value for the first pull to read. It
// is the engine, via Engine::setVolumeGain, that calls this; nothing else does.
void EngineRingSource::setGain(float linearGain) noexcept {
    m_gain.store(linearGain, std::memory_order_relaxed);
}

float EngineRingSource::gain() const noexcept {
    return m_gain.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Visualization tap. copyScope is the GUI-thread reader (forwarded from
// Engine::copyScope, called on the visualizer's timer): it just hands through to
// the tap's lock-free snapshot. setScopeSource / scopeSource are the
// controlling-thread knob and its read-back, a relaxed store/load pair matching
// the gain treatment: a one-block-stale choice is invisible, and nothing else
// needs to be ordered against it. None of these touch the RT path beyond the
// single relaxed load pull() already does.
std::size_t EngineRingSource::copyScope(float* out, std::size_t frames) const noexcept {
    return m_scopeTap.copyLatest(out, frames);
}

void EngineRingSource::setScopeSource(ScopeSource source) noexcept {
    m_scopeSource.store(source, std::memory_order_relaxed);
}

ScopeSource EngineRingSource::scopeSource() const noexcept {
    return m_scopeSource.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Observers. Acquire on the flags the engine acts on so it sees a coherent view,
// relaxed on the pure counters that are either single-thread (m_totalWritten) or
// only need eventual visibility. m_totalConsumed is read with acquire to pair
// with the RT advance; the offset likewise, so a rebase that precedes a read is
// ordered before it.
bool EngineRingSource::inputExhausted() const noexcept {
    return m_inputExhausted.load(std::memory_order_acquire);
}

bool EngineRingSource::finished() const noexcept {
    return m_finished.load(std::memory_order_acquire);
}

std::uint64_t EngineRingSource::underrunFrames() const noexcept {
    return m_underrunFrames.load(std::memory_order_relaxed);
}

bool EngineRingSource::configured() const noexcept {
    return m_ring != nullptr;
}

std::uint64_t EngineRingSource::totalFramesWritten() const noexcept {
    // Producer-thread-only: relaxed is sufficient (no cross-thread ordering need).
    return m_totalWritten.load(std::memory_order_relaxed);
}

std::uint64_t EngineRingSource::totalFramesConsumed() const noexcept {
    return m_totalConsumed.load(std::memory_order_acquire);
}

std::uint64_t EngineRingSource::playheadFrames() const noexcept {
    const auto consumed =
        static_cast<std::int64_t>(m_totalConsumed.load(std::memory_order_acquire));
    const std::int64_t offset = m_playheadOffset.load(std::memory_order_acquire);
    const std::int64_t pos    = consumed + offset;
    return pos < 0 ? 0u : static_cast<std::uint64_t>(pos);
}

}  // namespace rawform::audio
