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

// EngineRingSource.h
//
// The Engine's real-time surface, deliberately split into its own tiny pair of
// files so the one object the sink's RT thread touches is auditable in
// isolation. It owns the SPSC RingBuffer and the end-of-stream atomics, it
// implements IPullSource for the consumer side, and it exposes a producer side
// that the ENGINE thread (not a thread of its own) drives.
//
// Who calls what:
//   - RT / consumer thread (the sink): pull() ONLY. No lock, no allocation, no
//     system call; it drains the ring, advances the consumed counter by the real
//     frames it delivered, and zero-pads via the sink.
//   - Engine thread / producer: reconfigure(), reset(), write(), writableFrames(),
//     bufferedFrames(), markInputExhausted(), setPlayhead(), rebasePositionAtSeam(),
//     and the observers. These are called only on the engine thread. The three
//     that touch the ring's storage pointer or fully rebase the counters
//     (reconfigure, reset, setPlayhead) are called ONLY while the sink is closed
//     or stopped, i.e. while the RT thread is parked, so neither swapping the ring
//     nor zeroing the counters ever races a pull. The ONE exception is
//     rebasePositionAtSeam (see the position note below), which is engineered to
//     be safe with the RT thread running.
//
// The end-of-stream handshake: the producer publishes inputExhausted with a
// release store AFTER its final ring write, and pull() flips finished only on
// a zero read while exhausted, so no false "finished" is ever visible while
// tail frames are still in flight. The memory-ordering rationale lives in the
// .cpp. A GAPLESS stitch never sets inputExhausted, so finished never flips
// between stitched tracks; the handshake is reached only by the final track of
// a run, or by the stop / reconfigure / drain paths.
//
// The reported position is deliberately NOT a single rebased playhead. A
// single playhead would have two writers (pull() advancing it, the engine
// rebasing it to 0 at track start or to the target after a seek), so every
// rebase would need the RT thread parked; a gapless track change cannot park
// the RT thread. The position is therefore split into two values:
//   - m_totalConsumed: monotonic, advanced by pull() alone, NEVER rebased. The
//     RT thread is its sole writer.
//   - m_playheadOffset: a signed origin written ONLY by the engine thread.
// The reported source frame is clamp(m_totalConsumed + m_playheadOffset, 0). A
// fresh start or a seek (RT parked) zeroes the counters and sets the offset; a
// gapless seam (RT running) needs only to store a new offset (-threshold). That
// store is race-free precisely because the two threads write DIFFERENT
// atomics: the engine moves the origin while the RT thread keeps advancing the
// counter underneath it, with no shared write location. A position reader can
// momentarily observe a slightly skewed pair across the instant of a seam (off
// by at most one track length for a single sample), which is the same benign
// cross-atomic looseness already accepted for formatBits, and it self-corrects
// on the next read.
//
// m_totalWritten mirrors m_totalConsumed on the producer side: a monotonic count
// of frames accepted into the ring, in the SAME counting space (both are zeroed
// together by reconfigure/reset while the RT thread is parked). The engine snaps
// it at a track's end to get the seam threshold: the consumed-frame value at
// which the outgoing track has been fully heard and the next becomes audible.
//
// The master gain. This is the one place a master-volume multiply can
// live without spreading platform code into every sink: pull() is the single
// portable real-time chokepoint every interleaved float32 sample crosses on its
// way to any IAudioSink, CoreAudio and PipeWire alike. m_gain is a
// plain atomic float, written by the CONTROLLING thread (Engine::setVolumeGain
// forwards straight to setGain, no command queue: a volume change must be
// immediate and lock-free, never wait behind the transport queue) and read once
// per pull on the RT thread. The contract is deliberately narrow: this is a
// LINEAR gain, not a percentage and not a curve. The UI owns the perceptual taper
// (the cubic slider->gain mapping) and any ReplayGain composition; the source
// only multiplies. Two consequences worth stating because they are the whole
// point of putting it here:
//   - At exactly 1.0f, pull() SKIPS the multiply entirely, so unity volume is a
//     byte-identical passthrough: the bit-perfect path is preserved untouched
//     whenever the user is at 100% and unmuted. Attenuation, and only
//     attenuation, leaves that path, by design.
//   - The multiply scales the REAL frames pull() delivered; the sink's zero-pad
//     on a short read happens afterward and is unaffected (0 * g is still 0), and
//     the gain is orthogonal to the frame counters, so it never perturbs the seam
//     watch or the reported position.
// Relaxed ordering on both sides: a one-quantum-stale gain is inaudible, and no
// other memory needs to be ordered against the gain value, the same low-stakes
// treatment the end-of-stream-adjacent flags get above.

#pragma once

#include "ScopeTap.h"                   // the visualization tap (mono overwrite ring)
#include "rawform/audio/IAudioSink.h"  // IPullSource
#include "rawform/audio/RingBuffer.h"
#include "rawform/audio/Types.h"       // AudioFormat, ScopeSource

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace rawform::audio {

class EngineRingSource final : public IPullSource {
public:
    EngineRingSource();
    ~EngineRingSource() override;

    /// Not copyable or movable: it owns a heap ring shared by reference between
    /// the engine and RT threads, plus the cross-thread flags and counters.
    EngineRingSource(const EngineRingSource&)            = delete;
    EngineRingSource& operator=(const EngineRingSource&) = delete;
    EngineRingSource(EngineRingSource&&)                 = delete;
    EngineRingSource& operator=(EngineRingSource&&)      = delete;

    // ----- consumer side: the sink's real-time thread -------------------------

    /// Drain up to `frames` interleaved float32 frames into `out`; return the
    /// count supplied. The consumed counter advances by exactly that count (the
    /// real frames heard), never by the zero-pad. On a short read it either counts
    /// an underrun (producer still owes frames) or, once the ring is empty and
    /// input is exhausted, publishes finished. The SINK zero-pads the remainder.
    /// Applies the master gain to the delivered frames (skipped at unity; see the
    /// file header). Real-time safe.
    std::size_t pull(float* out, std::size_t frames) noexcept override;

    // ----- master gain: the CONTROLLING thread (Engine::setVolumeGain) ---------

    /// Set the linear master gain read by pull(). Called from the controlling
    /// thread, never the RT thread; a single relaxed atomic store, valid in any
    /// transport state including before the first track (a ring need not exist).
    /// 1.0f is unity (and makes pull() a passthrough); 0.0f is silence. The caller
    /// owns the valid range and the perceptual curve, this only stores and later
    /// multiplies. Lock-free and immediate by contract: NOT routed through the
    /// engine command queue.
    void setGain(float linearGain) noexcept;

    /// The current linear master gain. Readable from any thread; relaxed.
    [[nodiscard]] float gain() const noexcept;

    // ----- visualization tap: consumer (GUI) + controlling thread --------------
    //
    // The spectrum analyzer's feed. These sit beside the master gain on purpose:
    // the tap captures its mono block at the same pull() chokepoint the gain
    // multiplies, and scopeSource selects which side of that multiply it captures.
    // None of them is on the RT path's hot loop beyond a single relaxed read.

    /// Copy the most recent `frames` mono samples the output has produced into
    /// `out`, oldest first (so out[frames-1] is the latest). Zero-pads when fewer
    /// have been produced yet; clamps `frames` to the tap capacity; returns the
    /// count written. Called from the GUI thread by the visualizer timer, NEVER the
    /// RT thread. Lock-free and safe concurrently with pull().
    std::size_t copyScope(float* out, std::size_t frames) const noexcept;

    /// Select where the tap samples relative to the master gain (PostGain == as
    /// heard, the default; PreGain == the decoded source). Set from the controlling
    /// thread; a single relaxed store the RT thread reads once per pull, exactly
    /// like setGain. Valid in any state, including before the first track.
    void setScopeSource(ScopeSource source) noexcept;

    /// The current tap source. Readable from any thread; relaxed.
    [[nodiscard]] ScopeSource scopeSource() const noexcept;

    // ----- producer side: the engine thread -----------------------------------

    /// Build a fresh ring for a new track, clear the end-of-stream flags, and
    /// reset BOTH monotonic counters and the position origin to zero. Allocates,
    /// so it is NOT real-time safe and must be called only while the sink is
    /// closed or stopped (RT thread parked). Replaces any previous ring.
    void reconfigure(std::size_t capacityFrames, std::uint16_t channels);

    /// Flush the current ring, clear the end-of-stream flags, and reset both
    /// counters and the position origin to zero without reallocating. Like
    /// reconfigure, only valid while the sink is closed or stopped (RT thread
    /// parked). The HOLD-CUT path uses this to reuse the existing ring when the
    /// source format is unchanged.
    void reset() noexcept;

    /// Set the reported position to an absolute source frame, used by the seek
    /// handler to map the next sample heard to the post-seek source frame. Only
    /// valid while the RT thread is parked (sink stopped), the same window reset()
    /// lives in. Implemented as a position-origin store, not a counter rebase, so
    /// it is consistent with the seam path. Producer-only.
    void setPlayhead(std::uint64_t frame) noexcept;

    /// Rebase the position origin at a GAPLESS seam so the next track reports a
    /// position starting at 0: offset = -threshold makes the reported frame equal
    /// (consumed - threshold), which is 0 at the boundary and climbs into the new
    /// track. This is the ONLY position rebase that runs with the RT thread LIVE.
    /// It is safe because it writes only the engine-owned offset and never the
    /// RT-owned consumed counter; see the position note in the file header.
    /// Producer-only (the engine run loop).
    void rebasePositionAtSeam(std::uint64_t thresholdFrame) noexcept;

    /// Append up to `frames` frames from src into the ring; return the count
    /// accepted (less than `frames` when the ring fills) and advance the monotonic
    /// written counter by that count. Producer-only.
    std::size_t write(const float* src, std::size_t frames) noexcept;

    /// Producer-side free space, in frames. A lower bound under concurrency, so it
    /// is always safe to act on.
    [[nodiscard]] std::size_t writableFrames() const noexcept;

    /// Frames currently buffered. Used by the engine ONLY while priming with the
    /// RT thread parked (a fresh start, a seek, a HOLD-CUT), so there is no
    /// concurrent consumer at that point.
    [[nodiscard]] std::size_t bufferedFrames() const noexcept;

    /// Publish that the decoder reached end of stream. MUST be called AFTER the
    /// final write() so the release store orders the tail frames before the flag.
    /// Never called for a gapless stitch (see the file header). Producer-only.
    void markInputExhausted() noexcept;

    // ----- observers: the engine thread (position also the controlling thread) -

    [[nodiscard]] bool inputExhausted() const noexcept;  ///< producer's own flag, for control flow
    [[nodiscard]] bool finished() const noexcept;  ///< set by pull() at true end of stream
    [[nodiscard]] std::uint64_t underrunFrames() const noexcept;  ///< mid-stream starvation tally
    [[nodiscard]] bool configured() const noexcept;  ///< a ring exists (a track is loaded)

    /// Monotonic count of frames written into the current ring (since the last
    /// reconfigure/reset). The engine snaps this at a track's end to record the
    /// seam threshold. Engine-thread-only; cheap.
    [[nodiscard]] std::uint64_t totalFramesWritten() const noexcept;

    /// Monotonic count of frames consumed from the current ring (since the last
    /// reconfigure/reset). The run loop watches this against the front seam
    /// threshold to detect an audible boundary. RT-advanced; read by the engine.
    [[nodiscard]] std::uint64_t totalFramesConsumed() const noexcept;

    /// The reported source frame of the next sample to be heard:
    /// clamp(totalConsumed + offset, 0). Read by the engine thread (for
    /// onPositionChanged) and the controlling thread (for position()).
    [[nodiscard]] std::uint64_t playheadFrames() const noexcept;

private:
    /// Recreated by reconfigure() while the RT thread is parked; read by both
    /// the producer (write/writableFrames) and, through pull(), the consumer.
    /// Because the pointer is only ever swapped with the RT thread parked, the
    /// consumer never observes a half-swapped ring.
    std::unique_ptr<RingBuffer> m_ring;

    /// Cross-thread end-of-stream flags, each single-writer.
    /// Low frequency (once at EOS / on underrun), so they
    /// are left unaligned, unlike the block-frequency counters below.
    std::atomic<bool>          m_inputExhausted{false};  ///< W: engine    R: RT
    std::atomic<bool>          m_finished{false};        ///< W: RT        R: engine
    std::atomic<std::uint64_t> m_underrunFrames{0};      ///< W: RT        R: engine

    /// Monotonic block-frequency counters, written by DIFFERENT threads, so each
    /// sits on its own cache line to avoid false sharing, mirroring the
    /// false-sharing guard on RingBuffer's read/write positions. 64 is hardcoded
    /// for the same toolchain-portability reason RingBuffer hardcodes it.
    alignas(64) std::atomic<std::uint64_t> m_totalWritten{0};   ///< W: engine(producer) R: engine
    alignas(64) std::atomic<std::uint64_t> m_totalConsumed{0};  ///< W: RT R: engine + controlling

    /// The position origin. Engine-written ONLY (reconfigure/reset/setPlayhead and
    /// the seam rebase); read by the engine and the controlling thread for the
    /// reported position. The RT thread never touches it, which is exactly what
    /// makes rebasePositionAtSeam race-free with the RT thread advancing
    /// m_totalConsumed concurrently. Signed so the seam rebase can store
    /// -threshold; the reader clamps the sum at 0.
    alignas(64) std::atomic<std::int64_t> m_playheadOffset{0};  ///< W: engine R: engine + controlling

    /// The linear master gain. Written by the controlling thread via
    /// setGain, read once per pull on the RT thread. Defaults to 1.0f so a build
    /// that never sets a volume plays at unity (a passthrough, since pull() skips
    /// the multiply at 1.0f). On its own cache line for the same false-sharing
    /// reason the counters are: it is read on the RT thread every block, and we do
    /// not want that read contending with the engine's writes to the counters
    /// above. See the master-gain note in the file header.
    alignas(64) std::atomic<float> m_gain{1.0f};  ///< W: controlling R: RT

    /// The visualization tap (mono overwrite ring) and the source selector that
    /// picks which side of the gain multiply feeds it. m_scopeTap is written by the
    /// RT thread inside pull() and read by the GUI thread via copyScope; it carries
    /// its own lock-free synchronization, so it needs no atomic wrapper here. Its
    /// storage is allocated once at construction, so embedding it by value adds no
    /// per-track or per-pull allocation. m_scopeSource is the controlling-thread
    /// knob the RT thread reads once per block to decide where to push; like the EOS
    /// flags it is written rarely, so it is left unaligned rather than given its own
    /// cache line. Defaults to PostGain, so a build that never sets a source shows
    /// the signal as heard.
    ScopeTap                 m_scopeTap;                       ///< W: RT  R: GUI
    std::atomic<ScopeSource> m_scopeSource{ScopeSource::PostGain};  ///< W: controlling R: RT
};

}  // namespace rawform::audio
