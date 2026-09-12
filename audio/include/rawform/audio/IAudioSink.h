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

// IAudioSink.h
//
// The output boundary of the rawform audio engine: the abstraction every
// platform sink (CoreAudioSink on macOS, PipeWireSink on Linux, NullSink for
// headless runs) implements, plus the pull-source abstraction the sink reads
// through, the log receiver a sink reports through, and the event channel a
// sink signals through. Like Types.h and IDecoder.h this header is
// dependency-free past the shared value types: it pulls in Types.h, the
// rate-policy value types (RateDecision, SinkCapabilities), and the standard
// size, integer, string, and vector headers, and nothing else, so the engine,
// the CLI, the tests, and every concrete sink program against the same
// contract without inheriting any platform header. A CoreAudio or PipeWire
// include never reaches this file; the platform types live behind each sink's
// own pimpl.
//
// The pull model. The sink owns a real-time thread (the HAL IO thread on macOS,
// the PipeWire process thread on Linux, a plain std::thread in NullSink) that
// calls IPullSource::pull() to fetch the next block of interleaved float32.
// This is the same shape on every backend, which is exactly why the interface
// above the sink stays portable and only one file per platform changes.
//
// The rate seam. The sink decides nothing about sample rate: the Engine reads
// the device's capabilities() once per boundary, asks the portable RateManager
// for a RateDecision, and hands that decision to open() or reconfigure(). The
// sink executes it (switch the device or not, to the chosen rate) and reports
// the measured outcome separately through measuredDeviceRateHz(). The decision
// is PREDICTED by the RateManager; the realized outcome is MEASURED by the
// sink, because a device can accept a rate set, report success at every layer,
// and still misclock, so measurement outranks prediction everywhere the
// outcome is published.
//
// The session model. open() acquires the device for a listening session;
// reconfigure() retunes the same open device across a track boundary while
// the real-time thread is parked (no release/reacquire, no mid-session rate
// restore); close() releases it once at the end of the session. A sink that
// cannot retune in place declines reconfigure() and the Engine falls back to
// close()+open(). Any nominal-rate debt a sink accumulates by switching the
// device is repaid at close(); a sink that keeps a ledger publishes it through
// the event channel so the owner can persist it, and repayStaleRateDebt()
// settles a persisted debt left by a crashed session before the sink is ever
// opened.
//
// The device surface. enumerateDevices() lists the selectable outputs by
// persistent id (AudioDeviceInfo in Types.h) and selectDevice() pins one; the
// pin redirects what capabilities() and the next open() resolve, and the
// Engine drives the reopen that makes a change audible. The event channel
// (ISinkEventListener) is how a sink reports the world changing underneath a
// session: an external rate assertion, a default-device change, a device-list
// change, a ledger change. The defaults for all of these are honest no-ops, so
// a sink with no device concept (NullSink's discard path) is a complete
// IAudioSink without overriding them.
//
// Lifetime invariant, the reason the sink can hold the source non-owning:
//   stop()/close() MUST guarantee the real-time thread has finished and will
//   not call pull() again BEFORE the IPullSource and the buffer it reads are
//   destroyed. CoreAudio gives that through the synchronous
//   AudioOutputUnitStop; PipeWire gives it by disarming the pull under the
//   thread-loop lock, which excludes the process callback; NullSink joins its
//   thread. Callers therefore order teardown as: stop()/close() the sink
//   first, then destroy the source.

#pragma once

#include "rawform/audio/RateManager.h"  // RateDecision, SinkCapabilities
#include "rawform/audio/Types.h"

#include <cstddef>
#include <cstdint>  // measuredDeviceRateHz return type
#include <string>   // ILogOutput line type
#include <vector>   // enumerateDevices return type

namespace rawform::audio {

// ---------------------------------------------------------------------------
/// A line-oriented log receiver a sink can be handed. Named
/// ILogOutput rather than anything containing "sink" because "sink" is already
/// taken by the audio-output concept in this header; this is where log LINES
/// go, not where audio goes. Non-owning: the Engine implements it and installs
/// itself via IAudioSink::setLogOutput at injection, and the Engine outlives
/// every sink it owns, so the pointer is valid for the sink's whole life.
/// Contract: logLine is called from the ENGINE thread only (a sink's log sites
/// are its open/close paths, which the engine drives); never from the RT
/// thread, so an implementation may allocate, lock, and marshal freely.
struct ILogOutput {
    virtual ~ILogOutput() = default;
    virtual void logLine(const std::string& line) = 0;
};

/// The thing a sink pulls audio out of. One method, called on the sink's
/// real-time thread, so the contract on it is strict: no lock, no allocation, no
/// system call, no I/O, and it must not throw (hence noexcept). It writes up to
/// `frames` interleaved float32 frames into `out` (capacity frames*channels
/// floats) and returns how many it could actually supply. A return less than
/// `frames` is an underrun or end of stream; the SINK, not the source, zero-pads
/// the remainder of `out`, so a source that runs short simply returns the short
/// count and is done.
struct IPullSource {
    virtual ~IPullSource() = default;

    /// Real-time safe. Returns frames supplied (0..frames). The sink zero-pads
    /// out[supplied*channels .. frames*channels) on a short return.
    virtual std::size_t pull(float* out, std::size_t frames) noexcept = 0;
};

/// The sink-to-engine event channel: how a platform sink tells the
/// engine the world changed underneath it. THREADING CONTRACT: these may fire
/// from arbitrary platform threads (a CoreAudio dispatch queue, the PipeWire
/// loop thread), never from the real-time pull; implementations must be
/// thread-safe and quick (enqueue, do not process), and must tolerate calls at
/// any moment between setEventListener and the sink's destruction.
///
/// Semantics, chosen so the engine's reactions stay sink-agnostic:
///   - onExternalRateChanged: the device/graph rate moved WITHOUT this sink
///     initiating it: the user or another program asserted ownership. The sink
///     has already forgiven its own restore ledger where one exists;
///     newRateHz is the best-known new rate, 0 when unknown. The engine
///     republishes the outcome so the UI tracks reality mid-session.
///   - onDefaultDeviceChanged: the system default output changed AND this sink
///     cannot itself migrate a live stream to it (CoreAudio emits this; the
///     PipeWire session manager migrates streams on its own, so that sink
///     deliberately does not). The engine re-resolves, reopening at position
///     when it is following the default.
///   - onDeviceListChanged: the selectable device set, or its default-marking,
///     changed: a fresh enumeration would differ. The engine pushes one so
///     pickers stay current.
///   - onRateDebtChanged: the restore ledger changed; the engine forwards it
///     for persistence (crash recovery). All-zero arguments mean cleared.
///     Sinks whose repayment is structural (PipeWire) never emit it.
struct ISinkEventListener {
    virtual ~ISinkEventListener() = default;
    virtual void onExternalRateChanged(std::uint32_t /*newRateHz*/) {}
    virtual void onDefaultDeviceChanged() {}
    virtual void onDeviceListChanged() {}
    virtual void onRateDebtChanged(const std::string& /*deviceId*/,
                                   std::uint32_t /*originalRateHz*/,
                                   std::uint32_t /*borrowedRateHz*/) {}
};

/// A platform output device, configured for one interleaved float32 stream.
/// open() negotiates the device for a given source format and rate decision and
/// wires it to a source; reconfigure() retunes the already-open device for the
/// next track; start()/stop() run and park the real-time thread; close() tears
/// the device down. capabilities() reports what the target device can do,
/// independent of open(), so the Engine can consult the rate policy before
/// committing. The sink does NOT own the source: see the lifetime invariant in
/// the file header for why that is safe. Every method here is an engine-thread
/// entry point; only the IPullSource the sink pulls through runs on the
/// real-time thread.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    /// Report the target device's current capabilities (advertised rates,
    /// current rate, whether a rate switch is possible, name for logging), read
    /// fresh. The target is the device pinned by selectDevice(), else the
    /// system default. Readable independent of open(), and re-read by the
    /// Engine before every open() and reconfigure(). Not on the RT path, so
    /// allocation is fine. A target that cannot be resolved (no device, a
    /// vanished pin) reports an unusable, non-switchable set rather than
    /// silently describing some other device.
    [[nodiscard]] virtual SinkCapabilities capabilities() const = 0;

    /// The device surface, default-implemented so every sink is correct
    /// without modification. enumerateDevices() lists the selectable outputs
    /// (persistent ids; see AudioDeviceInfo in Types.h); the default reports
    /// none, honest for a sink with no device concept. selectDevice() pins the
    /// target that the NEXT capabilities() read and open() resolve; the empty
    /// id means follow the system default, which is the only selection the
    /// base accepts (a sink that cannot address devices cannot honor a
    /// non-empty pin). Selection never touches a live stream by itself: the
    /// Engine drives the reopen boundary that makes a change audible. Both are
    /// engine-thread entry points, like every non-RT method here.
    [[nodiscard]] virtual std::vector<AudioDeviceInfo> enumerateDevices() const {
        return {};
    }
    virtual bool selectDevice(const std::string& deviceId) {
        return deviceId.empty();
    }

    /// Install the engine's event listener; see ISinkEventListener above for
    /// the contract. Default ignores it, honest for a sink with no events to
    /// report.
    virtual void setEventListener(ISinkEventListener* listener) {
        (void)listener;
    }

    /// Crash recovery. Called by the OWNER before the sink is
    /// injected into an engine and before any open(), with the triple a
    /// previous session persisted from onRateDebtChanged: resolve the device by
    /// its persistent id and, IFF it still sits at the borrowed rate, restore
    /// the original; a device that is absent or has since been moved by anyone
    /// is left alone (the debt is moot or overridden). Returns whether a
    /// restore was performed. Default false: a sink that never emits debt
    /// (structural repayment) has nothing to repay.
    virtual bool repayStaleRateDebt(const std::string& deviceId,
                                    std::uint32_t originalRateHz,
                                    std::uint32_t borrowedRateHz) {
        (void)deviceId;
        (void)originalRateHz;
        (void)borrowedRateHz;
        return false;
    }

    /// Negotiate the device for `sourceFormat` (the interleaved float32 stream the
    /// source delivers) per `decision` (the rate the device should run at and
    /// whether to switch it there, as resolved by the RateManager), and attach
    /// `source` (non-owning; must outlive the sink). Returns false on any device
    /// failure, in which case the sink is left closed and nothing was started.
    /// `source` must be non-null. The sink executes the decision; it does not
    /// re-derive it.
    virtual bool open(const AudioFormat&  sourceFormat,
                      const RateDecision& decision,
                      IPullSource*        source) = 0;

    /// In-place reconfigure for a track-to-track transition: retune the
    /// ALREADY-OPEN device for a new source format and decision without closing
    /// it, so no release/reacquire churn and no mid-session rate restore. Called
    /// by the Engine only while the device is open and the real-time thread is
    /// parked (stop() has returned); the source registered at open() remains
    /// attached, and the Engine guarantees that object stays valid (it is the
    /// same ring source, merely reconfigured) across the transition. On true:
    /// the sink is negotiated for `sourceFormat` per `decision` and ready for
    /// start(), exactly as after a successful open(). On false: the Engine
    /// closes and re-opens, so the requirement on a declining or failing sink
    /// is not "untouched" but "close()-tolerant": a clean decline leaves the
    /// sink simply parked, while a failure partway through a retune may leave
    /// it degraded, and close() must cope with either, releasing the device
    /// and repaying any rate debt as always. The default declines, so a sink
    /// with no device to retune is correct without overriding it; both
    /// platform sinks override it, and NullSink accepts only when a test opts
    /// in, so the fallback path stays covered.
    virtual bool reconfigure(const AudioFormat&  sourceFormat,
                             const RateDecision& decision) {
        (void)sourceFormat;
        (void)decision;
        return false;
    }

    /// Begin pulling: the real-time thread starts and pull() is called from it.
    virtual void start() = 0;

    /// Stop pulling and JOIN the real-time thread before returning, so no pull()
    /// is in flight or will follow. This is the synchronization point the
    /// lifetime invariant relies on; it is safe to call when not started.
    virtual void stop() = 0;

    /// Release the device. Idempotent; implies stop() if still running.
    virtual void close() = 0;

    /// The interleaved float32 format the sink is actually pulling at: the source
    /// format passed to open(). It is the rate the source delivers and the
    /// callback consumes, which the engine's position math depends on. This is the
    /// SOURCE rate, not the device rate; in a resample case they differ, and the
    /// device rate is an internal sink detail the engine tracks via the decision.
    [[nodiscard]] virtual AudioFormat currentFormat() const = 0;

    /// Hand the sink a line logger. Default no-op, so a sink that has nothing
    /// to say (NullSink) opts out by doing nothing; the platform sinks store the
    /// pointer and forward every diagnostic line through it in ADDITION to
    /// stderr, preserving the CLI's output byte-for-byte while giving an
    /// embedding application (via Engine::Listener::onInfo) the same lines.
    /// Non-owning; nullptr detaches. Called by the Engine at injection, before
    /// any open().
    virtual void setLogOutput(ILogOutput* out) { (void)out; }

    /// The device rate the sink MEASURED for the current open (the AUHAL
    /// output-scope rate on CoreAudio, the io position clock on PipeWire), in
    /// Hz, refreshed by the sink when the world moves it; or 0 when the sink
    /// has no measurement (not open, or a sink that cannot measure; the
    /// default). The measured value is the authority over the RateDecision's
    /// prediction, because a device can accept a rate set and still misclock;
    /// the Engine publishes measurement-first with prediction fallback for the
    /// UI's bit-perfect reporting. Engine-thread query, not on the RT path.
    [[nodiscard]] virtual std::uint32_t measuredDeviceRateHz() const { return 0; }
};

}  // namespace rawform::audio
