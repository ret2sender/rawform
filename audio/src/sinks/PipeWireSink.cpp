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

// PipeWireSink.cpp
//
// PipeWire implementation of IAudioSink and the only translation unit that
// touches libpipewire; CMake compiles it only when pkg-config found
// libpipewire-0.3, and the tripwire below makes that explicit if it is ever fed
// to a build that did not.
//
// The shape of a PipeWire client, for the record and the next reader:
//   - pw_thread_loop: a loop plus its own thread. All PipeWire objects created
//     on it must be touched with the loop LOCK held from any other thread, and
//     the stream's process callback runs on the loop thread. The lock is a real
//     mutex the loop thread holds while dispatching, so holding it from the
//     engine thread excludes callbacks entirely; pw_thread_loop_wait/timed_wait
//     release it while waiting, which is what makes waiting for an event from
//     inside a locked section work.
//   - pw_context / pw_core: the daemon connection. Constructed once here and
//     held for the sink's whole life.
//   - pw_registry + settings metadata: the graph publishes its clock settings
//     (clock.rate, clock.force-rate, clock.allowed-rates) as properties of a
//     metadata object named "settings". This sink binds it at construction and
//     keeps a listener on it, so the graph's effective rate is a live atomic
//     read by capabilities() and the seed for the measurement at open. The same
//     registry walk maintains the Audio/Sink node inventory behind
//     enumerateDevices() and resolves the default sink through the "default"
//     metadata; these are the plumbing that rate switching, device selection,
//     and the external-change events all build on.
//   - pw_stream: the playback stream. Connected LIVE at open() but disarmed:
//     the armed flag keeps the process callback writing silence until start(),
//     so the IAudioSink contract (open() negotiates, start() begins pulling)
//     holds at the SOURCE boundary while the node itself already runs in the
//     graph. Running matters: probing showed node.force-rate is applied
//     only while the forcing node is active; a suspended node forces nothing,
//     so an INACTIVE connect made the open()-time rate confirmation stare at
//     the old rate until timeout while the real reclock happened, unwitnessed,
//     at activation. This mirrors CoreAudio anyway, where the device runs and
//     AudioOutputUnitStart merely gates our IO proc. MAP_BUFFERS gives the
//     process callback plain mapped memory, and RT_PROCESS runs it on the
//     realtime data thread.
//
// Rate model: the graph can be MOVED, not just followed. The stream opens at
// the source format; when the RateManager's decision says switch, the stream
// carries node.rate and node.force-rate properties, and node.force-rate makes
// the graph clock the target REGARDLESS of clock.allowed-rates, so bit-perfect
// works even on a stock PipeWire config. The force lives and dies with the
// stream: the daemon lifts it when the node leaves the graph, so close() repays
// the rate debt by construction and a client crash cannot strand the graph
// (the ledger CoreAudioSink has to keep does not exist on this backend, and
// repayStaleRateDebt() stays at its do-nothing default). capabilities()
// reports what is honestly available: the TARGET sink node's advertised rate
// spans (EnumFormat; the pinned node, else the default resolved through the
// "default" metadata and the registry inventory), the graph's effective clock
// as currentRate, and canSwitchRate true once a target node is resolved. The
// realized rate is verified after negotiation and measured live from the io
// position clock, so every outcome the engine publishes is the graph's truth,
// not the request's.
//
// The track-to-track transition: reconfigure() runs with the stream open and
// parked (stop() disarmed and deactivated it). Instead of destroying the
// stream and connecting a fresh one, it renegotiates the format in place
// (pw_stream_update_params), swaps the node's rate force
// (pw_stream_update_properties), reactivates DISARMED (a suspended node
// forces nothing, so the confirmations need it running), and confirms both
// the negotiated format and the realized graph rate against reality, bounded.
// It ends in exactly the state open() ends in. Any doubt returns false and
// the engine takes close()+open(), always correct here since the daemon
// session survives a stream close.
//
// Devices and events: enumerateDevices() serves the node inventory keyed by
// node.name with the session manager's default marked; selectDevice() pins a
// name from that inventory, which redirects capabilities() and sets
// PW_KEY_TARGET_OBJECT on the stream at the next open. The sink emits
// onDeviceListChanged (inventory or default-marking changed) and
// onExternalRateChanged (settings clock transitions, and a target-node driver
// format diverging from or re-converging with the graph clock); it never
// emits onDefaultDeviceChanged or onRateDebtChanged, for the reasons above.
//
// The join guarantee stop() gives (the IAudioSink lifetime invariant): stop()
// takes the thread-loop lock, disarms the pull flag, deactivates the stream,
// and unlocks. Taking the lock excludes the loop thread from being mid-process;
// the flag flip therefore happens-before any subsequent process invocation,
// which observes armed == false and writes silence without touching the source.
// Combined with close() destroying the stream under the same lock before the
// engine resets the ring, no pull() ever reaches a dead source or buffer.

#include "sinks/PipeWireSink.h"

#if !defined(RAWFORM_HAVE_PIPEWIRE) || !RAWFORM_HAVE_PIPEWIRE
#error "PipeWireSink.cpp should be compiled only when CMake found libpipewire-0.3."
#endif

#include "rawform/audio/IAudioSink.h"
#include "rawform/audio/RateManager.h"
#include "rawform/audio/Types.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/node/io.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format.h>  // SPA_FORMAT_AUDIO_rate (the EnumFormat rate key)
#include <spa/pod/iter.h>      // spa_pod_find_prop, spa_pod_get_values

#include <algorithm>  // std::sort for the discrete rate set
#include <atomic>
#include <cerrno>   // EIO: onCoreError's expected-first-arm squelch
#include <chrono>
#include <cstdarg>  // sinkLog's va_list formatting
#include <cstddef>
#include <cstdint>  // NOLINT: std::int*_t
#include <cstdio>
#include <cstdlib>  // strtoul for metadata value parsing
#include <cstring>
#include <mutex>    // std::once_flag for the process-wide pw_init
#include <string>
#include <thread>   // the graph-rate confirmation poll in open()
#include <vector>

namespace rawform::audio {

// ---------------------------------------------------------------------------
// Implementation state. Namespace-scope (not nested) so the free C callbacks
// can reach it through their void* data pointer. source is non-owning; the
// lifetime invariant guarantees the source outlives every callback that can
// still observe armed == true.
struct PipeWireSinkImpl {
    // ----- daemon session, constructed once, alive for the sink's life -------
    struct pw_thread_loop* loop     = nullptr;
    struct pw_context*     context  = nullptr;
    struct pw_core*        core     = nullptr;
    struct pw_registry*    registry = nullptr;
    struct pw_metadata*    metadata = nullptr;  // the "settings" metadata, once found
    struct spa_hook        coreListener{};
    struct spa_hook        registryListener{};
    struct spa_hook        metadataListener{};
    bool                   connected     = false;  // daemon connection established
    bool                   metadataBound = false;
    int                    syncSeq       = 0;      // last pw_core_sync sequence issued
    int                    doneSeq       = -1;     // last done sequence observed

    // The engine's event channel, guarded by the loop lock: written
    // by setEventListener (engine thread, lock taken), read by the registry
    // and metadata handlers (loop thread, lock held during dispatch). This
    // sink emits onDeviceListChanged (node inventory or default-marking
    // changes) and onExternalRateChanged (settings clock transitions); it
    // never emits onDefaultDeviceChanged (the session manager migrates live
    // streams itself, the documented asymmetry) nor onRateDebtChanged (the
    // force lifts with the node: repayment is structural, there is no ledger).
    ISinkEventListener* events = nullptr;

    // Graph clock settings, written by the metadata listener on the loop thread,
    // read by the engine thread. Atomics because the writer is another thread;
    // relaxed everywhere, since a momentarily stale rate only mislabels a log
    // line and the sync in capabilities() serializes the reads that matter.
    std::atomic<std::uint32_t> clockRate{0};   // settings clock.rate
    std::atomic<std::uint32_t> forceRate{0};   // settings clock.force-rate (0 = none)

    // ----- device inventory, all guarded by the loop lock ----------
    // The thread-loop lock is the natural guard for everything here: writers
    // are registry/metadata callbacks (loop thread, which holds the lock while
    // dispatching), and the engine-thread reader (capabilities()) already takes
    // it for the sync round trip.
    struct pw_metadata* defaultMetadata      = nullptr;  // the "default" metadata
    struct spa_hook     defaultMetadataListener{};
    bool                defaultMetadataBound = false;
    std::string         defaultSinkName;                 // default.audio.sink node.name

    struct SinkNodeEntry {
        std::uint32_t id = 0;       // transient registry id (bind handle)
        std::string   name;         // node.name, the persistent identity (the device id)
        std::string   description;  // node.description, the human label
    };
    std::vector<SinkNodeEntry> sinkNodes;  // every live media.class == Audio/Sink node

    // EnumFormat collection scratch for the node param query in capabilities().
    // One engine thread means one query in flight at a time; the flag keeps a
    // late straggler param event from writing into a finished collection.
    // Discrete values and ranges are kept APART: an Enum is an exact list, a
    // Range is an interval, and when a node publishes both, the exact list is
    // the better evidence, so capabilities() prefers discrete when any was
    // collected. Investigated on real hardware: the on-board HDA node publishes ONE
    // pod with a genuine Range (default 48000, 44100..192000; pw-cli renders
    // that as three bare Ints, easily misread as an enum), and an interior
    // rate (96000) forced through it was verified all the way to the driver's
    // configured format, so a Range from this stack is a real interval, not an
    // envelope. The theoretical residue (a driver snapping away from an
    // interior rate it cannot do, resampling the last mile below the graph
    // clock) is noted for the node param listeners.
    std::vector<std::uint32_t> paramDiscreteRates;
    std::vector<RateRange>     paramRangeRates;
    bool                       collectingParams = false;

    // The pinned output device by persistent node.name; empty =
    // follow the session manager's default routing. openTargetName snapshots
    // the pin at open() so reconfigure() can detect a mid-session repin and
    // decline (the close+open road re-targets; a default-device change while
    // UNPINNED needs no decline, because the daemon migrates live streams to
    // the new default itself). Engine thread only.
    std::string targetNodeName;
    std::string openTargetName;

    // Per-open force record: whether THIS open put a node.force-rate on the
    // stream, and to what. Engine thread only; close() logs the lift from it.
    bool          forcedThisOpen = false;
    std::uint32_t forcedRate     = 0;

    // ----- per-open stream state ---------------------------------------------
    struct pw_stream*       stream = nullptr;
    struct spa_hook         streamListener{};
    struct spa_io_position* position = nullptr;  // io area; loop-thread lifetime
    IPullSource*            source   = nullptr;  // non-owning
    std::size_t             channels = 0;
    AudioFormat             working{};           // the SOURCE format we pull at

    // The graph rate the stream is measured against: seeded from the settings
    // metadata at open, refreshed by the process callback from the io position
    // clock. Written on the loop thread, read on the engine thread.
    std::atomic<std::uint32_t> measuredRate{0};

    // The stream's NEGOTIATED audio format, written by the param_changed
    // handler on the loop thread (both at the initial connect and at every
    // in-place renegotiation), read by the engine thread. reconfigure() zeroes
    // them before pushing new params so its confirmation poll can only be
    // satisfied by the FRESH negotiation, never a stale one.
    std::atomic<std::uint32_t> negotiatedRate{0};
    std::atomic<std::uint32_t> negotiatedChannels{0};

    // ----- driver-format watch, the last-mile verification ---------
    // The io position clock reports the GRAPH rate; a driver could in
    // principle snap away from an interior rate it cannot realize and
    // resample the last mile BELOW the graph clock. The watch closes that gap:
    // a bound proxy on the resolved TARGET NODE with a Format param
    // subscription, so the driver's realized format is a live fact.
    // driverFormatRate is that fact (atomic: read locklessly by bitPerfect());
    // everything else is loop-lock-guarded plain state (the handler runs with
    // the lock held during dispatch, the engine entry points take it).
    //
    // watchArmed gates EMISSION, not collection: open()/reconfigure() keep it
    // false through their own commanded transitions (a reclock makes the
    // driver format and the graph clock update in arbitrary order, and
    // certifying that transient would race the boundary's own outcome
    // publication), then reconcile once the dust settles. driverCertRate is
    // the divergence latch: the driver rate last CERTIFIED to the engine via
    // onExternalRateChanged (0 = driver and graph agree), so a steady
    // divergence emits once, a re-convergence emits once, and nothing spams.
    struct pw_node* driverWatchNode    = nullptr;
    struct spa_hook driverWatchListener{};
    bool            driverWatchBound   = false;
    std::uint32_t   driverWatchNodeId  = 0;
    bool            watchArmed         = false;
    std::uint32_t   driverCertRate     = 0;
    std::atomic<std::uint32_t> driverFormatRate{0};

    // armed gates the pull in process (the stop() join mechanism; see the file
    // header). Written under the loop lock, read on the loop thread.
    std::atomic<bool> armed{false};

    bool        opened  = false;
    bool        started = false;
    ILogOutput* logOut = nullptr; // non-owning line logger
};

namespace {

constexpr int kSyncTimeoutSec     = 2;  // metadata round trips, same 2 s ceiling as the Mac's rate wait
constexpr int kConnectTimeoutSec  = 5;  // stream negotiation involves the session manager; give it headroom
constexpr int kRateConfirmTimeoutMs = 2000;  // graph reclock confirmation, mirroring the Mac's rate wait
constexpr int kRateReadTimeoutMs    = 300;   // expectation check when NOT commanding a reclock: a read, not a wait

// The single diagnostic funnel, byte-for-byte the CoreAudioSink shape: format
// once, write the line to BOTH stderr and the installed ILogOutput when
// present. Engine-thread only (open/close paths); never the process callback.
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

// Parse a metadata value string as a rate. Metadata sends plain decimal
// strings ("48000"); a null value means the key was removed, which reads as 0
// (unknown), the same sentinel the rest of the rate machinery already uses.
std::uint32_t parseRate(const char* value) {
    if (value == nullptr) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::strtoul(value, nullptr, 10));
}

// Driver-format watch helpers, defined after the node event tables
// they use; declared here because the registry-remove and default-metadata
// handlers above them in the file need to tear down and re-arm the watch.
// Both require the loop lock held (callbacks hold it during dispatch; engine
// entry points take it).
void disarmDriverFormatWatchLocked(PipeWireSinkImpl* impl);
bool armDriverFormatWatchLocked(PipeWireSinkImpl* impl);

// ---------------------------------------------------------------------------
// Core events: done drives the sync rendezvous every metadata round trip uses;
// error is logged as a breadcrumb (stderr only: this callback has no reliable
// engine-thread context, matching the logger-less device helpers on macOS).

void onCoreDone(void* data, std::uint32_t id, int seq) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (id == PW_ID_CORE) {
        impl->doneSeq = seq;
        pw_thread_loop_signal(impl->loop, false);
    }
}

void onCoreError(void* data, std::uint32_t id, int seq, int res,
                 const char* message) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    // Expected first-contact artifact of the driver-format watch, not an
    // error: arming subscribes the target node to SPA_PARAM_Format, and
    // subscribe_params issues an immediate enum of it; a sink node still
    // suspended (an ALSA device before anything has started it, i.e. the
    // first play of a session) has no current Format and the daemon answers
    // -EIO, which lands here naming the watch node's proxy. The subscription
    // itself stands: param_changed delivers the real Format the moment our
    // stream starts the node, so the last-mile verification only begins
    // slightly later, which is within the watch's design. Squelched because
    // an error-shaped line users must learn to ignore trains them to ignore
    // error lines. Scope is deliberately narrow (this proxy, this errno):
    // a steady-state watch-node failure travels with global_remove/default
    // migration handling and other errors still log. Loop thread, dispatch
    // holds the loop lock, same license as every reader of the watch fields.
    if (impl != nullptr && impl->driverWatchBound
        && impl->driverWatchNode != nullptr && res == -EIO
        && id == pw_proxy_get_id(
               reinterpret_cast<struct pw_proxy*>(impl->driverWatchNode))) {
        return;
    }
    std::fprintf(stderr, "PipeWireSink: core error id %u seq %d res %d: %s\n",
                 id, seq, res, message != nullptr ? message : "(null)");
}

// Event-table construction pattern, used for all four tables in this file:
// value-initialize the whole struct (every unhandled callback null), then
// assign the handled fields. A partially designated initializer would read
// nicer, but GCC's -Wextra flags every omitted member under
// -Wmissing-field-initializers (Clang does not), and enumerating the rest is
// brittle against future libpipewire versions growing the tables. The maker
// runs once at static initialization; the tables stay const.
struct pw_core_events makeCoreEvents() {
    struct pw_core_events ev{};
    ev.version = PW_VERSION_CORE_EVENTS;
    ev.done    = onCoreDone;
    ev.error   = onCoreError;
    return ev;
}
const struct pw_core_events kCoreEvents = makeCoreEvents();

// ---------------------------------------------------------------------------
// Metadata events: the graph's clock settings arrive here, both the full replay
// at bind time and every later change. Loop thread; atomics only.

int onMetadataProperty(void* data, std::uint32_t subject, const char* key,
                       const char* /*type*/, const char* value) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (subject != 0 || key == nullptr) {
        return 0;  // clock settings live on the global subject
    }
    // Settings transitions are the external-rate event source. The settings
    // metadata cannot see node forces, so a CLEARED settings force means the
    // graph returns to OUR node force when one is in effect, else to the
    // default clock: the emission carries that best-known
    // rate, which the engine's republication PREFERS over its own measurement
    // (the reclock lags the event by a driver reconfiguration; the measurement
    // catches up on the next process cycles). A clock.rate change only
    // matters when nothing forces (neither settings nor our node). Emissions
    // only while opened; the listener enqueues, so calling under the loop lock
    // is fine.
    if (std::strcmp(key, "clock.rate") == 0) {
        const std::uint32_t oldRate = impl->clockRate.load(std::memory_order_relaxed);
        const std::uint32_t newRate = parseRate(value);
        impl->clockRate.store(newRate, std::memory_order_relaxed);
        if (impl->opened && impl->events != nullptr && newRate != 0 &&
            newRate != oldRate && !impl->forcedThisOpen &&
            impl->forceRate.load(std::memory_order_relaxed) == 0) {
            impl->events->onExternalRateChanged(newRate);
        }
    } else if (std::strcmp(key, "clock.force-rate") == 0) {
        const std::uint32_t oldForce = impl->forceRate.load(std::memory_order_relaxed);
        const std::uint32_t newForce = parseRate(value);
        impl->forceRate.store(newForce, std::memory_order_relaxed);
        if (impl->opened && impl->events != nullptr && newForce != oldForce) {
            if (newForce != 0) {
                // A settings force overrides everything, including our node
                // force: the graph is moving to it.
                impl->events->onExternalRateChanged(newForce);
            } else {
                // Released: the graph returns to our node force when we hold
                // one, else to its default clock.
                const std::uint32_t back =
                    impl->forcedThisOpen
                        ? impl->forcedRate
                        : impl->clockRate.load(std::memory_order_relaxed);
                impl->events->onExternalRateChanged(back);
            }
        }
    }
    return 0;
}

struct pw_metadata_events makeMetadataEvents() {
    struct pw_metadata_events ev{};
    ev.version  = PW_VERSION_METADATA_EVENTS;
    ev.property = onMetadataProperty;
    return ev;
}
const struct pw_metadata_events kMetadataEvents = makeMetadataEvents();

// The "default" metadata publishes the session manager's default device
// choices as tiny JSON values: default.audio.sink -> {"name":"<node.name>"}.
// Extract the name with a hand scan rather than a JSON parser: the daemon
// emits exactly this one canonical shape, and the zero-Qt engine is not
// growing a JSON dependency for one key.
std::string extractJsonName(const char* value) {
    if (value == nullptr) {
        return {};
    }
    const char* p = std::strstr(value, "\"name\"");
    if (p == nullptr) {
        return {};
    }
    p = std::strchr(p + 6, ':');
    if (p == nullptr) {
        return {};
    }
    p = std::strchr(p, '"');
    if (p == nullptr) {
        return {};
    }
    ++p;
    const char* e = std::strchr(p, '"');
    if (e == nullptr) {
        return {};
    }
    return {p, e};
}

int onDefaultMetadataProperty(void* data, std::uint32_t subject, const char* key,
                              const char* /*type*/, const char* value) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (subject != 0 || key == nullptr) {
        return 0;
    }
    // default.audio.sink is the EFFECTIVE default (what streams route to now);
    // default.configured.audio.sink is only the user's remembered preference,
    // which may name hardware that is not present. The effective one is the
    // honest target.
    if (std::strcmp(key, "default.audio.sink") == 0) {
        std::string newName = extractJsonName(value);  // loop lock held (dispatch)
        if (newName != impl->defaultSinkName) {
            impl->defaultSinkName = std::move(newName);
            // The default-marking changed: pickers are stale. No
            // onDefaultDeviceChanged from this sink: the session manager
            // migrates live streams to the new default itself.
            if (impl->events != nullptr) {
                impl->events->onDeviceListChanged();
            }
            // The daemon migrates our UNPINNED live stream to the new
            // default, so the driver-format watch must follow it or it keeps
            // certifying the abandoned device. Re-arm onto the new resolution
            // and keep the armed state: this is steady-state migration, not
            // one of our own transitions. The fresh subscription re-delivers
            // the new node's current Format, so the divergence check runs
            // against the new hardware immediately.
            if (impl->opened && impl->openTargetName.empty()) {
                const bool wasArmed = impl->watchArmed;
                armDriverFormatWatchLocked(impl);
                impl->watchArmed = wasArmed;
            }
        }
    }
    return 0;
}

struct pw_metadata_events makeDefaultMetadataEvents() {
    struct pw_metadata_events ev{};
    ev.version  = PW_VERSION_METADATA_EVENTS;
    ev.property = onDefaultMetadataProperty;
    return ev;
}
const struct pw_metadata_events kDefaultMetadataEvents = makeDefaultMetadataEvents();

// ---------------------------------------------------------------------------
// Registry events: watch the globals for the "settings" and "default" metadata
// objects (bound once each) and maintain the live inventory of Audio/Sink
// nodes, which is where honest rate capabilities come from and where
// enumerateDevices() reads its list. Loop thread; the loop lock is held
// during dispatch, which is what licenses the plain-field writes.

void onRegistryGlobal(void* data, std::uint32_t id, std::uint32_t /*permissions*/,
                      const char* type, std::uint32_t /*version*/,
                      const struct spa_dict* props) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (type == nullptr || props == nullptr) {
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (name == nullptr) {
            return;
        }
        if (!impl->metadataBound && std::strcmp(name, "settings") == 0) {
            impl->metadata = static_cast<struct pw_metadata*>(
                pw_registry_bind(impl->registry, id, type, PW_VERSION_METADATA, 0));
            if (impl->metadata != nullptr) {
                pw_metadata_add_listener(impl->metadata, &impl->metadataListener,
                                         &kMetadataEvents, impl);
                impl->metadataBound = true;
            }
        } else if (!impl->defaultMetadataBound && std::strcmp(name, "default") == 0) {
            impl->defaultMetadata = static_cast<struct pw_metadata*>(
                pw_registry_bind(impl->registry, id, type, PW_VERSION_METADATA, 0));
            if (impl->defaultMetadata != nullptr) {
                pw_metadata_add_listener(impl->defaultMetadata,
                                         &impl->defaultMetadataListener,
                                         &kDefaultMetadataEvents, impl);
                impl->defaultMetadataBound = true;
            }
        }
        return;
    }

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        const char* mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (mediaClass == nullptr || std::strcmp(mediaClass, "Audio/Sink") != 0) {
            return;
        }
        const char* nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char* nodeDesc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        PipeWireSinkImpl::SinkNodeEntry entry;
        entry.id          = id;
        entry.name        = nodeName != nullptr ? nodeName : "";
        entry.description = nodeDesc != nullptr ? nodeDesc : entry.name;
        impl->sinkNodes.push_back(entry);
        if (impl->events != nullptr) {
            impl->events->onDeviceListChanged(); // Inventory grew
        }
    }
}

void onRegistryGlobalRemove(void* data, std::uint32_t id) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    // The watched node left the graph (unplug, profile flip). Tear the
    // watch down before its proxy dangles; the re-arm happens on the default
    // migration that typically accompanies the removal (the handler above), or
    // at the next open/reconfigure boundary otherwise. Checked before the
    // inventory scan because the watched node need not be an Audio/Sink entry
    // we track (it always is today; the order is cheap insurance).
    if (impl->driverWatchBound && id == impl->driverWatchNodeId) {
        disarmDriverFormatWatchLocked(impl);
    }
    for (std::size_t i = 0; i < impl->sinkNodes.size(); ++i) {
        if (impl->sinkNodes[i].id == id) {
            impl->sinkNodes.erase(impl->sinkNodes.begin() +
                                  static_cast<std::ptrdiff_t>(i));
            if (impl->events != nullptr) {
                impl->events->onDeviceListChanged(); // Inventory shrank
            }
            return;
        }
    }
    // Metadata removal (a daemon restart mid-session) is outside this sink's
    // recovery scope: the stream is gone with the daemon, and the session ends
    // through the engine's error path rather than a rebind.
}

struct pw_registry_events makeRegistryEvents() {
    struct pw_registry_events ev{};
    ev.version       = PW_VERSION_REGISTRY_EVENTS;
    ev.global        = onRegistryGlobal;
    ev.global_remove = onRegistryGlobalRemove;
    return ev;
}
const struct pw_registry_events kRegistryEvents = makeRegistryEvents();

// ---------------------------------------------------------------------------
// Node EnumFormat parsing: the rate property of an EnumFormat pod arrives as a
// plain int, an enumeration of ints, or a range, depending on the node. All
// three collapse into the engine's RateRange spans.

void addRateSpan(std::vector<RateRange>& out, std::uint32_t lo, std::uint32_t hi) {
    if (lo == 0 || hi < lo) {
        return;
    }
    for (const RateRange& r : out) {
        if (r.min == lo && r.max == hi) {
            return;  // several formats often advertise identical rate sets
        }
    }
    out.push_back(RateRange{.min = lo, .max = hi});
}

void addDiscreteRate(std::vector<std::uint32_t>& out, std::uint32_t rate) {
    if (rate == 0) {
        return;
    }
    for (const std::uint32_t r : out) {
        if (r == rate) {
            return;
        }
    }
    out.push_back(rate);
}

// Diagnostic: name a choice kind for the pod-inventory log below.
const char* choiceKindName(std::uint32_t choice) {
    switch (choice) {
    case SPA_CHOICE_None:  return "None";
    case SPA_CHOICE_Range: return "Range";
    case SPA_CHOICE_Step:  return "Step";
    case SPA_CHOICE_Enum:  return "Enum";
    case SPA_CHOICE_Flags: return "Flags";
    default:               return "?";
    }
}

void onNodeParam(void* data, int /*seq*/, std::uint32_t id, std::uint32_t index,
                 std::uint32_t /*next*/, const struct spa_pod* param) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (!impl->collectingParams || id != SPA_PARAM_EnumFormat || param == nullptr) {
        return;
    }

    // The pod-inventory diagnostic (env-gated): the raw EnumFormat
    // inventory, which settled the Range-vs-Enum misreading and stays one
    // environment variable away for the next hardware puzzle:
    //   RAWFORM_PW_PODS=1 ./rawform_audio_cli rates
    // Loop thread; plain fprintf, never the logger.
    static const bool kPodDiag = (std::getenv("RAWFORM_PW_PODS") != nullptr);
    std::uint32_t mediaType    = 0;
    std::uint32_t mediaSubtype = 0;
    const int parsed = spa_format_parse(param, &mediaType, &mediaSubtype);

    const struct spa_pod_prop* prop =
        spa_pod_find_prop(param, nullptr, SPA_FORMAT_AUDIO_rate);
    if (prop == nullptr) {
        if (kPodDiag) {
            std::fprintf(stderr,
                         "PipeWireSink: [pods] #%u media %u/%u (parse %d): no "
                         "rate property\n",
                         index, mediaType, mediaSubtype, parsed);
        }
        return;
    }
    std::uint32_t nVals  = 0;
    std::uint32_t choice = 0;
    const struct spa_pod* vals = spa_pod_get_values(&prop->value, &nVals, &choice);
    if (vals == nullptr || vals->type != SPA_TYPE_Int || nVals == 0) {
        if (kPodDiag) {
            std::fprintf(stderr,
                         "PipeWireSink: [pods] #%u media %u/%u: rate prop "
                         "present but unparsed (vals %p, type %u, n %u)\n",
                         index, mediaType, mediaSubtype,
                         static_cast<const void*>(vals),
                         vals != nullptr ? vals->type : 0u, nVals);
        }
        return;
    }
    if (kPodDiag) {
        // Values labeled by their SPA positional meaning, so the line reads
        // itself: None carries one value; Range and Step carry default, min,
        // max (Step adds the step size fourth); Enum carries the default
        // followed by the alternatives.
        const auto* dv = static_cast<const std::int32_t*>(SPA_POD_BODY(vals));
        char        line[256];
        int         off = std::snprintf(
            line, sizeof(line),
            "PipeWireSink: [pods] #%u media %u/%u rate choice=%s n=%u:", index,
            mediaType, mediaSubtype, choiceKindName(choice), nVals);
        auto append = [&](const char* fmt, std::int32_t v) {
            if (off > 0 && off < static_cast<int>(sizeof(line))) {
                off += std::snprintf(line + off,
                                     sizeof(line) - static_cast<std::size_t>(off),
                                     fmt, v);
            }
        };
        if (choice == SPA_CHOICE_None && nVals >= 1) {
            append(" value %d", dv[0]);
        } else if ((choice == SPA_CHOICE_Range || choice == SPA_CHOICE_Step) &&
                   nVals >= 3) {
            append(" default %d", dv[0]);
            append(" min %d", dv[1]);
            append(" max %d", dv[2]);
            if (choice == SPA_CHOICE_Step && nVals >= 4) {
                append(" step %d", dv[3]);
            }
        } else if (choice == SPA_CHOICE_Enum && nVals >= 1) {
            append(" default %d", dv[0]);
            if (nVals > 1 && off > 0 && off < static_cast<int>(sizeof(line))) {
                off += std::snprintf(
                    line + off, sizeof(line) - static_cast<std::size_t>(off),
                    " alts:");
            }
            for (std::uint32_t i = 1; i < nVals && i < 9; ++i) {
                append(" %d", dv[i]);
            }
        } else {
            for (std::uint32_t i = 0; i < nVals && i < 8; ++i) {
                append(" %d", dv[i]);
            }
        }
        std::fprintf(stderr, "%s\n", line);
    }
    // Choice-pod value layout (SPA convention): values[0] is the default;
    // for Range/Step, values[1] and values[2] are min and max; for Enum, the
    // alternatives follow the default.
    const auto* v = static_cast<const std::int32_t*>(SPA_POD_BODY(vals));
    switch (choice) {
    case SPA_CHOICE_None:
        addDiscreteRate(impl->paramDiscreteRates,
                        static_cast<std::uint32_t>(v[0]));
        break;
    case SPA_CHOICE_Range:
    case SPA_CHOICE_Step:
        if (nVals >= 3) {
            addRateSpan(impl->paramRangeRates, static_cast<std::uint32_t>(v[1]),
                        static_cast<std::uint32_t>(v[2]));
        }
        break;
    case SPA_CHOICE_Enum:
        // values[0] is the default and normally repeats in the alternatives;
        // collect all of them, the dedupe handles the repeat.
        for (std::uint32_t i = 0; i < nVals; ++i) {
            addDiscreteRate(impl->paramDiscreteRates,
                            static_cast<std::uint32_t>(v[i]));
        }
        break;
    default:
        break;  // Flags and future kinds carry no rate semantics for us
    }
}

struct pw_node_events makeNodeEvents() {
    struct pw_node_events ev{};
    ev.version = PW_VERSION_NODE_EVENTS;
    ev.param   = onNodeParam;
    return ev;
}
const struct pw_node_events kNodeEvents = makeNodeEvents();

// ---------------------------------------------------------------------------
// Target-node resolution, shared by capabilities() and the driver-format
// watch so the two can never disagree about which node "the output
// device" is. Loop lock held. The pinned device wins when set and resolves ONLY that
// node (a vanished pin returns null; the caller decides how to degrade);
// unpinned, the default sink by name when the "default" metadata named one
// and the inventory has it; otherwise the first sink node in the inventory (a
// one-device machine without a resolvable default is still a perfectly usable
// machine). The returned pointer aims into the inventory vector: use it
// immediately, under the same lock hold, never across a dispatch.
const PipeWireSinkImpl::SinkNodeEntry* resolveTargetEntry(
    const PipeWireSinkImpl* impl) {
    if (!impl->targetNodeName.empty()) {
        for (const auto& entry : impl->sinkNodes) {
            if (entry.name == impl->targetNodeName) {
                return &entry;
            }
        }
        return nullptr;  // pinned but vanished: never silently play elsewhere
    }
    if (!impl->defaultSinkName.empty()) {
        for (const auto& entry : impl->sinkNodes) {
            if (entry.name == impl->defaultSinkName) {
                return &entry;
            }
        }
    }
    if (!impl->sinkNodes.empty()) {
        return &impl->sinkNodes.front();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The driver-format watch: the last-mile verification the device
// investigation left as a named residue. The target NODE's live Format param
// is the driver's realized configuration (pw-top's FORMAT column reads the
// same fact), while the io position clock this sink measures is the GRAPH
// rate. When the two diverge, the driver is resampling the last mile below
// the graph clock and every graph-side measurement is flattered; the honest
// outcome is the DRIVER'S rate, so the watch certifies it to the engine
// through onExternalRateChanged, whose republication is event-rate-first
// for exactly this reason (the emitter certifies the rate). A later
// re-convergence is certified the same way, so the published outcome can heal.
//
// Emission discipline: watchArmed keeps this quiet through our OWN commanded
// transitions (open/reconfigure reconcile at their end instead: during a
// reclock the driver format and the graph clock move in arbitrary order and a
// transient certification would race the boundary's outcome publication);
// driverCertRate latches what was last certified so a steady state emits
// exactly once in each direction. Loop thread, lock held during dispatch;
// stderr only (ILogOutput is engine-thread, the CoreAudio listener
// precedent); the listener contract is enqueue-only, so emitting under the
// lock is fine.
void onDriverFormatParam(void* data, int /*seq*/, std::uint32_t id,
                         std::uint32_t /*index*/, std::uint32_t /*next*/,
                         const struct spa_pod* param) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (id != SPA_PARAM_Format || param == nullptr) {
        // A null Format is the driver clearing during its own renegotiation;
        // the fresh set follows on the same subscription. Keep the last known
        // value rather than certifying a phantom 0.
        return;
    }
    std::uint32_t mediaType    = 0;
    std::uint32_t mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 ||
        mediaType != SPA_MEDIA_TYPE_audio ||
        mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    struct spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) < 0 || info.rate == 0) {
        return;
    }
    impl->driverFormatRate.store(info.rate, std::memory_order_relaxed);

    if (!impl->watchArmed || !impl->opened || impl->events == nullptr) {
        return;
    }
    const std::uint32_t graph =
        impl->measuredRate.load(std::memory_order_relaxed);
    if (graph == 0) {
        return;  // no graph measurement yet: nothing to diverge from
    }
    if (info.rate != graph) {
        if (impl->driverCertRate != info.rate) {
            impl->driverCertRate = info.rate;
            std::fprintf(stderr,
                         "PipeWireSink: driver format %u Hz diverges from "
                         "graph clock %u Hz; certifying the driver rate "
                         "(last-mile resample)\n",
                         info.rate, graph);
            impl->events->onExternalRateChanged(info.rate);
        }
    } else if (impl->driverCertRate != 0) {
        impl->driverCertRate = 0;
        std::fprintf(stderr,
                     "PipeWireSink: driver format re-converged with the graph "
                     "at %u Hz\n",
                     info.rate);
        impl->events->onExternalRateChanged(info.rate);
    }
}

struct pw_node_events makeDriverNodeEvents() {
    struct pw_node_events ev{};
    ev.version = PW_VERSION_NODE_EVENTS;
    ev.param   = onDriverFormatParam;
    return ev;
}
const struct pw_node_events kDriverNodeEvents = makeDriverNodeEvents();

// Tear the watch down: unhook, destroy the bound proxy, clear every fact it
// produced. driverFormatRate resets too, so bitPerfect() never judges a fresh
// open against a dead device's format. Loop lock held.
void disarmDriverFormatWatchLocked(PipeWireSinkImpl* impl) {
    if (impl->driverWatchBound) {
        spa_hook_remove(&impl->driverWatchListener);
        pw_proxy_destroy(
            reinterpret_cast<struct pw_proxy*>(impl->driverWatchNode));
        impl->driverWatchNode   = nullptr;
        impl->driverWatchBound  = false;
        impl->driverWatchNodeId = 0;
    }
    impl->watchArmed     = false;
    impl->driverCertRate = 0;
    impl->driverFormatRate.store(0, std::memory_order_relaxed);
}

// Bind the watch onto the current target resolution. Any previous watch is
// torn down first; the Format subscription delivers the node's CURRENT param
// immediately, so driverFormatRate repopulates without waiting for a change.
// watchArmed stays false; the caller decides when emission is safe (open and
// reconfigure reconcile at their end; the default-migration handler restores
// the armed state it saw). Loop lock held. Returns false when no target
// resolves or the bind fails, which degrades to exactly the pre-watch behavior:
// graph-clock truth only, no last-mile verification.
bool armDriverFormatWatchLocked(PipeWireSinkImpl* impl) {
    disarmDriverFormatWatchLocked(impl);
    const PipeWireSinkImpl::SinkNodeEntry* target = resolveTargetEntry(impl);
    if (target == nullptr) {
        return false;
    }
    auto* node = static_cast<struct pw_node*>(
        pw_registry_bind(impl->registry, target->id, PW_TYPE_INTERFACE_Node,
                         PW_VERSION_NODE, 0));
    if (node == nullptr) {
        return false;
    }
    pw_node_add_listener(node, &impl->driverWatchListener, &kDriverNodeEvents,
                         impl);
    std::uint32_t ids[1] = {SPA_PARAM_Format};
    pw_node_subscribe_params(node, ids, 1);
    impl->driverWatchNode   = node;
    impl->driverWatchBound  = true;
    impl->driverWatchNodeId = target->id;
    return true;
}

// ---------------------------------------------------------------------------
// Stream events. state_changed wakes the open() wait; io_changed hands us the
// io position area the process callback measures the graph clock from; process
// is the real-time consumer.

void onStreamStateChanged(void* data, enum pw_stream_state /*old*/,
                          enum pw_stream_state state, const char* error) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (state == PW_STREAM_STATE_ERROR) {
        std::fprintf(stderr, "PipeWireSink: stream error: %s\n",
                     error != nullptr ? error : "(null)");
    }
    pw_thread_loop_signal(impl->loop, false);
}

void onStreamIoChanged(void* data, std::uint32_t id, void* area,
                       std::uint32_t /*size*/) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (id == SPA_IO_Position) {
        impl->position = static_cast<struct spa_io_position*>(area);
    }
}

// The negotiated stream format, delivered here at the initial connect and at
// every in-place renegotiation (reconfigure's update_params). A null param
// means the format was cleared (renegotiation in flight): store zeroes, so a
// waiter can only be satisfied by the fresh result.
void onStreamParamChanged(void* data, std::uint32_t id,
                          const struct spa_pod* param) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);
    if (id != SPA_PARAM_Format) {
        return;
    }
    if (param == nullptr) {
        impl->negotiatedRate.store(0, std::memory_order_relaxed);
        impl->negotiatedChannels.store(0, std::memory_order_relaxed);
        return;
    }
    std::uint32_t mediaType    = 0;
    std::uint32_t mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 ||
        mediaType != SPA_MEDIA_TYPE_audio ||
        mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
    }
    struct spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) >= 0) {
        impl->negotiatedRate.store(info.rate, std::memory_order_relaxed);
        impl->negotiatedChannels.store(info.channels, std::memory_order_relaxed);
    }
    pw_thread_loop_signal(impl->loop, false);
}

// The process callback: the real-time consumer. Pulls from the non-owning
// source into the mapped buffer and zero-pads any shortfall; when disarmed
// (stop() in flight or landed) it writes pure silence and never touches the
// source, which is half of the join guarantee. Must not throw, lock, or
// allocate; it does none of those.
void onStreamProcess(void* data) {
    auto* impl = static_cast<PipeWireSinkImpl*>(data);

    struct pw_buffer* b = pw_stream_dequeue_buffer(impl->stream);
    if (b == nullptr) {
        return;
    }
    struct spa_data* d = &b->buffer->datas[0];
    auto* out = static_cast<float*>(d->data);
    if (out == nullptr) {
        pw_stream_queue_buffer(impl->stream, b);
        return;
    }

    const std::size_t ch = impl->channels != 0 ? impl->channels : 1;
    const auto stride = static_cast<std::uint32_t>(ch * sizeof(float));

    std::size_t frames = d->maxsize / stride;
    if (b->requested != 0 && static_cast<std::size_t>(b->requested) < frames) {
        frames = static_cast<std::size_t>(b->requested);
    }

    std::size_t got = 0;
    if (impl->armed.load(std::memory_order_relaxed) && impl->source != nullptr) {
        got = impl->source->pull(out, frames);
    }
    if (got < frames) {
        std::memset(out + got * ch, 0, (frames - got) * ch * sizeof(float));
    }

    d->chunk->offset = 0;
    d->chunk->stride = static_cast<std::int32_t>(stride);
    d->chunk->size   = static_cast<std::uint32_t>(frames) * stride;

    // Live graph-clock measurement: the io position's clock rate is the graph
    // rate the stream is being consumed at (a fraction; nominal rates carry it
    // as 1/N, so the denominator is the rate in Hz). Refreshing it here keeps
    // measuredDeviceRateHz honest across mid-session graph changes.
    if (impl->position != nullptr) {
        const std::uint32_t denom = impl->position->clock.rate.denom;
        if (denom != 0) {
            impl->measuredRate.store(denom, std::memory_order_relaxed);
        }
    }

    pw_stream_queue_buffer(impl->stream, b);
}

struct pw_stream_events makeStreamEvents() {
    struct pw_stream_events ev{};
    ev.version       = PW_VERSION_STREAM_EVENTS;
    ev.state_changed = onStreamStateChanged;
    ev.io_changed    = onStreamIoChanged;
    ev.param_changed = onStreamParamChanged;
    ev.process       = onStreamProcess;
    return ev;
}
const struct pw_stream_events kStreamEvents = makeStreamEvents();

// ---------------------------------------------------------------------------
// One process-wide pw_init. libpipewire wants it before any other call; it is
// idempotent in effect but there is no reason to run it more than once.
void ensurePipeWireInit() {
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

// Round-trip rendezvous: issue a core sync and wait (releasing the loop lock)
// until the matching done arrives, bounded by `timeoutSec`. Call with the loop
// LOCKED. Returns true when the done landed; false on timeout, which callers
// treat as "proceed with what we have", never as fatal.
bool syncWait(PipeWireSinkImpl* impl, int timeoutSec) {
    impl->syncSeq = pw_core_sync(impl->core, PW_ID_CORE, impl->syncSeq);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    while (impl->doneSeq != impl->syncSeq) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        pw_thread_loop_timed_wait(impl->loop, 1);
    }
    return true;
}

// The graph's effective clock rate: a forced rate pins the graph outright, so
// it wins over the settings default when present.
std::uint32_t effectiveGraphRate(const PipeWireSinkImpl* impl) {
    const std::uint32_t forced = impl->forceRate.load(std::memory_order_relaxed);
    if (forced != 0) {
        return forced;
    }
    return impl->clockRate.load(std::memory_order_relaxed);
}

// Confirm the graph reclocked to `target` by polling the stream's io position
// clock, the only truthful witness: node.force-rate never touches the settings
// metadata, so the atomics above keep showing the graph's OWN rate while a
// force is in effect. The position area is shared memory the driver rewrites
// each cycle with no event to signal on, hence a short sleep-poll from the
// engine thread, lock taken per probe, bounded like every wait in the sinks.
// Returns the last observed rate either way, so the caller logs reality.
std::uint32_t pollGraphRate(const PipeWireSinkImpl* impl,
                            std::uint32_t target,
                            int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    for (;;) {
        pw_thread_loop_lock(impl->loop);
        const std::uint32_t observed =
            impl->position != nullptr ? impl->position->clock.rate.denom : 0;
        pw_thread_loop_unlock(impl->loop);
        if (observed == target) {
            return observed;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return observed;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// The transition-end reconcile shared by open() and reconfigure().
// Our own commanded reclocks keep emission disarmed (the driver format and
// the graph clock move in arbitrary order mid-transition, and certifying a
// transient would race the boundary's outcome publication); once the boundary
// has settled its graph rate, this arms the watch, clears the divergence
// latch, and, if the driver's realized format ALREADY disagrees with the
// settled graph, certifies the driver rate immediately. The emission is
// enqueue-only on the engine side, so the resulting republication runs after
// the boundary's own publication and corrects it, never races it. Engine
// thread; takes the lock itself.
void reconcileDriverFormat(PipeWireSinkImpl* impl, std::uint32_t graphRate) {
    pw_thread_loop_lock(impl->loop);
    impl->watchArmed     = impl->driverWatchBound;
    impl->driverCertRate = 0;
    const std::uint32_t driver =
        impl->driverFormatRate.load(std::memory_order_relaxed);
    ISinkEventListener* ev = impl->events;
    const bool certify = impl->driverWatchBound && ev != nullptr &&
                         driver != 0 && graphRate != 0 && driver != graphRate;
    if (certify) {
        impl->driverCertRate = driver;
    }
    pw_thread_loop_unlock(impl->loop);
    if (certify) {
        sinkLog(impl->logOut,
                "PipeWireSink: driver format %u Hz vs graph %u Hz at the "
                "boundary: last-mile resample; certifying the driver rate",
                driver, graphRate);
        ev->onExternalRateChanged(driver);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction: stand the daemon session up (loop, context, core, registry,
// settings metadata) and hold it for the sink's life. Two sync round trips
// bound the wait: the first flushes the registry globals (which binds the
// settings metadata inside its callback), the second flushes that metadata's
// property replay, so capabilities() has a real rate before the first open. A
// failed connect degrades to a permanently unusable sink with everything torn
// back down; open() then fails with a logged message, the same graceful path
// CoreAudioSink takes when there is no default output device.
PipeWireSink::PipeWireSink()
    : m_impl(std::make_unique<PipeWireSinkImpl>()) {
    ensurePipeWireInit();

    m_impl->loop = pw_thread_loop_new("rawform-audio", nullptr);
    if (m_impl->loop == nullptr) {
        sinkLog(nullptr, "PipeWireSink: pw_thread_loop_new failed");
        return;
    }
    m_impl->context =
        pw_context_new(pw_thread_loop_get_loop(m_impl->loop), nullptr, 0);
    if (m_impl->context == nullptr) {
        sinkLog(nullptr, "PipeWireSink: pw_context_new failed");
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
        return;
    }
    if (pw_thread_loop_start(m_impl->loop) != 0) {
        sinkLog(nullptr, "PipeWireSink: pw_thread_loop_start failed");
        pw_context_destroy(m_impl->context);
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->context = nullptr;
        m_impl->loop    = nullptr;
        return;
    }

    pw_thread_loop_lock(m_impl->loop);
    m_impl->core = pw_context_connect(m_impl->context, nullptr, 0);
    if (m_impl->core == nullptr) {
        pw_thread_loop_unlock(m_impl->loop);
        sinkLog(nullptr,
                "PipeWireSink: could not connect to the PipeWire daemon");
        pw_thread_loop_stop(m_impl->loop);
        pw_context_destroy(m_impl->context);
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->context = nullptr;
        m_impl->loop    = nullptr;
        return;
    }
    pw_core_add_listener(m_impl->core, &m_impl->coreListener, &kCoreEvents,
                         m_impl.get());
    m_impl->registry =
        pw_core_get_registry(m_impl->core, PW_VERSION_REGISTRY, 0);
    if (m_impl->registry != nullptr) {
        pw_registry_add_listener(m_impl->registry, &m_impl->registryListener,
                                 &kRegistryEvents, m_impl.get());
        syncWait(m_impl.get(), kSyncTimeoutSec);  // globals flushed; metadata bound
        syncWait(m_impl.get(), kSyncTimeoutSec);  // metadata property replay flushed
    }
    pw_thread_loop_unlock(m_impl->loop);

    m_impl->connected = true;
    if (!m_impl->metadataBound) {
        // Not fatal: the graph rate then reads unknown (0) and the decision
        // machinery already treats that coherently; the measurement at first
        // process corrects the reporting regardless.
        sinkLog(nullptr,
                "PipeWireSink: settings metadata not found; graph rate unknown");
    }
}

PipeWireSink::~PipeWireSink() {
    close();
    if (!m_impl->connected) {
        return;
    }
    pw_thread_loop_lock(m_impl->loop);
    if (m_impl->metadataBound) {
        spa_hook_remove(&m_impl->metadataListener);
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(m_impl->metadata));
    }
    if (m_impl->defaultMetadataBound) {
        // The "default" metadata proxy (the default-sink watch) is bound in
        // onRegistryGlobal and must be destroyed here, or the pw_proxy leaks.
        // Mirror of the settings-metadata block above, and like it, this must
        // precede the registry destroy: the proxy was minted through that
        // registry.
        spa_hook_remove(&m_impl->defaultMetadataListener);
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(m_impl->defaultMetadata));
    }
    if (m_impl->registry != nullptr) {
        spa_hook_remove(&m_impl->registryListener);
        pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(m_impl->registry));
    }
    spa_hook_remove(&m_impl->coreListener);
    pw_core_disconnect(m_impl->core);
    pw_thread_loop_unlock(m_impl->loop);

    pw_thread_loop_stop(m_impl->loop);
    pw_context_destroy(m_impl->context);
    pw_thread_loop_destroy(m_impl->loop);
}

// ---------------------------------------------------------------------------
// Read the graph's capabilities, fresh: one sync round trip so any in-flight
// settings or default-device change has landed, then resolve the TARGET SINK
// NODE (the pinned node when one is set, else the one the "default" metadata
// names; the registry inventory locates it) and query its EnumFormat params
// for the rate spans the hardware genuinely advertises. canSwitchRate is true
// once a target node resolved, because node.force-rate can clock the graph to
// any advertised rate regardless of clock.allowed-rates. When no node resolves
// (headless session manager, exotic setup), fall back to the follow-the-graph
// shape: the graph's effective clock as a single span, no switching, which
// keeps every decision executable. A sink that
// never reached the daemon reports an unusable device, which the RateManager
// reads as "cannot switch, resample at the (unknown) current rate", the safe
// interpretation.
SinkCapabilities PipeWireSink::capabilities() const {
    SinkCapabilities caps;
    if (!m_impl->connected) {
        caps.deviceName    = "PipeWire unavailable (no daemon connection)";
        caps.currentRate   = 0;
        caps.canSwitchRate = false;
        return caps;
    }

    std::string            deviceLabel;
    std::vector<RateRange> deviceRates;

    pw_thread_loop_lock(m_impl->loop);
    syncWait(m_impl.get(), kSyncTimeoutSec);

    // Resolve the target node through the shared resolver (the
    // driver-format watch resolves through the SAME function, so what the
    // watch certifies is always the device these caps described). A set pin
    // that resolves to null means the pinned hardware vanished: degrade to
    // unusable caps rather than silently playing elsewhere, mirroring
    // CoreAudio.
    const PipeWireSinkImpl::SinkNodeEntry* target =
        resolveTargetEntry(m_impl.get());
    if (target == nullptr && !m_impl->targetNodeName.empty()) {
        pw_thread_loop_unlock(m_impl->loop);
        caps.deviceName    = "selected output device not found";
        caps.currentRate   = 0;
        caps.canSwitchRate = false;
        return caps;
    }

    if (target != nullptr) {
        deviceLabel = target->description;
        // Bind the node, collect its EnumFormat rate spans, unbind. The param
        // events arrive during the sync wait; collectingParams fences the
        // scratch vector against stragglers after we stop caring.
        auto* node = static_cast<struct pw_node*>(
            pw_registry_bind(m_impl->registry, target->id,
                             PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (node != nullptr) {
            struct spa_hook nodeListener{};
            m_impl->paramDiscreteRates.clear();
            m_impl->paramRangeRates.clear();
            m_impl->collectingParams = true;
            pw_node_add_listener(node, &nodeListener, &kNodeEvents, m_impl.get());
            pw_node_enum_params(node, 0, SPA_PARAM_EnumFormat, 0, 0xffffffffu,
                                nullptr);
            syncWait(m_impl.get(), kSyncTimeoutSec);
            m_impl->collectingParams = false;
            spa_hook_remove(&nodeListener);
            pw_proxy_destroy(reinterpret_cast<struct pw_proxy*>(node));
            // Discrete evidence wins when present (see the scratch-field
            // comment): an exact list outranks an interval. A node that
            // publishes only a Range (the on-board HDA does) advertises the
            // interval, and execute-and-verify guards each rate taken from
            // it.
            if (!m_impl->paramDiscreteRates.empty()) {
                std::ranges::sort(m_impl->paramDiscreteRates);
                for (const std::uint32_t r : m_impl->paramDiscreteRates) {
                    deviceRates.push_back(RateRange{.min = r, .max = r});
                }
            } else {
                deviceRates = m_impl->paramRangeRates;
            }
        }
    }
    pw_thread_loop_unlock(m_impl->loop);

    // The current rate the next decision must compare against is the LIVE
    // graph clock, not the settings metadata: while this sink holds a node
    // force, the metadata still shows the graph's OWN (unforced) rate, and
    // trusting it here is exactly how a boundary once decided "already at
    // 48000" while the hardware clocked a forced 44100. The measurement (io
    // position clock, still holding the outgoing rate during a transition
    // park) is the truth while open; the metadata is the honest answer only
    // for a closed sink.
    std::uint32_t graphRate = 0;
    if (m_impl->opened) {
        graphRate = m_impl->measuredRate.load(std::memory_order_relaxed);
    }
    if (graphRate == 0) {
        graphRate = effectiveGraphRate(m_impl.get());
    }
    caps.currentRate = graphRate;
    if (!deviceRates.empty()) {
        caps.deviceName    = deviceLabel;
        caps.rates         = deviceRates;
        caps.canSwitchRate = true;  // node.force-rate reaches every advertised span
    } else {
        // Fallback: the honest follow-the-graph shape. Advertise only its
        // current clock, plan no switches.
        caps.deviceName    = deviceLabel.empty() ? "PipeWire default output"
                                                 : deviceLabel;
        caps.canSwitchRate = false;
        if (graphRate != 0) {
            caps.rates.push_back(RateRange{.min = graphRate, .max = graphRate});
        }
    }
    return caps;
}

// ---------------------------------------------------------------------------
// The device list, straight from the live registry inventory, fresh
// after one sync round trip. id is the persistent node.name; the entry whose
// name the "default" metadata currently names is marked default.
std::vector<AudioDeviceInfo> PipeWireSink::enumerateDevices() const {
    std::vector<AudioDeviceInfo> out;
    if (!m_impl->connected) {
        return out;
    }
    pw_thread_loop_lock(m_impl->loop);
    syncWait(m_impl.get(), kSyncTimeoutSec);
    for (const auto& entry : m_impl->sinkNodes) {
        if (entry.name.empty()) {
            continue;  // unaddressable without a persistent name
        }
        AudioDeviceInfo info;
        info.id        = entry.name;
        info.name      = entry.description;
        info.isDefault = (entry.name == m_impl->defaultSinkName);
        out.push_back(std::move(info));
    }
    pw_thread_loop_unlock(m_impl->loop);
    return out;
}

// Pin by node.name (empty = follow the session manager's default). A
// pin that does not resolve in the current inventory is REFUSED rather than
// stored, mirroring CoreAudio: the engine surfaces the refusal and the
// previous resolution stays in force. Selection never touches the live
// stream; the engine drives the reopen.
bool PipeWireSink::selectDevice(const std::string& deviceId) {
    if (deviceId.empty()) {
        m_impl->targetNodeName.clear();
        return true;
    }
    if (!m_impl->connected) {
        return false;
    }
    bool found = false;
    pw_thread_loop_lock(m_impl->loop);
    syncWait(m_impl.get(), kSyncTimeoutSec);
    for (const auto& entry : m_impl->sinkNodes) {
        if (entry.name == deviceId) {
            found = true;
            break;
        }
    }
    pw_thread_loop_unlock(m_impl->loop);
    if (!found) {
        sinkLog(m_impl->logOut,
                "PipeWireSink: selectDevice: no sink node named '%s'",
                deviceId.c_str());
        return false;
    }
    m_impl->targetNodeName = deviceId;
    return true;
}

// ---------------------------------------------------------------------------
bool PipeWireSink::open(const AudioFormat& sourceFormat,
                        const RateDecision& decision, IPullSource* source) {
    if (!sourceFormat.isValid() || source == nullptr) {
        sinkLog(m_impl->logOut, "PipeWireSink: open() called with an invalid "
                                "format or null source");
        return false;
    }
    if (!m_impl->connected) {
        sinkLog(m_impl->logOut,
                "PipeWireSink: open() refused, no daemon connection");
        return false;
    }
    if (m_impl->opened) {
        close();
    }

    m_impl->source   = source;
    m_impl->channels = static_cast<std::size_t>(sourceFormat.channels);
    m_impl->working  = sourceFormat;
    m_impl->measuredRate.store(0, std::memory_order_relaxed);
    m_impl->negotiatedRate.store(0, std::memory_order_relaxed);
    m_impl->negotiatedChannels.store(0, std::memory_order_relaxed);

    // 1. Create the stream and connect it LIVE at the SOURCE format, disarmed
    // (the armed flag keeps process on silence until start()). Live because a
    // rate force only applies while the node runs in the graph (see the file
    // header); disarmed so no source pull happens before start(). The
    // graph negotiates buffers and, when its clock differs from the source
    // rate, interposes its resampler; either way the process callback sees
    // interleaved float32 at the source rate, the engine's canonical format.
    pw_thread_loop_lock(m_impl->loop);

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Music",
        PW_KEY_APP_NAME,       "rawform",
        PW_KEY_NODE_NAME,      "rawform",
        nullptr);
    // An explicit pin targets the stream at that node by persistent
    // name; without one, the session manager routes to the default (and
    // migrates the stream if the default changes mid-session).
    if (!m_impl->targetNodeName.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT,
                          m_impl->targetNodeName.c_str());
    }
    m_impl->openTargetName = m_impl->targetNodeName;
    // Rate switch execution: the decision's target rides on the
    // stream as node properties. node.rate is the polite suggestion (honored
    // when clock.allowed-rates permits); node.force-rate is the instrument of
    // policy, reclocking the graph regardless of allowed-rates for as long as
    // this node exists. The daemon lifts the force when the stream is
    // destroyed, so the repayment ledger CoreAudio needs has no PipeWire
    // counterpart: close() IS the repayment, and a client crash cannot strand
    // the graph.
    m_impl->forcedThisOpen = false;
    m_impl->forcedRate     = 0;
    if (decision.switchDevice && decision.deviceRate != 0) {
        pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", decision.deviceRate);
        pw_properties_setf(props, PW_KEY_NODE_FORCE_RATE, "%u",
                           decision.deviceRate);
        m_impl->forcedThisOpen = true;
        m_impl->forcedRate     = decision.deviceRate;
    }
    m_impl->stream = pw_stream_new(m_impl->core, "rawform", props);
    if (m_impl->stream == nullptr) {
        pw_thread_loop_unlock(m_impl->loop);
        sinkLog(m_impl->logOut, "PipeWireSink: pw_stream_new failed");
        m_impl->source = nullptr;
        return false;
    }
    pw_stream_add_listener(m_impl->stream, &m_impl->streamListener,
                           &kStreamEvents, m_impl.get());

    std::uint8_t podBuffer[1024];
    struct spa_pod_builder podb{};
    podb.data = podBuffer;
    podb.size = sizeof(podBuffer);

    struct spa_audio_info_raw raw{};
    raw.format   = SPA_AUDIO_FORMAT_F32;  // interleaved float32, the canonical format
    raw.rate     = sourceFormat.sampleRate;
    raw.channels = sourceFormat.channels;
    if (sourceFormat.channels == 1) {
        raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (sourceFormat.channels == 2) {
        raw.position[0] = SPA_AUDIO_CHANNEL_FL;
        raw.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        // Beyond stereo the engine carries no layout semantics (interleaved
        // order is the decoder's), so tell the graph honestly rather than
        // invent one.
        raw.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
    }
    const struct spa_pod* params[1] = {
        spa_format_audio_raw_build(&podb, SPA_PARAM_EnumFormat, &raw)};

    const int connectRes = pw_stream_connect(
        m_impl->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<enum pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (connectRes < 0) {
        pw_stream_destroy(m_impl->stream);
        m_impl->stream = nullptr;
        pw_thread_loop_unlock(m_impl->loop);
        sinkLog(m_impl->logOut, "PipeWireSink: pw_stream_connect failed (%d)",
                connectRes);
        m_impl->source = nullptr;
        return false;
    }

    // 2. Wait for negotiation: the live stream settles at PAUSED once the
    // session manager linked it and buffers exist, then STREAMING as the
    // driver picks it up; either state means negotiated. Bounded, like every
    // wait in the sinks; the state_changed callback signals the loop.
    bool negotiated = false;
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(kConnectTimeoutSec);
        for (;;) {
            const char* streamError = nullptr;
            const enum pw_stream_state st =
                pw_stream_get_state(m_impl->stream, &streamError);
            if (st == PW_STREAM_STATE_ERROR) {
                break;
            }
            if (st == PW_STREAM_STATE_PAUSED || st == PW_STREAM_STATE_STREAMING) {
                negotiated = true;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
            pw_thread_loop_timed_wait(m_impl->loop, 1);
        }
    }
    if (!negotiated) {
        pw_stream_destroy(m_impl->stream);
        m_impl->stream   = nullptr;
        m_impl->position = nullptr;
        pw_thread_loop_unlock(m_impl->loop);
        sinkLog(m_impl->logOut,
                "PipeWireSink: stream did not reach PAUSED (error or timeout)");
        m_impl->source = nullptr;
        return false;
    }
    // Bind the driver-format watch onto the resolved target while the
    // lock is still held. Emission stays disarmed until the reconcile at the
    // end of this open; the subscription's immediate Format delivery populates
    // driverFormatRate in the meantime. A failed arm degrades to graph-clock
    // truth only, worth a breadcrumb but never a failed open. The inventory
    // this resolves against is the construction-time registry subscription's
    // live view; the stream connect above already round-tripped the daemon,
    // so it is as fresh as the routing decision the daemon itself just made.
    const bool watchBound = armDriverFormatWatchLocked(m_impl.get());
    pw_thread_loop_unlock(m_impl->loop);
    if (!watchBound) {
        sinkLog(m_impl->logOut,
                "PipeWireSink: driver-format watch not armed (no resolvable "
                "target node); outcome verification is graph-clock only");
    }

    // 3. Execute-and-verify, mirroring CoreAudioSink's set-then-confirm shape.
    // A switching decision was already placed on the stream as properties; the
    // graph reclocks asynchronously, so confirm against the io position clock
    // and log the reality either way. A non-switching decision logs the same
    // line as macOS.
    std::uint32_t realizedRate = 0;
    if (m_impl->forcedThisOpen) {
        sinkLog(m_impl->logOut,
                "PipeWireSink: forcing graph -> %u Hz (node.force-rate, "
                "bypasses clock.allowed-rates)",
                m_impl->forcedRate);
        realizedRate = pollGraphRate(m_impl.get(), m_impl->forcedRate,
                                     kRateConfirmTimeoutMs);
        if (realizedRate == m_impl->forcedRate) {
            sinkLog(m_impl->logOut, "PipeWireSink: graph confirmed at %u Hz",
                    realizedRate);
        } else {
            sinkLog(m_impl->logOut,
                    "PipeWireSink: graph rate unconfirmed after %d ms "
                    "(observed %u Hz; the node is live, so this is a slow or "
                    "refused driver reclock; the live measurement will tell)",
                    kRateConfirmTimeoutMs, realizedRate);
        }
    } else {
        sinkLog(m_impl->logOut,
                "PipeWireSink: no graph switch (target %u Hz, %s)",
                decision.deviceRate,
                decision.resampleNeeded ? "resample expected"
                                        : "already at rate");
        // Not commanding a reclock, but still MEASURING: read the live clock
        // against the decision's expectation with the short poll, so the seed
        // below is reality even when the metadata and the graph disagree.
        realizedRate = pollGraphRate(m_impl.get(), decision.deviceRate,
                                     kRateReadTimeoutMs);
    }

    // 4. Seed the measurement with the best truth available: the confirmed
    // realized rate when the switch verified; the last observed or the
    // graph's own effective clock otherwise. The process callback refreshes
    // it from the io position clock once frames flow. Seeding here means the
    // engine's measurement-first outcome publication, which runs before
    // start(), reads reality rather than falling back to the prediction.
    std::uint32_t graphRate = realizedRate;
    if (graphRate == 0) {
        graphRate = effectiveGraphRate(m_impl.get());
    }
    if (graphRate != 0) {
        m_impl->measuredRate.store(graphRate, std::memory_order_relaxed);
    }
    sinkLog(m_impl->logOut,
            "PipeWireSink: source %u Hz, graph %u Hz -> %s",
            sourceFormat.sampleRate, graphRate,
            graphRate == sourceFormat.sampleRate ? "bit-perfect"
                                                 : "resampled by PipeWire");

    m_impl->opened = true;
    // The boundary is settled; arm the watch's emission and certify
    // any last-mile divergence the driver already shows. Ordered after opened
    // so a steady-state Format event landing right now is not dropped by the
    // handler's opened gate.
    reconcileDriverFormat(m_impl.get(), graphRate);
    return true;
}

// ---------------------------------------------------------------------------
// The in-place transition, PipeWire leg. The engine calls this with
// the stream open and parked (stop() disarmed and deactivated it). Instead of
// destroying the stream and connecting a fresh one, renegotiate the format in
// place and swap the node's rate force, then reactivate DISARMED (the same
// rule as open(): a suspended node forces nothing and updates nothing, so the
// confirmations below need the node running; armed stays false and process
// writes silence until the engine's start()). The method ends in
// exactly the state open() ends in, and both confirmations are bounded with
// decline on doubt: a false return sends the engine down close+open, which is
// always correct here because only the stream dies, never the daemon session.
bool PipeWireSink::reconfigure(const AudioFormat& sourceFormat,
                               const RateDecision& decision) {
    if (!m_impl->opened || m_impl->stream == nullptr || !m_impl->connected ||
        !sourceFormat.isValid()) {
        return false;
    }

    // Device pinning: the stream was connected under the pin it had
    // at open; a repin since then must take the close+open road so the new
    // target gets a genuinely fresh open. A default-device change while
    // UNPINNED declines nothing: the daemon migrates the live stream itself.
    if (m_impl->targetNodeName != m_impl->openTargetName) {
        sinkLog(m_impl->logOut,
                "PipeWireSink: reconfigure declined (output device selection "
                "changed); close+open fallback will re-target");
        return false;
    }

    const bool hadForce = m_impl->forcedThisOpen;

    pw_thread_loop_lock(m_impl->loop);

    // Quiet the driver-format watch through our own transition (the
    // reconcile at the end of this method re-arms it against the settled
    // graph rate). The watch stays BOUND: its subscription keeps
    // driverFormatRate current through the renegotiation, which is exactly
    // what the reconcile reads.
    m_impl->watchArmed     = false;
    m_impl->driverCertRate = 0;

    // 1. Renegotiate the stream format in place. Zero the negotiated readout
    // FIRST, so the confirmation poll below can only be satisfied by the fresh
    // negotiation; then push the new EnumFormat.
    m_impl->negotiatedRate.store(0, std::memory_order_relaxed);
    m_impl->negotiatedChannels.store(0, std::memory_order_relaxed);

    std::uint8_t podBuffer[1024];
    struct spa_pod_builder podb{};
    podb.data = podBuffer;
    podb.size = sizeof(podBuffer);

    struct spa_audio_info_raw raw{};
    raw.format   = SPA_AUDIO_FORMAT_F32;
    raw.rate     = sourceFormat.sampleRate;
    raw.channels = sourceFormat.channels;
    if (sourceFormat.channels == 1) {
        raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (sourceFormat.channels == 2) {
        raw.position[0] = SPA_AUDIO_CHANNEL_FL;
        raw.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        raw.flags = SPA_AUDIO_FLAG_UNPOSITIONED;
    }
    const struct spa_pod* params[1] = {
        spa_format_audio_raw_build(&podb, SPA_PARAM_EnumFormat, &raw)};
    if (pw_stream_update_params(m_impl->stream, params, 1) < 0) {
        pw_thread_loop_unlock(m_impl->loop);
        sinkLog(m_impl->logOut,
                "PipeWireSink: reconfigure: pw_stream_update_params failed; "
                "close+open fallback");
        return false;
    }

    // 2. Swap the rate force on the node. A switching decision installs the
    // new target; a non-switching one clears any force the outgoing track
    // held (0 disables), so the graph returns to its own rate for a
    // follow-the-graph track. node.rate mirrors the same intent as the polite
    // suggestion.
    char rateBuf[24];
    char forceBuf[16];
    const bool forcing = decision.switchDevice && decision.deviceRate != 0;
    if (forcing) {
        std::snprintf(rateBuf, sizeof(rateBuf), "1/%u", decision.deviceRate);
        std::snprintf(forceBuf, sizeof(forceBuf), "%u", decision.deviceRate);
    } else {
        std::snprintf(rateBuf, sizeof(rateBuf), "0");
        std::snprintf(forceBuf, sizeof(forceBuf), "0");
    }
    const struct spa_dict_item items[2] = {
        {.key = PW_KEY_NODE_RATE,       .value = rateBuf},
        {.key = PW_KEY_NODE_FORCE_RATE, .value = forceBuf},
    };
    const struct spa_dict dict = {.flags = 0, .n_items = 2, .items = items};
    pw_stream_update_properties(m_impl->stream, &dict);
    m_impl->forcedThisOpen = forcing;
    m_impl->forcedRate     = forcing ? decision.deviceRate : 0;

    // 3. Commit the working format under the lock (the loop thread is
    // excluded, so the disarmed process callback's stride math never sees a
    // torn update), then reactivate DISARMED so the renegotiation and the
    // reclock actually happen.
    m_impl->working  = sourceFormat;
    m_impl->channels = static_cast<std::size_t>(sourceFormat.channels);
    pw_stream_set_active(m_impl->stream, true);

    pw_thread_loop_unlock(m_impl->loop);

    if (hadForce && !forcing) {
        // Observed on 1.6.8: clearing the force does NOT proactively reclock a
        // busy graph; the driver holds its current rate until an idle recalc
        // or node removal. So no revert is claimed here; the read-poll in
        // step 5 reports whatever the graph actually does.
        sinkLog(m_impl->logOut,
                "PipeWireSink: reconfigure: node.force-rate cleared");
    }

    // 4. Confirm the renegotiated format, bounded: the param_changed handler
    // publishes what the graph agreed to, and anything other than exactly the
    // requested source shape is a decline (garbled stride is not a risk worth
    // keeping a stream for).
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(kSyncTimeoutSec);
        for (;;) {
            if (m_impl->negotiatedRate.load(std::memory_order_relaxed) ==
                    sourceFormat.sampleRate &&
                m_impl->negotiatedChannels.load(std::memory_order_relaxed) ==
                    sourceFormat.channels) {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                sinkLog(m_impl->logOut,
                        "PipeWireSink: reconfigure: format renegotiation "
                        "unconfirmed; close+open fallback");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // 5. Execute-and-verify the rate outcome, the same shape as open(): the
    // realized graph rate from the io position clock, the honest log either
    // way, the measurement seeded with the best truth available.
    std::uint32_t realizedRate = 0;
    if (forcing) {
        sinkLog(m_impl->logOut,
                "PipeWireSink: reconfigure: forcing graph -> %u Hz "
                "(node.force-rate)",
                m_impl->forcedRate);
        realizedRate = pollGraphRate(m_impl.get(), m_impl->forcedRate,
                                     kRateConfirmTimeoutMs);
        if (realizedRate == m_impl->forcedRate) {
            sinkLog(m_impl->logOut, "PipeWireSink: graph confirmed at %u Hz",
                    realizedRate);
        } else {
            sinkLog(m_impl->logOut,
                    "PipeWireSink: graph rate unconfirmed after %d ms "
                    "(observed %u Hz; the live measurement will tell)",
                    kRateConfirmTimeoutMs, realizedRate);
        }
    } else {
        sinkLog(m_impl->logOut,
                "PipeWireSink: reconfigure: no graph switch (target %u Hz, %s)",
                decision.deviceRate,
                decision.resampleNeeded ? "resample expected"
                                        : "already at rate");
        // Not commanding a reclock, but still measuring: read the live clock
        // against the decision's expectation, so the seed below reports what
        // the graph genuinely does (the cleared-force case above may leave it
        // holding the old forced rate).
        realizedRate = pollGraphRate(m_impl.get(), decision.deviceRate,
                                     kRateReadTimeoutMs);
    }

    std::uint32_t graphRate = realizedRate;
    if (graphRate == 0) {
        graphRate = effectiveGraphRate(m_impl.get());
    }
    if (graphRate != 0) {
        m_impl->measuredRate.store(graphRate, std::memory_order_relaxed);
    }
    sinkLog(m_impl->logOut,
            "PipeWireSink: source %u Hz, graph %u Hz -> %s",
            sourceFormat.sampleRate, graphRate,
            graphRate == sourceFormat.sampleRate ? "bit-perfect"
                                                 : "resampled by PipeWire");
    // Same reconcile as open(); the boundary's graph rate is settled,
    // so re-arm emission and certify any last-mile divergence now.
    reconcileDriverFormat(m_impl.get(), graphRate);
    return true;
}

// ---------------------------------------------------------------------------
void PipeWireSink::start() {
    if (m_impl->opened && !m_impl->started) {
        // The stream is already live from open() (see the file header), so the
        // arming is the real event here: the next process cycle pulls the
        // source instead of writing silence. set_active is a no-op on the
        // first start and the genuine reactivation on resume after a pause
        // (stop() deactivates).
        pw_thread_loop_lock(m_impl->loop);
        m_impl->armed.store(true, std::memory_order_relaxed);
        const int res = pw_stream_set_active(m_impl->stream, true);
        pw_thread_loop_unlock(m_impl->loop);
        if (res < 0) {
            sinkLog(m_impl->logOut,
                    "PipeWireSink: pw_stream_set_active(true) failed (%d)", res);
            m_impl->armed.store(false, std::memory_order_relaxed);
            return;
        }
        m_impl->started = true;
    }
}

void PipeWireSink::stop() {
    if (m_impl->started) {
        // The join guarantee (file header): taking the loop lock excludes the
        // loop thread from being mid-process, so the disarm below
        // happens-before every subsequent process invocation, which then
        // writes silence and never touches the source. Deactivation parks the
        // stream; any straggling process during suspension is disarmed.
        pw_thread_loop_lock(m_impl->loop);
        m_impl->armed.store(false, std::memory_order_relaxed);
        const int res = pw_stream_set_active(m_impl->stream, false);
        pw_thread_loop_unlock(m_impl->loop);
        if (res < 0) {
            sinkLog(m_impl->logOut,
                    "PipeWireSink: pw_stream_set_active(false) failed (%d)", res);
        }
        m_impl->started = false;
    }
}

void PipeWireSink::close() {
    stop();
    if (m_impl->stream != nullptr) {
        pw_thread_loop_lock(m_impl->loop);
        // The watch dies with the open it verified; disarming first
        // also zeroes driverFormatRate so a later open is never judged
        // against this device's final format.
        disarmDriverFormatWatchLocked(m_impl.get());
        pw_stream_disconnect(m_impl->stream);
        pw_stream_destroy(m_impl->stream);  // removes its listeners; no callbacks after this
        m_impl->stream   = nullptr;
        m_impl->position = nullptr;
        pw_thread_loop_unlock(m_impl->loop);
    }
    if (m_impl->forcedThisOpen) {
        // The force is a property of the node we just destroyed, so the daemon
        // lifts it here by construction and the graph returns to its own rate:
        // this close IS the repayment, no ledger required. (Probe-verified on
        // 1.6.8; if a future PipeWire regressed this, the ledger persistence
        // machinery is the fallback home.)
        sinkLog(m_impl->logOut,
                "PipeWireSink: node.force-rate (%u Hz) lifted with the stream; "
                "graph reverts to its own rate",
                m_impl->forcedRate);
        m_impl->forcedThisOpen = false;
        m_impl->forcedRate     = 0;
    }
    m_impl->opened = false;
    m_impl->source = nullptr;
    m_impl->measuredRate.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
AudioFormat PipeWireSink::currentFormat() const {
    return m_impl->working;
}

bool PipeWireSink::bitPerfect() const noexcept {
    const std::uint32_t measured =
        m_impl->measuredRate.load(std::memory_order_relaxed);
    // The graph clocking the source rate is necessary but no longer
    // sufficient; the driver's realized format (the watch's live fact) must
    // agree with the graph, or the last mile is resampling below the clock.
    // 0 means no watch or no fact yet, which honestly degrades to the
    // graph-only judgment this method always made.
    const std::uint32_t driver =
        m_impl->driverFormatRate.load(std::memory_order_relaxed);
    return measured != 0 && measured == m_impl->working.sampleRate &&
           (driver == 0 || driver == measured);
}

// ---------------------------------------------------------------------------
// Sink-console seams, mirroring CoreAudioSink. measuredDeviceRateHz is gated
// on opened so a closed sink honestly reports "no measurement" rather than a
// stale rate; while open it is the metadata-seeded, process-refreshed graph
// clock the stream is consumed at.
void PipeWireSink::setEventListener(ISinkEventListener* listener) {
    // Loop lock as the guard where a loop exists (the handlers read this with
    // the lock held); a degraded no-daemon sink has no handlers to race.
    if (m_impl->connected) {
        pw_thread_loop_lock(m_impl->loop);
        m_impl->events = listener;
        pw_thread_loop_unlock(m_impl->loop);
    } else {
        m_impl->events = listener;
    }
}

void PipeWireSink::setLogOutput(ILogOutput* out) {
    m_impl->logOut = out;
}

std::uint32_t PipeWireSink::measuredDeviceRateHz() const {
    if (!m_impl->opened) {
        return 0;
    }
    return m_impl->measuredRate.load(std::memory_order_relaxed);
}

}  // namespace rawform::audio
