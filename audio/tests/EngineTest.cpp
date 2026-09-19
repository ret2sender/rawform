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

// EngineTest.cpp
//
// The transport and ThreadSanitizer target. It drives the WHOLE engine, its
// command queue, state machine, and play queue, concurrently with the real
// producer (the engine thread) and a real consumer (NullSink's pull thread).
// No device backend and no codec library are involved: an in-memory ramp
// decoder is injected through an IDecoderFactory, and NullSink discards the
// audio, so the concurrency under test is purely the engine's own.
//
// Seek and position coverage rides on top of the transport scenarios. The
// ramp decoder is SEEKABLE with a known totalFrames (a "noseek:" variant
// stays unseekable to exercise the refusal path), and because the ramp sample
// value equals its frame index, a small capturing sink can prove a seek landed
// at the target frame deterministically, the encoding trick every ramp-based
// test here relies on.
//
// Gapless coverage: the ramp gains an optional per-track value BASE so
// successive stitched tracks occupy distinct, unambiguous value bands; a
// recording sink captures the channel-0 sample of every real frame consumed (to
// prove sample-contiguity across a seam) and the RateDecision per open() (whose
// COUNT discriminates a gapless stitch, no open, from a reconfigure, an open).
// The rate-gate tests, which assert the per-track decision plumbing, pin
// gapless OFF via RAWFORM_NO_GAPLESS so every track still yields a decision; the
// gapless behavior is exercised in its own section.
//
// The rest of the engine's surface has a section each, all on the same
// NullSink: the in-place reconfigure offer at transition boundaries (NullSink's
// opt-in toggle drives both engine branches), output device enumeration and
// selection with the reopen-at-position boundary, the sink event channel
// (NullSink's fire* injectors call the engine from the test thread), the mode
// gates that void the same-format fast paths after a rate-mode flip, the
// device-outcome observers and the info-forwarding chain, and the
// playNow/setQueue primitives with the device-unavailable and seek-rejected
// hardening paths.
//
// Build with -DRAWFORM_SANITIZE=thread and run through ctest. The state and
// position assertions catch transport-logic regressions; TSan catches any race
// in the command/producer/consumer access pattern, including the playhead
// advance, the interleaved-seek soak, and the gapless seam machinery (seam push
// on the producer, crossing/rebase on the run loop, all racing the RT
// consumer). Same hand-rolled CHECK macro and deterministic encoding as the
// rest of the suite; main returns non-zero on any failure.
//
// A note on timing in the assertions: the engine publishes state with an atomic
// store and THEN fires the listener, so observing a state via state() can race
// slightly ahead of the matching listener callback. Where a test inspects the
// listener log it first waits for the terminal state and then settles briefly,
// which is ample for the listener call to land. The NullSink pacing is decoupled
// from real sample time (it consumes far faster than realtime, by design), so
// position assertions that must be deterministic use the FROZEN paused position
// or the captured decoded frame rather than a wall-clock-sensitive value.

#include "rawform/audio/Engine.h"
#include "rawform/audio/IAudioSink.h"
#include "rawform/audio/IDecoder.h"
#include "rawform/audio/IDecoderFactory.h"
#include "rawform/audio/Types.h"

#include "sinks/NullSink.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdlib.h>  // setenv/unsetenv are POSIX, not in <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using rawform::audio::AudioFormat;
using rawform::audio::Engine;
using rawform::audio::IAudioSink;
using rawform::audio::IDecoder;
using rawform::audio::ILogOutput;
using rawform::audio::IDecoderFactory;
using rawform::audio::IPullSource;
using rawform::audio::NullSink;
using rawform::audio::RateDecision;
using rawform::audio::RateManager;
using rawform::audio::RateMode;
using rawform::audio::RateRange;
using rawform::audio::SinkCapabilities;
using rawform::audio::SourceInfo;
using rawform::audio::State;
using rawform::audio::TrackInfo;

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

// ---------------------------------------------------------------------------
// In-memory ramp decoder, SEEKABLE for the seek tests. Produces a
// deterministic ramp of `totalFrames` frames, then end of stream, honoring the
// IDecoder "fill fully, short at EOS, 0 == EOS" contract so the engine's
// producer drives its end-of-stream handshake for real. The sample value equals
// its interleaved sample index PLUS a per-track value base, so a read after a
// seek to frame F yields enc(base + F * channels) in channel 0, which is how a
// seek is proven to land; base 0 is the default for the 2- and 4-part paths.
// The gapless tests use a non-zero base to give each stitched track a distinct
// value band, so a seam is unambiguous against the zero-fill (0.0) and against
// the neighboring track's samples.
inline float enc(std::uint64_t sampleIndex) noexcept {
    return static_cast<float>(sampleIndex & 0xFFFFFFu);
}

class RampDecoder final : public IDecoder {
public:
    RampDecoder(std::uint32_t rate, std::uint16_t channels,
                std::uint64_t totalFrames, bool seekable,
                std::uint64_t valueBase = 0, bool seekFails = false)
        : m_fmt{rate, channels}, m_total(totalFrames), m_seekable(seekable),
          m_valueBase(valueBase), m_seekFails(seekFails) {}

    [[nodiscard]] AudioFormat   format()      const override { return m_fmt; }
    [[nodiscard]] SourceInfo    sourceInfo()  const override { return SourceInfo{}; }
    [[nodiscard]] std::uint64_t totalFrames() const override { return m_total; }
    [[nodiscard]] bool          seekable()    const override { return m_seekable; }

    bool seek(std::uint64_t frame) override {
        if (!m_seekable) {
            return false;
        }
        // A SEEKABLE source whose seek is nonetheless REJECTED, to exercise
        // the engine's "decoder rejected the target" abandon-to-Stopped path. This
        // is distinct from an unseekable source, which refuses up front through
        // seekable() == false before the sink is ever parked.
        if (m_seekFails) {
            return false;
        }
        // The engine clamps before calling, but stay robust: never run past the
        // end. A seek to exactly m_total leaves the next read at EOS (returns 0).
        m_pos = (frame > m_total) ? m_total : frame;
        return true;
    }

    std::size_t read(float* dst, std::size_t frames) override {
        const std::size_t ch = m_fmt.channels;
        std::size_t       produced = 0;
        while (produced < frames && m_pos < m_total) {
            const std::uint64_t base = m_valueBase + m_pos * ch;
            for (std::size_t c = 0; c < ch; ++c) {
                dst[produced * ch + c] = enc(base + c);
            }
            ++m_pos;
            ++produced;
        }
        return produced;
    }

private:
    AudioFormat   m_fmt;
    std::uint64_t m_total;
    bool          m_seekable;
    std::uint64_t m_valueBase;
    bool          m_seekFails;
    std::uint64_t m_pos = 0;
};

// Maps synthetic paths to ramp decoders, so the engine's "path -> decoder" seam
// is driven without any file I/O. Recognized forms, where the prefix selects
// seekability:
//   "ramp:<frames>"                              -> 44100 Hz, 2 ch, seekable
//   "ramp:<rate>:<channels>:<frames>"            -> seekable
//   "ramp:<rate>:<channels>:<frames>:<base>"     -> seekable, ramp value base
//   "noseek:<frames>"                            -> 44100 Hz, 2 ch, NOT seekable
//   "noseek:<rate>:<channels>:<frames>"          -> NOT seekable
//   "noseek:<rate>:<channels>:<frames>:<base>"   -> NOT seekable, value base
//   "seekfail:<frames>" (+ the rate/ch/base forms) -> seekable() true but seek()
//                                                   is rejected (the abandon path)
// Anything else fails to open (returns nullptr), which is how the open-failure
// skip path is exercised. The factory is stateless and reentrant; the engine
// thread is its only caller, but reentrancy keeps it honest.
class RampDecoderFactory final : public IDecoderFactory {
public:
    std::unique_ptr<IDecoder> open(const std::string& path,
                                   std::string*       error) override {
        std::vector<std::string> parts;
        std::string              cur;
        for (char ch : path) {
            if (ch == ':') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        parts.push_back(cur);

        bool seekable  = true;
        bool seekFails = false;
        if (!parts.empty() && parts[0] == "ramp") {
            seekable = true;
        } else if (!parts.empty() && parts[0] == "noseek") {
            seekable = false;
        } else if (!parts.empty() && parts[0] == "seekfail") {
            // Seekable() reports true, but seek() is rejected, so the engine
            // takes its "decoder rejected the target" abandon path.
            seekable  = true;
            seekFails = true;
        } else {
            if (error) {
                *error = "unknown test path";
            }
            return nullptr;
        }

        std::uint32_t rate      = 44100;
        std::uint16_t channels  = 2;
        std::uint64_t frames    = 0;
        std::uint64_t valueBase = 0;
        if (parts.size() == 2) {
            frames = parseU64(parts[1]);
        } else if (parts.size() == 4) {
            rate     = static_cast<std::uint32_t>(parseU64(parts[1]));
            channels = static_cast<std::uint16_t>(parseU64(parts[2]));
            frames   = parseU64(parts[3]);
        } else if (parts.size() == 5) {
            rate      = static_cast<std::uint32_t>(parseU64(parts[1]));
            channels  = static_cast<std::uint16_t>(parseU64(parts[2]));
            frames    = parseU64(parts[3]);
            valueBase = parseU64(parts[4]);
        } else {
            if (error) {
                *error = "malformed ramp path";
            }
            return nullptr;
        }
        if (rate == 0 || channels == 0) {
            if (error) {
                *error = "ramp path has zero rate or channels";
            }
            return nullptr;
        }
        return std::make_unique<RampDecoder>(rate, channels, frames, seekable,
                                             valueBase, seekFails);
    }

private:
    static std::uint64_t parseU64(const std::string& s) noexcept {
        std::uint64_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') {
                break;
            }
            v = v * 10u + static_cast<std::uint64_t>(c - '0');
        }
        return v;
    }
};

// ---------------------------------------------------------------------------
// A capturing sink: a NullSink-shaped consumer that, after each start(), records
// the channel-0 sample of the FIRST frame it pulls. Because the engine primes
// the ring from the post-seek frame before resuming, that first sample equals
// enc(target * channels), which deterministically proves the decoder repositioned
// where we asked. Every start() re-arms, so after a Playing seek (which does
// stop() then start()) the captured value reflects the seek target, not the
// original frame 0.
class CapturingSink final : public IAudioSink {
public:
    explicit CapturingSink(std::size_t blockFrames = 1024)
        : m_blockFrames(blockFrames == 0 ? 1u : blockFrames) {}
    ~CapturingSink() override { close(); }

    CapturingSink(const CapturingSink&)            = delete;
    CapturingSink& operator=(const CapturingSink&) = delete;
    CapturingSink(CapturingSink&&)                 = delete;
    CapturingSink& operator=(CapturingSink&&)      = delete;

    // Capabilities() reports a permissive switchable device (this sink is
    // used by the seek test, where the rate decision is irrelevant), and open()
    // takes the RateDecision the engine resolved. The decision is ignored here:
    // there is no device to reconfigure and the seek assertion is rate-agnostic.
    [[nodiscard]] SinkCapabilities capabilities() const override {
        SinkCapabilities caps;
        caps.deviceName    = "CapturingSink";
        caps.rates.push_back(RateRange{.min = 8000, .max = 768000});
        caps.currentRate   = 48000;
        caps.canSwitchRate = true;
        return caps;
    }

    bool open(const AudioFormat& format, const RateDecision& /*decision*/,
              IPullSource* source) override {
        if (!format.isValid() || source == nullptr) {
            return false;
        }
        if (m_opened) {
            close();
        }
        m_format = format;
        m_source = source;
        m_scratch.assign(m_blockFrames * static_cast<std::size_t>(format.channels), 0.0f);
        m_opened = true;
        return true;
    }

    void start() override {
        if (m_opened && !m_running.load(std::memory_order_relaxed)) {
            m_armed.store(true, std::memory_order_release);  // capture the next pull
            m_running.store(true, std::memory_order_release);
            m_thread = std::thread(&CapturingSink::pumpLoop, this);
        }
    }

    void stop() override {
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void close() override {
        stop();
        m_opened = false;
        m_source = nullptr;
    }

    [[nodiscard]] AudioFormat currentFormat() const override { return m_format; }

    // The channel-0 sample of the first frame pulled after the most recent
    // start(), or -1.0 if nothing has been captured. Read after stop().
    [[nodiscard]] float firstFrameAfterStart() const noexcept {
        return m_firstFrame.load(std::memory_order_acquire);
    }

private:
    void pumpLoop() {
        while (m_running.load(std::memory_order_acquire)) {
            if (m_source != nullptr) {
                m_source->pull(m_scratch.data(), m_blockFrames);
                if (m_armed.exchange(false, std::memory_order_acq_rel)) {
                    m_firstFrame.store(m_scratch[0], std::memory_order_release);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::size_t        m_blockFrames;
    AudioFormat        m_format{};
    IPullSource*       m_source = nullptr;
    std::vector<float> m_scratch;
    std::atomic<bool>  m_running{false};
    std::atomic<bool>  m_armed{false};
    std::atomic<float> m_firstFrame{-1.0f};
    std::thread        m_thread;
    bool               m_opened = false;
};

// ---------------------------------------------------------------------------
// A recording sink: like NullSink it advertises configurable capabilities,
// records the RateDecision handed to each open(), and models the device
// following a switch; additionally it records the channel-0 sample of EVERY real
// frame it pulls, in order, across all opens. Two things are then provable:
//   - the number of recorded decisions: a gapless stitch does NOT reopen the
//     sink, so it produces NO new decision, whereas a reconfigure does, so the
//     decision COUNT distinguishes the two paths;
//   - the recorded channel-0 sequence: sample-contiguity across a boundary (no
//     frame dropped or duplicated at the seam), which the distinct per-track
//     value bands make unambiguous.
// Paced (1 ms per pull) so the producer stays ahead and the consumed stream is
// gap-free and deterministic. Thread-safety: the channel-0 vector is written
// only by the (single, at any instant) pull thread; successive pull threads are
// ordered by the engine's stop()-join / start()-spawn between opens, and the
// test reads only after the engine reaches Stopped (the final pull thread
// joined), so the join plus the state publication give a clean happens-before.
class RecordingSink final : public IAudioSink {
public:
    explicit RecordingSink(std::size_t blockFrames = 1024)
        : m_blockFrames(blockFrames == 0 ? 1u : blockFrames) {}
    ~RecordingSink() override { close(); }

    RecordingSink(const RecordingSink&)            = delete;
    RecordingSink& operator=(const RecordingSink&) = delete;
    RecordingSink(RecordingSink&&)                 = delete;
    RecordingSink& operator=(RecordingSink&&)      = delete;

    void setCapabilities(const SinkCapabilities& caps) { m_caps = caps; }

    SinkCapabilities capabilities() const override { return m_caps; }

    // Sink-console seams, modeled: the logger is parked exactly as a real
    // sink parks it, one line is emitted per open to prove the sink -> Engine ->
    // Listener::onInfo forwarding chain end to end, and the measurement reports
    // the executed device rate while open (a well-behaved device that truly
    // clocks what it accepted; misclock modeling can override m_measuredRate if
    // a future test needs the disagreement case).
    void setLogOutput(ILogOutput* out) override { m_log = out; }

    std::uint32_t measuredDeviceRateHz() const override {
        return m_opened ? m_measuredRate : 0;
    }

    bool open(const AudioFormat& format, const RateDecision& decision,
              IPullSource* source) override {
        if (!format.isValid() || source == nullptr) {
            return false;
        }
        if (m_opened) {
            close();
        }
        // Recorded on the engine thread. There is no concurrent WRITER, but the
        // TEST thread copies the log through recordedDecisions() while later
        // gapless opens may still push (TSan: "as if synchronized via
        // sleep" is not an edge), so the log gets the same mutex discipline
        // NullSink's observers have.
        {
            std::lock_guard<std::mutex> lock(m_obsMtx);
            m_decisions.push_back(decision);
        }
        if (decision.switchDevice) {
            m_caps.currentRate = decision.deviceRate;  // model device-follows-switch
        }
        m_measuredRate = decision.deviceRate;  // well-behaved: clocks what it accepted
        if (m_log != nullptr) {
            m_log->logLine("RecordingSink: open at " +
                           std::to_string(decision.deviceRate) + " Hz");
        }
        m_format   = format;
        m_source   = source;
        m_channels = static_cast<std::size_t>(format.channels);
        m_scratch.assign(m_blockFrames * (m_channels == 0 ? 1u : m_channels), 0.0f);
        m_opened   = true;
        return true;
    }

    void start() override {
        if (m_opened && !m_running.load(std::memory_order_relaxed)) {
            m_running.store(true, std::memory_order_release);
            m_thread = std::thread(&RecordingSink::pumpLoop, this);
        }
    }

    void stop() override {
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void close() override {
        stop();
        m_opened = false;
        m_source = nullptr;
    }

    AudioFormat currentFormat() const override { return m_format; }

    // Copy-out observers, safe from any thread at any time (m_obsMtx).
    std::vector<float> recordedChannel0() const {
        std::lock_guard<std::mutex> lock(m_obsMtx);
        return m_channel0;
    }
    std::vector<RateDecision> recordedDecisions() const {
        std::lock_guard<std::mutex> lock(m_obsMtx);
        return m_decisions;
    }

private:
    void pumpLoop() {
        while (m_running.load(std::memory_order_acquire)) {
            if (m_source != nullptr) {
                const std::size_t got =
                    m_source->pull(m_scratch.data(), m_blockFrames);
                // Record only the REAL frames the source supplied (the sink would
                // zero-pad the remainder; that pad is not recorded).
                {
                    // Same observer discipline: the test thread may copy the
                    // capture while this pull thread appends.
                    std::lock_guard<std::mutex> lock(m_obsMtx);
                    for (std::size_t i = 0; i < got; ++i) {
                        m_channel0.push_back(m_scratch[i * m_channels]);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::size_t               m_blockFrames;
    std::size_t               m_channels = 0;
    AudioFormat               m_format{};
    IPullSource*              m_source = nullptr;
    std::vector<float>        m_scratch;
    mutable std::mutex        m_obsMtx;      // guards the two observer logs below
    std::vector<float>        m_channel0;    // pull-thread-written; test-thread-read
    std::vector<RateDecision> m_decisions;   // engine-thread-written in open()
    SinkCapabilities          m_caps;
    ILogOutput*               m_log          = nullptr;  // non-owning (the console seam)
    std::uint32_t             m_measuredRate = 0;        // modeled AUHAL-style read
    std::atomic<bool>         m_running{false};
    std::thread               m_thread;
    bool                      m_opened = false;
};

// ---------------------------------------------------------------------------
// A sink that advertises a normal switchable device but FAILS every open(),
// modeling a device that cannot be acquired (busy, unplugged, permission denied).
// Used to prove the engine reports the failure through onError and settles cleanly
// in Stopped without crashing, on both the play() and playNow() entry paths.
// start/stop/close are no-ops and currentFormat() is empty: open never succeeds,
// so no RT thread is ever spawned and there is nothing to capture.
class FailingSink final : public IAudioSink {
public:
    [[nodiscard]] SinkCapabilities capabilities() const override {
        SinkCapabilities caps;
        caps.deviceName    = "FailingSink";
        caps.currentRate   = 48000;
        caps.canSwitchRate = true;
        caps.rates.push_back(RateRange{.min = 8000, .max = 768000});
        return caps;
    }
    bool open(const AudioFormat&, const RateDecision&, IPullSource*) override {
        return false;  // device unavailable: the engine must treat this as a start failure
    }
    void        start() override {}
    void        stop()  override {}
    void        close() override {}
    [[nodiscard]] AudioFormat currentFormat() const override { return AudioFormat{}; }
};

// ---------------------------------------------------------------------------
// Thread-safe listener that records the engine's notifications for assertion.
// The engine fires these on its own thread, so every access is guarded. It
// keeps a position log, and an optional engine pointer so a track change can also
// snapshot the reported position at the instant it fires (used to prove the
// position origin resets per stitched track).
class RecordingListener final : public Engine::Listener {
public:
    // Set before play() so the engine thread sees it when onTrackChanged fires.
    void setEngine(const Engine* engine) {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_engine = engine;
    }

    void onStateChanged(State s) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_states.push_back(s);
    }
    void onTrackChanged(const TrackInfo& t) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_trackPaths.push_back(t.path);
        if (m_engine != nullptr) {
            // position() reads only atomics, no engine lock, so it is safe to
            // call from inside this callback while holding the listener lock.
            m_trackChangePositions.push_back(m_engine->position());
        }
    }
    void onPositionChanged(double seconds) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_positions.push_back(seconds);
    }
    void onError(const std::string&) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        ++m_errors;
    }
    void onInfo(const std::string& message) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_infos.push_back(message);
    }
    void onOutputDevices(
        const std::vector<rawform::audio::AudioDeviceInfo>& devices) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_deviceLists.push_back(devices);
    }
    void onDeviceOutcomeChanged(std::uint32_t deviceRateHz, bool bitPerfect) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_outcomes.push_back({deviceRateHz, bitPerfect});
    }
    void onRateDebtChanged(const std::string& deviceId, std::uint32_t originalRateHz,
                           std::uint32_t borrowedRateHz) override {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_debts.push_back({deviceId, originalRateHz, borrowedRateHz});
    }

    std::vector<State> states() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_states;
    }
    std::vector<std::vector<rawform::audio::AudioDeviceInfo>> deviceLists() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_deviceLists;
    }
    struct Outcome { std::uint32_t rateHz; bool bitPerfect; };
    std::vector<Outcome> outcomes() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_outcomes;
    }
    struct Debt { std::string deviceId; std::uint32_t originalHz; std::uint32_t borrowedHz; };
    std::vector<Debt> debts() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_debts;
    }
    std::vector<std::string> trackPaths() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_trackPaths;
    }
    std::vector<double> trackChangePositions() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_trackChangePositions;
    }
    std::size_t positionCount() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_positions.size();
    }
    double lastPosition() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_positions.empty() ? -1.0 : m_positions.back();
    }
    int errors() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_errors;
    }
    std::vector<std::string> infos() const {
        std::lock_guard<std::mutex> lock(m_mtx);
        return m_infos;
    }

private:
    mutable std::mutex       m_mtx;
    std::vector<State>       m_states;
    std::vector<std::string> m_trackPaths;
    std::vector<std::vector<rawform::audio::AudioDeviceInfo>> m_deviceLists;
    std::vector<Outcome> m_outcomes;
    std::vector<Debt>    m_debts;
    std::vector<double>      m_positions;
    std::vector<double>      m_trackChangePositions;
    std::vector<std::string> m_infos;
    const Engine*            m_engine = nullptr;
    int                      m_errors = 0;
};

// ---------------------------------------------------------------------------
// Poll the engine state until it reaches `target` or the timeout elapses.
// Returns true if reached. A 1 ms poll keeps it responsive without busy burning.
bool waitForState(const Engine& engine, State target, int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (engine.state() == target) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return engine.state() == target;
}

// Wait for a play()-then-complete cycle to finish. A freshly constructed engine
// STARTS in Stopped, and play() is asynchronous (it posts a command the engine
// thread processes later), so a bare waitForState(Stopped) right after play()
// would return immediately on that initial Stopped, before playback even began.
// This first waits for Playing (proof the Play command landed and a track
// started), then for the return to Stopped (completion). Returns false if either
// leg times out.
bool waitForCompletion(const Engine& engine, int timeoutMs = 5000) {
    if (!waitForState(engine, State::Playing, timeoutMs)) {
        return false;
    }
    return waitForState(engine, State::Stopped, timeoutMs);
}

void settle(int ms = 30) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Ceiling for waits that require a FULL paced drain of the ~1M-frame track
// (about 1024 one-block pulls, each behind the pull loop's 1 ms sleep). On a
// developer machine that drain takes roughly a second, but the 1 ms sleep is a
// request, not a guarantee: on a loaded or virtualized host (CI runners) the
// realized quantum stretches to 5-15 ms and the same drain legitimately takes
// tens of seconds. This generous ceiling keeps the wait a hang detector rather
// than a host-speed assertion. Tests that drain only short tracks, or that cut
// the long track with playNow instead of draining it, stay on the 5 s default;
// the pacing itself is untouched because the pause-window reasoning in the
// mode-gate tests depends on the drain being slow relative to command latency.
constexpr int kPacedDrainTimeoutMs = 60000;

// Configure a freshly constructed engine: inject the ramp factory, a NullSink in
// the requested mode, and a listener. The factory and listener must outlive the
// engine; callers declare them before the engine for that reason.
void configure(Engine& engine, RampDecoderFactory& factory,
               RecordingListener& listener, NullSink::Mode mode) {
    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::make_unique<NullSink>(mode));
}

// Scoped knob to force gapless OFF for the duration of its scope. The Engine
// reads RAWFORM_NO_GAPLESS once, on the constructing thread, inside its
// constructor, so an Engine declared while a guard is in scope sees gapless
// disabled; the guard restores the environment on destruction. Used by the
// rate-gate tests, which assert one decision per track.
struct NoGaplessEnvGuard {
    NoGaplessEnvGuard()  { ::setenv("RAWFORM_NO_GAPLESS", "1", 1); }
    ~NoGaplessEnvGuard() { ::unsetenv("RAWFORM_NO_GAPLESS"); }
};

// ---------------------------------------------------------------------------
// TRANSPORT SCENARIOS (the ramp is seekable but none of these
// seek). With gapless ON by default, the same-rate auto-advance and the next()
// skip exercise the stitch and hold-cut paths, but the observable
// outcomes (paths reported, terminal state) are identical, so the assertions
// stand as written and these double as gapless smoke coverage.
// ---------------------------------------------------------------------------

// A single short track plays to completion and the engine returns to Stopped on
// its own (queue exhaustion), having reported Playing then Stopped and the
// track once.
void testPlayToCompletion() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:100000");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    CHECK(engine.state() == State::Stopped);
    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 1);
    if (!paths.empty()) {
        CHECK(paths[0] == "ramp:100000");
    }
    const auto states = listener.states();
    CHECK(!states.empty());
    if (!states.empty()) {
        CHECK(states.front() == State::Playing);
        CHECK(states.back() == State::Stopped);
    }
    CHECK(listener.errors() == 0);
}

// Pause is honored promptly and resume returns to Playing, with no flush and no
// completion in between.
void testPauseResume() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:5000000");  // long enough not to finish during the test
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    settle(20);
    engine.pause();
    CHECK(waitForState(engine, State::Paused));

    settle(20);
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
}

// Stop rewinds and remembers the track; a subsequent play() restarts
// the SAME track, firing onTrackChanged for it again.
void testStopRewindsAndReplays() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:5000000");
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    settle(20);
    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
    settle();

    // Replay: with no enqueue, play() must restart the remembered track.
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle();

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == "ramp:5000000");
        CHECK(paths[1] == "ramp:5000000");
    }

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
}

// Two queued tracks auto-advance: the first finishing plays the second, then the
// exhausted queue returns to Stopped. Tracks are reported in order. (Same rate,
// so under gapless this advance is a stitch; the reported order is unchanged.)
void testQueueAdvanceAndExhaustion() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:80000");
    engine.enqueue("ramp:90000");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == "ramp:80000");
        CHECK(paths[1] == "ramp:90000");
    }
    CHECK(engine.state() == State::Stopped);
}

// next() skips to the queued track immediately. (Same rate, so under gapless this
// is a HOLD-CUT; the reported order is unchanged.)
void testNextSkips() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:5000000");  // long; we will skip it before it ends
    engine.enqueue("ramp:90000");    // short; this one runs to completion
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    settle(20);
    engine.next();

    CHECK(waitForState(engine, State::Stopped));
    settle();

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == "ramp:5000000");
        CHECK(paths[1] == "ramp:90000");
    }
}

// clearQueue empties the pending tracks without disturbing the current one; the
// cleared track never plays.
void testClearQueue() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:90000");
    engine.enqueue("ramp:90000:never");  // distinct path so we can tell it apart
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    engine.clearQueue();

    CHECK(waitForState(engine, State::Stopped));
    settle();

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 1);
    if (!paths.empty()) {
        CHECK(paths[0] == "ramp:90000");
    }
}

// A queued path that fails to open is skipped, an error is reported, and the
// next openable path plays.
void testOpenFailureSkips() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("bogus-path");   // factory returns nullptr
    engine.enqueue("ramp:90000");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    CHECK(listener.errors() >= 1);
    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 1);
    if (!paths.empty()) {
        CHECK(paths[0] == "ramp:90000");
    }
}

// ---------------------------------------------------------------------------
// SEEK AND POSITION SCENARIOS.
// ---------------------------------------------------------------------------

// Position is monotonic while Playing and frozen while Paused. With the Paced
// sink consuming faster than realtime, two samples a settle apart show the
// climb; pausing then sampling twice shows the freeze (the playhead does not
// advance with the RT thread parked), and they are exactly equal.
void testPositionMonotonicAndFrozen() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:5000000");
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    settle(20);
    const double p1 = engine.position();
    settle(20);
    const double p2 = engine.position();
    CHECK(p2 >= p1);  // monotonic while Playing
    CHECK(p1 > 0.0);  // and actually advancing

    engine.pause();
    CHECK(waitForState(engine, State::Paused));
    settle(20);
    const double f1 = engine.position();
    settle(20);
    const double f2 = engine.position();
    CHECK(f2 == f1);  // frozen while Paused (same playhead, exact)

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
    // Position reads 0.0 once Stopped (playhead rebased, format invalidated).
    CHECK(engine.position() == 0.0);
}

// A seek while Paused jumps the reported position to the target and stays Paused
// and frozen there. Deterministic: the paused playhead does not drift, so
// position() equals target/rate within rounding.
void testSeekPausedJumpsAndStaysPaused() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:5000000");  // 44100 Hz, 2 ch
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(20);

    engine.pause();
    CHECK(waitForState(engine, State::Paused));
    settle(10);
    const double beforeSeek = engine.position();

    constexpr double target = 1.0;  // seconds
    engine.seek(target);
    settle(20);

    CHECK(engine.state() == State::Paused);          // stays Paused
    const double afterSeek = engine.position();
    CHECK(std::fabs(afterSeek - target) < 0.05);     // landed at the target
    CHECK(afterSeek < beforeSeek || beforeSeek < target);  // a real jump occurred
    CHECK(listener.lastPosition() >= 0.0);           // the immediate fire landed

    // Resume plays on from the target.
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
}

// A seek repositions the DECODER: the first decoded frame pulled after the seek
// encodes the target frame. Proven with the capturing sink and the ramp's
// frame-index encoding, with no dependence on wall-clock pacing.
void testSeekDecodedFrameLandsAtTarget() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);

    auto                capturing = std::make_unique<CapturingSink>();
    CapturingSink*      sinkPtr   = capturing.get();
    engine.setSink(std::move(capturing));

    engine.enqueue("ramp:5000000");  // 44100 Hz, 2 ch
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(20);

    constexpr double        targetSeconds = 2.0;
    constexpr std::uint64_t targetFrame   = 88200;  // 2.0 s * 44100 Hz
    constexpr std::uint64_t channels      = 2;
    const float             expected      = enc(targetFrame * channels);  // channel 0

    engine.seek(targetSeconds);
    settle(40);  // let the resumed RT thread pull the first post-seek block

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));

    const float captured = sinkPtr->firstFrameAfterStart();
    CHECK(std::fabs(captured - expected) <= 1.5f);  // within the LSB tolerance
}

// onPositionChanged fires on cadence while Playing. Over a window of N ticks we
// expect roughly N fires; the bounds are loose to tolerate scheduling jitter,
// the immediate track-start fire, and the ~100 ms cadence.
void testPositionCadence() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:9000000");  // long enough not to finish during the window
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    settle(420);  // ~4 ticks at 100 ms, plus the immediate fire
    const std::size_t count = listener.positionCount();
    CHECK(count >= 2);   // clearly ticking
    CHECK(count <= 12);  // not spinning far above the 10 Hz cadence

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
}

// A seek at or past a known end clamps to the end, where the next read is EOS, so
// the finished -> advance/stop path runs and the (empty) queue returns to
// Stopped. This covers both the clamp and the EOS edge.
void testSeekPastEndClampsToCompletion() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:200000");  // ~4.5 s at 44100 Hz
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(10);

    engine.seek(1000.0);  // far past the end; clamps to totalFrames -> EOS

    CHECK(waitForState(engine, State::Stopped));
    CHECK(engine.state() == State::Stopped);
}

// A seek on an unseekable source is ignored: playback is not interrupted (still
// Playing, position still advancing) and an error is reported.
void testSeekUnseekableIgnored() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("noseek:5000000");  // unseekable ramp
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(20);

    const int errorsBefore = listener.errors();
    engine.seek(1.0);
    settle(20);

    CHECK(engine.state() == State::Playing);          // not interrupted
    CHECK(listener.errors() == errorsBefore + 1);     // refusal reported
    CHECK(engine.position() > 0.0);                   // still advancing

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
}

// A seek while Stopped is ignored entirely: no error, no state change, no track.
void testSeekStoppedIgnored() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.seek(1.0);  // nothing loaded
    settle(20);

    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
    CHECK(listener.trackPaths().empty());
}

// ---------------------------------------------------------------------------
// The ThreadSanitizer workload: a FreeRun sink (maximal producer/consumer
// interleaving) while the controlling thread hammers the transport with a dense,
// deterministic command sequence, now including interleaved seeks so the playhead
// advance and the seek rebase race the consumer. With gapless ON (default) the
// same-rate auto-advances stitch and the next()s hold-cut, so this also soaks the
// seam push/cross/rebase machinery under TSan. The assertion is that it survives
// clean and settles in Stopped; TSan and ASan catch any race or use-after-free.
void testConcurrentHammer() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::FreeRun);

    for (int i = 0; i < 6; ++i) {
        engine.enqueue("ramp:300000");
    }

    // A fixed, dense sequence of transport ops with sub-millisecond spacing, so
    // commands land while the producer and consumer are mid-flight.
    engine.play();
    for (int round = 0; round < 40; ++round) {
        engine.seek(0.25 * static_cast<double>(round % 5));  // interleaved seeks
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        engine.pause();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        engine.play();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        engine.seek(0.1 * static_cast<double>(round % 3));
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        engine.next();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        if ((round % 5) == 0) {
            engine.stop();
            engine.play();
        }
        if ((round % 7) == 0) {
            engine.enqueue("ramp:120000");
        }
    }

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
    CHECK(engine.state() == State::Stopped);
}

// Destroying the engine mid-stream must tear down cleanly: shutdown stops the
// engine thread, which closes the sink (joining the RT pull thread) before the
// ring and decoder are destroyed. Reaching past the scope is the assertion;
// ASan/TSan validate the teardown order. A seek just before destruction stresses
// the parked-then-destroyed path.
void testDestroyMidStream() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    {
        Engine engine;
        configure(engine, factory, listener, NullSink::Mode::FreeRun);
        engine.enqueue("ramp:9000000");
        engine.play();
        CHECK(waitForState(engine, State::Playing));
        settle(20);
        engine.seek(1.5);
        // engine destroyed here, while Playing, shortly after a seek.
    }
    CHECK(true);
}

// ---------------------------------------------------------------------------
// RATE SCENARIOS: the engine consults the RateManager per track and publishes the
// decision (including the gate) to the sink. These drive the wiring end to end,
// capabilities() -> decide() -> open(), over a mixed-rate queue, and assert the
// decisions the engine handed down. They assert ONE decision per track, which is
// the pre-gapless behavior, so they pin gapless OFF via NoGaplessEnvGuard: with
// gapless ON a same-source-format auto-advance would stitch (no open, no
// decision) and change the decision count. The gapless behavior itself is
// exercised in its own section. They are codec-free: the ramp factory fabricates
// tracks at chosen rates and channel counts. The exhaustive policy truth table
// lives in RateManagerTest; here the point is the ENGINE plumbing and the
// cross-track gate, which depends on engine state (openDeviceFormat) that a pure
// policy test cannot exercise.
// ---------------------------------------------------------------------------

// Helper: build a capability set from discrete advertised rates.
SinkCapabilities makeCaps(std::initializer_list<std::uint32_t> rates,
                          std::uint32_t current, bool canSwitch) {
    SinkCapabilities c;
    c.deviceName    = "engine-test device";
    c.currentRate   = current;
    c.canSwitchRate = canSwitch;
    for (std::uint32_t r : rates) {
        c.rates.push_back(RateRange{.min = r, .max = r});
    }
    return c;
}

// A fixed-rate device (cannot switch, runs at 48000) plays a mixed-rate queue.
// Every track therefore resolves to the device's 48000 and resamples when the
// source differs; the interesting output is the gate across the queue:
//   t0 48000/2 : fresh start          -> reconfigure
//   t1 44100/2 : resamples to 48000   -> HOLD (same device rate and channels)
//   t2 88200/2 : resamples to 48000   -> HOLD
//   t3 48000/1 : same rate, mono      -> reconfigure (channel count changed)
// This proves the engine carries the prior device format across auto-advance and
// that a channel change forces a reconfigure even when the rate does not move.
// Gapless is pinned OFF so each track yields a decision (the source formats here
// all differ consecutively, so no stitch would occur anyway, but the pin keeps
// the test's intent explicit and robust to future rate edits).
void testRateGateFixedDevice() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    NoGaplessEnvGuard  noGapless;
    Engine             engine;

    auto       sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink*  sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({48000}, 48000, /*canSwitch=*/false));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:48000:2:60000");
    engine.enqueue("ramp:44100:2:60000");
    engine.enqueue("ramp:88200:2:60000");
    engine.enqueue("ramp:48000:1:60000");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    const std::vector<RateDecision> d = sinkPtr->recordedDecisions();
    CHECK(d.size() == 4);
    if (d.size() == 4) {
        // Every track runs the device at its fixed 48000.
        CHECK(d[0].deviceRate == 48000);
        CHECK(d[1].deviceRate == 48000);
        CHECK(d[2].deviceRate == 48000);
        CHECK(d[3].deviceRate == 48000);

        // The device never switches (it cannot).
        CHECK(d[0].switchDevice == false);
        CHECK(d[3].switchDevice == false);

        // Resample where the source rate differs from 48000.
        CHECK(d[0].resampleNeeded == false);
        CHECK(d[1].resampleNeeded == true);
        CHECK(d[2].resampleNeeded == true);
        CHECK(d[3].resampleNeeded == false);

        // The gate: fresh start reconfigures, same-config transitions hold, the
        // channel change reconfigures.
        CHECK(d[0].needsDeviceReconfigure == true);
        CHECK(d[1].keepSink == true);
        CHECK(d[2].keepSink == true);
        CHECK(d[3].needsDeviceReconfigure == true);
    }
    CHECK(listener.errors() == 0);
}

// A switchable device under BitPerfectWhenAvailable follows the track when the
// rate is advertised. Because NullSink models the device following each switch,
// a repeated rate is recognized as already-current (no redundant switch) and the
// gate holds the sink:
//   t0 44100 : device at 48000 -> switch to 44100, fresh start -> reconfigure
//   t1 44100 : device now 44100 -> no switch, same config       -> HOLD
//   t2 48000 : advertised       -> switch to 48000              -> reconfigure
// Gapless is pinned OFF: with it ON the t0 -> t1 advance (identical 44100/2
// source format) would STITCH, producing no t1 decision and collapsing the count
// to 2, which would defeat the per-track decision assertion this test exists for.
void testRateGateBitPerfectSwitch() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    NoGaplessEnvGuard  noGapless;
    Engine             engine;

    auto       sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink*  sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100, 48000}, 48000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:60000");
    engine.enqueue("ramp:44100:2:60000");
    engine.enqueue("ramp:48000:2:60000");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    const std::vector<RateDecision> d = sinkPtr->recordedDecisions();
    CHECK(d.size() == 3);
    if (d.size() == 3) {
        CHECK(d[0].deviceRate == 44100);
        CHECK(d[0].switchDevice == true);    // device was at 48000
        CHECK(d[0].resampleNeeded == false);  // bit-perfect
        CHECK(d[0].needsDeviceReconfigure == true);  // fresh start

        CHECK(d[1].deviceRate == 44100);
        CHECK(d[1].switchDevice == false);   // device already at 44100
        CHECK(d[1].resampleNeeded == false);
        CHECK(d[1].keepSink == true);         // same device config: hold

        CHECK(d[2].deviceRate == 48000);
        CHECK(d[2].switchDevice == true);
        CHECK(d[2].resampleNeeded == false);
        CHECK(d[2].needsDeviceReconfigure == true);  // 48000 != prior 44100
    }
    CHECK(listener.errors() == 0);
}

// AlwaysResample never switches, even when the source rate is advertised and the
// device could switch: the device stays put and the source is resampled to it.
// A single track, so gapless never engages; no pin needed.
void testRateModeAlwaysResampleNeverSwitches() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto       sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink*  sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100, 48000}, 48000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::AlwaysResample);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:60000");  // advertised, but must NOT switch
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    const std::vector<RateDecision> d = sinkPtr->recordedDecisions();
    CHECK(d.size() == 1);
    if (d.size() == 1) {
        CHECK(d[0].deviceRate == 48000);    // left at the device's current rate
        CHECK(d[0].switchDevice == false);
        CHECK(d[0].resampleNeeded == true);  // 44100 source resampled to 48000
    }
    CHECK(listener.errors() == 0);
}

// ---------------------------------------------------------------------------
// DEVICE-HOLD SCENARIOS: the in-place reconfigure offer at transition boundaries.
// Same three-track queue with a rate change at EVERY boundary (which also
// guarantees no gapless stitch, so each boundary is a genuine drain-then-
// reconfigure); NullSink's toggle drives the two engine branches.
// ---------------------------------------------------------------------------

// Toggle OFF (the IAudioSink default, which a sink with nothing to retune
// keeps): the engine offers reconfigure, the sink declines, and the close+open
// fallback runs at each boundary. Three opens, three genuine closes (two
// boundary fallbacks plus the final release at Stopped), an empty reconfigure
// log.
void testReconfigureDeclinedFallsBackToCloseOpen() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100, 48000}, 48000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:60000");
    engine.enqueue("ramp:48000:2:60000");
    engine.enqueue("ramp:44100:2:60000");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    CHECK(sinkPtr->recordedDecisions().size() == 3);   // one genuine open per track
    CHECK(sinkPtr->recordedReconfigures().empty());    // every offer declined
    CHECK(sinkPtr->closeCount() == 3);                 // two boundary closes + the final release
    CHECK(listener.trackPaths().size() == 3);
    CHECK(listener.errors() == 0);
}

// Toggle ON: the engine's offer is accepted at both boundaries. ONE open for
// the whole session, one reconfigure per boundary carrying the switch decision,
// and exactly ONE close (the final release at Stopped): the device-hold ledger shape,
// "one open per listening session, one repayment at the final close".
void testReconfigureAcceptedKeepsDeviceOpen() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100, 48000}, 48000, /*canSwitch=*/true));
    sinkPtr->setReconfigureSupported(true);

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:60000");
    engine.enqueue("ramp:48000:2:60000");
    engine.enqueue("ramp:44100:2:60000");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    CHECK(sinkPtr->recordedDecisions().size() == 1);  // one open for the session

    const std::vector<RateDecision> r = sinkPtr->recordedReconfigures();
    CHECK(r.size() == 2);  // one per boundary
    if (r.size() == 2) {
        // Each boundary's decision is a genuine rate switch computed against
        // the surviving openDeviceFormat: 44100 -> 48000, then back.
        CHECK(r[0].deviceRate == 48000);
        CHECK(r[0].switchDevice == true);
        CHECK(r[0].needsDeviceReconfigure == true);
        CHECK(r[1].deviceRate == 44100);
        CHECK(r[1].switchDevice == true);
        CHECK(r[1].needsDeviceReconfigure == true);
    }

    CHECK(sinkPtr->closeCount() == 1);          // the single final repayment at Stopped
    CHECK(listener.trackPaths().size() == 3);   // announcements unchanged by the branch
    CHECK(listener.errors() == 0);
}

// ---------------------------------------------------------------------------
// DEVICE-SELECTION SCENARIOS: output device enumeration and selection. NullSink models a
// two-device machine; the engine's command/listener flow and the
// reopen-at-position boundary are the subjects.
// ---------------------------------------------------------------------------

std::vector<rawform::audio::AudioDeviceInfo> twoTestDevices() {
    using rawform::audio::AudioDeviceInfo;
    std::vector<AudioDeviceInfo> v;
    v.push_back(AudioDeviceInfo{"dev-a", "Device A", true});
    v.push_back(AudioDeviceInfo{"dev-b", "Device B", false});
    return v;
}

// requestOutputDevices answers through the listener with the sink's list.
void testDeviceEnumerationPushesThroughListener() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setDevices(twoTestDevices());
    (void)sinkPtr;

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::move(sink));

    engine.requestOutputDevices();
    settle();

    const auto lists = listener.deviceLists();
    CHECK(lists.size() == 1);
    if (lists.size() == 1) {
        CHECK(lists[0].size() == 2);
        if (lists[0].size() == 2) {
            CHECK(lists[0][0].id == "dev-a");
            CHECK(lists[0][0].isDefault == true);
            CHECK(lists[0][1].id == "dev-b");
            CHECK(lists[0][1].name == "Device B");
        }
    }
    CHECK(listener.errors() == 0);
}

// A selection while Stopped parks on the sink; an unknown id is refused with
// onError and leaves the pin untouched.
void testSelectDeviceWhileStoppedParksAndRefusesUnknown() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setDevices(twoTestDevices());

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::move(sink));

    engine.selectOutputDevice("dev-b");
    settle();
    CHECK(sinkPtr->selectedDeviceId() == "dev-b");
    CHECK(engine.state() == State::Stopped);
    CHECK(sinkPtr->recordedDecisions().empty());  // no open happened
    CHECK(listener.errors() == 0);

    engine.selectOutputDevice("dev-nonexistent");
    settle();
    CHECK(sinkPtr->selectedDeviceId() == "dev-b");  // refusal left the pin alone
    CHECK(listener.errors() == 1);
}

// A selection while Playing performs the reopen-at-position boundary: a full
// close of the old device, a fresh open, still Playing, the track announced
// again, and the pending queue untouched.
void testSelectDeviceWhilePlayingReopens() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setDevices(twoTestDevices());

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:600000");  // long enough to still be Playing
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    engine.selectOutputDevice("dev-b");
    settle();

    CHECK(engine.state() == State::Playing);
    CHECK(sinkPtr->selectedDeviceId() == "dev-b");
    CHECK(sinkPtr->recordedDecisions().size() == 2);  // the reopen is a genuine open
    CHECK(sinkPtr->closeCount() >= 1);                // the old device was released
    CHECK(listener.trackPaths().size() == 2);         // the track announced again
    CHECK(listener.errors() == 0);

    engine.stop();
    settle();
}

// The same boundary from Paused restores Paused.
void testSelectDeviceWhilePausedStaysPaused() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setDevices(twoTestDevices());

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:600000");
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    engine.pause();
    CHECK(waitForState(engine, State::Paused));

    engine.selectOutputDevice("dev-b");
    settle();

    CHECK(engine.state() == State::Paused);
    CHECK(sinkPtr->selectedDeviceId() == "dev-b");
    CHECK(sinkPtr->recordedDecisions().size() == 2);
    CHECK(listener.errors() == 0);

    engine.stop();
    settle();
}

// ---------------------------------------------------------------------------
// SINK-EVENT SCENARIOS: the sink event channel. NullSink's fire* injectors call
// the engine's ISinkEventListener from the TEST thread, proving the
// any-thread enqueue; the reactions are then asserted through the listener.
// ---------------------------------------------------------------------------

// An external rate change republishes the outcome mid-track: the packed
// observers and the onDeviceOutcomeChanged push both catch up with the moved
// device, with no reopen and no state change.
void testExternalRateChangeRepublishesOutcome() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100, 48000, 96000}, 48000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:600000");
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    CHECK(engine.outputDeviceRateHz() == 44100);  // bit-perfect switch executed
    CHECK(engine.outputBitPerfect() == true);

    sinkPtr->fireExternalRateChanged(96000);  // the user asserted 96 k underneath us
    settle();

    CHECK(engine.state() == State::Playing);
    CHECK(engine.outputDeviceRateHz() == 96000);   // the published outcome caught up
    CHECK(engine.outputBitPerfect() == false);     // 44.1 source on a 96 k device
    const auto outcomes = listener.outcomes();
    CHECK(!outcomes.empty());
    if (!outcomes.empty()) {
        CHECK(outcomes.back().rateHz == 96000);
        CHECK(outcomes.back().bitPerfect == false);
    }
    CHECK(sinkPtr->recordedDecisions().size() == 1);  // no reopen happened
    CHECK(listener.errors() == 0);

    engine.stop();
    settle();
}

// A default-device change while FOLLOWING the default reopens at position (the
// audible re-resolve) and pushes a fresh enumeration.
void testDefaultDeviceChangeReopensWhenFollowing() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setDevices(twoTestDevices());
    sinkPtr->setCapabilities(makeCaps({44100, 48000}, 48000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:600000");
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    sinkPtr->fireDefaultDeviceChanged();
    settle();

    CHECK(engine.state() == State::Playing);
    CHECK(sinkPtr->recordedDecisions().size() == 2);  // the reopen is a fresh open
    CHECK(!listener.deviceLists().empty());           // pickers got the refresh
    CHECK(listener.errors() == 0);

    engine.stop();
    settle();
}

// The same event with a PIN in force refreshes the pickers but does not touch
// playback: a pinned device does not care where the default went.
void testDefaultDeviceChangeIgnoredWhenPinned() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setDevices(twoTestDevices());
    sinkPtr->setCapabilities(makeCaps({44100, 48000}, 48000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::move(sink));

    engine.selectOutputDevice("dev-b");  // parked pin, before any playback
    engine.enqueue("ramp:44100:2:600000");
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    sinkPtr->fireDefaultDeviceChanged();
    settle();

    CHECK(engine.state() == State::Playing);
    CHECK(sinkPtr->recordedDecisions().size() == 1);  // no reopen
    CHECK(!listener.deviceLists().empty());
    CHECK(listener.errors() == 0);

    engine.stop();
    settle();
}

// A device-list change pushes a fresh enumeration and nothing else; a debt
// change relays the triple to the listener verbatim.
void testDeviceListAndDebtEventsRelay() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto      sink    = std::make_unique<NullSink>(NullSink::Mode::Paced);
    NullSink* sinkPtr = sink.get();
    sinkPtr->setDevices(twoTestDevices());

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::move(sink));

    sinkPtr->fireDeviceListChanged();
    settle();
    CHECK(listener.deviceLists().size() == 1);
    CHECK(engine.state() == State::Stopped);

    sinkPtr->fireRateDebtChanged("dev-a", 48000, 96000);
    sinkPtr->fireRateDebtChanged("", 0, 0);  // cleared
    settle();
    const auto debts = listener.debts();
    CHECK(debts.size() == 2);
    if (debts.size() == 2) {
        CHECK(debts[0].deviceId == "dev-a");
        CHECK(debts[0].originalHz == 48000);
        CHECK(debts[0].borrowedHz == 96000);
        CHECK(debts[1].deviceId.empty());
        CHECK(debts[1].originalHz == 0);
    }
    CHECK(listener.errors() == 0);
}

// ---------------------------------------------------------------------------
// GAPLESS SCENARIOS: gapless playback. These use the recording sink, whose decision
// COUNT distinguishes a stitch (no open) from a reconfigure (an open) and whose
// channel-0 capture proves sample-contiguity across a boundary. Tracks are kept
// inside the ring (~17640 frames at 44100) where it matters so the seam layout is
// deterministic. Distinct per-track value bands make the captured stream
// self-identifying.
// ---------------------------------------------------------------------------

// Build the expected channel-0 stream of a 2-channel ramp track: channel 0 of
// frame `pos` is enc(valueBase + 2*pos).
std::vector<float> expectedChannel0(std::uint64_t valueBase, std::uint64_t frames) {
    std::vector<float> v;
    v.reserve(static_cast<std::size_t>(frames));
    for (std::uint64_t pos = 0; pos < frames; ++pos) {
        v.push_back(enc(valueBase + pos * 2u));
    }
    return v;
}

// Element-wise compare within the suite's 1.5 LSB tolerance (the ramp values are
// exact integers in float32, so this is generous).
bool channel0Matches(const std::vector<float>& got,
                     const std::vector<float>& want) {
    if (got.size() != want.size()) {
        return false;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (std::fabs(got[i] - want[i]) > 1.5f) {
            return false;
        }
    }
    return true;
}

// Two same-source-format tracks auto-advance GAPLESSLY: the sink is opened once
// (one decision), both tracks are announced in order, and the consumed channel-0
// stream is exactly track A's band followed by track B's band, with nothing
// dropped or duplicated at the seam.
void testGaplessStitchSameFormat() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto           sink    = std::make_unique<RecordingSink>();
    RecordingSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100}, 44100, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    const std::string a = "ramp:44100:2:4000:1048576";  // band 0x100000
    const std::string b = "ramp:44100:2:6000:2097152";  // band 0x200000
    engine.enqueue(a);
    engine.enqueue(b);
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    // Same source format across the boundary: the engine STITCHED, so the sink
    // was opened exactly once (one open == one decision, no second decision).
    CHECK(sinkPtr->recordedDecisions().size() == 1);

    // Both tracks announced, in order.
    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == a);
        CHECK(paths[1] == b);
    }

    // The consumed channel-0 stream is A's band then B's band, sample-contiguous.
    std::vector<float>       expected = expectedChannel0(1048576u, 4000u);
    const std::vector<float> bExp     = expectedChannel0(2097152u, 6000u);
    expected.insert(expected.end(), bExp.begin(), bExp.end());
    const std::vector<float> got = sinkPtr->recordedChannel0();
    CHECK(got.size() == expected.size());
    CHECK(channel0Matches(got, expected));

    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// Two DIFFERENT-source-rate tracks auto-advance: the format gate forbids a stitch, so the
// engine drains then reconfigures (the sink is opened TWICE). No frame is lost in
// the process, because completion waits for the ring to drain before teardown, so
// the consumed channel-0 stream is still A's band then B's band, contiguous in
// VALUE (the gap is in time, not in samples). The decision count is the
// discriminator versus the stitch case.
void testGaplessNoStitchDifferentRate() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto           sink    = std::make_unique<RecordingSink>();
    RecordingSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({48000}, 48000, /*canSwitch=*/false));  // fixed

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    const std::string a = "ramp:44100:2:4000:1048576";
    const std::string b = "ramp:88200:2:6000:2097152";  // different source rate
    engine.enqueue(a);
    engine.enqueue(b);
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    // Different source rate: reconfigure, so the sink was opened twice.
    CHECK(sinkPtr->recordedDecisions().size() == 2);

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == a);
        CHECK(paths[1] == b);
    }

    std::vector<float>       expected = expectedChannel0(1048576u, 4000u);
    const std::vector<float> bExp     = expectedChannel0(2097152u, 6000u);
    expected.insert(expected.end(), bExp.begin(), bExp.end());
    const std::vector<float> got = sinkPtr->recordedChannel0();
    CHECK(got.size() == expected.size());
    CHECK(channel0Matches(got, expected));

    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// Three same-format tracks all inside the ring stitch into one continuous stream:
// one open, two pending seam markers, crossed in order, three tracks announced,
// and a fully contiguous channel-0 stream across both seams.
void testGaplessMultipleSeams() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto           sink    = std::make_unique<RecordingSink>();
    RecordingSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100}, 44100, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::move(sink));

    const std::string a = "ramp:44100:2:3000:1048576";
    const std::string b = "ramp:44100:2:2000:2097152";
    const std::string c = "ramp:44100:2:4000:3145728";
    engine.enqueue(a);
    engine.enqueue(b);
    engine.enqueue(c);
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    CHECK(sinkPtr->recordedDecisions().size() == 1);  // one open for all three

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 3);
    if (paths.size() == 3) {
        CHECK(paths[0] == a);
        CHECK(paths[1] == b);
        CHECK(paths[2] == c);
    }

    std::vector<float>       expected = expectedChannel0(1048576u, 3000u);
    const std::vector<float> bExp     = expectedChannel0(2097152u, 2000u);
    const std::vector<float> cExp     = expectedChannel0(3145728u, 4000u);
    expected.insert(expected.end(), bExp.begin(), bExp.end());
    expected.insert(expected.end(), cExp.begin(), cExp.end());
    const std::vector<float> got = sinkPtr->recordedChannel0();
    CHECK(got.size() == expected.size());
    CHECK(channel0Matches(got, expected));

    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// The reported position resets to ~0 at each gapless boundary rather than
// continuing to climb from the first track. Two LONG same-format tracks (~4.5 s
// each at 44100); the position snapshotted at each onTrackChanged must be near 0.
// A track whose origin was NOT rebased would report ~4.5 s at the second track's
// announcement, so the < 0.5 s bound cleanly proves the rebase.
void testGaplessPositionResetsPerTrack() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);
    listener.setEngine(&engine);  // snapshot position at each onTrackChanged

    engine.enqueue("ramp:44100:2:200000");
    engine.enqueue("ramp:44100:2:200000");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);  // both announced

    const auto pos = listener.trackChangePositions();
    CHECK(pos.size() == 2);
    for (double p : pos) {
        CHECK(p >= 0.0);
        CHECK(p < 0.5);  // near 0 at each track's start; ~4.5 if it had NOT reset
    }

    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// Gapless soak under FreeRun: many same-format tracks stitched back to back with
// maximal producer/consumer interleaving. Every seam must still be crossed once
// and in order, so all tracks are announced and the run settles in Stopped.
// TSan/ASan validate the concurrent seam push (producer) vs cross/rebase (run
// loop) vs the RT consumer advancing the consumed counter.
void testGaplessFreeRunSoak() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::FreeRun);

    constexpr int kTracks = 8;
    for (int i = 0; i < kTracks; ++i) {
        engine.enqueue("ramp:44100:2:40000");  // identical source format: every advance stitches
    }
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    CHECK(static_cast<int>(listener.trackPaths().size()) == kTracks);
    CHECK(engine.state() == State::Stopped);
}

// With gapless pinned OFF, a same-format auto-advance falls back to
// drain-then-reconfigure: the sink is opened once per track (two decisions for
// two tracks), which is the pre-gapless behavior and the inverse of
// testGaplessStitchSameFormat. This proves the RAWFORM_NO_GAPLESS knob actually
// disables stitching.
void testGaplessDisabledByEnv() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    NoGaplessEnvGuard  noGapless;
    Engine             engine;

    auto           sink    = std::make_unique<RecordingSink>();
    RecordingSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100}, 44100, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    engine.enqueue("ramp:44100:2:4000:1048576");
    engine.enqueue("ramp:44100:2:6000:2097152");
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    // No stitch: each track opened the sink, so two decisions.
    CHECK(sinkPtr->recordedDecisions().size() == 2);
    CHECK(listener.trackPaths().size() == 2);
    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// ---------------------------------------------------------------------------
// The mode gates (rate-fallback): a rate mode flipped mid-run must void the
// same-source-format fast paths, because "same format means the same device
// decision" only holds while the mode is unchanged. One test per fast path.
// Both reproduce the same scenario: AlwaysResample parks a 44100 stream
// on a 96000 device, the mode flips to bit-perfect, and the next 44100 track
// must land the device on 44100 instead of riding the stale 96000 stream.

// Natural advance. Track A is far longer than the ring, so pausing right after
// Playing parks the producer with A's end of stream unreachable; the mode is
// flipped in that quiescent window, deterministically before the boundary. On
// resume, A drains, and the stitch gate must refuse (mode changed): two
// decisions, the second re-decided under bit-perfect, device switched to 44100.
void testRateModeChangeReconfiguresOnAdvance() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto           sink    = std::make_unique<RecordingSink>();
    RecordingSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100, 96000}, 96000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::AlwaysResample);
    engine.setSink(std::move(sink));

    // A: ~1M frames at 44100, roughly 50x the ring capacity (~20k frames at
    // 400 ms), so end of stream cannot be reached while paused, the flip below
    // is strictly before the boundary, and even a badly stalled scheduler
    // cannot drain A between Playing and the pause landing (~1 s of paced
    // pulls). B: a short tail. B's value band sits past A's whole 2x-stride
    // span (channel 0 of frame p is enc(base + 2p)), keeping the bands disjoint.
    const std::string a = "ramp:44100:2:1048576:1048576";
    const std::string b = "ramp:44100:2:4000:4194304";
    engine.enqueue(a);
    engine.enqueue(b);
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    engine.pause();
    CHECK(waitForState(engine, State::Paused));
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);  // the mid-run flip
    engine.play();

    // Full paced drain of A plus B; see kPacedDrainTimeoutMs for why the
    // default 5 s ceiling is wrong for this wait on a slow host.
    CHECK(waitForCompletion(engine, kPacedDrainTimeoutMs));
    settle();

    // The boundary reconfigured instead of stitching: two opens, two decisions.
    const std::vector<RateDecision> decisions = sinkPtr->recordedDecisions();
    CHECK(decisions.size() == 2);
    if (decisions.size() == 2) {
        // A opened under AlwaysResample: device left at its standing 96000.
        CHECK(decisions[0].deviceRate == 96000);
        CHECK(decisions[0].switchDevice == false);
        CHECK(decisions[0].resampleNeeded == true);
        // B re-decided under the flipped mode: bit-perfect at 44100.
        CHECK(decisions[1].deviceRate == 44100);
        CHECK(decisions[1].switchDevice == true);
        CHECK(decisions[1].resampleNeeded == false);
    }

    // Both tracks announced in order, and the drain lost no frames: the
    // consumed channel-0 stream is A's full band then B's, contiguous in value.
    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == a);
        CHECK(paths[1] == b);
    }
    std::vector<float>       expected = expectedChannel0(1048576u, 1048576u);
    const std::vector<float> bExp     = expectedChannel0(4194304u, 4000u);
    expected.insert(expected.end(), bExp.begin(), bExp.end());
    const std::vector<float> got = sinkPtr->recordedChannel0();
    CHECK(got.size() == expected.size());
    CHECK(channel0Matches(got, expected));

    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// Manual cut. Same device and same flip, but the boundary is a playNow of a
// same-format track, which without the mode gate would HOLD-CUT (no reopen,
// no re-decision) and carry the stale 96000 stream across the user's flip.
// The flip is sequenced before the playNow post from the same thread, so the
// engine reads it inside cutOrStartTo deterministically. The cut flushes A's
// remaining ring content by design, so the stream assertion here is B-only:
// the recorded tail is B's full
// band (the decision count is the path discriminator regardless).
void testRateModeChangeReconfiguresOnManualCut() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto           sink    = std::make_unique<RecordingSink>();
    RecordingSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100, 96000}, 96000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::AlwaysResample);
    engine.setSink(std::move(sink));

    const std::string a = "ramp:44100:2:1048576:1048576";  // long: still playing at the cut
    const std::string b = "ramp:44100:2:4000:4194304";
    engine.playNow(a);
    CHECK(waitForState(engine, State::Playing));

    engine.setRateMode(RateMode::BitPerfectWhenAvailable);  // the mid-run flip
    engine.playNow(b);                                      // same format: the old HOLD-CUT bait

    CHECK(waitForCompletion(engine));
    settle();

    // The cut reconfigured instead of holding: two opens, two decisions, the
    // second under the flipped mode.
    const std::vector<RateDecision> decisions = sinkPtr->recordedDecisions();
    CHECK(decisions.size() == 2);
    if (decisions.size() == 2) {
        CHECK(decisions[0].deviceRate == 96000);
        CHECK(decisions[0].resampleNeeded == true);
        CHECK(decisions[1].deviceRate == 44100);
        CHECK(decisions[1].switchDevice == true);
        CHECK(decisions[1].resampleNeeded == false);
    }

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == a);
        CHECK(paths[1] == b);
    }

    // B's full band closes the recorded stream.
    const std::vector<float> bExp = expectedChannel0(4194304u, 4000u);
    const std::vector<float> got  = sinkPtr->recordedChannel0();
    CHECK(got.size() >= bExp.size());
    if (got.size() >= bExp.size()) {
        const std::vector<float> tail(got.end() - static_cast<std::ptrdiff_t>(bExp.size()),
                                      got.end());
        CHECK(channel0Matches(tail, bExp));
    }

    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// ---------------------------------------------------------------------------
// Sink console: the outcome observers and the info-forwarding chain.
// One mixed-rate two-track run covers all of it: the bit-perfect open (44100
// on a switchable device) publishes rate 44100 / bit-perfect true, readable
// mid-playback; the resampled open (88200 source on a device whose ceiling is
// 48000, family fallback to 44100) publishes the fallback rate with
// bit-perfect false; entering Stopped clears both to the no-device answer;
// and every open forwarded its sink log line through the Engine to
// Listener::onInfo.
void testOutcomeObserversAndInfoForwarding() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto           sink    = std::make_unique<RecordingSink>();
    RecordingSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100, 48000}, 48000, /*canSwitch=*/true));

    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    // Track A long enough (about 50x the ring) that the mid-playback observer
    // reads below are unambiguously during A's open, not after some boundary.
    const std::string a = "ramp:44100:2:1048576:1048576";  // bit-perfect at 44100
    const std::string b = "ramp:88200:2:4000:4194304";     // falls back to 44100
    engine.enqueue(a);
    engine.enqueue(b);
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    // Mid-playback: A's open is bit-perfect at the source rate.
    CHECK(engine.outputDeviceRateHz() == 44100);
    CHECK(engine.outputBitPerfect() == true);

    // Full paced drain of A plus B; see kPacedDrainTimeoutMs.
    CHECK(waitForCompletion(engine, kPacedDrainTimeoutMs));
    settle();

    // Two opens (88200 != 44100 fails the stitch's format gate), and B's
    // decision was the family fallback: 88200 / 2 = 44100, advertised, so the
    // device stays there and the outcome was resampled-not-bit-perfect. The
    // observers themselves have since cleared (Stopped), so B's outcome is
    // asserted through the recorded decision.
    const std::vector<RateDecision> decisions = sinkPtr->recordedDecisions();
    CHECK(decisions.size() == 2);
    if (decisions.size() == 2) {
        CHECK(decisions[1].deviceRate == 44100);
        CHECK(decisions[1].resampleNeeded == true);
    }

    // Stopped: device released, the observers read the no-device answer.
    CHECK(engine.outputDeviceRateHz() == 0);
    CHECK(engine.outputBitPerfect() == false);

    // The forwarding chain: one sink line per open reached Listener::onInfo,
    // in open order, carrying each open's executed device rate.
    const std::vector<std::string> infos = listener.infos();
    CHECK(infos.size() == 2);
    if (infos.size() == 2) {
        CHECK(infos[0] == "RecordingSink: open at 44100 Hz");
        CHECK(infos[1] == "RecordingSink: open at 44100 Hz");
    }

    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// ---------------------------------------------------------------------------
// PLAYNOW / SETQUEUE SCENARIOS: the playNow / setQueue primitives, the
// currentTrackInfo() and duration() observers, and the device-unavailable and
// seek-rejected hardening paths. These pin the API: the primitives that let a
// controller say "play this exact track now, with this list behind it" without
// the engine owning a cursor, the pull observers a late-attaching UI needs, and
// graceful failure when the sink cannot open or a seekable decoder refuses a target.
// ---------------------------------------------------------------------------

// playNow from a STOPPED state plays the requested track directly, with no blip of
// the remembered track. We play then stop track R (so a bare play() would replay
// R), then playNow a DIFFERENT track T: the first frame the sink pulls after the
// playNow is T's band (frame 0, channel 0 == enc(valueBase)), never R's, and T is
// the track announced. This is the engine-level guarantee that retires the old CLI
// from-Stopped-on-another-track blip.
void testPlayNowFromStoppedNoBlip() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);

    auto           capturing = std::make_unique<CapturingSink>();
    CapturingSink* sinkPtr   = capturing.get();
    engine.setSink(std::move(capturing));

    const std::string remembered = "ramp:44100:2:5000000:1048576";  // band 0x100000
    const std::string target     = "ramp:44100:2:5000000:2097152";  // band 0x200000

    // Play then stop R: stop REMEMBERS it, so a later play() would replay R.
    engine.enqueue(remembered);
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(20);
    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
    settle();

    // playNow the different track. From Stopped this opens and starts T directly;
    // the first frame heard is T's band, proving R was not replayed first.
    engine.playNow(target);
    CHECK(waitForState(engine, State::Playing));
    settle(40);  // let the RT thread pull the first post-start block

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));

    const float captured = sinkPtr->firstFrameAfterStart();
    CHECK(std::fabs(captured - enc(2097152u)) <= 1.5f);  // T's band, not 0x100000

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == remembered);
        CHECK(paths[1] == target);  // playNow announced T, with no remembered replay
    }
    CHECK(listener.errors() == 0);
}

// playNow while Playing CUTS to the new track, and a same-format track queued
// behind it (via setQueue) then advances gaplessly. A long track A is playing;
// setQueue({C}) + playNow(B), all same 44100/2 format. The cut is a HOLD-CUT and
// the B -> C advance is a stitch, so the sink is opened exactly ONCE (for A). The
// cut flushes A, so the recorded channel-0 stream is some A prefix then all of B
// then all of C: its suffix of length (B + C) frames is B's band then C's band,
// sample-contiguous across the seam.
void testPlayNowCutsWhilePlaying() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;

    auto           sink    = std::make_unique<RecordingSink>();
    RecordingSink* sinkPtr = sink.get();
    sinkPtr->setCapabilities(makeCaps({44100}, 44100, /*canSwitch=*/true));
    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setRateMode(RateMode::BitPerfectWhenAvailable);
    engine.setSink(std::move(sink));

    const std::string a = "ramp:44100:2:5000000:1048576";  // long; cut away from it. band 0x100000
    const std::string b = "ramp:44100:2:4000:2097152";     // band 0x200000
    const std::string c = "ramp:44100:2:6000:3145728";     // band 0x300000

    engine.enqueue(a);
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(20);

    engine.setQueue(std::vector<std::string>{c});  // what plays after the cut target
    engine.playNow(b);                             // cut to B now, same format -> HOLD-CUT

    CHECK(waitForCompletion(engine));
    settle();

    // One open (A). The HOLD-CUT to B reopened nothing; the B -> C advance stitched.
    CHECK(sinkPtr->recordedDecisions().size() == 1);

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 3);
    if (paths.size() == 3) {
        CHECK(paths[0] == a);
        CHECK(paths[1] == b);
        CHECK(paths[2] == c);
    }

    std::vector<float>       expected = expectedChannel0(2097152u, 4000u);
    const std::vector<float> cExp     = expectedChannel0(3145728u, 6000u);
    expected.insert(expected.end(), cExp.begin(), cExp.end());

    const std::vector<float> got = sinkPtr->recordedChannel0();
    CHECK(got.size() >= expected.size());
    if (got.size() >= expected.size()) {
        const std::vector<float> suffix(
            got.end() - static_cast<std::ptrdiff_t>(expected.size()), got.end());
        CHECK(channel0Matches(suffix, expected));
    }
    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// setQueue REPLACES the pending queue wholesale. Two paths are enqueued, then
// setQueue swaps in two different ones before play(); only the replacement pair
// plays, in order. The discarded paths are distinct and openable, so if setQueue
// had appended (or not replaced) they would show up in the reported order.
void testSetQueueReplacesPending() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:81000");  // discarded by setQueue
    engine.enqueue("ramp:82000");  // discarded by setQueue
    engine.setQueue(std::vector<std::string>{"ramp:90000", "ramp:95000"});
    engine.play();

    CHECK(waitForCompletion(engine));
    settle();

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 2);
    if (paths.size() == 2) {
        CHECK(paths[0] == "ramp:90000");
        CHECK(paths[1] == "ramp:95000");
    }
    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() == 0);
}

// setQueue({}) clears the pending queue without disturbing the current track. A
// is playing with B pending; the empty setQueue lands long before A's EOS (one
// command versus the many produce steps A's length takes to drain), so when A
// finishes the queue is empty and the engine stops, having played only A.
void testSetQueueEmptyClears() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:200000");  // A, current
    engine.enqueue("ramp:91000");   // B, pending (distinct), would stitch if not cleared
    engine.play();
    CHECK(waitForState(engine, State::Playing));

    engine.setQueue(std::vector<std::string>{});  // clears B, leaves A untouched

    CHECK(waitForCompletion(engine));  // A finishes, empty queue -> Stopped
    settle();

    const auto paths = listener.trackPaths();
    CHECK(paths.size() == 1);
    if (!paths.empty()) {
        CHECK(paths[0] == "ramp:200000");  // only A; B was cleared before its turn
    }
    CHECK(engine.state() == State::Stopped);
}

// playNow on an UNOPENABLE path leaves the transport exactly as it was: the
// current track keeps playing, an error is reported, and no new track is
// announced. An explicit request that cannot be honored never interrupts what is
// already playing (the deliberate divergence from next()).
void testPlayNowBadPathUnchanged() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("ramp:5000000");  // A, long
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(20);

    const int errorsBefore = listener.errors();
    engine.playNow("bogus-path");  // factory returns nullptr
    settle(20);

    CHECK(engine.state() == State::Playing);                       // not interrupted
    CHECK(listener.errors() == errorsBefore + 1);                  // refusal reported
    CHECK(engine.currentTrackInfo().path == "ramp:5000000");       // still A
    CHECK(listener.trackPaths().size() == 1);                      // no new announcement

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
}

// A sink that cannot open is handled as a start failure on BOTH the play() and the
// playNow() paths: an error fires, the engine settles in Stopped, and no track is
// ever announced. The assertion that the engine reaches Stopped without hanging or
// crashing is the point; ASan/TSan validate the teardown.
void testDeviceUnavailableErrors() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    engine.setDecoderFactory(&factory);
    engine.setListener(&listener);
    engine.setSink(std::make_unique<FailingSink>());

    engine.enqueue("ramp:90000");
    engine.play();      // startTrack: sink->open fails -> error -> queue empty -> Stopped
    settle(60);
    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() >= 1);

    engine.playNow("ramp:91000");  // opens the decoder, but the sink still fails to open
    settle(60);
    CHECK(engine.state() == State::Stopped);
    CHECK(listener.errors() >= 2);

    CHECK(listener.trackPaths().empty());  // nothing ever became audible
}

// A seek on a SEEKABLE source whose decoder REJECTS the target abandons playback:
// the engine reports the rejection and settles in Stopped (it has already parked
// the RT thread and flushed, so it cannot safely continue). This is distinct from
// the unseekable case (testSeekUnseekableIgnored), which refuses up front and
// keeps playing.
void testSeekRejectedAbandons() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    engine.enqueue("seekfail:5000000");  // seekable() true, seek() returns false
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(20);

    const int errorsBefore = listener.errors();
    engine.seek(1.0);

    CHECK(waitForState(engine, State::Stopped));        // abandoned to Stopped
    CHECK(listener.errors() == errorsBefore + 1);       // rejection reported
}

// currentTrackInfo() and duration() reflect the loaded track and reset when
// Stopped. Empty while Stopped; while Playing they expose the path, total frames,
// native rate, seekability, and a duration matching frames / rate; empty again
// after stop. A final unseekable track confirms the seekable flag tracks the
// decoder.
void testCurrentTrackInfoAndDuration() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::Paced);

    CHECK(engine.currentTrackInfo().path.empty());  // Stopped: empty snapshot
    CHECK(engine.duration() == 0.0);

    engine.enqueue("ramp:44100:2:88200");  // 88200 frames @ 44100 Hz == 2.0 s, seekable
    engine.play();
    CHECK(waitForState(engine, State::Playing));
    settle(20);

    const TrackInfo info = engine.currentTrackInfo();
    CHECK(info.path == "ramp:44100:2:88200");
    CHECK(info.totalFrames == 88200u);
    CHECK(info.format.sampleRate == 44100u);
    CHECK(info.seekable == true);
    CHECK(std::fabs(engine.duration() - 2.0) < 0.01);

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
    settle();
    CHECK(engine.currentTrackInfo().path.empty());  // reset on stop
    CHECK(engine.duration() == 0.0);

    // An unseekable source reports seekable == false (the UI scrubber would
    // disable itself). playNow here also re-confirms it plays the requested track,
    // not the remembered one.
    engine.playNow("noseek:44100:2:50000");
    CHECK(waitForState(engine, State::Playing));
    settle(20);
    CHECK(engine.currentTrackInfo().seekable == false);

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
}

// The ThreadSanitizer workload for the playNow/setQueue primitives: a FreeRun sink while
// the controlling thread hammers playNow / setQueue / seek / next (and the odd
// pause/play) against a small pool of identical-format paths, so cuts HOLD and
// advances STITCH, racing the producer and consumer maximally. It must survive
// clean and settle in Stopped; TSan and ASan catch any race or use-after-free in
// doPlayNow / doSetQueue / cutOrStartTo.
void testPrimitiveFreeRunSoak() {
    RampDecoderFactory factory;
    RecordingListener  listener;
    Engine             engine;
    configure(engine, factory, listener, NullSink::Mode::FreeRun);

    const std::vector<std::string> pool = {
        "ramp:44100:2:120000:1048576",  // band 0x100000
        "ramp:44100:2:140000:2097152",  // band 0x200000
        "ramp:44100:2:90000:3145728",   // band 0x300000
        "ramp:44100:2:160000:4194304",  // band 0x400000
    };
    const int n = static_cast<int>(pool.size());

    engine.setQueue(pool);
    engine.play();

    for (int round = 0; round < 60; ++round) {
        engine.playNow(pool[static_cast<std::size_t>(round % n)]);
        std::this_thread::sleep_for(std::chrono::microseconds(150));
        engine.setQueue(std::vector<std::string>{
            pool[static_cast<std::size_t>((round + 1) % n)],
            pool[static_cast<std::size_t>((round + 2) % n)]});
        std::this_thread::sleep_for(std::chrono::microseconds(150));
        engine.seek(0.1 * static_cast<double>(round % 4));
        std::this_thread::sleep_for(std::chrono::microseconds(150));
        engine.next();
        std::this_thread::sleep_for(std::chrono::microseconds(150));
        if ((round % 6) == 0) {
            engine.pause();
            engine.play();
        }
    }

    engine.stop();
    CHECK(waitForState(engine, State::Stopped));
    CHECK(engine.state() == State::Stopped);
}

}  // namespace

int main() {
    testPlayToCompletion();
    testPauseResume();
    testStopRewindsAndReplays();
    testQueueAdvanceAndExhaustion();
    testNextSkips();
    testClearQueue();
    testOpenFailureSkips();

    testPositionMonotonicAndFrozen();
    testSeekPausedJumpsAndStaysPaused();
    testSeekDecodedFrameLandsAtTarget();
    testPositionCadence();
    testSeekPastEndClampsToCompletion();
    testSeekUnseekableIgnored();
    testSeekStoppedIgnored();

    testConcurrentHammer();
    testDestroyMidStream();

    testRateGateFixedDevice();
    testRateGateBitPerfectSwitch();
    testRateModeAlwaysResampleNeverSwitches();

    testReconfigureDeclinedFallsBackToCloseOpen();
    testReconfigureAcceptedKeepsDeviceOpen();

    testDeviceEnumerationPushesThroughListener();
    testSelectDeviceWhileStoppedParksAndRefusesUnknown();
    testSelectDeviceWhilePlayingReopens();
    testSelectDeviceWhilePausedStaysPaused();

    testExternalRateChangeRepublishesOutcome();
    testDefaultDeviceChangeReopensWhenFollowing();
    testDefaultDeviceChangeIgnoredWhenPinned();
    testDeviceListAndDebtEventsRelay();

    testGaplessStitchSameFormat();
    testGaplessNoStitchDifferentRate();
    testGaplessMultipleSeams();
    testGaplessPositionResetsPerTrack();
    testGaplessFreeRunSoak();
    testGaplessDisabledByEnv();
    testRateModeChangeReconfiguresOnAdvance();
    testRateModeChangeReconfiguresOnManualCut();
    testOutcomeObserversAndInfoForwarding();

    testPlayNowFromStoppedNoBlip();
    testPlayNowCutsWhilePlaying();
    testSetQueueReplacesPending();
    testSetQueueEmptyClears();
    testPlayNowBadPathUnchanged();
    testDeviceUnavailableErrors();
    testSeekRejectedAbandons();
    testCurrentTrackInfoAndDuration();
    testPrimitiveFreeRunSoak();

    if (g_failures == 0) {
        std::puts("EngineTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "EngineTest: %d check(s) failed\n", g_failures);
    return 1;
}
