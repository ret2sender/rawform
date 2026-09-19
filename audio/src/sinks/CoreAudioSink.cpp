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

// CoreAudioSink.cpp
//
// macOS AUHAL implementation of IAudioSink. This is the only translation unit
// that touches CoreAudio; CMake compiles it on Apple only, and the tripwire
// below makes that explicit if it is ever fed to a non-Apple compiler.
//
// Executing the rate decision. The portable RateManager decides and the Engine
// passes a RateDecision down; this file executes it and reports what actually
// happened:
//
//   - capabilities() reports what the TARGET device (the pinned one, else the
//     system default) can do: advertised nominal-rate ranges, current rate,
//     whether the rate is settable, and the device name for logging, read
//     fresh. The Engine calls it before every open() and reconfigure() to feed
//     the RateManager; a gapless stitch makes no call.
//
//   - open() switches the device nominal rate to decision.deviceRate iff
//     decision.switchDevice, then configures an interleaved float32 AUHAL at
//     the SOURCE rate on the input scope, so if the device ends up clocking a
//     different rate the AUHAL resamples to it.
//
//   - reconfigure() is the track-to-track transition on an ALREADY-OPEN unit:
//     uninitialize, move the nominal rate iff the decision asks, set the
//     input-scope format for the new source, re-initialize, re-measure. No
//     dispose/new, no mid-session restore. It declines (false, nothing
//     touched) when the resolved device changed underneath, and returns false
//     on any CoreAudio failure; either way the Engine falls back to
//     close()+open(), and close() copes with a partly-retuned unit.
//
//   - bitPerfect() reports the honest MEASURED outcome from the AUHAL output
//     scope (the rate the unit actually clocks at). This is the authority on
//     whether playback is truly bit-perfect, overriding the decision's
//     prediction, because a device can accept a rate set, report success at
//     every layer, then misclock with no programmatic signal. open() and
//     reconfigure() log the device, the executed decision, and the measured
//     outcome so the result is visible rather than inferred.
//
// The rate ledger. Leaving a device at a rate its hardware cannot faithfully
// clock corrupts all system audio, so a borrowed rate is a debt. The ledger
// (originalRate, rateChanged) starts at open(), accumulates across
// reconfigure() calls, and is repaid once by the final close(). It is
// published on change through ISinkEventListener::onRateDebtChanged so the
// owner can persist it, and repayStaleRateDebt() settles a persisted debt from
// a crashed session before the sink is ever opened. An external nominal-rate
// change on the bound device (the user or another program asserting
// ownership) FORGIVES the debt: the asserted rate becomes the new baseline,
// and the measurement follows. Our own sets are told apart from external ones
// by a marker the initiating call plants before it changes the rate.
//
// Devices and events. The sink resolves its target device through a pin set by
// selectDevice(), or the system default when no pin is set. Two system-wide
// property listeners live for the sink's whole life and report default-device
// and device-list changes to the engine; a third listener is installed per
// open() on the bound device for the nominal-rate watch above. All listener
// callbacks arrive on CoreAudio's own thread and hand off through the
// implementation's mutex, never touching the render path.
//
// stop() calls AudioOutputUnitStop, which is synchronous: after it returns the
// HAL IO thread will not invoke the callback again. THAT is the
// real-time-thread join the IAudioSink lifetime invariant depends on; only
// after it is it safe to reconfigure the unit or destroy the IPullSource.

#include "sinks/CoreAudioSink.h"

#ifndef __APPLE__
#error "CoreAudioSink.cpp is macOS-only; CMake should compile it under if(APPLE) only."
#endif

#include "rawform/audio/IAudioSink.h"
#include "rawform/audio/RateManager.h"
#include "rawform/audio/Types.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <cmath>  // NOLINT: std::fabs, std::lround, etc.
#include <condition_variable>
#include <cstdarg>  // sinkLog's va_list formatting
#include <cstddef>
#include <cstdint>  // NOLINT: std::uintN_t
#include <cstdio>
#include <cstring>  // NOLINT: std::memset in renderCallback
#include <mutex>
#include <string>
#include <vector>

// kAudioObjectPropertyElementMain is the modern (macOS 12+) name for the
// element that older SDKs called kAudioObjectPropertyElementMaster. Supply the
// modern name only when building against a pre-12 SDK that lacks it, gated on
// the SDK version rather than on #ifndef: on SDKs that DO have it the name is
// an enum CONSTANT, not a preprocessor macro, so an #ifndef would always be
// true and would alias the modern name back onto the deprecated one on every
// build. The two constants share the same value, so the fallback is a spelling
// difference with no behavioral change.
#include <AvailabilityMacros.h>
#if !defined(MAC_OS_VERSION_12_0) \
    || (MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_VERSION_12_0)
#ifndef kAudioObjectPropertyElementMain
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif
#endif

namespace rawform::audio {

// ---------------------------------------------------------------------------
// Implementation state. Namespace-scope (not nested) so the free render
// callback can reach it through its refcon. source is non-owning; the lifetime
// invariant guarantees the source outlives every callback.
struct CoreAudioSinkImpl {
    AudioUnit     unit         = nullptr;
    AudioDeviceID device       = kAudioObjectUnknown;
    IPullSource*  source       = nullptr;  // non-owning
    std::size_t   channels     = 0;
    AudioFormat   working{};               // the SOURCE format we pull at
    // deviceRate and bitPerfect are ATOMIC because of the event machinery: the
    // nominal-rate property listener updates them from CoreAudio's notification
    // thread when the world changes the rate underneath us, while bitPerfect() and
    // measuredDeviceRateHz() read them lock-free (and noexcept) on the engine
    // thread.
    std::atomic<double> deviceRate{0.0}; // AUHAL output-scope rate, measured
    std::atomic<bool>   bitPerfect{false}; // measured: output scope == source rate
    double        originalRate = 0.0; // device nominal rate at SESSION open, for restore
    bool          rateChanged  = false; // does the device sit off originalRate?
    bool          opened       = false;
    bool          started      = false;
    ILogOutput*   logOut       = nullptr; // non-owning line logger

    // The pinned output device by persistent UID; empty = follow the
    // system default. Set by selectDevice() and read by every device
    // resolution below. Engine-thread only, like all non-RT state here.
    std::string   targetDeviceUid;

    // ----- sink event machinery ---------------------------------------------
    // stateMtx guards what BOTH the engine thread and CoreAudio's notification
    // threads touch: the restore ledger (originalRate, rateChanged), the
    // working format the forgiveness callback compares against, the
    // self-change marker, the installed event listener, and the last debt
    // shape published. Every guarded section is short and allocation-light;
    // the RT render path never touches any of it.
    std::mutex          stateMtx;
    ISinkEventListener* events             = nullptr;
    bool                hasPendingSelfRate = false;  // our own rate set in flight
    double              pendingSelfRate    = 0.0;
    bool                systemListeners    = false;  // ctor-installed pair
    bool                deviceListener     = false;  // per-open nominal-rate listener
    std::string         deviceUidStr;                // bound device's UID, for debt events
    bool                lastDebtActive     = false;  // last published debt shape
    std::uint32_t       lastDebtBorrowed   = 0;
};

namespace {

// The single diagnostic funnel: format once, then write the line to BOTH
// stderr (the CLI's byte-for-byte output; the trailing newline is added here,
// so format strings carry none) and the installed ILogOutput when present.
// Engine-thread only, like every log site in this file (open/close paths);
// never the render callback.
__attribute__((format(printf, 2, 3)))
void sinkLog(ILogOutput* out, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::fprintf(stderr, "%s\n", buf);
    if (out != nullptr) {
        out->logLine(std::string(buf));
    }
}

// The logger-aware OSStatus check, used from the instance methods (open/start/
// stop/close paths) so device-failure breadcrumbs reach the application
// console too. The logger-less overload below stays for the free device-query
// helpers, which have no instance context; those breadcrumbs remain
// stderr-only, and any failure that matters to playback surfaces through the
// instance paths (or the engine's own onError) regardless.
bool osOk(OSStatus status, const char* what, ILogOutput* out) {
    if (status != noErr) {
        sinkLog(out, "CoreAudioSink: %s failed (OSStatus %ld)", what,
                static_cast<long>(status));
        return false;
    }
    return true;
}

bool osOk(OSStatus status, const char* what) {
    return osOk(status, what, nullptr);
}

bool ratesEqual(double a, double b) noexcept {
    return std::fabs(a - b) < 1.0;  // sample rates are integers in practice
}

// Resolve the current default output device, or kAudioObjectUnknown on failure.
AudioDeviceID defaultOutputDevice() {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDefaultOutputDevice,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioDeviceID dev  = kAudioObjectUnknown;
    UInt32        size = sizeof(dev);
    const OSStatus s =
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                   &size, &dev);
    if (s != noErr) {
        osOk(s, "get DefaultOutputDevice");
        return kAudioObjectUnknown;
    }
    return dev;
}

// Read a device's current nominal sample rate, or 0.0 on failure.
double deviceNominalRate(AudioDeviceID dev) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 rate = 0.0;
    UInt32  size = sizeof(rate);
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &rate) != noErr) {
        return 0.0;
    }
    return static_cast<double>(rate);
}

// Whether the device's nominal sample rate is settable at all. A device that is
// not settable cannot be switched (some aggregate and locked devices), which the
// policy needs to know so it does not plan a switch that cannot happen.
bool deviceRateSettable(AudioDeviceID dev) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Boolean settable = false;
    if (AudioObjectIsPropertySettable(dev, &addr, &settable) != noErr) {
        return false;
    }
    return settable == true;
}

// Read the AUHAL's stream-format sample rate for a given scope. The output scope
// of element 0 is the device side: the rate the unit will actually clock at.
double auhalScopeRate(AudioUnit unit, AudioUnitScope scope) {
    AudioStreamBasicDescription fmt{};
    UInt32 size = sizeof(fmt);
    if (AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, scope, 0, &fmt,
                             &size) != noErr) {
        return 0.0;
    }
    return fmt.mSampleRate;
}

// Fetch the device's advertised nominal sample-rate ranges. Empty on failure.
std::vector<AudioValueRange> availableRates(AudioDeviceID dev) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyAvailableNominalSampleRates,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr ||
        size == 0) {
        return {};
    }
    std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, ranges.data()) !=
        noErr) {
        return {};
    }
    return ranges;
}

// The device's persistent identity: the UID string survives reboots and
// re-enumeration, which is exactly what AudioDeviceInfo::id promises and what
// any remembered selection above the engine must be keyed by. Empty on
// failure; callers skip devices they cannot identify.
std::string deviceUid(AudioDeviceID dev) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyDeviceUID,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef cf   = nullptr;
    UInt32      size = sizeof(cf);  // the property's payload is the ref itself
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &cf) != noErr ||
        cf == nullptr) {
        return {};
    }
    char buf[256] = {0};
    const bool converted =
        CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cf);
    return converted ? std::string(buf) : std::string();
}

// Every device with at least one OUTPUT stream: the hardware list
// behind enumerateDevices() and the UID resolution. Input-only devices
// (microphones) are filtered out by the output-scope stream check.
std::vector<AudioDeviceID> outputDeviceIds() {
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0,
                                       nullptr, &size) != noErr ||
        size == 0) {
        return {};
    }
    std::vector<AudioDeviceID> all(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                   &size, all.data()) != noErr) {
        return {};
    }
    std::vector<AudioDeviceID> out;
    for (const AudioDeviceID dev : all) {
        AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams,
                                           kAudioObjectPropertyScopeOutput,
                                           kAudioObjectPropertyElementMain};
        UInt32 streamsSize = 0;
        if (AudioObjectGetPropertyDataSize(dev, &streams, 0, nullptr,
                                           &streamsSize) == noErr &&
            streamsSize > 0) {
            out.push_back(dev);
        }
    }
    return out;
}

// UID -> live device id, or kAudioObjectUnknown when nothing currently
// enumerates under that UID (unplugged, or a stale remembered selection).
AudioDeviceID deviceByUid(const std::string& uid) {
    for (const AudioDeviceID dev : outputDeviceIds()) {
        if (deviceUid(dev) == uid) {
            return dev;
        }
    }
    return kAudioObjectUnknown;
}

// The one resolution rule, shared by capabilities(), open(), and
// reconfigure()'s device pinning: an empty pin follows the system default; a
// non-empty pin resolves by UID or, honestly, fails.
AudioDeviceID resolveTargetDevice(const std::string& targetUid) {
    if (targetUid.empty()) {
        return defaultOutputDevice();
    }
    return deviceByUid(targetUid);
}

// The device's human-readable name, for unambiguous logging across devices.
std::string deviceName(AudioDeviceID dev) {
    AudioObjectPropertyAddress addr{kAudioObjectPropertyName,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    CFStringRef cf   = nullptr;
    UInt32      size = sizeof(cf);  // the property's payload is the ref itself
    if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &cf) != noErr ||
        cf == nullptr) {
        return "unknown device";
    }
    char buf[256] = {0};
    const bool converted =
        CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(cf);
    return converted ? std::string(buf) : std::string("unknown device");
}

// One-shot rendezvous so we can wait for nominal-rate change notifications.
struct RateChangeWaiter {
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    changed = false;
};

OSStatus onNominalRateChanged(AudioObjectID /*obj*/, UInt32 /*nAddrs*/,
                              const AudioObjectPropertyAddress* /*addrs*/,
                              void* ctx) {
    auto* w = static_cast<RateChangeWaiter*>(ctx);
    {
        std::lock_guard<std::mutex> lock(w->mtx);
        w->changed = true;
    }
    w->cv.notify_all();
    return noErr;
}

// Drive the device to targetRate and wait until the readback actually reaches it,
// bounded by a 2 s ceiling. Writes the rate the device ended up at to *achieved.
// Returns true only when the device genuinely reached targetRate. Runs only in
// open()/close(), never on the RT thread.
bool setDeviceNominalRate(AudioDeviceID dev, double targetRate, double* achieved) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyNominalSampleRate,
                                    kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};

    const double current = deviceNominalRate(dev);
    if (current == 0.0) {
        *achieved = 0.0;
        return false;
    }
    if (ratesEqual(current, targetRate)) {
        *achieved = current;
        return true;
    }

    RateChangeWaiter waiter;
    AudioObjectAddPropertyListener(dev, &addr, &onNominalRateChanged, &waiter);

    const auto target = static_cast<Float64>(targetRate);
    if (!osOk(AudioObjectSetPropertyData(dev, &addr, 0, nullptr, sizeof(target),
                                         &target),
              "set NominalSampleRate")) {
        AudioObjectRemovePropertyListener(dev, &addr, &onNominalRateChanged, &waiter);
        *achieved = current;
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(waiter.mtx);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            if (ratesEqual(deviceNominalRate(dev), targetRate)) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            waiter.changed = false;
            waiter.cv.wait_until(lock, deadline, [&waiter] { return waiter.changed; });
        }
    }

    AudioObjectRemovePropertyListener(dev, &addr, &onNominalRateChanged, &waiter);

    const double settled = deviceNominalRate(dev);
    *achieved = (settled != 0.0 ? settled : current);
    return ratesEqual(*achieved, targetRate);
}

// The AUHAL render callback: the consumer / real-time thread. Pulls from the
// non-owning source into the single interleaved buffer and zero-pads any
// shortfall. Must not throw, lock, or allocate; it does none of those.
OSStatus renderCallback(void* inRefCon, AudioUnitRenderActionFlags* /*flags*/,
                        const AudioTimeStamp* /*ts*/, UInt32 /*bus*/,
                        UInt32 inNumberFrames, AudioBufferList* ioData) noexcept {
    auto* impl = static_cast<CoreAudioSinkImpl*>(inRefCon);

    if (ioData == nullptr || ioData->mNumberBuffers == 0) {
        return noErr;
    }
    auto* out = static_cast<float*>(ioData->mBuffers[0].mData);
    if (out == nullptr) {
        return noErr;
    }

    const auto frames = static_cast<std::size_t>(inNumberFrames);
    const std::size_t ch = impl->channels;

    std::size_t got = 0;
    if (impl->source != nullptr) {
        got = impl->source->pull(out, frames);
    }
    if (got < frames) {
        std::memset(out + got * ch, 0, (frames - got) * ch * sizeof(float));
    }
    return noErr;
}

}  // namespace

// ---------------------------------------------------------------------------
namespace {

// Property-listener procs and the debt publication helper. The procs
// run on CoreAudio's internal notification threads: everything they touch is
// stateMtx-guarded or atomic, the event-listener calls are enqueues by
// contract, and diagnostics go to stderr ONLY (the ILogOutput funnel is an
// engine-thread facility; the engine's own reaction prints the console line).

OSStatus onSystemObjectEvent(AudioObjectID /*objectId*/, UInt32 nAddrs,
                             const AudioObjectPropertyAddress* addrs,
                             void* clientData) {
    auto* impl = static_cast<CoreAudioSinkImpl*>(clientData);
    bool  defChanged  = false;
    bool  listChanged = false;
    for (UInt32 i = 0; i < nAddrs; ++i) {
        if (addrs[i].mSelector == kAudioHardwarePropertyDefaultOutputDevice) {
            defChanged = true;
        } else if (addrs[i].mSelector == kAudioHardwarePropertyDevices) {
            listChanged = true;
        }
    }
    ISinkEventListener* l = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl->stateMtx);
        l = impl->events;
    }
    if (l != nullptr) {
        if (defChanged) {
            l->onDefaultDeviceChanged();
        }
        if (listChanged) {
            l->onDeviceListChanged();
        }
    }
    return noErr;
}

// The forgiveness site: a nominal-rate change on the bound device that WE
// did not initiate is the user (or another program) asserting ownership. The
// asserted rate becomes the session baseline: the debt is gone, the final
// close restores nothing, and the measurement follows so the engine's
// republication tells the truth. Our own sets are recognized by the marker the
// initiating call planted; a marker mismatch means external. (A late-arriving
// self notification after the marker was withdrawn would be misread as
// external; the consequence is a forgiven debt, i.e. no restore of a rate the
// user has since seen the device reach: benign, and documented rather than
// chased.)
OSStatus onDeviceRateEvent(AudioObjectID objectId, UInt32 /*nAddrs*/,
                           const AudioObjectPropertyAddress* /*addrs*/,
                           void* clientData) {
    auto* impl = static_cast<CoreAudioSinkImpl*>(clientData);
    const double newRate =
        deviceNominalRate(static_cast<AudioDeviceID>(objectId));
    if (newRate <= 0.0) {
        return noErr;
    }
    ISinkEventListener* l        = nullptr;
    bool                external = false;
    {
        std::lock_guard<std::mutex> lock(impl->stateMtx);
        if (impl->hasPendingSelfRate &&
            ratesEqual(newRate, impl->pendingSelfRate)) {
            impl->hasPendingSelfRate = false;  // our own set landing; no event
        } else {
            external           = true;
            impl->originalRate = newRate;
            impl->rateChanged  = false;
            impl->deviceRate.store(newRate, std::memory_order_relaxed);
            impl->bitPerfect.store(
                ratesEqual(newRate,
                           static_cast<double>(impl->working.sampleRate)),
                std::memory_order_relaxed);
            impl->lastDebtActive   = false;
            impl->lastDebtBorrowed = 0;
            l = impl->events;
        }
    }
    if (external) {
        std::fprintf(stderr,
                     "CoreAudioSink: external device rate change to %.0f Hz "
                     "(rate debt forgiven)\n",
                     newRate);
        if (l != nullptr) {
            l->onExternalRateChanged(
                static_cast<std::uint32_t>(std::lround(newRate)));
            l->onRateDebtChanged(std::string{}, 0, 0);  // cleared
        }
    }
    return noErr;
}

// Publish the ledger's shape when it CHANGED since the last publication (the
// persistence feed). Engine thread; takes and releases stateMtx itself.
void publishDebt(CoreAudioSinkImpl* impl) {
    ISinkEventListener* l = nullptr;
    bool                active   = false;
    std::string         uid;
    std::uint32_t       original = 0;
    std::uint32_t       borrowed = 0;
    {
        std::lock_guard<std::mutex> lock(impl->stateMtx);
        active = impl->rateChanged && impl->device != kAudioObjectUnknown &&
                 impl->originalRate > 0.0;
        borrowed = active ? static_cast<std::uint32_t>(std::lround(
                                impl->deviceRate.load(std::memory_order_relaxed)))
                          : 0;
        if (active == impl->lastDebtActive &&
            borrowed == impl->lastDebtBorrowed) {
            return;  // same shape; nothing to say
        }
        impl->lastDebtActive   = active;
        impl->lastDebtBorrowed = borrowed;
        original = static_cast<std::uint32_t>(std::lround(impl->originalRate));
        uid      = impl->deviceUidStr;
        l        = impl->events;
    }
    if (l == nullptr) {
        return;
    }
    if (active) {
        l->onRateDebtChanged(uid, original, borrowed);
    } else {
        l->onRateDebtChanged(std::string{}, 0, 0);
    }
}

}  // namespace

CoreAudioSink::CoreAudioSink()
    : m_impl(std::make_unique<CoreAudioSinkImpl>()) {
    // System-wide ears, alive for the sink's whole life so pickers can
    // refresh even while Stopped. Registration failure is logged and degrades
    // to deafness to external changes, never fatal.
    AudioObjectPropertyAddress defAddr{kAudioHardwarePropertyDefaultOutputDevice,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectPropertyAddress listAddr{kAudioHardwarePropertyDevices,
                                        kAudioObjectPropertyScopeGlobal,
                                        kAudioObjectPropertyElementMain};
    const OSStatus s1 = AudioObjectAddPropertyListener(
        kAudioObjectSystemObject, &defAddr, onSystemObjectEvent, m_impl.get());
    const OSStatus s2 = AudioObjectAddPropertyListener(
        kAudioObjectSystemObject, &listAddr, onSystemObjectEvent, m_impl.get());
    m_impl->systemListeners = (s1 == noErr && s2 == noErr);
    if (!m_impl->systemListeners) {
        std::fprintf(stderr,
                     "CoreAudioSink: system property listeners not installed "
                     "(%d/%d); device events disabled\n",
                     static_cast<int>(s1), static_cast<int>(s2));
    }
}

CoreAudioSink::~CoreAudioSink() {
    close();
    if (m_impl->systemListeners) {
        AudioObjectPropertyAddress defAddr{
            kAudioHardwarePropertyDefaultOutputDevice,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectPropertyAddress listAddr{kAudioHardwarePropertyDevices,
                                            kAudioObjectPropertyScopeGlobal,
                                            kAudioObjectPropertyElementMain};
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &defAddr,
                                          onSystemObjectEvent, m_impl.get());
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &listAddr,
                                          onSystemObjectEvent, m_impl.get());
        m_impl->systemListeners = false;
    }
}

// ---------------------------------------------------------------------------
// Read the target output device's capabilities (the pinned device, else the
// system default), fresh. Pure query: it touches no AUHAL and changes no
// device state, so it is safe to call before open() and at every rate
// decision. A failure to resolve the device yields an empty, non-switchable
// capability set, which the RateManager reads as "cannot switch, resample at
// the (unknown) current rate", the safe interpretation.
SinkCapabilities CoreAudioSink::capabilities() const {
    SinkCapabilities caps;

    const AudioDeviceID dev = resolveTargetDevice(m_impl->targetDeviceUid);
    if (dev == kAudioObjectUnknown) {
        caps.deviceName    = m_impl->targetDeviceUid.empty()
                                 ? "no default output device"
                                 : "selected output device not found";
        caps.currentRate   = 0;
        caps.canSwitchRate = false;
        return caps;
    }

    caps.deviceName    = deviceName(dev);
    caps.currentRate   = static_cast<std::uint32_t>(deviceNominalRate(dev));
    caps.canSwitchRate = deviceRateSettable(dev);

    const std::vector<AudioValueRange> ranges = availableRates(dev);
    caps.rates.reserve(ranges.size());
    for (const AudioValueRange& r : ranges) {
        caps.rates.push_back(
            RateRange{static_cast<std::uint32_t>(r.mMinimum),
                      static_cast<std::uint32_t>(r.mMaximum)});
    }
    return caps;
}

// ---------------------------------------------------------------------------
// The hardware list, UID-keyed. isDefault marks the CURRENT system
// default so a picker can label it; devices that fail to produce a UID are
// skipped rather than listed unaddressably.
std::vector<AudioDeviceInfo> CoreAudioSink::enumerateDevices() const {
    std::vector<AudioDeviceInfo> out;
    const AudioDeviceID          def = defaultOutputDevice();
    for (const AudioDeviceID dev : outputDeviceIds()) {
        AudioDeviceInfo info;
        info.id = deviceUid(dev);
        if (info.id.empty()) {
            continue;
        }
        info.name      = deviceName(dev);
        info.isDefault = (dev == def);
        out.push_back(std::move(info));
    }
    return out;
}

// Pin by UID (empty = follow the system default). A pin that does not
// currently resolve is REFUSED rather than stored: the engine surfaces the
// refusal as an error and the previous resolution stays in force, which is
// what a picker offering only live devices expects. Selection never touches
// the open unit; the engine drives the reopen boundary.
bool CoreAudioSink::selectDevice(const std::string& deviceId) {
    if (deviceId.empty()) {
        m_impl->targetDeviceUid.clear();
        return true;
    }
    if (deviceByUid(deviceId) == kAudioObjectUnknown) {
        sinkLog(m_impl->logOut,
                "CoreAudioSink: selectDevice: no output device with UID '%s'",
                deviceId.c_str());
        return false;
    }
    m_impl->targetDeviceUid = deviceId;
    return true;
}

bool CoreAudioSink::open(const AudioFormat& sourceFormat,
                         const RateDecision& decision, IPullSource* source) {
    if (!sourceFormat.isValid() || source == nullptr) {
        sinkLog(m_impl->logOut, "CoreAudioSink: open() called with an invalid "
                                "format or null source");
        return false;
    }
    if (m_impl->opened) {
        close();
    }

    m_impl->source   = source;
    m_impl->channels = static_cast<std::size_t>(sourceFormat.channels);
    {
        std::lock_guard<std::mutex> lock(m_impl->stateMtx);  // forgiveness reads working
        m_impl->working = sourceFormat;
    }

    // 1. Resolve the output device (the pinned device, or the system default),
    // remember its rate for restore.
    const AudioDeviceID dev = resolveTargetDevice(m_impl->targetDeviceUid);
    if (dev == kAudioObjectUnknown) {
        sinkLog(m_impl->logOut,
                m_impl->targetDeviceUid.empty()
                    ? "CoreAudioSink: no default output device"
                    : "CoreAudioSink: selected output device not found");
        m_impl->source = nullptr;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->stateMtx);
        m_impl->device       = dev;
        m_impl->originalRate = deviceNominalRate(dev);
        m_impl->rateChanged  = false;
        m_impl->deviceUidStr = deviceUid(dev);  // the debt events' persistent key
    }

    // 2. Create the AUHAL and bind it to the device.
    AudioComponentDescription desc{};
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
    if (comp == nullptr) {
        sinkLog(m_impl->logOut, "CoreAudioSink: no AUHAL output component");
        m_impl->source = nullptr;
        return false;
    }
    if (!osOk(AudioComponentInstanceNew(comp, &m_impl->unit),
              "AudioComponentInstanceNew", m_impl->logOut)) {
        m_impl->source = nullptr;
        return false;
    }
    if (!osOk(AudioUnitSetProperty(m_impl->unit, kAudioOutputUnitProperty_CurrentDevice,
                                   kAudioUnitScope_Global, 0, &dev, sizeof(dev)),
              "set CurrentDevice", m_impl->logOut)) {
        close();
        return false;
    }

    // 3. Execute the rate decision. The policy already decided; here we only act.
    // Switch the device nominal rate to the chosen rate iff the decision asks,
    // remembering that we changed it so close() can restore. No advertised-list
    // gate here: that reasoning is the RateManager's and is already folded into
    // decision.switchDevice / decision.deviceRate.
    if (decision.switchDevice) {
        double achieved = 0.0;
        const bool reached =
            setDeviceNominalRate(dev, static_cast<double>(decision.deviceRate),
                                 &achieved);
        {
            std::lock_guard<std::mutex> lock(m_impl->stateMtx);
            m_impl->rateChanged =
                reached && !ratesEqual(achieved, m_impl->originalRate);
        }
        sinkLog(m_impl->logOut,
                "CoreAudioSink: switch -> %u Hz %s (device now %.0f Hz)",
                decision.deviceRate, reached ? "reached" : "NOT reached",
                achieved);
    } else {
        sinkLog(m_impl->logOut,
                "CoreAudioSink: no device switch (target %u Hz, %s)",
                decision.deviceRate,
                decision.resampleNeeded ? "resample expected"
                                        : "already at rate");
    }

    // 4. Interleaved, packed float32 at the SOURCE rate on the input scope. If the
    // device clocks at a different rate the AUHAL resamples to it.
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate       = static_cast<Float64>(sourceFormat.sampleRate);
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mFramesPerPacket  = 1;
    asbd.mChannelsPerFrame = sourceFormat.channels;
    asbd.mBitsPerChannel   = 32;
    asbd.mBytesPerFrame    = sourceFormat.channels * static_cast<UInt32>(sizeof(float));
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;
    if (!osOk(AudioUnitSetProperty(m_impl->unit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input, 0, &asbd, sizeof(asbd)),
              "set StreamFormat", m_impl->logOut)) {
        close();
        return false;
    }

    AURenderCallbackStruct cb{};
    cb.inputProc       = &renderCallback;
    cb.inputProcRefCon = m_impl.get();
    if (!osOk(AudioUnitSetProperty(m_impl->unit, kAudioUnitProperty_SetRenderCallback,
                                   kAudioUnitScope_Input, 0, &cb, sizeof(cb)),
              "set RenderCallback", m_impl->logOut)) {
        close();
        return false;
    }
    if (!osOk(AudioUnitInitialize(m_impl->unit), "AudioUnitInitialize",
              m_impl->logOut)) {
        close();
        return false;
    }

    // 5. Honest device rate from the AUHAL output scope; decides MEASURED
    // bit-perfect, which can disagree with the decision's prediction if a
    // device accepted a switch but did not truly clock it. Read AFTER
    // AudioUnitInitialize: initialize re-synchronizes the unit's cached
    // device-side format, so the value is trustworthy immediately after a
    // nominal-rate switch, whereas a read before initialize can still report
    // the pre-switch rate in exactly the case the measurement exists to catch.
    const double auhalDeviceRate = auhalScopeRate(m_impl->unit, kAudioUnitScope_Output);
    m_impl->deviceRate.store(auhalDeviceRate, std::memory_order_relaxed);
    const bool openBitPerfect =
        ratesEqual(auhalDeviceRate, sourceFormat.sampleRate);
    m_impl->bitPerfect.store(openBitPerfect, std::memory_order_relaxed);

    sinkLog(m_impl->logOut,
            "CoreAudioSink: source %u Hz, device %.0f Hz -> %s",
            sourceFormat.sampleRate, auhalDeviceRate,
            openBitPerfect ? "bit-perfect" : "resampled by device");

    m_impl->opened = true;

    // With the device configured, start listening for nominal-rate
    // changes on it (external assertions between here and close; our own
    // reconfigure sets are recognized by the marker). Then publish the ledger
    // shape this open produced.
    {
        AudioObjectPropertyAddress rateAddr{
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        m_impl->deviceListener =
            (AudioObjectAddPropertyListener(dev, &rateAddr, onDeviceRateEvent,
                                            m_impl.get()) == noErr);
    }
    publishDebt(m_impl.get());
    return true;
}

// ---------------------------------------------------------------------------
// The in-place transition. The engine calls this with the unit open and parked
// (AudioOutputUnitStop returned). A close-then-open boundary would cost two
// nominal-rate transitions (restore the session's original rate, then switch
// to the new track's rate), the second of them potentially cross-family, which
// is precisely the churn implicated in the AppleMCA2-T8112 kernel panic. This
// does ONE transition on the existing unit and touches the driver as little as
// possible: uninitialize, move the nominal rate iff the decision asks, set the
// input-scope format for the new source, re-initialize, re-measure. No
// dispose/new, no mid-session restore; the session ledger (originalRate from
// the first open) accumulates and the final close() repays it once.
bool CoreAudioSink::reconfigure(const AudioFormat& sourceFormat,
                                const RateDecision& decision) {
    if (!m_impl->opened || m_impl->unit == nullptr || !sourceFormat.isValid()) {
        return false;
    }

    // Device pinning: retune only the SAME device this unit is bound to. The
    // resolved device is the pinned device when one is set, else the system default;
    // either way, a mid-session change of the resolution must take the
    // close+open road, so the old device gets its rate restored and the new
    // one a genuinely fresh open. Declining here touches nothing, the
    // clean-refusal half of the contract.
    const AudioDeviceID dev = resolveTargetDevice(m_impl->targetDeviceUid);
    if (dev == kAudioObjectUnknown || dev != m_impl->device) {
        sinkLog(m_impl->logOut,
                "CoreAudioSink: reconfigure declined (resolved output device "
                "changed); close+open fallback will re-resolve");
        return false;
    }

    // 1. Quiesce the unit so the rate and format changes land on an
    // uninitialized AU. Past this point a failure returns false in a degraded
    // state, which the contract permits: the engine's fallback close() copes
    // (uninitialize again is harmless, dispose and the rate restore proceed).
    if (!osOk(AudioUnitUninitialize(m_impl->unit),
              "AudioUnitUninitialize (reconfigure)", m_impl->logOut)) {
        return false;
    }

    // 2. Execute the rate decision on the live device binding. The ledger
    // bookkeeping collapses to this: originalRate keeps the SESSION's
    // first-open capture, and rateChanged is recomputed as "does the device
    // now sit anywhere other than that original", so a transition landing back
    // ON the original rate repays the debt by itself and the final close then
    // correctly restores nothing.
    if (decision.switchDevice) {
        // Plant the self-change marker BEFORE the set, so the persistent
        // nominal-rate listener recognizes the resulting notification as our
        // own rather than forgiving the ledger for it.
        {
            std::lock_guard<std::mutex> lock(m_impl->stateMtx);
            m_impl->hasPendingSelfRate = true;
            m_impl->pendingSelfRate = static_cast<double>(decision.deviceRate);
        }
        double achieved = 0.0;
        const bool reached =
            setDeviceNominalRate(dev, static_cast<double>(decision.deviceRate),
                                 &achieved);
        const double nowRate =
            (achieved > 0.0) ? achieved : deviceNominalRate(dev);
        {
            std::lock_guard<std::mutex> lock(m_impl->stateMtx);
            m_impl->hasPendingSelfRate = false;  // consumed or withdrawn (see the proc's comment)
            m_impl->rateChanged =
                (nowRate > 0.0) && !ratesEqual(nowRate, m_impl->originalRate);
        }
        sinkLog(m_impl->logOut,
                "CoreAudioSink: reconfigure: switch -> %u Hz %s (device now "
                "%.0f Hz)",
                decision.deviceRate, reached ? "reached" : "NOT reached",
                nowRate);
    } else {
        sinkLog(m_impl->logOut,
                "CoreAudioSink: reconfigure: no device switch (target %u Hz, "
                "%s)",
                decision.deviceRate,
                decision.resampleNeeded ? "resample expected"
                                        : "already at rate");
    }

    // 3. Input-scope stream format for the NEW source, same shape as open()'s:
    // interleaved packed float32 at the source rate; the AUHAL resamples to
    // the device clock when they differ. The render callback and its refcon
    // are untouched: same impl, same (engine-reconfigured) ring source behind
    // it, so no SetRenderCallback round trip.
    AudioStreamBasicDescription asbd{};
    asbd.mSampleRate       = static_cast<Float64>(sourceFormat.sampleRate);
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mFramesPerPacket  = 1;
    asbd.mChannelsPerFrame = sourceFormat.channels;
    asbd.mBitsPerChannel   = 32;
    asbd.mBytesPerFrame    = sourceFormat.channels * static_cast<UInt32>(sizeof(float));
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;
    if (!osOk(AudioUnitSetProperty(m_impl->unit, kAudioUnitProperty_StreamFormat,
                                   kAudioUnitScope_Input, 0, &asbd, sizeof(asbd)),
              "set StreamFormat (reconfigure)", m_impl->logOut)) {
        return false;
    }

    // 4. Re-initialize; only now commit the working format and channel count
    // (the render callback's memset math), so a failed retune leaves them
    // describing the last configuration that actually ran. RT is parked, so
    // the write is race-free.
    if (!osOk(AudioUnitInitialize(m_impl->unit),
              "AudioUnitInitialize (reconfigure)", m_impl->logOut)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->stateMtx);  // forgiveness reads working
        m_impl->working = sourceFormat;
    }
    m_impl->channels = static_cast<std::size_t>(sourceFormat.channels);

    // 5. Honest measurement, the read-after-initialize discipline applied
    // identically here: the output scope is trustworthy only after the
    // initialize re-synchronized the unit's cached device-side format.
    const double auhalDeviceRate =
        auhalScopeRate(m_impl->unit, kAudioUnitScope_Output);
    m_impl->deviceRate.store(auhalDeviceRate, std::memory_order_relaxed);
    const bool retunedBitPerfect =
        ratesEqual(auhalDeviceRate, sourceFormat.sampleRate);
    m_impl->bitPerfect.store(retunedBitPerfect, std::memory_order_relaxed);

    sinkLog(m_impl->logOut,
            "CoreAudioSink: source %u Hz, device %.0f Hz -> %s",
            sourceFormat.sampleRate, auhalDeviceRate,
            retunedBitPerfect ? "bit-perfect" : "resampled by device");

    publishDebt(m_impl.get()); // the ledger shape after this boundary
    return true;
}

// ---------------------------------------------------------------------------
void CoreAudioSink::start() {
    if (m_impl->opened && !m_impl->started) {
        if (osOk(AudioOutputUnitStart(m_impl->unit), "AudioOutputUnitStart",
                 m_impl->logOut)) {
            m_impl->started = true;
        }
    }
}

void CoreAudioSink::stop() {
    if (m_impl->started) {
        osOk(AudioOutputUnitStop(m_impl->unit), "AudioOutputUnitStop",
             m_impl->logOut);
        m_impl->started = false;
    }
}

void CoreAudioSink::close() {
    // Stop listening to the device BEFORE the restore below, so the
    // restore's own notification never needs suppressing (and a race between
    // an external change and this close resolves as "closing anyway").
    if (m_impl->deviceListener) {
        AudioObjectPropertyAddress rateAddr{
            kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectRemovePropertyListener(m_impl->device, &rateAddr,
                                          onDeviceRateEvent, m_impl.get());
        m_impl->deviceListener = false;
    }
    if (m_impl->unit != nullptr) {
        if (m_impl->started) {
            AudioOutputUnitStop(m_impl->unit);
            m_impl->started = false;
        }
        AudioUnitUninitialize(m_impl->unit);
        AudioComponentInstanceDispose(m_impl->unit);
        m_impl->unit = nullptr;
    }

    // Restore the device's original nominal rate if WE changed it. Leaving a
    // device at a rate its hardware does not faithfully clock corrupts all
    // system audio, so this is mandatory, not etiquette.
    bool          restoreNeeded = false;
    double        restoreTo     = 0.0;
    AudioDeviceID restoreDev    = kAudioObjectUnknown;
    {
        std::lock_guard<std::mutex> lock(m_impl->stateMtx);
        restoreNeeded = m_impl->rateChanged &&
                        m_impl->device != kAudioObjectUnknown &&
                        m_impl->originalRate > 0.0;
        restoreTo  = m_impl->originalRate;
        restoreDev = m_impl->device;
    }
    if (restoreNeeded) {
        // The restore's wait blocks; the mutex is NOT held across it.
        double restored = 0.0;
        setDeviceNominalRate(restoreDev, restoreTo, &restored);
        sinkLog(m_impl->logOut, "CoreAudioSink: restored device rate to %.0f Hz",
                restored);
        std::lock_guard<std::mutex> lock(m_impl->stateMtx);
        m_impl->rateChanged = false;
    }
    publishDebt(m_impl.get()); // cleared, when there was one to clear

    m_impl->opened = false;
    m_impl->source = nullptr;
}

// ---------------------------------------------------------------------------
AudioFormat CoreAudioSink::currentFormat() const {
    return m_impl->working;
}

bool CoreAudioSink::bitPerfect() const noexcept {
    return m_impl->bitPerfect.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The sink-console seams. setLogOutput just parks the non-owning pointer;
// the Engine installs itself at injection and outlives this sink, so no
// lifetime dance is needed here. measuredDeviceRateHz exposes the same AUHAL
// output-scope measurement bitPerfect() is derived from, rounded to the
// nearest Hz (the scope reports a double; nominal rates are integers in
// practice, and the RateManager side of the comparison is integral), gated on
// opened so a closed sink honestly reports "no measurement" rather than a
// stale rate.
// The crash-recovery restore. Runs pre-injection with no logger
// installed, so diagnostics go to stderr; the owner narrates to its own
// console. Conservative by design: an absent device or one that anyone has
// since moved is left exactly as found.
bool CoreAudioSink::repayStaleRateDebt(const std::string& deviceId,
                                       std::uint32_t originalRateHz,
                                       std::uint32_t borrowedRateHz) {
    if (deviceId.empty() || originalRateHz == 0 || borrowedRateHz == 0) {
        return false;
    }
    const AudioDeviceID dev = deviceByUid(deviceId);
    if (dev == kAudioObjectUnknown) {
        std::fprintf(stderr,
                     "CoreAudioSink: stale rate ledger names an absent device "
                     "('%s'); nothing to repay\n",
                     deviceId.c_str());
        return false;
    }
    const double current = deviceNominalRate(dev);
    if (!ratesEqual(current, static_cast<double>(borrowedRateHz))) {
        std::fprintf(stderr,
                     "CoreAudioSink: stale rate ledger for '%s' is moot "
                     "(device at %.0f Hz, not the borrowed %u); leaving it\n",
                     deviceId.c_str(), current, borrowedRateHz);
        return false;
    }
    double restored = 0.0;
    const bool reached = setDeviceNominalRate(
        dev, static_cast<double>(originalRateHz), &restored);
    std::fprintf(stderr,
                 "CoreAudioSink: repaid stale rate debt on '%s': %u -> %.0f Hz "
                 "(%s)\n",
                 deviceId.c_str(), borrowedRateHz, restored,
                 reached ? "restored" : "restore NOT confirmed");
    return reached;
}

void CoreAudioSink::setEventListener(ISinkEventListener* listener) {
    std::lock_guard<std::mutex> lock(m_impl->stateMtx);
    m_impl->events = listener;
}

void CoreAudioSink::setLogOutput(ILogOutput* out) {
    m_impl->logOut = out;
}

std::uint32_t CoreAudioSink::measuredDeviceRateHz() const {
    const double rate = m_impl->deviceRate.load(std::memory_order_relaxed);
    if (!m_impl->opened || rate <= 0.0) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::round(rate));
}

}  // namespace rawform::audio
