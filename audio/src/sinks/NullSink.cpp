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

// NullSink.cpp
//
// Implementation of the headless sink. The pull thread is the consumer; it calls
// the source's pull() exactly the way a real device's RT thread does, into a
// fixed scratch buffer it then throws away. The subtlety is the stop()/join
// being the synchronization point the IAudioSink contract promises: after stop()
// returns, no pull() is in flight or will follow, so the engine may safely reset
// the ring the source reads, or call reconfigure() to retune the parked sink.
//
// Everything else here is a fake of the platform sinks' device-side behavior,
// under one mutex (m_mtx) that the engine thread writes through and the test
// thread reads through after the run settles:
//   - capabilities() returns the configured simulated device. open() and
//     reconfigure() record the RateDecision they were handed and move the
//     simulated device's currentRate to the executed device rate, so the fake
//     follows the decision the way real hardware follows a nominal-rate set.
//     reconfigure() also enforces the contract's preconditions (opted in,
//     genuinely open, valid format) and declines otherwise, so an engine that
//     calls it in the wrong state is caught rather than humored.
//   - The device surface is a configured list; selection pins by id and
//     refuses ids that do not resolve, matching the platform sinks.
//   - The event injectors copy the listener pointer out under the lock and
//     invoke it outside the lock, on whatever thread the test called from.
//     fireExternalRateChanged moves the fake device before reporting, so the
//     observable state matches a real sink's forgiven-ledger state at the
//     moment it emits.

#include "sinks/NullSink.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace rawform::audio {

namespace {

// A broad default capability set so the rate-agnostic transport and TSan tests
// behave as if no rate model existed: it advertises the common rates as a
// single wide continuous span, sits at 48000, and permits switching. Rate tests
// override this with setCapabilities() to model a specific device.
SinkCapabilities broadDefaultCapabilities() {
    SinkCapabilities c;
    c.deviceName    = "NullSink";
    c.rates.push_back(RateRange{.min = 8000, .max = 768000});  // one wide span covers all common rates
    c.currentRate   = 48000;
    c.canSwitchRate = true;
    return c;
}

}  // namespace

// ---------------------------------------------------------------------------
NullSink::NullSink(Mode mode, std::size_t blockFrames)
    : m_mode(mode),
      m_blockFrames(blockFrames == 0 ? 1u : blockFrames),
      m_caps(broadDefaultCapabilities()) {}

NullSink::~NullSink() {
    close();
}

// ---------------------------------------------------------------------------
void NullSink::setCapabilities(SinkCapabilities caps) {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_caps = std::move(caps);
}

std::vector<RateDecision> NullSink::recordedDecisions() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_decisions;
}

// ---------------------------------------------------------------------------
SinkCapabilities NullSink::capabilities() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_caps;
}

bool NullSink::open(const AudioFormat& sourceFormat, const RateDecision& decision,
                    IPullSource* source) {
    if (!sourceFormat.isValid() || source == nullptr) {
        return false;
    }
    if (m_opened) {
        close();
    }
    m_format = sourceFormat;
    m_source = source;
    // Discard buffer for one block of interleaved float32 at the source shape.
    m_scratch.assign(
        m_blockFrames * static_cast<std::size_t>(sourceFormat.channels), 0.0f);

    // Record the decision for the rate test. One open() per track without gapless,
    // so this log has one entry per track played. Also move the simulated device's
    // current rate to the executed device rate, so the engine's NEXT
    // capabilities() read reflects the rate we are now running at, exactly as a
    // real device would after a nominal-rate change. This is what lets a
    // multi-track scenario exercise the engine's gate (switch, then hold) without
    // the fake reporting a stale current rate.
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_decisions.push_back(decision);
        if (decision.deviceRate != 0) {
            m_caps.currentRate = decision.deviceRate;
        }
    }

    m_opened = true;
    return true;
}

// In-place reconfigure. Accept only when the test opted in AND the device is
// genuinely open (the engine's contract says it only calls in that state,
// but a fake that enforces the contract catches an engine that breaks it).
// The engine parked the RT thread before calling (stop() joined the pump),
// so touching the format and scratch is race-free; the source stays attached,
// exactly the contract's "same ring source, merely reconfigured".
bool NullSink::reconfigure(const AudioFormat&  sourceFormat,
                           const RateDecision& decision) {
    if (!m_reconfigureSupported || !m_opened || !sourceFormat.isValid()) {
        return false;
    }
    m_format = sourceFormat;
    m_scratch.assign(
        m_blockFrames * static_cast<std::size_t>(sourceFormat.channels), 0.0f);
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_reconfigures.push_back(decision);
        // Model the device following the executed rate, exactly as open() does,
        // so the engine's NEXT capabilities() read sees the rate we now run at.
        if (decision.deviceRate != 0) {
            m_caps.currentRate = decision.deviceRate;
        }
    }
    return true;
}

void NullSink::setReconfigureSupported(bool on) {
    m_reconfigureSupported = on;
}

std::vector<RateDecision> NullSink::recordedReconfigures() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_reconfigures;
}

int NullSink::closeCount() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_closeCount;
}

// ---------------------------------------------------------------------------
// Device surface: a configurable fake, mirroring how the platform sinks
// behave (enumeration lists what exists; selection pins by persistent id and
// refuses ids that do not resolve).
void NullSink::setDevices(std::vector<AudioDeviceInfo> devices) {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_devices = std::move(devices);
}

std::vector<AudioDeviceInfo> NullSink::enumerateDevices() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_devices;
}

bool NullSink::selectDevice(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(m_mtx);
    if (deviceId.empty()) {
        m_selectedDeviceId.clear();
        return true;
    }
    for (const AudioDeviceInfo& d : m_devices) {
        if (d.id == deviceId) {
            m_selectedDeviceId = deviceId;
            return true;
        }
    }
    return false;  // unknown id: refuse, so the engine's onError path is testable
}

std::string NullSink::selectedDeviceId() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_selectedDeviceId;
}

// ---------------------------------------------------------------------------
// The event channel and its test injectors (header comment has the
// contract). The pointer is copied out under the lock and invoked outside it.

void NullSink::setEventListener(ISinkEventListener* listener) {
    std::lock_guard<std::mutex> lock(m_mtx);
    m_events = listener;
}

std::uint32_t NullSink::measuredDeviceRateHz() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_opened ? m_caps.currentRate : 0;
}

void NullSink::fireExternalRateChanged(std::uint32_t newRateHz) {
    ISinkEventListener* l = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        if (newRateHz != 0) {
            m_caps.currentRate = newRateHz;  // the device moved; forgiven state
        }
        l = m_events;
    }
    if (l != nullptr) {
        l->onExternalRateChanged(newRateHz);
    }
}

void NullSink::fireDefaultDeviceChanged() {
    ISinkEventListener* l = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        l = m_events;
    }
    if (l != nullptr) {
        l->onDefaultDeviceChanged();
    }
}

void NullSink::fireDeviceListChanged() {
    ISinkEventListener* l = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        l = m_events;
    }
    if (l != nullptr) {
        l->onDeviceListChanged();
    }
}

void NullSink::fireRateDebtChanged(const std::string& deviceId,
                                   std::uint32_t originalRateHz,
                                   std::uint32_t borrowedRateHz) {
    ISinkEventListener* l = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        l = m_events;
    }
    if (l != nullptr) {
        l->onRateDebtChanged(deviceId, originalRateHz, borrowedRateHz);
    }
}

// ---------------------------------------------------------------------------
void NullSink::start() {
    if (m_opened && !m_running.load(std::memory_order_relaxed)) {
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread(&NullSink::pumpLoop, this);
    }
}

// stop() flips running and JOINS the pull thread, so no pull() follows. Safe to
// call when not started.
void NullSink::stop() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void NullSink::close() {
    stop();  // idempotent; implies the join if still running
    if (m_opened) {
        std::lock_guard<std::mutex> lock(m_mtx);
        ++m_closeCount;  // count genuine releases only, not redundant closes
    }
    m_opened = false;
    m_source = nullptr;
}

// ---------------------------------------------------------------------------
AudioFormat NullSink::currentFormat() const {
    return m_format;
}

// ---------------------------------------------------------------------------
// The consumer thread. Pulls a block, discards it, and either yields (FreeRun)
// or naps a small fixed interval (Paced). The nap is intentionally NOT tied to
// the sample rate: the transport soak cares about correctness and concurrency, not
// realtime cadence, so a coarse fixed nap keeps tests fast while still letting
// the producer's full-ring path engage.
void NullSink::pumpLoop() {
    while (m_running.load(std::memory_order_acquire)) {
        if (m_source != nullptr) {
            m_source->pull(m_scratch.data(), m_blockFrames);
        }
        if (m_mode == Mode::Paced) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            std::this_thread::yield();
        }
    }
}

}  // namespace rawform::audio
