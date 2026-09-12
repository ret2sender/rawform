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

// Engine.h
//
// The transport core of the rawform audio engine and the durable public
// surface the Qt AudioController wraps. The Engine owns the engine thread, a
// command queue, a transport state machine, and a play queue with automatic
// advance, and it drives any IAudioSink through a small internal real-time
// source.
//
// Three threads touch an Engine, and the split is the whole point:
//   1. The controlling thread (the CLI or the Qt UI) calls the transport
//      methods below. Every one of them is non-blocking: it enqueues a command
//      and returns. It also reads the observable atomics (state, currentFormat,
//      position, duration, the live bitrate, the device outcome).
//   2. The engine thread drains commands, runs the state machine, owns the
//      decoder and the ring, keeps the ring full, advances the play queue, and
//      fires Listener notifications. It is the ONLY thread that touches the sink.
//   3. The sink's real-time thread calls pull() on the engine's internal ring
//      source and nothing else; it never sees the command queue.
//
// Pimpl, like the decoders and the sinks: every thread, lock, queue, and
// atomic lives in Engine::Impl in the .cpp, so this header stays as clean as the
// rest of the public interface and exposes no <thread>/<mutex>/<atomic>.
//
// Lifetime: ~Engine() shuts the engine thread down, and that thread closes the
// sink (joining the RT thread) BEFORE any buffer it reads is destroyed, so the
// IAudioSink lifetime invariant holds without the caller ordering anything. See
// the teardown note in the .cpp.
//
// What the surface is, and is not. It carries everything the controller needs
// to wrap the engine: the transport commands including the playNow/setQueue
// primitives, output device selection, the observable atomics, the
// currentTrackInfo() pull observer for a late-attaching UI, the Listener
// callbacks, the master gain, and the visualization tap. Deliberately NOT here,
// because the engine stays a forward-queue transport with no cursor model: any
// persistent playlist, cursor, or history (previous) is a controller concern.
//
// Master volume: setVolumeGain/volumeGain are a LINEAR gain applied at the
// real-time pull chokepoint (EngineRingSource), not a transport command: a
// direct lock-free store, immediate in any state. They break the bit-perfect
// path BY DESIGN, but only when attenuating; at unity (1.0f) the chokepoint
// multiply is skipped and output stays byte-identical. The perceptual taper
// and any ReplayGain composition live above this surface, in the controller;
// the engine takes a bare linear gain. Delegating attenuation to the output
// device instead (the bit-perfect-preserving alternative) would be sink-level,
// per-platform mechanics and is outside this surface.

#pragma once

#include "rawform/audio/RateManager.h"  // RateMode
#include "rawform/audio/Types.h"

#include <memory>
#include <string>
#include <vector>  // setQueue's parameter

#include <cstddef>  // copyScope's frame-count type
#include <cstdint>  // liveBitrateKbps return type

namespace rawform::audio {

class  IAudioSink;       ///< injected output; forward-declared to keep this header light
struct IDecoderFactory;  ///< injected opener; default is the built-in DecoderFactory
                         // (a struct in IDecoderFactory.h; tag matched here to keep
                         //  -Wmismatched-tags quiet)

class Engine {
public:
    Engine();
    ~Engine();

    /// Not copyable or movable: it owns a running thread, a queue, and a sink
    /// referenced by a live RT callback. Moving it out from under those makes no
    /// sense.
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&)                 = delete;
    Engine& operator=(Engine&&)      = delete;

    // ----- configuration (call before the first play(); see the .cpp) ---------

    /// Inject the output device. Ownership transfers to the Engine on success. The
    /// sink is touched freely by the engine thread during playback, so injection
    /// is only legal while the engine is Stopped: any other state refuses, returns
    /// false, and the rejected sink is destroyed with the argument. Every
    /// current caller injects once, before the first play(), and may ignore the
    /// return (deliberately not [[nodiscard]] for that reason); a caller that
    /// swaps sinks at runtime must stop first and must check. Choosing among a
    /// sink's OWN devices is not a swap: that is selectOutputDevice below, on
    /// the sink already injected. The CLI passes the platform sink for its
    /// build, EngineTest a NullSink.
    bool setSink(std::unique_ptr<IAudioSink> sink);

    /// Override how paths become decoders. Non-owning; must outlive the Engine.
    /// Defaults to the built-in format-dispatching DecoderFactory, so leaving it
    /// unset still plays every format the build supports; tests inject an
    /// in-memory factory. Passing nullptr restores the default.
    void setDecoderFactory(IDecoderFactory* factory);

    /// Choose how the engine reconciles each track's native rate with the output
    /// device. Defaults to RateMode::BitPerfectWhenAvailable, a "follow the track
    /// when the device can clock it, otherwise resample" policy. An atomic store,
    /// valid in any state: the engine thread reads it at each track start to
    /// consult the RateManager, and a change while a track is playing takes
    /// effect at the next boundary (that boundary re-decides under the new mode
    /// instead of stitching or holding the device). The CLI maps its
    /// --resample / --force-rate flags to this; the UI maps a settings toggle.
    void setRateMode(RateMode mode);

    // ----- output device selection ------------------------------------
    ///
    /// Both are commands: non-blocking, serviced on the engine thread (the only
    /// thread that touches the sink). requestOutputDevices() answers through
    /// Listener::onOutputDevices with the sink's current list. selectOutputDevice
    /// pins the output by PERSISTENT id (AudioDeviceInfo::id; the empty string
    /// returns to following the system default): while Stopped the pin simply
    /// parks on the sink for the next open, while Playing or Paused the engine
    /// performs a reopen-at-position boundary (full close of the old device,
    /// fresh open of the new one, seek back, Paused restored), so the change is
    /// audible immediately without losing the listening position.
    void requestOutputDevices();
    void selectOutputDevice(std::string deviceId);

    // ----- notifications, fired from the ENGINE thread, never the RT thread ----
    ///
    /// The listener is invoked on the engine thread, so an implementation that
    /// touches UI state must marshal to its own thread (the Qt layer does). The
    /// pointer is non-owning and must outlive the Engine; set it before play().
    struct Listener {
        virtual ~Listener() = default;
        virtual void onStateChanged(State /*state*/) {}
        virtual void onTrackChanged(const TrackInfo& /*track*/) {}
        virtual void onPositionChanged(double /*seconds*/) {}
        virtual void onError(const std::string& /*message*/) {}
        /// Informational diagnostics worth a console line but not an error: the
        /// output sink's device-negotiation lines (rate switches, measured
        /// bit-perfect outcomes, session-close restores, platform error
        /// breadcrumbs) forwarded verbatim from the sink's ILogOutput seam, plus
        /// the engine's own device-selection and external-change notices.
        /// Default no-op, so a listener that only wants transport events (the
        /// CLI's) ignores it; the sink also still writes stderr regardless.
        virtual void onInfo(const std::string& /*message*/) {}
        /// The reply to requestOutputDevices(): the sink's current
        /// device list, fired on the engine thread like everything above.
        /// Default no-op, additive for existing listeners.
        virtual void onOutputDevices(
            const std::vector<AudioDeviceInfo>& /*devices*/) {}
        /// Mid-session outcome republication: the device rate changed
        /// under a live track (an external assertion the sink reported), and
        /// this is the fresh truth the UI's suffix should show. Only fired
        /// between onTrackChanged announcements; each announcement carries its
        /// own outcome as before.
        virtual void onDeviceOutcomeChanged(std::uint32_t /*deviceRateHz*/,
                                            bool /*bitPerfect*/) {}
        /// The sink's restore ledger changed; all-zero means cleared.
        /// Persisted upstream for crash recovery. Engine thread, like everything
        /// here.
        virtual void onRateDebtChanged(const std::string& /*deviceId*/,
                                       std::uint32_t /*originalRateHz*/,
                                       std::uint32_t /*borrowedRateHz*/) {}
    };
    void setListener(Listener* listener);

    // ----- transport: controlling thread, non-blocking, each enqueues a command-

    /// Append a path to the play queue. Does NOT start playback; play() does.
    void enqueue(std::string path);

    /// Make `path` the current track immediately and play it. From Stopped
    /// this plays THAT path with no transient blip of a previously remembered track:
    /// it opens and starts `path` directly, where play() from Stopped would instead
    /// replay the remembered currentPath. While Playing or Paused it CUTS to `path`
    /// using the same hold-or-reconfigure machinery as next() (a click-free swap
    /// when the source format is identical, a brief reconfigure gap otherwise), and
    /// the result is always Playing. It does NOT touch the pending queue: pair it
    /// with setQueue() to set what plays after. An unopenable `path` is reported
    /// through onError and leaves the transport exactly as it was, current track and
    /// queue both undisturbed; an explicit request that cannot be honored never
    /// interrupts what is already playing. This is the "play this specific track
    /// now" primitive a playlist controller (the CLI or the Qt AudioController)
    /// needs for a double-click; the controller keeps the persistent
    /// playlist and cursor, the engine gains no cursor model.
    void playNow(std::string path);

    /// Atomically REPLACE the pending forward queue with `paths`. Does not
    /// disturb the current track; the new paths take effect at the next natural
    /// advance or next(). Where enqueue() appends one path, this replaces the whole
    /// queue in one command, and an empty vector clears it (like clearQueue). A
    /// controller pairs setQueue(tail) with playNow(front) to express "play this
    /// list starting here": both commands drain in the same engine-thread pass
    /// before any advance, so the pair is atomic with respect to the engine's own
    /// queue advance, which is why no fused play-list-from-index call is needed.
    void setQueue(std::vector<std::string> paths);

    /// Start or resume. From Paused, resumes instantly from the same spot (no ring
    /// flush). From Stopped, replays the REMEMBERED track from frame 0 if one
    /// exists (stop rewinds), otherwise pops and plays the next queued path. A
    /// no-op while already Playing.
    void play();

    /// Pause without flushing: the sink stops consuming, the decoder position and
    /// the buffered ring are held, so resume is instant. A no-op unless Playing.
    void pause();

    /// Halt and rewind: stop the sink (restoring the device rate), flush the ring,
    /// drop the live decoder, and return to Stopped, but REMEMBER the current
    /// track so the next play() restarts it from frame 0. The pending queue is
    /// preserved (use clearQueue to empty it). A no-op while already Stopped.
    void stop();

    /// Skip to the next queued track. Tears down the current one and plays the
    /// next pending path, or returns to Stopped if the queue is empty. A no-op
    /// while Stopped.
    void next();

    /// Empty the pending queue. Does not affect the currently playing track.
    void clearQueue();

    /// Reposition the CURRENT track so the next audio heard begins at `seconds`.
    /// Non-blocking like the rest: it enqueues a command the engine thread runs.
    /// Distinct from the stop-rewind path: seek repositions the LIVE decoder
    /// (decoder->seek), where stop+play opens a fresh decoder from frame 0. The
    /// target is clamped to [0, duration]; the transport state is preserved (a
    /// seek while Paused stays Paused and resumes from the new spot). A seek while
    /// Stopped, or on an unseekable source, is ignored; an unseekable source also
    /// reports through onError. See the seek handler in the .cpp for the full
    /// mechanics and edge cases.
    void seek(double seconds);

    // ----- master volume, control thread, lock-free, immediate -------

    /// Set the LINEAR master gain applied to every frame at the real-time pull
    /// chokepoint. Unlike the transport above this does NOT enqueue a command: it
    /// is a direct atomic store (volume must respond instantly, not wait behind the
    /// queue), valid in any state including Stopped and before the first play().
    /// 1.0f is unity, and at unity the multiply is skipped so output is a
    /// byte-identical passthrough (the bit-perfect path is intact at 100%); below
    /// 1.0f is attenuation and leaves the bit-perfect path by design. The engine
    /// takes a bare linear gain only: the perceptual percentage->gain taper and any
    /// ReplayGain composition belong to the controller. The caller owns the valid
    /// range; the engine does not clamp. Readable back via volumeGain().
    void setVolumeGain(float linearGain) noexcept;

    /// The current linear master gain (the value last set, default 1.0f). Readable
    /// from any thread.
    [[nodiscard]] float volumeGain() const noexcept;

    // ----- visualization tap (spectrum feed), control/GUI thread ---------------
    //
    // The engine's contribution to the spectrum analyzer: a lock-free tap on the
    // real-time pull chokepoint, the one portable place every output sample
    // crosses regardless of sink backend (CoreAudio or PipeWire). It is an
    // additive, read-only observer in the same spirit as liveBitrateKbps(): it
    // touches no transport state, no command queue, and no existing contract, so
    // the freeze of the transport surface stands. The DSP (windowing, FFT,
    // banding) lives one layer up in the
    // portable SpectrumAnalyzer; the engine only exposes the samples.

    /// Copy the most recent `frames` mono samples the output has produced into
    /// `out`, oldest first (out[frames-1] is the latest), downmixed to one channel
    /// and taken from the side of the master gain that setScopeSource selects. Zero-
    /// pads when fewer samples exist yet, and clamps `frames` to the internal tap
    /// capacity; returns the count written. Call from the GUI thread on the
    /// visualizer's timer, NEVER from the RT thread. Lock-free and safe in any
    /// state, including Stopped (it simply returns zeros once the tail drains).
    std::size_t copyScope(float* out, std::size_t frames) const noexcept;

    /// Choose whether the tap captures the signal as heard (ScopeSource::PostGain,
    /// the default, so the bars follow the volume fader) or the decoded source
    /// (ScopeSource::PreGain, so the bars ignore it). A lock-free store the RT thread
    /// reads once per pull; immediate, like setVolumeGain, and valid in any state.
    void setScopeSource(ScopeSource source) noexcept;

    /// The current tap source (the value last set, default PostGain). Readable from
    /// any thread.
    [[nodiscard]] ScopeSource scopeSource() const noexcept;

    // ----- observable state (atomics, readable from any thread) ----------------

    /// Current transport state.
    [[nodiscard]] State state() const noexcept;

    /// The interleaved float32 format the engine is currently pulling at, or an
    /// invalid (zeroed) AudioFormat when Stopped.
    [[nodiscard]] AudioFormat currentFormat() const noexcept;

    /// The playback position in seconds: the source time of the next sample to be
    /// heard, i.e. the real frames consumed by the sink divided by the sample
    /// rate. Monotonic while Playing, frozen while Paused, jumps on a seek, and
    /// reads 0.0 while Stopped. Device output latency is ignored (sub-buffer,
    /// irrelevant to a progress readout). Readable from any thread; the engine
    /// also pushes the same value through Listener::onPositionChanged on a fixed
    /// cadence while Playing, plus once on a seek and once on entering Pause.
    [[nodiscard]] double position() const noexcept;

    /// The track the engine currently has loaded, as a whole TrackInfo:
    /// identity, format, the descriptive source facts, total frames, and
    /// seekability. Returns a default (empty path, zeroed) TrackInfo while Stopped.
    /// This is the PULL companion to the onTrackChanged push: a controller or UI
    /// that attaches mid-playback can populate itself at once (track name, codec,
    /// bitrate, duration for the status line, scrubber enable from seekable) instead
    /// of waiting for the next track change to fire. Readable from any thread; it is
    /// published whenever onTrackChanged fires and reset when Stopped. Not noexcept:
    /// returning the TrackInfo copies its std::string. Never call it from the RT
    /// thread (it is not, and must not be, on the audio path).
    [[nodiscard]] TrackInfo currentTrackInfo() const;

    /// The current track's length in seconds, or 0.0 while Stopped or when the
    /// length is unknown. Derived from the same published facts position() reads, so
    /// the two form a matched lock-free pair for a progress bar: position() is the
    /// numerator, duration() the denominator. Equivalent to
    /// currentTrackInfo().durationSeconds() but without copying a string, which is
    /// why it earns a separate accessor for the hot UI binding. Readable from any
    /// thread.
    [[nodiscard]] double duration() const noexcept;

    /// The moment-to-moment decode bitrate in kbps for the status line:
    /// a variable-rate stream's value as it is right now, distinct from the fixed
    /// currentTrackInfo().source.bitrateKbps nominal. The engine refreshes it on the
    /// position cadence while Playing (and once on a seek or on entering Pause),
    /// from the active decoder's own per-frame measurement, falling back to the
    /// track nominal for sources that report no live figure (PCM). 0 while Stopped.
    /// Readable from any thread; pair it with position() inside onPositionChanged to
    /// drive the readout with no dedicated callback.
    [[nodiscard]] std::uint32_t liveBitrateKbps() const noexcept;

    /// The output device outcome for the current open, the
    /// pull pair behind the UI's "(Bit Perfect)" / "(Resampled to N Hz)" suffix.
    /// outputDeviceRateHz() is the rate the device is actually running at, and
    /// outputBitPerfect() whether that equals the source rate; both are published
    /// after each successful sink open, hold steady across gapless stitches,
    /// HOLD-CUTs, seeks, and pause (the open is unchanged through all of those),
    /// and clear on entering Stopped (device released: rate reads 0, bit-perfect
    /// false). Measurement-first: the sink's measured device rate
    /// (IAudioSink::measuredDeviceRateHz) is the authority when available, with
    /// the RateDecision's prediction as the fallback for sinks that cannot
    /// measure (NullSink). One packed atomic backs both
    /// reads, so they are individually lock-free and readable from any thread;
    /// read them inside onTrackChanged for values coherent with that track.
    [[nodiscard]] std::uint32_t outputDeviceRateHz() const noexcept;
    [[nodiscard]] bool          outputBitPerfect() const noexcept;

private:
    struct Impl;                   ///< engine thread, command queue, state machine
    std::unique_ptr<Impl> m_impl;  ///< dtor defined in the .cpp where Impl is complete
};

}  // namespace rawform::audio
