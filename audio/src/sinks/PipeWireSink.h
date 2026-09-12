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

// PipeWireSink.h
//
// IAudioSink backed by a PipeWire playback stream: the Linux leg of the engine,
// one of the two platform sinks (CoreAudioSink is the other) living behind the
// same portable interface. Everything above IAudioSink stays portable; this
// file and its .cpp are the only translation units in the project that know
// PipeWire exists.
//
// Dependency containment, same spirit as CoreAudioSink: no PipeWire header
// appears here. The thread loop, the context, the core connection, the stream,
// the registry and metadata proxies, and the process callback all live behind a
// forward-declared implementation struct defined only in the .cpp, so consumers
// include this header without inheriting the PipeWire include path.
//
// The pimpl is forward-declared at NAMESPACE scope (PipeWireSinkImpl) rather
// than as a private nested type, for the same reason CoreAudioSinkImpl is: the
// PipeWire event tables are C-style free-function callbacks that reach the
// implementation state through a void* data pointer, and a free function cannot
// name a private nested type without a friend dance.
//
// Threading model, mapped onto the IAudioSink contract:
//   - The pw_thread_loop owns the loop thread; the stream's process callback
//     runs there and is the real-time consumer that calls IPullSource::pull().
//   - Every engine-thread entry point below (capabilities/enumerateDevices/
//     selectDevice/open/reconfigure/start/stop/close) takes the thread-loop
//     lock around PipeWire calls, which is the library's own required
//     discipline AND the join guarantee stop() needs: holding the lock excludes
//     the loop thread from any callback, so a flag flipped under it is observed
//     by every subsequent process invocation.
//
// The rate model: this sink can MOVE the graph. capabilities() resolves the
// target sink node (the pinned one, else the session manager's default,
// through the "default" metadata and the registry's node inventory) and
// reports the rate spans the hardware genuinely advertises; a switching
// decision rides on the stream as node.rate plus node.force-rate, the latter
// reclocking the graph regardless of clock.allowed-rates, so bit-perfect works
// on a stock PipeWire config. The force lives and dies with the stream, which
// makes close() the repayment by construction: there is no rate ledger on this
// backend. The realized rate is confirmed after negotiation and measured live
// thereafter (measuredDeviceRateHz), so every published outcome is the graph's
// truth.
//
// The session model: open() connects the stream live but disarmed;
// reconfigure() renegotiates the same stream in place across a track boundary
// (format and rate force) and ends in exactly the state open() ends in, so
// start() behaves identically after either; any unconfirmed renegotiation or
// reclock declines and the engine falls back to close()+open(), which is
// always safe here because only the stream dies, never the daemon session.
//
// The device surface: enumerateDevices() lists the live Audio/Sink nodes by
// node.name with the current default marked; selectDevice() pins one, which
// redirects capabilities() and targets the stream at the next open. The sink
// reports device-list changes and external clock transitions through the
// event channel; it deliberately does not report default-device changes (the
// session manager migrates live streams itself) or rate debt (repayment is
// structural).

#pragma once

#include "rawform/audio/IAudioSink.h"
#include "rawform/audio/RateManager.h"
#include "rawform/audio/Types.h"

#include <memory>

namespace rawform::audio {

/// Defined in the .cpp; holds the thread loop, context, core, registry and
/// metadata proxies, the stream, the non-owning source pointer, and the
/// negotiated format.
struct PipeWireSinkImpl;

class PipeWireSink final : public IAudioSink {
public:
    /// Connects to the PipeWire daemon and begins tracking the graph's clock
    /// settings, the default output device, and the live inventory of sink
    /// nodes (persistent, cheap registry/metadata subscriptions), so
    /// capabilities() has real hardware facts before any open(). A machine
    /// with no reachable daemon degrades gracefully: construction succeeds,
    /// capabilities() reports an unusable device, and open() fails with a
    /// logged message, mirroring CoreAudioSink's no-default-device behavior.
    PipeWireSink();
    ~PipeWireSink() override;  ///< closes the stream and drops the daemon connection

    /// Not copyable or movable: it owns a thread loop and is referenced by live
    /// C callbacks through a raw impl pointer.
    PipeWireSink(const PipeWireSink&)            = delete;
    PipeWireSink& operator=(const PipeWireSink&) = delete;
    PipeWireSink(PipeWireSink&&)                 = delete;
    PipeWireSink& operator=(PipeWireSink&&)      = delete;

    /// IAudioSink. capabilities() resolves the target sink node (the pinned
    /// node, else the default) and reports its advertised rate spans with
    /// canSwitchRate true (node.force-rate reaches any of them), the LIVE graph
    /// clock as currentRate (the measurement while open, since the settings
    /// metadata never reflects a node force; the metadata only when closed),
    /// and the node description as the device name; with no resolvable node it
    /// falls back to the follow-the-graph shape (single span, no switching).
    /// open() creates a playback stream at the SOURCE format (interleaved
    /// float32), carrying the decision's rate force when one was planned,
    /// connects it live but disarmed (no source pull before start(); a rate
    /// force only applies while the node runs), waits for negotiation, and
    /// confirms the realized
    /// graph rate. start()/stop() activate and deactivate the stream; stop()'s
    /// thread-loop lock discipline is the synchronization point the lifetime
    /// invariant relies on. close() destroys the stream, which lifts any rate
    /// force with it: the repayment is structural, not bookkept.
    [[nodiscard]] SinkCapabilities capabilities() const override;
    /// enumerateDevices lists the live Audio/Sink nodes, keyed by
    /// node.name (the persistent id AudioDeviceInfo promises), with the
    /// session manager's current default marked; selectDevice pins by that
    /// name, refusing names absent from the inventory. The pin redirects what
    /// capabilities() resolves and targets the stream (PW_KEY_TARGET_OBJECT)
    /// at the next open; the engine drives the audible reopen.
    [[nodiscard]] std::vector<AudioDeviceInfo> enumerateDevices() const override;
    bool selectDevice(const std::string& deviceId) override;
    /// Install the engine's event channel. This sink emits
    /// onDeviceListChanged (node inventory or default-marking changed) and
    /// onExternalRateChanged (settings clock transitions, with the cleared-
    /// force return rate inferred, since the metadata cannot see node forces;
    /// and a target-node driver format diverging from or re-
    /// converging with the graph clock, the last-mile verification);
    /// deliberately never onDefaultDeviceChanged (the session manager migrates
    /// live streams itself) nor onRateDebtChanged (repayment is structural).
    void                         setEventListener(ISinkEventListener* listener) override;
    bool             open(const AudioFormat&  sourceFormat,
                          const RateDecision& decision,
                          IPullSource*        source) override;
    /// Retune the LIVE stream for a new source format and decision
    /// without destroying it: renegotiate the stream format in place
    /// (pw_stream_update_params), swap the rate force on the node
    /// (pw_stream_update_properties), reactivate disarmed, and confirm both the
    /// negotiated format and the realized graph rate against reality, bounded.
    /// Ends in exactly the state open() ends in (live, disarmed, measured), so
    /// start() behaves identically after either. Any doubt (renegotiation or
    /// reclock unconfirmed) returns false and the engine's close+open fallback
    /// runs, which is always correct here since the daemon session survives a
    /// stream close.
    bool reconfigure(const AudioFormat&  sourceFormat,
                     const RateDecision& decision) override;
    void start() override;
    void stop()  override;
    void close() override;
    [[nodiscard]] AudioFormat currentFormat() const override;

    /// The measured counterpart to CoreAudioSink::bitPerfect(), same semantics:
    /// true when the graph is clocking exactly the source sample rate AND
    /// the target node's realized driver format agrees with the graph, per
    /// the driver-format watch below. False means PipeWire is resampling,
    /// either in the graph or on the driver's last mile. The CLI's Output line
    /// reads this through the platform-sink alias.
    [[nodiscard]] bool bitPerfect() const noexcept;

    /// Sink-console seams, mirroring CoreAudioSink. setLogOutput stores the
    /// non-owning line logger; every diagnostic this sink prints goes to BOTH
    /// stderr and the logger. measuredDeviceRateHz reports the graph rate the
    /// stream is actually clocked against: seeded from the settings metadata at
    /// open, then refreshed live from the io position clock by the process
    /// callback, so a mid-session graph rate change is reflected. 0 while not
    /// open. The GRAPH rate deliberately remains this method's meaning; the
    /// driver-format watch verifies the last mile separately, by
    /// certifying a diverging driver rate to the engine through
    /// onExternalRateChanged, whose republication is event-rate-first for
    /// exactly that reason.
    void setLogOutput(ILogOutput* out) override;
    [[nodiscard]] std::uint32_t measuredDeviceRateHz() const override;

private:
    std::unique_ptr<PipeWireSinkImpl> m_impl;
};

}  // namespace rawform::audio
