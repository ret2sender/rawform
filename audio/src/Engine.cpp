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

// Engine.cpp
//
// The transport core. All of the Engine's machinery lives in Engine::Impl: the
// engine thread, the command queue (mutex + condition variable, NOT lock-free,
// because the RT thread never touches it), the play queue, the decoder, the
// internal real-time source, and the state machine. The public Engine just
// forwards to Impl, so the header stays free of <thread>, <mutex>, and <atomic>.
//
// Concurrency map (the design's whole point):
//   - thread 1 (controlling): the transport methods push a Command and notify;
//     each returns immediately. They also read the observable atomics, including
//     the reported position.
//   - thread 2 (engine): drains commands, runs the state machine, owns the
//     decoder + the play queue + the staging buffer + the seam-marker queue,
//     drives the ring source's producer side to keep the ring full, advances the
//     queue, fires the listener (including the position tick), and is the ONLY
//     thread that touches the sink.
//   - thread 3 (RT, inside the sink): EngineRingSource::pull() only, which also
//     advances the consumed counter by the real frames it delivers.
//
// The command mutex is the ONLY lock in the engine. The play queue, the decoder,
// the staging state, and the seam-marker queue are all engine-thread-private, so
// there is no second lock and no shared playlist structure. The mutex is never
// held across a decode, a ring write, or any call into the sink: sinks post
// their events from platform threads with a sink lock held, so a sink call
// under the command mutex would be a lock-order inversion (see setSink).
//
// Gapless. At a natural end-of-track advance the producer, instead of
// marking the input exhausted, opens the next track and (when the source format
// is IDENTICAL: rate AND channels) keeps writing it into the SAME
// running ring with no flush and no sink touch, so the listener hears no gap. It
// records a seam marker {threshold, nextTrackInfo} where threshold is every
// frame the outgoing track wrote; the run loop watches the consumed counter and,
// as it crosses each threshold (the audible boundary), pops the marker, fires
// onTrackChanged for the now-audible track, and rebases the reported position to
// 0. A genuine boundary (different source format) falls back to the existing
// drain-then-reconfigure path with its brief, accepted gap. A manual next() is a
// CUT: it flushes immediately, holding the device (park the RT thread, swap the
// decoder, no teardown) ONLY when the next source format is identical (the same
// identical-format gate as the stitch), and reconfiguring otherwise. A
// seek flushes the ring and clears ALL pending stitch state, then reseeks the
// live (producer-logical, most-recently-opened) decoder.
//
// Gapless can be disabled at runtime via the RAWFORM_NO_GAPLESS environment
// variable (a runtime knob so the public interface stays unchanged), which
// forces the plain drain-then-advance behavior for A/B listening. The flag is
// read once at engine construction.
//
// Wait strategy on the engine thread. Each iteration it either has
// immediate producer work (room in the ring and input not exhausted, while
// Playing or Paused) and does one decode/write step with no wait, or it sleeps:
//   - bounded (a few ms) while Playing, because the RT thread changes the world
//     without signaling us (it drains the ring, advances the consumed counter,
//     and sets finished at EOS) and is forbidden from waking the producer, so we
//     must periodically re-check; this same bounded wake also drives the
//     position tick and the seam-crossing watch on cadence;
//   - indefinite otherwise (Stopped, or Paused with a full ring), because only a
//     command can change anything and commands notify.
// A command always wakes the wait promptly via the condition variable.
//
// Transport-to-sink mapping:
//   - pause = sink.stop() (joins the RT thread; ring and decoder untouched);
//     resume = sink.start(); instant, no flush.
//   - stop / queue-exhaustion = sink.stop()+close() (repay any rate debt,
//     release the device) + ring reset + drop the decoder + clear seams. stop
//     REWINDS: it REMEMBERS the current path, so the next play() reopens it.
//   - seek = sink.stop() (the pause primitive, NOT close) + ring reset + clear
//     seams + decoder->seek(target) + position rebase + re-prime, then
//     sink.start() iff the entry state was Playing. State is preserved.
//   - natural advance, same source format AND unchanged rate mode = NO sink
//     touch (gapless stitch).
//   - natural advance, different source format (or a rate mode changed since
//     the stream opened) = drain, PARK the device (sink.stop(), no close),
//     re-decide, then sink.reconfigure() in place; a sink that declines gets
//     close()+open(). A brief, accepted gap either way.
//   - manual next / playNow, same source format AND unchanged rate mode =
//     sink.stop() + ring reset + swap + reprime + sink.start() (HOLD the
//     device; responsive cut, no teardown).
//   - manual next / playNow, different source format (or a changed rate mode)
//     = the same park-and-reconfigure transition as the natural case.
//   - output device selection, and a default-device change while following
//     the default = full teardown (the outgoing device gets its restore) +
//     reopen of the SAME track at the captured position, so the change is
//     audible without losing the place.
//
// Teardown order (the safety crux). On a stop and on shutdown the sink is
// stopped/closed FIRST, which joins the RT thread (every sink's stop() returns
// only once no pull() is in flight or will follow, per the IAudioSink lifetime
// invariant); only THEN is the ring reset and the decoder dropped. ~Engine
// signals shutdown and joins the engine thread; the engine thread runs that
// same teardown before exiting, so by the time Impl's members destruct the
// sink is closed, the RT thread is gone, and nothing references the ring
// source.

#include "rawform/audio/Engine.h"

#include "rawform/audio/IAudioSink.h"
#include "rawform/audio/IDecoder.h"
#include "rawform/audio/IDecoderFactory.h"
#include "rawform/audio/RateManager.h"
#include "rawform/audio/Types.h"

#include "EngineRingSource.h"
#include "DecoderFactory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <memory>  // shared_ptr / make_shared, for the published current-track box
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <version>  // __cpp_lib_atomic_shared_ptr feature-test macro

namespace rawform::audio {

namespace {

// Playback tuning. The ring holds ~kTargetLatencyMs of audio at the source
// rate, enough runway that a scheduling hiccup on the engine thread never
// starves the RT pull; the producer decodes in kBlockFrames-frame granules, a
// size that keeps each decode call short so the loop stays responsive to
// commands; and the engine wakes at most every kPollIntervalMs while Playing to
// top the ring up, to notice EOS, and to process seam crossings.
constexpr std::uint32_t kTargetLatencyMs = 400;
constexpr std::size_t   kBlockFrames     = 4096;
constexpr int           kPollIntervalMs  = 5;  // << ring latency; bounds top-up, EOS, and seam gaps

// Position notification cadence. The engine fires onPositionChanged
// at most this often while Playing, gated on a steady_clock timestamp checked
// each loop iteration. Because the loop wakes at least every kPollIntervalMs
// while Playing, the gate lands within a few ms of target, well inside one tick.
// 10 Hz is the default for a smooth progress bar; change this one constant
// to retune.
constexpr int kPositionIntervalMs = 100;

// Ring capacity in frames from a target latency, with a floor so a full decode
// block (and slack) always fits even at very low sample rates.
std::size_t computeCapacityFrames(std::uint32_t sampleRate,
                                  std::uint32_t targetLatencyMs,
                                  std::size_t   blockFrames) noexcept {
    const std::uint64_t cap =
        (static_cast<std::uint64_t>(sampleRate) * targetLatencyMs) / 1000u;
    auto frames = static_cast<std::size_t>(cap);
    const std::size_t floor  = (blockFrames == 0 ? 1u : blockFrames) * 2u;
    if (frames < floor) {
        frames = floor;
    }
    return frames;
}

// Pack an AudioFormat into one 64-bit word so the controlling thread can read a
// consistent {rate, channels} pair from a single atomic, with no torn-read
// window between two separate atomics. sampleRate occupies the upper bits,
// channels the low 16.
std::uint64_t packFormat(const AudioFormat& f) noexcept {
    return (static_cast<std::uint64_t>(f.sampleRate) << 16) |
           static_cast<std::uint64_t>(f.channels);
}
AudioFormat unpackFormat(std::uint64_t bits) noexcept {
    AudioFormat f;
    f.channels   = static_cast<std::uint16_t>(bits & 0xFFFFu);
    f.sampleRate = static_cast<std::uint32_t>(bits >> 16);
    return f;
}

// Read the RAWFORM_NO_GAPLESS environment knob once (a runtime A/B
// switch with no public interface change). Gapless is ON by default; it is
// disabled when the variable is set to anything other than empty or "0". The CLI
// maps its --no-gapless flag to setting this before constructing the Engine.
bool gaplessEnabledFromEnv() noexcept {
    const char* v = std::getenv("RAWFORM_NO_GAPLESS");
    const bool disabled = (v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0'));
    return !disabled;
}

// Build the engine-to-controller description for a track from its decoder. A
// pure function of its two arguments (no engine state consulted), hence a free
// helper rather than an Impl member. Used both by the fresh-start path
// (startTrack) and, snapshotted, by the gapless stitch (so the deferred
// onTrackChanged does not depend on the live decoder, which may have advanced
// past the announced track by crossing time).
TrackInfo makeTrackInfo(const std::string& path, const IDecoder& dec) {
    TrackInfo info;
    info.path        = path;
    info.format      = dec.format();
    info.source      = dec.sourceInfo();
    info.totalFrames = dec.totalFrames();
    info.seekable    = dec.seekable();  // Surfaced for the UI scrubber
    return info;
}

// The Engine's built-in default opener is the format-dispatching
// DecoderFactory. Leaving setDecoderFactory unset therefore plays every format
// the build supports (WAV/AIFF via libsndfile, FLAC via libFLAC, MP3 via
// libmpg123, and the FFmpeg long tail when it is built in) with no caller
// involvement. Tests inject their own codec-free factory the same way. The
// default lives as a by-value member of Impl below.

// The commands thread 1 hands to thread 2. Shutdown is NOT a command; it is the
// dtor's shutdown flag, so it cannot be starved behind a backlog of transport
// commands. Seek carries its target as a time in seconds; the handler
// resolves it to a frame against the live track's rate and length.
enum class CommandType { Enqueue, PlayNow, SetQueue, Play, Pause, Stop, Next, ClearQueue, Seek,
                         RequestOutputDevices, SelectOutputDevice,
                         ExternalRateChanged, DefaultDeviceChanged, DeviceListChanged,
                         RateDebtChanged };

struct Command {
    CommandType              type;
    std::string              path;           // used by Enqueue, PlayNow, and SelectOutputDevice (device id)
    double                   seconds = 0.0;  // used only by Seek
    std::vector<std::string> paths;          // used only by SetQueue
    std::uint32_t            rateA = 0;      // Sink event payloads: ExternalRateChanged's
    std::uint32_t            rateB = 0;      // rate; RateDebtChanged's original/borrowed
};

// A pending gapless boundary. The producer pushes one when it
// stitches a new track into the running ring; the run loop pops it when the
// consumed counter crosses thresholdFrame (the audible boundary). It carries a
// SNAPSHOT of the incoming track because by crossing time the live decoder may
// already be one or more tracks further on, so onTrackChanged cannot re-read it
// from `decoder`. Engine-thread-private: both the push (producer) and the pop
// (run loop) are the engine thread, so the queue needs no synchronization and
// the RT thread never touches it (which is why it lives here and not in the
// RT-shared EngineRingSource).
struct SeamMarker {
    std::uint64_t thresholdFrame = 0;  // consumed-frame value at which `info` becomes audible
    TrackInfo     info;                // snapshot taken when this track's decoder was opened
};

// ---------------------------------------------------------------------------
// Published current-track snapshot, the backing store for Engine::currentTrackInfo()
//. TrackInfo owns a std::string, so unlike formatBits it cannot ride a
// single atomic word; it needs a whole-object publish from the engine thread (at
// each track boundary) to the controlling thread (a UI reading the active track).
// Neither end is the RT thread, so this is NEVER on the audio path: a lock here
// can never cause a dropout, which is what makes the fallback below acceptable.
//
// Two interchangeable backends, selected by the STANDARD LIBRARY feature-test
// macro for std::atomic<std::shared_ptr<>> (P0718), so the choice tracks the
// stdlib's capability rather than a hardcoded platform name:
//   - lock-free (preferred): an immutable snapshot swapped through
//     std::atomic<std::shared_ptr<const TrackInfo>>. Present on libstdc++,
//     the Linux build's standard library.
//   - mutex fallback: the same snapshot behind a tiny BOX-LOCAL mutex. Used on
//     libc++ (AppleClang, the macOS toolchain), which does not ship the
//     atomic<shared_ptr> specialization.
//
// The fallback's mutex is PRIVATE to this box and is never the engine's command
// mutex, so "the command mutex guards only the command queue" holds on both
// backends, and the box lock never nests with it (publish/reset run outside the
// command-drain critical section, so no lock-ordering concern exists). The call
// sites (publish at each boundary, reset at teardown, load in the getter) are
// identical across backends: the day libc++ defines __cpp_lib_atomic_shared_ptr,
// this whole fallback can be deleted with no other change, and until then each of
// the macOS and Linux builds compiles and exercises exactly one branch, so
// neither rots. Empty (a default TrackInfo) is the Stopped readout on both paths.
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
#define RAWFORM_ATOMIC_SHARED_PTR 1
#else
#define RAWFORM_ATOMIC_SHARED_PTR 0
#endif

class CurrentTrackBox {
public:
    CurrentTrackBox() = default;

    // Engine-thread-only after construction (publish/reset); read by any thread.
    CurrentTrackBox(const CurrentTrackBox&)            = delete;
    CurrentTrackBox& operator=(const CurrentTrackBox&) = delete;
    CurrentTrackBox(CurrentTrackBox&&)                 = delete;
    CurrentTrackBox& operator=(CurrentTrackBox&&)      = delete;

    // Make `info` the published current track. Called at each boundary where
    // onTrackChanged fires (engine thread).
    void publish(const TrackInfo& info) {
#if RAWFORM_ATOMIC_SHARED_PTR
        m_info.store(std::make_shared<const TrackInfo>(info),
                     std::memory_order_release);
#else
        std::lock_guard<std::mutex> lock(m_mtx);
        m_info = info;
#endif
    }

    // Clear back to the empty (Stopped) readout. Called from teardownCurrent.
    void reset() {
#if RAWFORM_ATOMIC_SHARED_PTR
        m_info.store(nullptr, std::memory_order_release);
#else
        std::lock_guard<std::mutex> lock(m_mtx);
        m_info = TrackInfo{};
#endif
    }

    // A coherent snapshot of the current track, or a default TrackInfo when none.
    [[nodiscard]] TrackInfo load() const {
#if RAWFORM_ATOMIC_SHARED_PTR
        const std::shared_ptr<const TrackInfo> p =
            m_info.load(std::memory_order_acquire);
        return p ? *p : TrackInfo{};
#else
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_info;
#endif
    }

private:
#if RAWFORM_ATOMIC_SHARED_PTR
    std::atomic<std::shared_ptr<const TrackInfo>> m_info{nullptr};
#else
    mutable std::mutex m_mtx;
    TrackInfo          m_info{};
#endif
};

}  // namespace

// ===========================================================================
// Impl: everything that is not the four-line public forwarding layer.
// Impl doubles as the sink's ILogOutput: the sink's
// diagnostic lines land in logLine (engine thread, per the ILogOutput
// contract) and are forwarded to Listener::onInfo. Implemented on Impl itself
// rather than a separate adapter object because the lifetime is then free:
// the Engine owns the sink, so the Impl outlives every sink it installs
// itself into, including through the close() the sink runs during Impl
// destruction (members destroy in reverse order, and `listener` going first
// is harmless: notifyInfo just reads a null and drops the line).
struct Engine::Impl : ILogOutput, ISinkEventListener {
    // ----- the real-time surface ----------------------------------------------
    // Declared FIRST, before `sink`, so that on Impl destruction `sink` (which
    // holds a non-owning IPullSource* into this) is destroyed before the ring
    // source it points at (members destroy in reverse declaration order). The
    // dynamic guarantee already comes from the engine thread closing the sink
    // before it exits; this declaration order is the static belt-and-suspenders.
    EngineRingSource ringSource;

    // ----- injected configuration ---------------------------------------------
    std::unique_ptr<IAudioSink> sink;                  // engine-thread-only after set
    DecoderFactory              defaultFactory;        // the format-dispatching opener
    IDecoderFactory*            factory = nullptr;     // -> defaultFactory unless overridden
    std::atomic<Engine::Listener*> listener{nullptr};  // any-thread set, engine-thread call

    // ----- rate policy ----------------------------------------------------
    RateManager            rateManager;
    std::atomic<RateMode>  rateMode{RateMode::BitPerfectWhenAvailable};

    // The DEVICE format the engine considers itself transitioning FROM: the
    // resolved device rate plus the source channel count of the currently or
    // most-recently open stream. An invalid (zeroed) value means "transitioning
    // from a closed device" (a fresh start). The RateManager reads it as
    // `currentOpen` to compute the gate (needsDeviceReconfigure / keepSink). Set
    // after each successful open; cleared by enterStopped(). Deliberately NOT
    // cleared in teardownCurrent(), so a track-to-track reconfigure still compares
    // the incoming track against the outgoing one.
    AudioFormat openDeviceFormat{};

    // Whether the sink currently holds an OPEN device. Distinct from
    // openDeviceFormat above, which deliberately SURVIVES teardown as the
    // decision's comparison seed: this flag tracks the physical open/closed
    // truth, so startTrack knows whether an in-place reconfigure is even on the
    // table and enterStopped knows whether a parked device is left to release.
    // Engine-thread-only.
    bool deviceOpen = false;

    // The applied output pin: mirrors what doSelectOutputDevice last
    // handed the sink; empty means following the system default, which is the
    // fact doDefaultDeviceChanged's reopen decision turns on. Engine-thread
    // only.
    std::string outputDeviceId;

    // The rate mode that was IN FORCE when the current stream was opened,
    // snapshotted in startTrack. The same-source-format fast paths (the gapless
    // stitch in handleProducerEos, the HOLD-CUT in cutOrStartTo) exist because
    // "same source format" implies "the rate decision would resolve
    // identically", so re-deciding would be pure waste. That implication has a
    // premise: decide() is a pure function of (source, caps, mode), and the
    // mode is user-flippable mid-run through the settings toggle. So both fast
    // paths additionally require the live rateMode to still EQUAL this
    // snapshot; a flip forces the next boundary down the park-then-reconfigure
    // path, where startTrack re-decides under the new mode and re-snapshots. A
    // value compare, deliberately not a generation counter: a flip that is
    // flipped back before any boundary nets to no change and the stitch/hold
    // correctly survives. Engine-thread-only.
    RateMode openRateMode = RateMode::BitPerfectWhenAvailable;

    // ----- gapless policy -------------------------------------------------
    // Read once at construction from RAWFORM_NO_GAPLESS. When false the engine
    // behaves exactly as if gapless never existed (every natural advance drains
    // then reconfigures). Engine-thread-only after construction.
    bool gaplessEnabled = true;

    // ----- engine-thread-private playback state -------------------------------
    std::unique_ptr<IDecoder> decoder;       // the producer-logical current; live while Playing/Paused
    std::string               currentPath;   // REMEMBERED across stop (rewind); "" == none
    std::deque<std::string>   pending;       // the play queue
    AudioFormat               format{};      // the current track's format (== a stitched track's, by the gate)
    std::size_t               channels = 0;  // = format.channels, for indexing
    std::vector<float>        staging;       // producer scratch, one decode block

    // The seam-marker queue. Front is the next audible boundary. Touched
    // only by the engine thread (producer push, run-loop pop).
    std::deque<SeamMarker>    seams;

    // Carry of a partially written decode block across loop iterations, so the
    // ring-full wait happens at the engine's command-aware condvar rather than
    // inside a blocking writeFully: a producer that blocked inside the ring
    // write could not see a command arrive.
    std::size_t stagedFrames     = 0;
    std::size_t stagedOffset     = 0;
    bool        pendingFinalBlock = false;

    // ----- position tick bookkeeping (engine thread only) ----------------------
    std::chrono::steady_clock::time_point lastPositionFire{};

    // ----- observable atomics (read by thread 1) ------------------------------
    std::atomic<State>         stateAtomic{State::Stopped};
    std::atomic<std::uint64_t> formatBits{0};  // packed AudioFormat; 0 == invalid

    // The output device outcome for the current open:
    // [bit 33: valid][bit 32: bit-perfect][low 32: device rate in Hz], one word
    // so the two public reads can never see each other's torn halves. Published
    // in startTrack after each successful open (measurement-first, prediction
    // fallback; see the publication site), untouched across stitches, HOLD-CUTs,
    // seeks, and pause (the open persists through all of those), cleared by
    // enterStopped beside openDeviceFormat (device released).
    std::atomic<std::uint64_t> outcomeBits{0};

    // The current track's length in frames, published with formatBits at each
    // track change. duration() divides it by the published rate, the lock-
    // free mirror of how position() divides the playhead by the rate. 0 when none.
    std::atomic<std::uint64_t> totalFramesAtomic{0};

    // The published current TrackInfo for currentTrackInfo(). Written on the
    // engine thread at each boundary (announceTrack) and cleared at teardown; read
    // by the controlling thread. Lock-free where the stdlib supports it, behind a
    // private mutex otherwise; see CurrentTrackBox.
    CurrentTrackBox            currentInfo;

    // ----- live bitrate -------------------------------------------
    // The moment-to-moment decode bitrate for the player status line. firePosition
    // publishes it (from the active decoder's currentBitrateKbps()) on the same
    // cadence as position; the controlling thread reads it via liveBitrateKbps().
    // currentNominalKbps is the engine-thread-private fallback (this track's
    // nominal/average), seeded at announceTrack and substituted whenever the
    // decoder reports 0 ("no live figure", e.g. PCM). Both reset at teardown so a
    // Stopped engine reads 0.
    std::atomic<std::uint32_t> liveBitrateAtomic{0};
    std::uint32_t              currentNominalKbps = 0;  // engine thread only

    // ----- command queue (thread 1 -> thread 2) -------------------------------
    std::mutex              mtx;
    std::condition_variable cv;
    std::deque<Command>     commands;
    bool                    shutdown = false;

    // ----- the engine thread (declared last; joined explicitly in ~Engine) ----
    std::thread thread;

    // --- lifecycle ---
    void start();                       // spawn the engine thread
    void requestShutdownAndJoin();      // dtor helper

    // --- thread 1 helpers ---
    void post(Command c);

    // --- thread 2: the loop and the state machine ---
    void run();
    void process(const Command& c);
    void doPlay();
    void doPlayNow(const std::string& path);             // Play an explicit path now
    void doSetQueue(const std::vector<std::string>& paths); // Replace the pending queue
    void doPause();
    void doStop();
    void doNext();
    void doSeek(double seconds);
    void doRequestOutputDevices();                       // enumerate via listener push
    void doSelectOutputDevice(const std::string& id);    // pin device, reopen-at-position
    void doExternalRateChanged(std::uint32_t newRateHz); // republish the outcome
    void doDefaultDeviceChanged();                       // refresh; reopen when following
    void doDeviceListChanged();                          // push a fresh enumeration
    void doRateDebtChanged(const std::string& deviceId,  // forward for persistence
                           std::uint32_t originalRateHz, std::uint32_t borrowedRateHz);
    void reopenAtPosition();                             // the shared audible-change boundary

    // ----- ISinkEventListener ------------------------------------
    // Fired by the sink from ARBITRARY platform threads (see the interface
    // contract); each simply enqueues an engine command (post's mutex is
    // any-thread), so the reaction runs where every reaction runs: the engine
    // thread, serialized with everything else that touches the sink.
    void onExternalRateChanged(std::uint32_t newRateHz) override {
        post(Command{CommandType::ExternalRateChanged, {}, 0.0, {}, newRateHz, 0});
    }
    void onDefaultDeviceChanged() override {
        post(Command{CommandType::DefaultDeviceChanged, {}, 0.0, {}, 0, 0});
    }
    void onDeviceListChanged() override {
        post(Command{CommandType::DeviceListChanged, {}, 0.0, {}, 0, 0});
    }
    void onRateDebtChanged(const std::string& deviceId, std::uint32_t originalRateHz,
                           std::uint32_t borrowedRateHz) override {
        post(Command{CommandType::RateDebtChanged, deviceId, 0.0, {}, originalRateHz,
                     borrowedRateHz});
    }

    bool startTrack(std::unique_ptr<IDecoder> dec, const std::string& path);  // configure ring, open sink, play
    bool openAndStart(const std::string& path);          // factory open then startTrack
    bool cutOrStartTo(std::unique_ptr<IDecoder> dec, const std::string& path); // HOLD-CUT or reconfigure to an opened decoder
    void advanceToPendingOrStop();                       // pop next openable, else Stopped
    void teardownCurrent();                              // stop + CLOSE sink, then flush (stop/error/shutdown paths)
    void parkForTransition();                            // stop sink, KEEP device open, then flush (transition boundaries)
    void flushEngineState();                             // the shared post-park flush (ring, decoder, seams, readouts)
    std::size_t produceOneStep();                        // one decode/write granule
    void primeUpTo(std::size_t thresholdFrames);         // pre-buffer before start
    void handleProducerEos();                            // Stitch next track gaplessly, or mark exhausted
    void crossSeam(const SeamMarker& marker);            // Fire onTrackChanged + position rebase at a boundary

    // --- publication / notification (engine thread) ---
    void      setState(State s);
    void      enterStopped();                            // clear the gate origin + go Stopped
    void      publishFormat(const AudioFormat& f);
    void      announceTrack(const TrackInfo& info);      // Publish observers + notify listener
    void      notifyTrackChanged(const TrackInfo& info) const;
    void      notifyError(const std::string& message) const;
    void      notifyInfo(const std::string& message) const;   // Listener::onInfo forward
    void      notifyOutputDevices(const std::vector<AudioDeviceInfo>& devices) const;
    void      logLine(const std::string& line) override;  // ILogOutput: sink lines in
    void      firePosition();                            // push the current position now
    double    currentPositionSeconds() const;            // playhead / rate, 0 if no rate
};

// ---------------------------------------------------------------------------
// thread 1 -> thread 2 handoff.
void Engine::Impl::post(Command c) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        commands.push_back(std::move(c));
    }
    cv.notify_one();
}

void Engine::Impl::start() {
    factory        = &defaultFactory;
    gaplessEnabled = gaplessEnabledFromEnv(); // Read the A/B knob once, before the thread runs
    thread         = std::thread(&Engine::Impl::run, this);
}

void Engine::Impl::requestShutdownAndJoin() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        shutdown = true;
    }
    cv.notify_one();
    if (thread.joinable()) {
        thread.join();
    }
}

// ---------------------------------------------------------------------------
// The engine thread. See the file header for the wait strategy.
void Engine::Impl::run() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mtx);

            // 1. Drain every pending command. We unlock around each one so a
            // decode or a sink call inside process() never blocks thread 1 from
            // posting the next command.
            while (!commands.empty()) {
                Command c = std::move(commands.front());
                commands.pop_front();
                lock.unlock();
                process(c);
                lock.lock();
            }

            if (shutdown) {
                lock.unlock();
                break;
            }

            // 2. Decide the wait. producerHasWork: there is room to decode into
            // right now. While Playing we must still wake periodically even with
            // no work, because the RT thread drains the ring, advances the
            // consumed counter (which we watch for seam crossings), and sets
            // finished without signaling us, and because the position tick is due.
            const State st = stateAtomic.load(std::memory_order_relaxed);
            const bool active = (st == State::Playing || st == State::Paused);
            const bool producerHasWork =
                active && !ringSource.inputExhausted() && ringSource.writableFrames() > 0;

            if (!producerHasWork) {
                if (st == State::Playing) {
                    cv.wait_for(lock, std::chrono::milliseconds(kPollIntervalMs),
                                [this] { return !commands.empty() || shutdown; });
                } else {
                    cv.wait(lock, [this] { return !commands.empty() || shutdown; });
                }
            }
        }

        // 3. Producer step, seam-crossing watch, and end-of-stream advance, all
        // outside the lock. The state is re-read because a command in step 1 may
        // have changed it.
        const State st = stateAtomic.load(std::memory_order_relaxed);
        const bool active = (st == State::Playing || st == State::Paused);
        if (active && !ringSource.inputExhausted()) {
            produceOneStep();
        }

        // Gapless seam crossings. As the consumer drains past each pending
        // boundary, the outgoing track has been fully heard and the next track's
        // frame 0 is now audible: fire its onTrackChanged and rebase the reported
        // position. A while loop drains several markers in one pass for tracks
        // shorter than the ring.
        if (st == State::Playing) {
            while (!seams.empty() &&
                   ringSource.totalFramesConsumed() >= seams.front().thresholdFrame) {
                SeamMarker marker = std::move(seams.front());
                seams.pop_front();
                crossSeam(marker);
            }
        }

        // End of playback. Only the final track of a run (no stitch) ever sets
        // inputExhausted, so a gapless run never reaches here between tracks. The
        // seams.empty() guard makes sure every boundary onTrackChanged has fired
        // before we settle into Stopped, even if the consumer raced ahead of this
        // loop.
        if (st == State::Playing && seams.empty() &&
            ringSource.inputExhausted() && ringSource.finished()) {
            parkForTransition();  // keep the device for the next track's reconfigure offer
            advanceToPendingOrStop();
        }

        // 4. Position tick. Fire onPositionChanged on cadence while Playing.
        // Re-read the state: a completion in step 3 may have just dropped us to
        // Stopped, in which case there is nothing to report. Paused emits no
        // periodic tick; its single fire happened in doPause. The gate keeps this
        // to ~10 Hz despite the loop iterating far more often.
        if (stateAtomic.load(std::memory_order_relaxed) == State::Playing) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastPositionFire >=
                std::chrono::milliseconds(kPositionIntervalMs)) {
                firePosition();
                lastPositionFire = now;
            }
        }
    }

    // Shutdown: close the sink (joins the RT thread), reset the ring, drop the
    // decoder, clear seams, so Impl's members destruct with nothing left running.
    teardownCurrent();
}

// ---------------------------------------------------------------------------
// Command dispatch.
void Engine::Impl::process(const Command& c) {
    switch (c.type) {
        case CommandType::Enqueue:    pending.push_back(c.path); break;
        case CommandType::PlayNow:    doPlayNow(c.path);         break;
        case CommandType::SetQueue:   doSetQueue(c.paths);       break;
        case CommandType::Play:       doPlay();                  break;
        case CommandType::Pause:      doPause();                 break;
        case CommandType::Stop:       doStop();                  break;
        case CommandType::Next:       doNext();                  break;
        case CommandType::ClearQueue: pending.clear();           break;
        case CommandType::Seek:       doSeek(c.seconds);         break;
        case CommandType::RequestOutputDevices: doRequestOutputDevices(); break;
        case CommandType::SelectOutputDevice:   doSelectOutputDevice(c.path); break;
        case CommandType::ExternalRateChanged:  doExternalRateChanged(c.rateA); break;
        case CommandType::DefaultDeviceChanged: doDefaultDeviceChanged(); break;
        case CommandType::DeviceListChanged:    doDeviceListChanged(); break;
        case CommandType::RateDebtChanged:      doRateDebtChanged(c.path, c.rateA, c.rateB); break;
    }
}

// play(): resume from Paused, or (from Stopped) replay the remembered track from
// frame 0, or pop the next queued path. A no-op while Playing.
void Engine::Impl::doPlay() {
    const State st = stateAtomic.load(std::memory_order_relaxed);
    if (st == State::Playing) {
        return;
    }
    if (st == State::Paused) {
        if (sink) {
            sink->start();
        }
        setState(State::Playing);
        return;
    }

    // Stopped. By design, a remembered track (set by stop or by a finished queue)
    // is replayed from the start; only when there is none do we draw from the
    // pending queue.
    if (!currentPath.empty()) {
        if (!openAndStart(currentPath)) {
            // The remembered file no longer opens; settle into Stopped. We keep
            // currentPath so the user can see what failed and retry.
            enterStopped();
        }
        return;
    }
    if (!pending.empty()) {
        advanceToPendingOrStop();
        return;
    }
    notifyError("play: nothing queued");
}

// pause(): stop the sink without flushing, hold the decoder and ring. A no-op
// unless Playing. The producer keeps topping the ring up until it is full, then
// the loop parks indefinitely, so resume is instant. The single onPositionChanged
// here lets the UI freeze the readout at the exact paused position.
void Engine::Impl::doPause() {
    if (stateAtomic.load(std::memory_order_relaxed) != State::Playing) {
        return;
    }
    if (sink) {
        sink->stop();  // joins the RT thread; ring + decoder untouched
    }
    setState(State::Paused);
    firePosition();    // reflect the frozen position at once
}

// stop(): halt and rewind. Tear the current track down but REMEMBER its path so
// play() restarts it from frame 0. Preserve the pending queue. teardownCurrent
// clears the seam queue, so any pending stitch state is discarded; the remembered
// path is the producer-logical current, which is what the user is transitioning
// away from.
void Engine::Impl::doStop() {
    if (stateAtomic.load(std::memory_order_relaxed) == State::Stopped) {
        return;
    }
    teardownCurrent();  // closes the sink (restores rate), flushes ring, drops decoder, clears seams
    // currentPath intentionally retained.
    enterStopped();
}

// next(): skip to the next queued track, or stop if the queue is empty. A no-op
// while Stopped (next has meaning only relative to a current track). A manual
// skip always CUTS (the user asked to move now), so any pending gapless stitch is
// void; the device is HELD (parked, swapped, resumed, no teardown) only when the
// next source format is identical (the same gate the gapless stitch uses).
//
// The cut/start mechanics live in cutOrStartTo, shared with playNow. next()
// itself: clear seams, find the next openable pending path (reporting and
// skipping any that fail), and on queue exhaustion tear down to Stopped;
// otherwise hand the opened decoder to cutOrStartTo, and on a start failure skip
// on through the queue (the queue-exhaustion semantics that distinguish next()
// from playNow()).
void Engine::Impl::doNext() {
    if (stateAtomic.load(std::memory_order_relaxed) == State::Stopped) {
        return;
    }

    // Void any pending gapless stitch state up front.
    seams.clear();

    // Find the next openable pending track, skipping (and reporting) any that
    // fail to open. We open it BEFORE deciding hold-vs-reconfigure so its format
    // is known; the opened decoder is reused on whichever path cutOrStartTo takes,
    // so there is no redundant open here.
    std::unique_ptr<IDecoder> dec;
    std::string               nextPath;
    while (!pending.empty()) {
        nextPath = pending.front();
        pending.pop_front();
        std::string err;
        dec = factory->open(nextPath, &err);
        if (dec) {
            break;
        }
        notifyError("could not open '" + nextPath + "': " + err);
        nextPath.clear();
    }

    if (!dec) {
        // Nothing left to skip to: tear down and settle into Stopped, keeping the
        // remembered currentPath so a later play() can replay it.
        teardownCurrent();
        enterStopped();
        return;
    }

    // Bring it current. On a reconfigure start failure, next() skips on through
    // the queue (legitimate at a skip boundary); contrast playNow, which settles
    // Stopped instead.
    if (!cutOrStartTo(std::move(dec), nextPath)) {
        advanceToPendingOrStop();
    }
}

// playNow(path): make `path` the current track immediately and play it. From
// Stopped, play() replays the REMEMBERED currentPath, so a controller that
// wants a different track cannot get there with play() alone without briefly
// hearing the remembered one; playNow opens and starts `path` DIRECTLY, with no
// remembered-replay, so the first audio heard is `path`. While Playing or
// Paused it is a CUT, using the same cutOrStartTo machinery as next(). The
// pending queue is untouched (pair with setQueue to set what follows). The
// engine gains no cursor model: `path` is just the new producer-logical current.
//
// Failure handling is the one deliberate divergence from next(): an explicit
// request that cannot be opened reports through onError and leaves the transport
// EXACTLY as it was (current track and queue undisturbed); and if the path opens
// but the fresh start fails (no sink, sink open failure), playNow settles Stopped
// rather than chewing the pending queue, because the user asked for one specific
// track, not "advance".
void Engine::Impl::doPlayNow(const std::string& path) {
    // The user asked to move now, so any pending gapless stitch is void, exactly
    // as for a manual next().
    seams.clear();

    std::string               err;
    std::unique_ptr<IDecoder> dec = factory->open(path, &err);
    if (!dec) {
        notifyError("could not open '" + path + "': " + err);
        return;  // transport unchanged: a rejected request never interrupts playback
    }

    if (!cutOrStartTo(std::move(dec), path)) {
        // Opened, but the start failed (sink). startTrack already committed
        // currentPath = path before failing, so a later play() can retry it.
        enterStopped();
    }
}

// setQueue(paths): atomically replace the pending forward queue. Does
// not touch the current track; the new paths take effect at the next natural
// advance or next(). Replaces, where Enqueue appends; an empty vector clears it,
// like ClearQueue. The engine holds no cursor: this is purely the pending list the
// auto-advance and next() draw from. A controller posts setQueue(tail) then
// playNow(front) for "play this list from here"; both land in the same command-
// drain pass before any advance, so the pair is atomic with respect to the
// engine's own queue advance.
void Engine::Impl::doSetQueue(const std::vector<std::string>& paths) {
    pending.assign(paths.begin(), paths.end());
}

// Enumeration is a sink query, and the engine thread is the only sink
// toucher, so it rides the command queue and answers through the listener, the
// same one-way flow every other engine fact takes. An engine with no sink
// answers with an empty list rather than an error: "no devices" is a truthful
// answer for a UI to render.
void Engine::Impl::doRequestOutputDevices() {
    std::vector<AudioDeviceInfo> devices;
    if (sink) {
        devices = sink->enumerateDevices();
    }
    notifyOutputDevices(devices);
}

// Pin the output device (empty id = follow the system default) and
// make the change audible. While Stopped the pin just parks on the sink for
// the next open. While Playing or Paused the device genuinely changes, which
// is a FULL close+open boundary by design: the outgoing device must get its
// rate restored and the incoming one a fresh open (the in-place reconfigure's
// device pinning would refuse a cross-device retune anyway). So: capture the
// position, tear down, reopen the same track, seek back, restore Paused if
// that is where we were. A refused id leaves everything untouched with an
// onError; a failed reopen (device vanished mid-flight) settles Stopped with
// onError, exactly like any failed start, with the pending queue preserved.
void Engine::Impl::doSelectOutputDevice(const std::string& deviceId) {
    if (!sink) {
        notifyError("select output device: no sink set");
        return;
    }
    if (!sink->selectDevice(deviceId)) {
        notifyError("select output device: sink refused '" + deviceId + "'");
        return;
    }
    outputDeviceId = deviceId;
    notifyInfo(deviceId.empty()
                   ? std::string("output device: following the system default")
                   : "output device selected: " + deviceId);
    reopenAtPosition();
}

// The shared audible-change boundary (factored from device selection
// and reused by the default-device reaction): capture the position, full
// teardown (the outgoing device gets its restore), reopen the SAME track, seek
// back, restore Paused if that is where we were. A no-op from Stopped or with
// nothing current; a failed reopen settles Stopped with onError, the pending
// queue preserved. Position from the same source firePosition uses, so the
// resume target is exactly what the user last saw.
void Engine::Impl::reopenAtPosition() {
    const State entryState = stateAtomic.load(std::memory_order_relaxed);
    if (entryState == State::Stopped || !decoder || currentPath.empty()) {
        return;
    }
    const std::uint32_t rate = format.sampleRate;
    const double        resumeAt =
        (rate != 0)
            ? static_cast<double>(ringSource.playheadFrames()) /
                  static_cast<double>(rate)
            : 0.0;
    const bool wasPaused = (entryState == State::Paused);

    teardownCurrent();
    if (!openAndStart(currentPath)) {
        enterStopped();
        return;
    }
    if (resumeAt > 0.0) {
        doSeek(resumeAt);
    }
    if (wasPaused) {
        doPause();
    }
}

// Sink event reactions. All engine-thread (dispatched commands), all sink-agnostic.

// The engine half of external-rate forgiveness: the sink already forgave its
// ledger; here the PUBLISHED
// outcome catches up with reality so the UI suffix stops lying the moment the
// world changes, not at the next boundary. The EVENT'S rate wins over the live
// measurement, the REVERSE of startTrack's publication: the emitter certified
// the rate (CoreAudio reads the device property before emitting; a PipeWire
// settings-force transition is deterministic about where the graph goes),
// while the measurement can lag by a whole driver reconfiguration; testing
// caught this reaction outrunning an ALSA reclock and printing
// yesterday's rate. The measurement remains the fallback for an emitter that
// could not name a rate.
void Engine::Impl::doExternalRateChanged(std::uint32_t newRateHz) {
    if (!sink || !deviceOpen) {
        return;  // stale event racing a close: nothing published, nothing to fix
    }
    const std::uint32_t measured = sink->measuredDeviceRateHz();
    const std::uint32_t outRate  = (newRateHz != 0) ? newRateHz : measured;
    if (outRate == 0) {
        notifyInfo("device rate changed externally (new rate unknown)");
        return;
    }
    const bool outBitPerfect = format.isValid() && outRate == format.sampleRate;
    outcomeBits.store((std::uint64_t{1} << 33) |
                          (outBitPerfect ? (std::uint64_t{1} << 32) : 0u) |
                          std::uint64_t{outRate},
                      std::memory_order_release);
    notifyInfo("device rate changed externally to " + std::to_string(outRate) +
               " Hz (" + (outBitPerfect ? "bit-perfect" : "resampled") + ")");
    if (Engine::Listener* l = listener.load(std::memory_order_acquire)) {
        l->onDeviceOutcomeChanged(outRate, outBitPerfect);
    }
}

// The default output moved and this sink cannot migrate a live stream itself
// (the only sinks that emit this; see the interface contract). Following the
// default means the user asked for "wherever the system points", so re-resolve
// audibly at the kept position; a pinned device is unaffected. Either way the
// pickers get a fresh enumeration, since the default-marking changed.
void Engine::Impl::doDefaultDeviceChanged() {
    notifyInfo("system default output device changed");
    doRequestOutputDevices();
    if (outputDeviceId.empty()) {
        reopenAtPosition();
    }
}

// The selectable set changed (hot-plug, profile flip): push a fresh
// enumeration so any open picker tracks it live.
void Engine::Impl::doDeviceListChanged() {
    doRequestOutputDevices();
}

// Ledger forward (persisted upstream). Pure relay: the sink owns the truth, the
// listener owns the file.
void Engine::Impl::doRateDebtChanged(const std::string& deviceId,
                                     std::uint32_t originalRateHz,
                                     std::uint32_t borrowedRateHz) {
    if (Engine::Listener* l = listener.load(std::memory_order_acquire)) {
        l->onRateDebtChanged(deviceId, originalRateHz, borrowedRateHz);
    }
}

// Bring an already-opened decoder current and Playing, choosing the cheapest
// transition: a HOLD-CUT (keep the device open, park the RT thread, swap the
// decoder, reprime, resume) when the source format is IDENTICAL to what is
// already running, otherwise a park-and-reconfigure through startTrack. Shared
// by next() and playNow(); the callers differ only in how they obtained `dec`
// and what they do on a failed start (this returns the success bool and leaves
// that to them). Assumes the caller has already cleared any pending seams.
// Returns true if a track is now current and Playing; false if the reconfigure
// start failed (already torn down to a closed device).
bool Engine::Impl::cutOrStartTo(std::unique_ptr<IDecoder> dec,
                                const std::string&        path) {
    // The hold gate (the manual-cut form of the stitch gate): hold the device
    // only when it is actually OPEN and the next source format is identical, so
    // the sink's negotiated stream format does not have to change. The state
    // != Stopped term is load-bearing for playNow, which (unlike next, a no-op
    // while Stopped) can reach here from Stopped: after a teardown the ring can
    // still report configured() and `format` can still hold the prior track's
    // value, so without this term a same-format playNow from Stopped would
    // wrongly HOLD-CUT onto a closed sink instead of starting fresh. From
    // Stopped this is therefore always false and we take the reconfigure path
    // below (a real open). A different source format never holds, even when
    // the device rate would survive: the sink's stream format is part of what
    // startTrack renegotiates, so that case goes through reconfigure. The
    // rateMode term is the mode gate, the manual-cut twin of the stitch gate in
    // handleProducerEos: a mode flipped since this stream opened voids the
    // same-format-same-decision premise, so the cut reconfigures and
    // re-decides under the live mode.
    const bool canHold =
        stateAtomic.load(std::memory_order_relaxed) != State::Stopped &&
        sink && ringSource.configured() && (dec->format() == format) &&
        (rateMode.load(std::memory_order_acquire) == openRateMode);

    if (canHold) {
        // HOLD-CUT: keep the device open at its negotiated rate. Park the RT
        // thread (the pause primitive, NOT close, so there is no rate
        // renegotiation), flush the ring, swap the decoder, reprime, and resume.
        // A responsive, click-free cut with only a brief sub-buffer gap, and no
        // RateManager / sink->open round trip.
        sink->stop();        // joins the RT thread; ring + device otherwise untouched
        ringSource.reset();  // RT parked: flush, clear EOS flags, counters + position origin to 0

        decoder     = std::move(dec);
        currentPath = path;
        // format and channels are unchanged (the gate guaranteed it), so the
        // existing ring (same capacity) and the staging buffer stay valid.
        stagedFrames      = 0;
        stagedOffset      = 0;
        pendingFinalBlock = false;

        const std::size_t capacity = ringSource.writableFrames();  // empty ring: exact capacity
        // Snapshot before priming for the same reason as startTrack: a held track
        // shorter than the prime threshold can stitch the next track during the
        // prime, reassigning decoder/currentPath.
        const TrackInfo cutInfo = makeTrackInfo(currentPath, *decoder);
        primeUpTo(capacity / 2);
        publishFormat(format);  // unchanged; kept coherent

        lastPositionFire = std::chrono::steady_clock::now() -
                           std::chrono::milliseconds(kPositionIntervalMs);
        if (sink) {
            sink->start();
        }
        setState(State::Playing);  // no-op if already Playing; Paused -> Playing on a cut
        announceTrack(cutInfo);
        firePosition();
        return true;
    }

    // Genuine reconfigure: different device config (rate or channels), or no live
    // ring (a fresh start, including every playNow from Stopped). Park the
    // current track (RT joined, engine state flushed, device KEPT open when it
    // was; idempotent when already torn down) and start fresh with the decoder
    // we already opened (no redundant open); startTrack offers the parked
    // device an in-place reconfigure before falling back to close+open.
    parkForTransition();
    return startTrack(std::move(dec), path);
}

// seek(seconds): reposition the LIVE (producer-logical, most-recently-opened)
// decoder within the current track. Runs on the engine thread with no lock held;
// it parks the RT thread for the duration, so it is the only actor touching the
// ring and the decoder. Distinct from the stop-rewind: that opens a fresh decoder
// from 0, this repositions the existing one. The transport state is preserved
// A seek clears ALL pending stitch state; a seek landing in
// the brief drain-overlap window therefore targets the upcoming (already-opened)
// track, an accepted wrinkle of the gapless design (the producer-logical
// current is the only live decoder there is to reseek).
void Engine::Impl::doSeek(double seconds) {
    const State entryState = stateAtomic.load(std::memory_order_relaxed);

    // Seek while Stopped is meaningless: no live decoder to reposition. Ignore.
    if (entryState == State::Stopped || !decoder) {
        return;
    }

    // An unseekable source cannot honor the request. Leave playback entirely
    // untouched (position keeps reporting) and report the refusal. Checked BEFORE
    // the sink is parked so an ignored seek never interrupts the audio.
    if (!decoder->seekable()) {
        notifyError("seek: source is not seekable");
        return;
    }

    // Resolve the target frame. Clamp below at 0; clamp above at totalFrames only
    // when the length is known. A 0 total means the length is unknown (rare for a
    // seekable source), so we pass the raw target through and let the decoder rule
    // on it. A target at or past a known end lands exactly at the end and lets the
    // finished -> advance/stop path run, exactly as natural completion would.
    const std::uint32_t rate = format.sampleRate;
    double              tf   = seconds * static_cast<double>(rate);
    if (tf < 0.0) {
        tf = 0.0;
    }
    auto target = static_cast<std::uint64_t>(tf);
    const std::uint64_t total  = decoder->totalFrames();
    if (total > 0 && target > total) {
        target = total;
    }

    // Park the RT consumer. This is the pause primitive, NOT close: the device
    // stays open at its negotiated rate, so there is no rate renegotiation and no
    // glitch beyond the brief gap. Idempotent when already Paused. After this
    // returns no pull() is in flight, so the engine thread is the sole actor on
    // the ring, the counters, and the position origin.
    if (sink) {
        sink->stop();
    }

    // With the RT thread parked, flush the ring and clear the EOS flags (so a
    // backward seek out of a near-EOS drain is correct), reset the counters and
    // position origin, clear pending stitch state, and drop any staged
    // pre-seek decode block so its stale samples never bleed into the fresh ring.
    ringSource.reset();
    seams.clear();
    stagedFrames      = 0;
    stagedOffset      = 0;
    pendingFinalBlock = false;

    if (!decoder->seek(target)) {
        // false == stop: the read position is now unspecified, so the only safe
        // move is to abandon the track. Tear down (restores the device rate) and
        // settle into Stopped, keeping currentPath remembered so a later play()
        // replays it from frame 0. Rare once the target is clamped.
        notifyError("seek: decoder rejected the target");
        teardownCurrent();
        enterStopped();
        return;
    }

    // Map the next sample heard to the post-seek source frame BEFORE refilling, so
    // the first frames pulled after start() report the right position.
    ringSource.setPlayhead(target);

    // Re-prime so the restart does not underrun, the same half-ring threshold as
    // a fresh open. The ring is empty here, so writableFrames() is the exact
    // capacity (no concurrent consumer to make it a lower bound).
    const std::size_t capacity = ringSource.writableFrames();
    primeUpTo(capacity / 2);

    // Resume the RT thread only if we were Playing; a Paused seek stays Paused
    // with the sink parked and resumes from the new spot on the next play().
    if (entryState == State::Playing && sink) {
        sink->start();
    }

    // Reflect the new position at once (Playing or Paused) and rebase the tick
    // clock so the periodic fire is measured from here.
    lastPositionFire = std::chrono::steady_clock::now();
    firePosition();
}

// ---------------------------------------------------------------------------
// Start an already-opened decoder for a FRESH track: configure the ring for its
// format, pre-buffer, resolve the rate decision, open and start the sink.
// Returns false on any failure (no sink, sink open), having fired onError and
// torn down. The factory->open step lives in openAndStart, not here, so doNext
// and playNow can hand in a decoder they already opened (no redundant open at
// a reconfigure boundary).
//
// On entry the previous track is already torn down OR parked for transition
// (either way: RT joined, ring reset, decoder dropped; deviceOpen says whether
// the device itself survived, which decides the reconfigure offer below).
// currentPath is committed here so a failed open never poisons the remembered
// track.
bool Engine::Impl::startTrack(std::unique_ptr<IDecoder> dec,
                              const std::string&        path) {
    decoder     = std::move(dec);
    currentPath = path;
    format      = decoder->format();
    channels    = static_cast<std::size_t>(format.channels);

    // Producer scratch and staged-block carry, fresh for this track.
    staging.assign(kBlockFrames * (channels == 0 ? 1u : channels), 0.0f);
    stagedFrames      = 0;
    stagedOffset      = 0;
    pendingFinalBlock = false;

    const std::size_t capacity =
        computeCapacityFrames(format.sampleRate, kTargetLatencyMs, kBlockFrames);
    ringSource.reconfigure(capacity, format.channels);  // fresh ring, flags + counters + origin cleared
    publishFormat(format);

    // Load the rate mode ONCE for this whole open and snapshot it as the mode
    // in force. The snapshot must land BEFORE primeUpTo: a
    // multi-track start whose first track is shorter than the prime threshold
    // stitches the next track DURING the prime, and that stitch's mode gate
    // must compare against this open's mode, not against a stale snapshot from
    // a previous open (which would wrongly refuse a perfectly coherent stitch).
    const RateMode mode = rateMode.load(std::memory_order_acquire);
    openRateMode        = mode;

    // Snapshot THIS track's identity for its onTrackChanged BEFORE priming.
    // primeUpTo can reach end of stream on a track shorter than the prime
    // threshold and stitch the next track via handleProducerEos, which reassigns
    // `decoder` and `currentPath`; reading them after the prime would announce the
    // stitched track's identity instead. The seam(s) that prime pushes are crossed
    // later by the run loop and announce those subsequent tracks correctly.
    const TrackInfo startedInfo = makeTrackInfo(currentPath, *decoder);

    // Pre-buffer about half the ring (or until the source ends) so the very first
    // callbacks do not underrun.
    primeUpTo(capacity / 2);

    if (!sink) {
        notifyError("play: no sink set");
        teardownCurrent();
        return false;
    }

    // Resolve the rate decision before opening. Read the device's
    // capabilities ONCE, ask the portable RateManager what to do given this
    // track's source format, the chosen mode, and the device format we are
    // transitioning from (openDeviceFormat; invalid on a fresh start), then hand
    // the decision to the sink, which executes it without re-deriving anything.
    const SinkCapabilities caps = sink->capabilities();
    const RateDecision     decision =
        rateManager.decide(format, caps, mode, openDeviceFormat);

    // When the device survived the boundary (a transition PARKED it rather than
    // closing), offer the sink an in-place reconfigure: retune the existing
    // unit, no release/reacquire churn, no mid-session rate restore. A sink
    // that cannot (the default) or will not (device changed underneath)
    // declines, and the close+open fallback below is the full boundary. The
    // pull source registered at the original open remains attached and valid:
    // ringSource is the same object across every transition, merely
    // reconfigured above.
    bool sinkReady = false;
    if (deviceOpen) {
        sinkReady = sink->reconfigure(format, decision);
    }
    if (!sinkReady) {
        if (deviceOpen) {
            sink->close();  // declined: release, then a genuine open below
            deviceOpen = false;
        }
        sinkReady = sink->open(format, decision, &ringSource);
    }
    if (!sinkReady) {
        notifyError("could not open audio sink");
        teardownCurrent();
        return false;
    }
    deviceOpen = true;

    // Remember the resolved device config so the NEXT track's decision compares
    // against it. The "device format" is the chosen device rate paired with this
    // track's channel count (the sink's stream format carries the source
    // channels, so a channel change is a reconfigure even at the same rate).
    openDeviceFormat.sampleRate = decision.deviceRate;
    openDeviceFormat.channels   = format.channels;

    // Publish the device outcome for the UI, measurement
    // first: the sink's measured device rate is the authority when it has one
    // (real hardware can accept a switch and still misclock), the decision's
    // prediction the fallback for sinks that cannot measure (NullSink).
    // Published BEFORE announceTrack below, so a listener reading the
    // observers inside onTrackChanged sees values coherent with that track.
    // Stitches and HOLD-CUTs never pass through here, and correctly so: the
    // open, and therefore the outcome, is unchanged across them.
    {
        const std::uint32_t measured = sink->measuredDeviceRateHz();
        const std::uint32_t outRate =
            (measured != 0) ? measured : decision.deviceRate;
        const bool outBitPerfect =
            (measured != 0) ? (measured == format.sampleRate)
                            : !decision.resampleNeeded;
        outcomeBits.store((std::uint64_t{1} << 33) |
                              (outBitPerfect ? (std::uint64_t{1} << 32) : 0u) |
                              std::uint64_t{outRate},
                          std::memory_order_release);
    }

    // Prime the position tick so the first loop iteration fires position 0.0
    // promptly, the moment onTrackChanged lands, rather than up to a tick later.
    lastPositionFire =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(kPositionIntervalMs);

    sink->start();

    setState(State::Playing);
    announceTrack(startedInfo);
    return true;
}

// Open `path` through the factory, then start it fresh. Returns false on a failed
// open (onError already fired) or a failed start.
bool Engine::Impl::openAndStart(const std::string& path) {
    std::string err;
    std::unique_ptr<IDecoder> dec = factory->open(path, &err);
    if (!dec) {
        notifyError("could not open '" + path + "': " + err);
        return false;
    }
    return startTrack(std::move(dec), path);
}

// Pop pending paths until one opens and starts, else settle into Stopped (the
// remembered currentPath stays whatever the last attempt set it to, so a later
// play() replays that track).
void Engine::Impl::advanceToPendingOrStop() {
    while (!pending.empty()) {
        std::string p = std::move(pending.front());
        pending.pop_front();
        if (openAndStart(p)) {
            return;
        }
        // open failed: onError already fired; try the next queued path.
    }
    enterStopped();
}

// Stop and close the sink (the RT join), then flush the engine state. The full
// teardown, for the paths that genuinely END a listening session: stop, error,
// shutdown, and a failed start. Closing is where the sink repays any rate
// debt, so this must NOT run at a mere track boundary; transitions use
// parkForTransition below instead.
void Engine::Impl::teardownCurrent() {
    if (sink) {
        sink->stop();   // synchronous join of the RT thread
        sink->close();  // restore device rate, release the device
    }
    deviceOpen = false;
    flushEngineState();
}

// The transition-boundary sibling: park the RT thread but KEEP the
// device open, so the startTrack that follows can offer the sink an in-place
// reconfigure instead of a close/open round trip. Safe from Stopped (no sink
// open: the stop is skipped and this is a pure flush), and a transition that
// then dead-ends (empty queue, every pending path failing) is caught by
// enterStopped, which releases the parked device.
void Engine::Impl::parkForTransition() {
    if (sink && deviceOpen) {
        sink->stop();  // join only; the device stays open
    }
    flushEngineState();
}

// The shared flush: with the RT thread parked (by either caller above), reset
// the ring, drop the decoder, and clear the seam queue. Order is load-bearing:
// the consumer must be gone before the ring it reads is reset. currentPath and
// the pending queue are NOT touched here; callers decide what to remember.
// ring.reset() also resets the counters and position origin, so a subsequent
// position() reads 0 once the format goes invalid.
void Engine::Impl::flushEngineState() {
    ringSource.reset();  // RT parked: safe to flush, clear the EOS flags, reset counters/origin
    decoder.reset();
    seams.clear();       // Any pending gapless stitch state is void after a teardown

    stagedFrames      = 0;
    stagedOffset      = 0;
    pendingFinalBlock = false;
    publishFormat(AudioFormat{});  // currentFormat() / position() read invalid while not loaded
    totalFramesAtomic.store(0, std::memory_order_release);  // duration() reads 0 while not loaded
    currentInfo.reset();           // currentTrackInfo() reads an empty TrackInfo while Stopped
    currentNominalKbps = 0;
    liveBitrateAtomic.store(0, std::memory_order_release);  // liveBitrateKbps() reads 0 while Stopped
}

// One decode/write granule. Returns the frames written into the ring this call.
// The ordering rule is decode-then-write, with "publish inputExhausted with a
// release store AFTER the final ring write"; the two end-of-stream points route
// through handleProducerEos, which decides whether to stitch the next track
// gaplessly or to mark the input exhausted.
std::size_t Engine::Impl::produceOneStep() {
    if (stagedFrames == 0) {
        const std::size_t got = decoder->read(staging.data(), kBlockFrames);
        if (got == 0) {
            // Already at end of stream (an exact-block-multiple track ends here).
            // The previous block was fully written before we got here, so a stitch
            // or markInputExhausted now correctly accounts for all outgoing frames.
            handleProducerEos();
            return 0;
        }
        stagedFrames      = got;
        stagedOffset      = 0;
        pendingFinalBlock = (got < kBlockFrames);  // short read == final block
    }

    const std::size_t remaining = stagedFrames - stagedOffset;
    const std::size_t n =
        ringSource.write(staging.data() + stagedOffset * channels, remaining);
    stagedOffset += n;

    if (stagedOffset == stagedFrames) {
        const bool wasFinal = pendingFinalBlock;
        stagedFrames      = 0;
        stagedOffset      = 0;
        pendingFinalBlock = false;
        if (wasFinal) {
            // The final (short) block has now been fully written, so every frame
            // of this track is in the ring. Stitch the next track gaplessly, or
            // mark exhausted; either way the seam threshold (if any) is exact.
            handleProducerEos();
        }
    }
    // n == 0 means the ring is full; the engine loop's bounded wait handles it.
    return n;
}

// The producer-logical current track has reached end of stream. Decide
// whether to stitch the next pending track gaplessly (the gate: identical source
// format) or to mark the input exhausted and let the drain -> finished ->
// teardown -> advance path run (a genuine boundary, or the end of the queue).
//
// On a stitch this opens the next decoder at EOS and keeps writing into the SAME
// running ring, relying on the outgoing ring tail as runway (local files only;
// slow-storage runway is out of scope). It records a seam marker but does NOT set
// inputExhausted and does NOT touch the sink, so the consumer sees no
// discontinuity. The format/channels are unchanged by the gate, so the ring,
// the staging buffer, and the position rate all stay valid; only identity and the
// descriptive facts differ, which the marker carries for the deferred
// onTrackChanged.
void Engine::Impl::handleProducerEos() {
    if (!gaplessEnabled) {
        ringSource.markInputExhausted();
        return;
    }

    // Mode gate, checked before any candidate: if the rate mode changed since
    // this stream was opened, the "same source format means the same device
    // decision" premise behind stitching is void, so refuse the stitch and
    // drain. The finished -> park -> advanceToPendingOrStop path then reopens
    // the next track through startTrack, which re-decides under the live mode
    // and re-snapshots it. The queue is deliberately left untouched here,
    // exactly like the format-gate refusal below. The scenario this prevents:
    // AlwaysResample holding a 44100 track at a 96000 device, the user flips to
    // bit-perfect, and without the gate the next 44100 track would stitch into
    // the 96000 stream as if nothing had changed.
    if (rateMode.load(std::memory_order_acquire) != openRateMode) {
        ringSource.markInputExhausted();
        return;
    }

    while (!pending.empty()) {
        const std::string nextPath = pending.front();
        std::string       err;
        std::unique_ptr<IDecoder> dec = factory->open(nextPath, &err);
        if (!dec) {
            // A broken file mid-album: report it, drop it, and try the next path
            // for a stitch rather than aborting the run.
            notifyError("could not open '" + nextPath + "': " + err);
            pending.pop_front();
            continue;
        }
        if (dec->format() != format) {
            // The gate fails: the source format differs, so the frames cannot be
            // written sample-contiguously into the running ring. Fall back to the
            // drain-then-reconfigure path. Leave the probe path on the queue (do
            // NOT pop) so advanceToPendingOrStop reopens it; the one redundant
            // open is accepted at a genuine rate/channel boundary where a brief
            // gap is expected anyway.
            ringSource.markInputExhausted();
            return;
        }
        // The gate passes. Stitch: snap the seam threshold (every frame the
        // outgoing track wrote into this ring), snapshot the incoming track for
        // the deferred onTrackChanged, swap the producer-logical current to the
        // new decoder, and keep producing into the same ring.
        SeamMarker marker;
        marker.thresholdFrame = ringSource.totalFramesWritten();
        marker.info           = makeTrackInfo(nextPath, *dec);
        seams.push_back(std::move(marker));

        decoder     = std::move(dec);
        currentPath = nextPath;  // producer-logical current (what a seek targets)
        // format / channels unchanged (gate guaranteed dec->format() == format).
        pending.pop_front();
        return;  // the next produceOneStep reads the new decoder into the ring
    }

    // Queue empty: the genuine end of playback. Mark exhausted so the existing
    // finished -> teardown -> advanceToPendingOrStop (-> enterStopped) path runs.
    ringSource.markInputExhausted();
}

// An audible gapless boundary has passed. Rebase the reported position so it
// reads 0 for the now-audible track, publish its format (identical across a
// stitch, but kept coherent), and fire onTrackChanged for the track now being
// heard. The rebase is the ONLY position rebase that runs with the RT thread
// live; it is race-free because it writes only the engine-owned origin (see
// EngineRingSource). The position tick clock is reset so position ~0 fires
// promptly for the new track, matching the prompt fire a fresh open gets.
void Engine::Impl::crossSeam(const SeamMarker& marker) {
    ringSource.rebasePositionAtSeam(marker.thresholdFrame);
    publishFormat(marker.info.format);
    announceTrack(marker.info);
    lastPositionFire =
        std::chrono::steady_clock::now() - std::chrono::milliseconds(kPositionIntervalMs);
}

// Decode and write until at least thresholdFrames are buffered or the source
// ends. Runs before the sink is started (fresh open) or while it is parked (after
// a seek or during a HOLD-CUT), so there is no concurrent consumer: reading
// bufferedFrames() here is safe. If the track ends during priming, produceOneStep
// may stitch the next track via handleProducerEos and keep going; the loop's
// inputExhausted check terminates it when the queue is truly empty.
void Engine::Impl::primeUpTo(std::size_t thresholdFrames) {
    while (ringSource.bufferedFrames() < thresholdFrames &&
           !ringSource.inputExhausted()) {
        const std::size_t n = produceOneStep();
        if (n == 0 && ringSource.writableFrames() == 0) {
            break;  // ring full before the threshold (only on a pathologically tiny ring)
        }
    }
}

// ---------------------------------------------------------------------------
// Publication and notification. All run on the engine thread; the listener
// pointer is loaded with acquire so a setListener from another thread is seen.
void Engine::Impl::setState(State s) {
    const State prev = stateAtomic.exchange(s, std::memory_order_release);
    if (prev != s) {
        if (Engine::Listener* l = listener.load(std::memory_order_acquire)) {
            l->onStateChanged(s);
        }
    }
}

// Settle into Stopped, releasing a parked device and clearing the gate origin.
// A transition that dead-ends here arrives with the device parked open; once
// the engine is Stopped the device is closed, so the next playback transitions
// from nothing: the gate must read a fresh open rather than comparing against
// the track that just ended. Distinct from the bare setState(Stopped) so that a
// track-to-track reconfigure, which does NOT pass through here, keeps
// openDeviceFormat for the incoming decision.
void Engine::Impl::enterStopped() {
    // A transition that dead-ends here (empty queue, or every pending path
    // failing to open) arrived with the device parked open; settling
    // into Stopped means releasing it, which is also where the sink repays any
    // rate debt. After a full teardownCurrent this is a no-op.
    if (sink && deviceOpen) {
        sink->close();
        deviceOpen = false;
    }
    openDeviceFormat = AudioFormat{};
    outcomeBits.store(0, std::memory_order_release);  // device released: no outcome
    setState(State::Stopped);
}

void Engine::Impl::publishFormat(const AudioFormat& f) {
    formatBits.store(packFormat(f), std::memory_order_release);
}

// Publish the now-current track for the lock-free observers, then notify the
// listener. The order matters: make currentTrackInfo()/duration() readable
// BEFORE firing onTrackChanged, so a listener that reacts by pulling
// currentTrackInfo() sees exactly the track it was just told about. Engine-thread
// only; called from startTrack, crossSeam, and the cutOrStartTo HOLD-CUT, the same
// three points a track becomes current.
void Engine::Impl::announceTrack(const TrackInfo& info) {
    currentInfo.publish(info);
    totalFramesAtomic.store(info.totalFrames, std::memory_order_release);
    // Seed the live bitrate with this track's nominal so the status
    // line shows a figure at once, before the first position tick refreshes it
    // from the decoder. This single funnel (startTrack, crossSeam, the HOLD-CUT)
    // keeps the nominal correct across gapless seams too.
    currentNominalKbps = info.source.bitrateKbps;
    liveBitrateAtomic.store(currentNominalKbps, std::memory_order_release);
    notifyTrackChanged(info);
}

void Engine::Impl::notifyTrackChanged(const TrackInfo& info) const {
    if (Engine::Listener* l = listener.load(std::memory_order_acquire)) {
        l->onTrackChanged(info);
    }
}

void Engine::Impl::notifyError(const std::string& message) const {
    if (Engine::Listener* l = listener.load(std::memory_order_acquire)) {
        l->onError(message);
    }
}

void Engine::Impl::notifyInfo(const std::string& message) const {
    if (Engine::Listener* l = listener.load(std::memory_order_acquire)) {
        l->onInfo(message);
    }
}

void Engine::Impl::notifyOutputDevices(
    const std::vector<AudioDeviceInfo>& devices) const {
    if (Engine::Listener* l = listener.load(std::memory_order_acquire)) {
        l->onOutputDevices(devices);
    }
}

// ILogOutput: the sink's diagnostic lines arrive here on
// the engine thread and go straight out as onInfo. No buffering, no filtering:
// the sink already decided the line was worth printing, and the listener side
// (the Qt bridge) owns any marshalling.
void Engine::Impl::logLine(const std::string& line) {
    notifyInfo(line);
}

// Push the current position to the listener now. Called on the engine thread
// from the cadence tick, from doPause, from doSeek, and from cutOrStartTo's
// HOLD-CUT, every one of which holds a valid `format`, so
// currentPositionSeconds() reads a real rate.
void Engine::Impl::firePosition() {
    // Refresh the live decode bitrate on the same cadence as position.
    // The decoder reports 0 when it publishes no live figure (PCM, the test ramp),
    // in which case the track nominal stands in. `decoder` is valid in every
    // firePosition caller (each holds a loaded track), but the guard keeps it safe
    // regardless.
    if (decoder) {
        const std::uint32_t live = decoder->currentBitrateKbps();
        liveBitrateAtomic.store(live != 0 ? live : currentNominalKbps,
                                std::memory_order_release);
    }
    if (Engine::Listener* l = listener.load(std::memory_order_acquire)) {
        l->onPositionChanged(currentPositionSeconds());
    }
}

// playhead / sampleRate, in seconds. Reads the engine-thread-private `format`, so
// it is for engine-thread callers only; the public Engine::position() reads the
// published formatBits instead. Returns 0.0 when the rate is unknown so a caller
// never divides by zero. Across a gapless stitch `format` is unchanged, so
// the rate stays correct through the boundary.
double Engine::Impl::currentPositionSeconds() const {
    const std::uint32_t rate = format.sampleRate;
    if (rate == 0) {
        return 0.0;
    }
    return static_cast<double>(ringSource.playheadFrames()) /
           static_cast<double>(rate);
}

// ===========================================================================
// Public forwarding layer.
Engine::Engine() : m_impl(std::make_unique<Impl>()) {
    m_impl->start();
}

Engine::~Engine() {
    if (m_impl) {
        m_impl->requestShutdownAndJoin();
    }
}

bool Engine::setSink(std::unique_ptr<IAudioSink> sink) {
    // Lock discipline first, because it shapes the order below: the engine
    // NEVER calls into a sink while holding the command mutex. Sinks deliver
    // ISinkEventListener events from their own platform threads with a sink
    // lock held (PipeWire holds its loop lock for the whole registry dispatch,
    // and the handler cannot drop it), and every event is a post() that takes
    // the command mutex. A sink call made under that mutex would take the two
    // locks in the opposite order: the inversion that closes into a deadlock
    // the moment a sink with a live event channel is stopped or destroyed
    // under the mutex. run() already honors this (it unlocks around
    // process()); this method is the only other sink toucher.
    //
    // So the wiring happens on the argument BEFORE the mutex, while the sink
    // is still private to the caller's unique_ptr and no thread can see it:
    // the Impl is the ILogOutput, and the ISinkEventListener too, so both
    // calls just hand the sink a pointer to its own owner, valid for the
    // sink's whole life (default no-ops on sinks with nothing to say, NullSink).
    // A hot-plug event landing between this wiring and the install below
    // posts a DeviceListChanged that doRequestOutputDevices answers with an
    // empty list (it tolerates a missing sink); PipeWireSink drains its
    // initial registry enumeration before construction returns, CoreAudioSink
    // enumerates on demand, and every caller re-enumerates explicitly, so
    // nothing is lost.
    if (sink) {
        sink->setLogOutput(m_impl.get());
        sink->setEventListener(m_impl.get());
    }

    // The pointer store is under the command mutex so the release here is
    // ordered before the engine thread's next mutex acquire (which it does to
    // drain commands), making the sink, wiring included, visible to the
    // engine thread before it processes any command posted after this
    // returns. The Stopped gate makes the "set before first play()" contract a
    // structural refusal rather than a comment: outside Stopped the engine
    // thread reads the sink WITHOUT this mutex, so a swap would be a data
    // race on a live object. Stopped means the engine thread no longer
    // touches the sink until it processes a subsequent play command, and that
    // processing happens-after this store via the mutex. A play command
    // already queued but not yet processed is fine for the same reason: it
    // will open the NEW sink. On refusal the rejected sink is destroyed with
    // the argument and the current sink is untouched.
    //
    // The retired sink (a runtime swap; null on the first injection) leaves
    // the scope AFTER the mutex does, for the discipline above: its
    // destructor stops its platform thread under its own lock, and that
    // thread may be inside a post() waiting for the mutex this method holds.
    std::unique_ptr<IAudioSink> retired;
    {
        std::lock_guard<std::mutex> lock(m_impl->mtx);
        if (m_impl->stateAtomic.load(std::memory_order_acquire) != State::Stopped) {
            return false;
        }
        retired = std::exchange(m_impl->sink, std::move(sink));
    }
    return true;
}

void Engine::setDecoderFactory(IDecoderFactory* factory) {
    std::lock_guard<std::mutex> lock(m_impl->mtx);
    m_impl->factory = (factory != nullptr) ? factory : &m_impl->defaultFactory;
}

void Engine::setListener(Listener* listener) {
    m_impl->listener.store(listener, std::memory_order_release);
}

void Engine::setRateMode(RateMode mode) {
    // Atomic, like setListener: set from the controlling thread before the first
    // play(), read on the engine thread at each track start. Release here pairs
    // with the engine thread's acquire load in startTrack.
    m_impl->rateMode.store(mode, std::memory_order_release);
}

// Master volume. Forwarded straight to the real-time source, NOT posted
// as a command: a volume change must take effect now, not behind whatever is
// queued, and it is a single lock-free atomic store the RT thread reads in pull().
// The ringSource is a long-lived Impl member, so this is safe in any state,
// including Stopped and before the first play(); the stored gain simply waits for
// the next pull. No engine lock: setGain does its own atomic publish, and there
// is no other engine state to keep consistent with it.
void Engine::setVolumeGain(float linearGain) noexcept {
    m_impl->ringSource.setGain(linearGain);
}

float Engine::volumeGain() const noexcept {
    return m_impl->ringSource.gain();
}

// Visualization tap (spectrum feed). All three forward straight to the long-lived
// ringSource, like the master-volume pair above: copyScope is the GUI-thread
// reader the visualizer timer drives, and setScopeSource / scopeSource are the
// source selector and its read-back. No command queue and no engine lock: the tap
// owns its own lock-free synchronization, the source selector is a self-contained
// atomic, and there is no other engine state to keep consistent with either. Safe
// in any state, including Stopped and before the first play().
std::size_t Engine::copyScope(float* out, std::size_t frames) const noexcept {
    return m_impl->ringSource.copyScope(out, frames);
}

void Engine::setScopeSource(ScopeSource source) noexcept {
    m_impl->ringSource.setScopeSource(source);
}

ScopeSource Engine::scopeSource() const noexcept {
    return m_impl->ringSource.scopeSource();
}

void Engine::enqueue(std::string path) {
    m_impl->post(Command{CommandType::Enqueue, std::move(path), 0.0, {}});
}
void Engine::playNow(std::string path) {
    m_impl->post(Command{CommandType::PlayNow, std::move(path), 0.0, {}});
}
void Engine::setQueue(std::vector<std::string> paths) {
    m_impl->post(Command{CommandType::SetQueue, std::string{}, 0.0, std::move(paths)});
}
void Engine::play()       { m_impl->post(Command{CommandType::Play,       {}, 0.0, {}}); }
void Engine::pause()      { m_impl->post(Command{CommandType::Pause,      {}, 0.0, {}}); }
void Engine::stop()       { m_impl->post(Command{CommandType::Stop,       {}, 0.0, {}}); }
void Engine::next()       { m_impl->post(Command{CommandType::Next,       {}, 0.0, {}}); }
void Engine::clearQueue() { m_impl->post(Command{CommandType::ClearQueue, {}, 0.0, {}}); }
void Engine::seek(double seconds) {
    m_impl->post(Command{CommandType::Seek, {}, seconds, {}});
}

void Engine::requestOutputDevices() {
    m_impl->post(Command{CommandType::RequestOutputDevices, {}, 0.0, {}});
}

void Engine::selectOutputDevice(std::string deviceId) {
    m_impl->post(
        Command{CommandType::SelectOutputDevice, std::move(deviceId), 0.0, {}});
}

State Engine::state() const noexcept {
    return m_impl->stateAtomic.load(std::memory_order_acquire);
}

AudioFormat Engine::currentFormat() const noexcept {
    return unpackFormat(m_impl->formatBits.load(std::memory_order_acquire));
}

// Position for the controlling thread: read the PUBLISHED format (not the
// engine-private one) so it is correctly 0.0 while Stopped, and the playhead from
// the RT-touched source. A tiny inconsistency between the two atomics across a
// track change (a fresh open or a gapless seam) is harmless: a zero rate yields
// 0.0 regardless, and an identical-rate stitch keeps the divisor correct.
double Engine::position() const noexcept {
    const AudioFormat f =
        unpackFormat(m_impl->formatBits.load(std::memory_order_acquire));
    if (f.sampleRate == 0) {
        return 0.0;
    }
    return static_cast<double>(m_impl->ringSource.playheadFrames()) /
           static_cast<double>(f.sampleRate);
}

// The current track, pulled from the published box. Reads no engine-private state
// and takes no engine lock (the box owns its own synchronization), so it is safe
// from any non-RT thread; returns an empty TrackInfo while Stopped. Not noexcept:
// the returned TrackInfo copies a std::string.
TrackInfo Engine::currentTrackInfo() const {
    return m_impl->currentInfo.load();
}

// Duration of the current track in seconds, the lock-free denominator paired with
// position()'s numerator: same published-rate divisor, the frame count from its
// own atomic. 0.0 while Stopped (rate 0) or when the length is unknown.
double Engine::duration() const noexcept {
    const AudioFormat f =
        unpackFormat(m_impl->formatBits.load(std::memory_order_acquire));
    if (f.sampleRate == 0) {
        return 0.0;
    }
    return static_cast<double>(
               m_impl->totalFramesAtomic.load(std::memory_order_acquire)) /
           static_cast<double>(f.sampleRate);
}

// Live decode bitrate for the controlling thread: the lock-free read
// of the figure firePosition() publishes each tick. 0 while Stopped. Thread-safe
// and cheap; the controller reads it alongside position() to drive the status
// line's "NNN kbps" with no dedicated callback.
std::uint32_t Engine::liveBitrateKbps() const noexcept {
    return m_impl->liveBitrateAtomic.load(std::memory_order_acquire);
}

// The device-outcome pull pair. One packed word backs both
// (see Impl::outcomeBits for the layout), so each read is a single acquire
// load; a cleared word (Stopped) reads as rate 0 / not bit-perfect, the
// honest "no device open" answer.
std::uint32_t Engine::outputDeviceRateHz() const noexcept {
    const std::uint64_t bits =
        m_impl->outcomeBits.load(std::memory_order_acquire);
    if ((bits & (std::uint64_t{1} << 33)) == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(bits & 0xFFFFFFFFu);
}

bool Engine::outputBitPerfect() const noexcept {
    const std::uint64_t bits =
        m_impl->outcomeBits.load(std::memory_order_acquire);
    return (bits & (std::uint64_t{1} << 33)) != 0 &&
           (bits & (std::uint64_t{1} << 32)) != 0;
}

}  // namespace rawform::audio
