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

// CoreAudioSink.h
//
// IAudioSink backed by the macOS AUHAL default-output AudioUnit: the macOS leg
// of the engine, one of the two platform sinks (PipeWireSink is the other)
// living behind the same portable interface. Everything above IAudioSink stays
// portable; this file and its .cpp are the only translation units in the
// project that know CoreAudio exists.
//
// Dependency containment, same spirit as the decoders: no CoreAudio header
// appears here. All of <AudioUnit/...>, <CoreAudio/...>, the AudioUnit handle,
// the AudioDeviceID, the property listeners, and the render callback live
// behind a forward-declared implementation struct defined only in the .cpp, so
// consumers include this header without inheriting the CoreAudio include path.
//
// The implementation struct is forward-declared at NAMESPACE scope
// (CoreAudioSinkImpl) rather than as a private nested type. The reason is the
// C-style AURenderCallback and the AudioObject property listeners: they are
// free functions that must reach the implementation state through a void*
// refcon, and a free function cannot name a private nested type without a
// friend dance. A namespace-scope struct keeps the callbacks clean and the
// access rules simple.
//
// Construction is split from open() because the IAudioSink contract puts the
// real work in open(): a default-constructed CoreAudioSink is an empty, closed
// shell, and open() returns false rather than throwing on any device failure.
//
// Division of rate responsibilities: the portable RateManager DECIDES whether
// and to what rate the device should switch; the Engine hands the result to
// open() or reconfigure() as a RateDecision; this sink EXECUTES it and never
// re-derives it. What stays here is inherently device-side: capabilities()
// reads what the target device can do, bitPerfect() reports the MEASURED
// outcome after negotiation (a device can accept a rate set and still misclock,
// and only the readback knows), and the restore of a borrowed nominal rate is
// device mechanics owned by this file.
//
// The session model: open() acquires the device and starts a rate ledger
// (originalRate at that moment); reconfigure() retunes the same unit across
// track boundaries without disposing it and without a mid-session restore, so
// the ledger accumulates; the final close() repays it once. That is one
// nominal-rate transition per boundary instead of two, which is what keeps
// the built-in audio controller's cross-family clock churn (the AppleMCA2
// panic) out of the picture. The ledger is published through the event
// channel so the owner can persist it and repayStaleRateDebt() can settle it
// after a crash.
//
// The device surface: enumerateDevices() lists every output-capable device by
// UID, and selectDevice() pins one; the pin redirects what capabilities(),
// open(), and reconfigure()'s device check resolve, and the engine drives the
// reopen that makes a change audible. System-wide listeners report
// default-device and device-list changes, and a per-open listener on the bound
// device reports external nominal-rate changes, which forgive the ledger.

#pragma once

#include "rawform/audio/IAudioSink.h"
#include "rawform/audio/RateManager.h"
#include "rawform/audio/Types.h"

#include <memory>

namespace rawform::audio {

/// Defined in the .cpp; holds the AudioUnit, the device id, the non-owning
/// source pointer, and the negotiated format.
struct CoreAudioSinkImpl;

class CoreAudioSink final : public IAudioSink {
public:
    CoreAudioSink();
    ~CoreAudioSink() override;  ///< closes the device if still open

    /// Not copyable or movable: it owns an AudioUnit and is referenced by a live
    /// render callback through a raw this/impl pointer.
    CoreAudioSink(const CoreAudioSink&)            = delete;
    CoreAudioSink& operator=(const CoreAudioSink&) = delete;
    CoreAudioSink(CoreAudioSink&&)                 = delete;
    CoreAudioSink& operator=(CoreAudioSink&&)      = delete;

    /// IAudioSink. capabilities() reads the target output device's advertised
    /// nominal-rate ranges, its current rate, whether a rate switch is settable,
    /// and its name, all fresh; the target is the device pinned by
    /// selectDevice(), else the system default. It is independent of open() so
    /// the Engine can consult the rate policy before committing. open()
    /// resolves the same target, executes `decision` (switching the device
    /// nominal rate to decision.deviceRate iff decision.switchDevice, repaid at
    /// the session's final close), configures an interleaved float32 AUHAL at
    /// the SOURCE rate, and records the honest measured bit-perfect outcome.
    /// Returns false on any CoreAudio failure with a message on stderr.
    /// start()/stop() run and park the HAL IO thread; AudioOutputUnitStop in
    /// stop() is the synchronization point the lifetime invariant relies on.
    [[nodiscard]] SinkCapabilities capabilities() const override;
    /// enumerateDevices lists every output-capable device, UID-keyed
    /// (the persistent id AudioDeviceInfo promises); selectDevice pins by UID,
    /// refusing ids that do not currently resolve. The pin redirects what
    /// capabilities(), open(), and reconfigure()'s device check resolve; the
    /// engine drives the audible reopen.
    [[nodiscard]] std::vector<AudioDeviceInfo> enumerateDevices() const override;
    bool                         selectDevice(const std::string& deviceId) override;
    /// Install the engine's event channel. This sink emits all four
    /// events: default-device and device-list changes from system-wide
    /// listeners alive for the sink's life, external nominal-rate changes
    /// (with ledger forgiveness) from a per-open listener on the bound
    /// device, and restore-ledger changes for the ledger persistence.
    void                         setEventListener(ISinkEventListener* listener) override;
    /// The crash-recovery restore; resolves by UID and repays iff the
    /// device still sits at the borrowed rate. Pre-injection, pre-open.
    bool                         repayStaleRateDebt(const std::string& deviceId,
                                                    std::uint32_t originalRateHz,
                                                    std::uint32_t borrowedRateHz) override;
    bool             open(const AudioFormat&  sourceFormat,
                          const RateDecision& decision,
                          IPullSource*        source) override;
    /// Retune the ALREADY-OPEN unit for a new source format and decision
    /// without disposing it: uninitialize, move the device nominal rate iff the
    /// decision asks (accumulating the session's rate debt, no mid-session
    /// restore), set the input-scope format, re-initialize, re-measure. Declines
    /// (false, nothing touched) when the target device (pinned, else default)
    /// no longer resolves to the device this unit is bound to, so the old
    /// device gets its restore through the engine's close+open fallback. A
    /// CoreAudio failure mid-retune also returns false; the engine then closes
    /// and re-opens per the IAudioSink contract.
    bool             reconfigure(const AudioFormat&  sourceFormat,
                                 const RateDecision& decision) override;
    void             start() override;
    void             stop()  override;
    void             close() override;
    [[nodiscard]] AudioFormat currentFormat() const override;

    /// The measured-outcome diagnostic: true when the hardware reached
    /// the exact source sample rate (the AUHAL output scope clocks at the source
    /// rate). False means the device is resampling, whether the policy planned it
    /// (resampleNeeded) or a forced switch did not truly take. This MEASURED value
    /// overrides the RateDecision's PREDICTED resampleNeeded for "was it really
    /// bit-perfect" reporting.
    [[nodiscard]] bool bitPerfect() const noexcept;

    /// The sink-console seams. setLogOutput stores the non-owning line logger;
    /// every diagnostic this sink prints (the decision-execution lines, the
    /// measured-outcome line, the close-time restore, and the open-path OSStatus
    /// breadcrumbs) then goes to BOTH stderr (the pre-existing CLI behavior,
    /// unchanged) and the logger. measuredDeviceRateHz reports the AUHAL
    /// output-scope rate captured after the last successful open, rounded to the
    /// nearest Hz, or 0 while not open; it is the same measurement bitPerfect()
    /// is derived from, exposed portably for the Engine's outcome publication.
    void          setLogOutput(ILogOutput* out) override;
    [[nodiscard]] std::uint32_t measuredDeviceRateHz() const override;

private:
    std::unique_ptr<CoreAudioSinkImpl> m_impl;
};

}  // namespace rawform::audio
