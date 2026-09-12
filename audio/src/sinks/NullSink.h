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

// NullSink.h
//
// A headless IAudioSink: it spins a pull thread, discards the audio, and is the
// portable stand-in for a real device. Its reason to exist is the transport
// soak: with NullSink and an injected in-memory decoder, the full command/state
// machine plus the real producer and consumer threads run under
// ThreadSanitizer with no device backend and no codec library in the process.
// It is a faithful IAudioSink, so it also exercises the engine's teardown
// ordering for real: stop() joins its pull thread, which is the synchronization
// point the lifetime invariant depends on.
//
// It is portable (no platform headers), so unlike the device sinks it is always
// compiled into the library; it is a legitimate "null output" besides being a
// test aid.
//
// Beyond the transport, NullSink is a configurable FAKE of everything the
// platform sinks expose, so the engine's device-side logic is testable without
// hardware. Each fake models the sink-side half honestly and records what the
// engine handed it, for the test to assert after the run settles:
//   - capabilities(): a test-injected SinkCapabilities (which rates the fake
//     device advertises, its current rate, whether it can switch). A broad
//     default keeps the plain transport tests, which never inspect rate,
//     unaffected.
//   - open() and reconfigure(): each records the RateDecision the engine
//     resolved (two separate logs) and advances the fake device's current
//     rate to the executed device rate, so the engine's next capabilities()
//     read reflects the rate the device now runs at, the way a real device
//     would after a switch. reconfigure() is opt-in (setReconfigureSupported),
//     default off, so the whole existing suite keeps exercising the engine's
//     close+open fallback. closeCount() makes the "one open per session, one
//     release at the final close" shape assertable.
//   - The rate-gate tests pin gapless OFF (via RAWFORM_NO_GAPLESS), so the
//     engine reopens at every boundary and there is one recorded decision per
//     track; with gapless ON a same-format boundary is a stitch with no open()
//     at all, and the gapless tests use the decision COUNT to tell a stitch
//     from a reconfigure.
//   - Devices: setDevices() configures the list enumerateDevices() reports;
//     selectDevice() pins by id and refuses unknown ids, mirroring the platform
//     sinks so the engine's error path is reachable.
//   - Events: setEventListener() installs the engine's channel, and the fire*
//     injectors emit each ISinkEventListener event from the CALLING thread,
//     proving the engine's any-thread enqueue contract under TSan. An injected
//     external rate change moves the fake device first, so the sink is in the
//     same forgiven state a real one is in when it reports.
// The logs are mutex-guarded because the engine thread writes them while the
// test thread reads after the run settles.
//
// Two pacing modes:
//   - Paced: pull a block, then sleep a small FIXED interval. The interval is
//     decoupled from real sample time (this is correctness, not realtime),
//     so "play briefly, pause, advance, exhaust" sequences are expressible and
//     the producer's full-ring path is exercised, while tests still finish in a
//     fraction of real time.
//   - FreeRun: pull back-to-back, yielding only. Maximal producer/consumer
//     interleaving for the ThreadSanitizer workload.

#pragma once

#include "rawform/audio/IAudioSink.h"
#include "rawform/audio/RateManager.h"
#include "rawform/audio/Types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rawform::audio {

class NullSink final : public IAudioSink {
public:
    enum class Mode {
        Paced,    ///< a small fixed sleep between pulls; the default for transport tests
        FreeRun,  ///< no sleep; maximal interleaving for the TSan soak
    };

    explicit NullSink(Mode mode = Mode::Paced, std::size_t blockFrames = 1024);
    ~NullSink() override;  ///< closes if still open

    /// Not copyable or movable: owns a thread and is referenced by it through this.
    NullSink(const NullSink&)            = delete;
    NullSink& operator=(const NullSink&) = delete;
    NullSink(NullSink&&)                 = delete;
    NullSink& operator=(NullSink&&)      = delete;

    // ----- rate-test configuration / observation ------------------------------

    /// Replace the simulated device capabilities. Call before injecting the sink
    /// into the engine (the engine queries capabilities() at track start). The
    /// default set is broad enough that the rate-agnostic transport tests are
    /// unaffected.
    void setCapabilities(SinkCapabilities caps);

    /// The rate decisions the engine handed to open(), in order, one per track.
    /// Read after the run has settled (no open() in flight); mutex-guarded so the
    /// read synchronizes with the engine thread's writes for TSan.
    [[nodiscard]] std::vector<RateDecision> recordedDecisions() const;

    // ----- device-hold test configuration / observation -----------------------

    /// Opt this sink into the in-place reconfigure branch. Default OFF, so
    /// every scenario that does not opt in keeps the close+open path and the
    /// engine's fallback branch stays covered by the whole existing suite.
    /// Call before injecting the sink into the engine, like setCapabilities.
    void setReconfigureSupported(bool on);

    /// The decisions the engine handed to reconfigure(), in order, one per
    /// ACCEPTED in-place transition. Same read discipline as recordedDecisions.
    [[nodiscard]] std::vector<RateDecision> recordedReconfigures() const;

    /// How many times close() released a genuinely open device. With the
    /// reconfigure branch on, a whole multi-track run should show exactly one
    /// (the final release at Stopped): the "one open per session, one
    /// repayment at the final close" ledger shape, made assertable.
    [[nodiscard]] int closeCount() const;

    // ----- device-list test configuration / observation -----------------------

    /// Configure the simulated device list enumerateDevices() reports. Call
    /// before injecting the sink, like setCapabilities.
    void setDevices(std::vector<AudioDeviceInfo> devices);

    /// The device id currently pinned by selectDevice() (empty = following the
    /// system default, the initial state). Same read-after-settle discipline as
    /// the decision logs.
    [[nodiscard]] std::string selectedDeviceId() const;

    // ----- sink-event test injection ------------------------------------------
    /// Fire the installed ISinkEventListener from the CALLING thread, proving
    /// the engine's any-thread enqueue contract under TSan. Each models the
    /// sink-side half honestly first: an external rate change moves the fake
    /// device (capabilities' currentRate and the measurement) to the new rate
    /// before reporting, exactly the forgiven-ledger state a real sink is in
    /// when it emits.
    void fireExternalRateChanged(std::uint32_t newRateHz);
    void fireDefaultDeviceChanged();
    void fireDeviceListChanged();
    void fireRateDebtChanged(const std::string& deviceId,
                             std::uint32_t originalRateHz,
                             std::uint32_t borrowedRateHz);

    // ----- IAudioSink ---------------------------------------------------------
    /// capabilities() returns the configured set. open() sizes the discard scratch
    /// from the source format, records the decision, and attaches the source;
    /// start() spins the pull thread; stop() JOINS it (the lifetime
    /// synchronization point); close() releases. currentFormat() echoes open()'s
    /// source format. The decision is recorded for assertion and the simulated
    /// device's currentRate follows its executed rate; nothing else about the
    /// discard path changes, since there is no real device behind it.
    [[nodiscard]] SinkCapabilities capabilities() const override;
    bool             open(const AudioFormat&  sourceFormat,
                          const RateDecision& decision,
                          IPullSource*        source) override;
    /// Declines unless setReconfigureSupported(true); when accepting, it mirrors
    /// open()'s bookkeeping (format echo, scratch resize, device-follow of the
    /// executed rate) with the source kept attached, and records the decision in
    /// the reconfigure log instead of the open log.
    bool             reconfigure(const AudioFormat&  sourceFormat,
                                 const RateDecision& decision) override;
    /// enumerateDevices() reports the configured list; selectDevice() accepts
    /// the empty id (follow default) or any id present in that list, records
    /// it, and refuses everything else, so the engine's refusal path is
    /// testable too.
    [[nodiscard]] std::vector<AudioDeviceInfo> enumerateDevices() const override;
    bool                         selectDevice(const std::string& deviceId) override;
    void                         setEventListener(ISinkEventListener* listener) override;
    void             start() override;
    void             stop()  override;
    void             close() override;
    [[nodiscard]] AudioFormat currentFormat() const override;
    /// Models a measuring device: the capability model's currentRate,
    /// which open()/reconfigure() move to each executed switch, IS the measured
    /// rate while open. Keeps the engine's measurement-first publication and
    /// mid-session republication testable without hardware.
    [[nodiscard]] std::uint32_t measuredDeviceRateHz() const override;

private:
    void pumpLoop();  ///< the consumer / pull thread

    Mode               m_mode;
    std::size_t        m_blockFrames;
    AudioFormat        m_format{};
    IPullSource*       m_source = nullptr;  ///< non-owning
    std::vector<float> m_scratch;           ///< discard buffer, sized at open()
    std::atomic<bool>  m_running{false};
    std::thread        m_thread;
    bool               m_opened = false;

    /// Simulated device capabilities and the recorded decision log, both touched
    /// only off the RT path (capabilities() and open() run on the engine thread;
    /// the log is read by the test thread after settling), so a plain mutex is the
    /// right tool.
    mutable std::mutex        m_mtx;
    SinkCapabilities          m_caps;
    std::vector<RateDecision> m_decisions;

    /// Device-hold state. The toggle is set once before injection and read on the
    /// engine thread only, so a plain bool; the reconfigure log and close count
    /// share m_mtx with the open log for the same TSan-clean read-after-settle
    /// discipline.
    bool                      m_reconfigureSupported = false;
    std::vector<RateDecision> m_reconfigures;
    int                       m_closeCount = 0;

    /// Device-list state, same m_mtx discipline.
    std::vector<AudioDeviceInfo> m_devices;
    std::string                  m_selectedDeviceId;  ///< empty = system default

    /// The installed event listener. Written once, before any playback
    /// (Engine::setSink installs it), read by the fire* helpers from arbitrary
    /// test threads; m_mtx guards the pointer for TSan cleanliness, the calls
    /// themselves run outside the lock (the listener enqueues, never
    /// re-enters).
    ISinkEventListener*          m_events = nullptr;
};

}  // namespace rawform::audio
