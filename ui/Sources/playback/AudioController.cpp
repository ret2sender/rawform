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

// AudioController.cpp
//
// Implementation of the Qt wrapper over the rawform_audio Engine. See the header
// for the threading model and the cursor/organic-advance design; this file is
// the mechanics. The shape, in one breath: build the platform sink and a Listener
// bridge in the ctor; every engine callback marshals to a GUI-thread applier; the
// transport verbs drive the engine through playNow/setQueue; and the cursor (a
// QPersistentModelIndex into the playing model) plus the lookahead projection
// keep playback following the playlist.

#include "playback/AudioController.h"

#include "utils/YamlFile.h"

#include "media/TrackData.h"
#include "media/ReplayGainTags.h"
#include "playlist/PlaylistModel.h"
#include "playlist/PlaylistTabs.h"
#include "paths/Paths.h"  // userConfigDir() for playback.yaml

#include "rawform/audio/Types.h"

// The platform sink. CoreAudio on macOS, PipeWire on Linux, and a silent
// NullSink as the last resort (a UNIX box where libpipewire-0.3 was missing at
// configure time, or any other platform), so every build links and runs. The
// RAWFORM_HAVE_* capability defines propagate PUBLIC from the linked
// rawform_audio library, and the sink headers live under that library's src/
// (added to this target's include path in CMake), exactly as the engine's own
// CLI reaches them.
#if RAWFORM_HAVE_COREAUDIO
#include "sinks/CoreAudioSink.h"
#elif RAWFORM_HAVE_PIPEWIRE
#include "sinks/PipeWireSink.h"
#else
#include "sinks/NullSink.h"
#endif

#include <QAbstractItemModel>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QItemSelectionModel>
#include <QtGlobal>
#include <QtMath>  // qRound

#include <yaml-cpp/yaml.h>

#include <cmath>   // std::pow for the volume taper, std::abs for the seek landing
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rawform {

namespace {

// playback.yaml lives under userConfigDir(), beside the column/custom-column
// config. It holds the playback state that survives a restart: master volume and
// mute, the ReplayGain settings, the output rate policy, and the output device
// selection (id and friendly name).
QString playbackSettingsPath() {
    return userConfigDir() + QStringLiteral("/playback.yaml");
}

// rate_ledger.yaml: exists exactly while a CoreAudio session holds a
// rate debt; a crash leaves it behind, and the next launch repays from it
// before the sink is injected. PipeWire never writes one (its repayment is
// structural), so on Linux this file simply never appears.
QString rateLedgerPath() {
    return userConfigDir() + QStringLiteral("/rate_ledger.yaml");
}

// The debounce window for coalescing a volume drag's many ticks into one write.
constexpr int kPersistDebounceMs = 300;

// Relative (keyboard) seeking, see seekBy. The throttle is the shortest spacing
// between two engine seeks from a held key: each one is a ring flush and
// re-prime, so 150 ms keeps a held Right at 6 to 7 seeks a second rather than
// one per auto-repeat, while the accumulated target still advances by a full
// step per repeat. The tolerance is how close a position report must land to
// the committed target to count as the seek having arrived (a tick that was
// already in flight before the command reports the OLD spot, a full step away);
// it mirrors the scrubber's own landing window. The backstop bounds how long
// the latch can hold if the landing is never observed (a slow seek, a report
// coalesced away), the same 600 ms the scrubber uses.
constexpr int    kSeekRepeatThrottleMs      = 150;
constexpr double kSeekLandToleranceSeconds  = 0.5;
constexpr int    kSeekLandBackstopMs        = 600;

// How far into a track the Previous button switches from "step back a track" to
// "restart the current track". Within the first few seconds Previous goes to the
// previous row; at or past this point it rewinds the current track to 0 instead,
// the familiar media-player behavior. Keyed off the playback POSITION, so a seek
// forward past this also makes Previous restart.
constexpr double kPreviousRestartThresholdSeconds = 5.0;

// Perceptual volume taper: gain = fraction ^ kVolumeTaperExponent. This is the
// single knob for how the slider feels; tune it by ear against a reference. What
// the 50% slider position becomes for a few values:
//   1.0 (linear)  -> 0.500  (-6 dB)
//   1.5           -> 0.354  (-9 dB)
//   2.0 (square)  -> 0.250  (-12 dB)   <- default; gentler, louder mids
//   2.5           -> 0.177  (-15 dB)
//   3.0 (cubic)   -> 0.125  (-18 dB)   <- too quiet at half travel
// Lower is louder through the middle of the travel (toward a straight linear
// fader); higher is steeper. The curve is smooth to true silence at 0 regardless,
// so the bottom of the slider still reaches quiet. Square law gives the expected
// half-slider feel; this one number is the whole tuning surface.
constexpr double kVolumeTaperExponent = 2.0;

// ReplayGain factory defaults: one source of truth, returned by the *Default
// reads and used for the Settings window's modified-marker and reset. The
// defaults are a deliberate no-op (Off, 0 dB pre-amps), so a fresh install plays
// exactly as if ReplayGain did not exist, bit-perfect path intact.
constexpr int    kRgModeDefault             = 0;     // ReplayGainOff
constexpr double kRgPreampDbDefault         = 0.0;
constexpr double kRgUntaggedPreampDbDefault = 0.0;
constexpr bool   kRgClipPreventionDefault   = true;
constexpr bool   kRgScanSkipExistingDefault = false;  // a fresh scan measures all

// Output rate policy factory default: bit-perfect on, which
// is also the engine's own construction default, so the first pushRateMode call
// is a semantic no-op on a fresh install and playback.yaml-less runs behave
// exactly as every build before the toggle existed.
constexpr bool   kBitPerfectDefault         = true;

// The linear ReplayGain factor for one track under the given settings. Off is 1.0.
// Track uses the track tags; Album uses the album tags and falls back to the track
// tags when a file carries no album gain (the standard behavior). A track with no
// usable gain for the chosen mode is "untagged" and gets only the untagged
// pre-amp, so 0 dB leaves it at unity and keeps the bit-perfect path. A tagged
// track gets gain + the tagged pre-amp, and when clip prevention is on and a peak
// is known the factor is capped at 1/peak so it cannot clip even at full master
// (master only attenuates from there, so the cap is master-independent). The tag
// parsing is shared with the Properties view via media/ReplayGainTags.h. Takes
// the PARSED values rather than a TrackData: the caller reads them
// once and keeps the same parse as its detachment snapshot, so the live path
// and the fallback path cannot diverge on parsing.
float computeReplayGainLinear(const replaygain::Values& v, int mode,
                              double preampDb, double untaggedPreampDb,
                              bool clipPrevention) {
    if (mode == 0)  // Off
        return 1.0f;

    std::optional<double> gainDb;
    std::optional<double> peak;
    if (mode == 2) {  // Album, with track fallback
        gainDb = v.albumGainDb ? v.albumGainDb : v.trackGainDb;
        peak   = v.albumPeak   ? v.albumPeak   : v.trackPeak;
    } else {  // Track
        gainDb = v.trackGainDb;
        peak   = v.trackPeak;
    }

    if (!gainDb) {
        // Untagged for this mode: only the untagged pre-amp applies.
        return static_cast<float>(std::pow(10.0, untaggedPreampDb / 20.0));
    }

    const double totalDb = *gainDb + preampDb;
    double linear = std::pow(10.0, totalDb / 20.0);
    if (clipPrevention && peak && *peak > 0.0) {
        const double cap = 1.0 / *peak;
        if (linear > cap)
            linear = cap;
    }
    return static_cast<float>(linear);
}

}  // namespace

// ===========================================================================
// Engine-thread -> GUI-thread bridge
// ===========================================================================
//
// Each Listener override runs on the engine thread. It snapshots its payload into
// GUI-thread-safe value types and posts a queued functor to the controller, which
// lives on the GUI thread, so the property/signal surface is only ever touched
// there. The QString/primitive snapshots mean nothing non-trivially-copyable
// crosses the queued call, and Qt6's invokeMethod(contextObject, functor, type)
// runs the functor in the controller's thread with no Q_ARG metatype ceremony.
//
// The raw m_owner pointer is safe: the controller owns this bridge and outlives
// it, and ~Engine (which runs before this bridge is destroyed, per the member
// order in the header) joins the engine thread, so no callback can fire after the
// owner starts going away. Any already-posted events are dropped by Qt when the
// owner QObject is destroyed.
struct EngineListenerBridge final : public rawform::audio::Engine::Listener {
    explicit EngineListenerBridge(AudioController* owner) : m_owner(owner) {}

    void onStateChanged(rawform::audio::State s) override {
        const int st = static_cast<int>(s);
        QMetaObject::invokeMethod(
            m_owner, [owner = m_owner, st] { owner->applyState(st); },
            Qt::QueuedConnection);
    }

    void onTrackChanged(const rawform::audio::TrackInfo& t) override {
        AudioController::EngineTrackFacts f;
        f.path            = QString::fromStdString(t.path);
        f.codec           = static_cast<int>(t.source.codec);
        f.bitrateKbps     = static_cast<int>(t.source.bitrateKbps);
        f.sampleRateHz    = static_cast<int>(t.format.sampleRate);
        f.channels        = static_cast<int>(t.format.channels);
        f.totalFrames     = static_cast<quint64>(t.totalFrames);
        f.seekable        = t.seekable;
        f.durationSeconds = t.durationSeconds();
        // Device outcome: lock-free engine reads, sampled
        // here on the engine thread so they are coherent with THIS track's
        // announcement (the publication happens-before announceTrack), the same
        // pattern the live bitrate rides in onPositionChanged below.
        f.deviceRateHz    = static_cast<int>(m_owner->m_engine.outputDeviceRateHz());
        f.bitPerfect      = m_owner->m_engine.outputBitPerfect();
        QMetaObject::invokeMethod(
            m_owner, [owner = m_owner, f] { owner->applyTrack(f); },
            Qt::QueuedConnection);
    }

    void onOutputDevices(
        const std::vector<rawform::audio::AudioDeviceInfo>& devices) override {
        // Convert on the engine thread (cheap, a handful of small strings),
        // marshal the finished QVariantList across, the onTrackChanged shape.
        QVariantList list;
        list.reserve(static_cast<qsizetype>(devices.size()));
        for (const rawform::audio::AudioDeviceInfo& d : devices) {
            QVariantMap m;
            m.insert(QStringLiteral("id"), QString::fromStdString(d.id));
            m.insert(QStringLiteral("name"), QString::fromStdString(d.name));
            m.insert(QStringLiteral("isDefault"), d.isDefault);
            list.push_back(m);
        }
        QMetaObject::invokeMethod(
            m_owner, [owner = m_owner, list] { owner->applyOutputDevices(list); },
            Qt::QueuedConnection);
    }

    // A mid-track device outcome republication (the world changed the
    // rate underneath a live track); same marshal shape as everything here.
    void onDeviceOutcomeChanged(std::uint32_t deviceRateHz,
                                bool bitPerfect) override {
        QMetaObject::invokeMethod(
            m_owner,
            [owner = m_owner, rate = static_cast<int>(deviceRateHz), bitPerfect] {
                owner->applyDeviceOutcome(rate, bitPerfect);
            },
            Qt::QueuedConnection);
    }

    // The ledger feed; persisted immediately (crash protection has no
    // patience for debouncing).
    void onRateDebtChanged(const std::string& deviceId, std::uint32_t originalRateHz,
                           std::uint32_t borrowedRateHz) override {
        QMetaObject::invokeMethod(
            m_owner,
            [owner = m_owner, id = QString::fromStdString(deviceId),
             orig = static_cast<quint32>(originalRateHz),
             borrowed = static_cast<quint32>(borrowedRateHz)] {
                owner->applyRateDebt(id, orig, borrowed);
            },
            Qt::QueuedConnection);
    }

    void onPositionChanged(double seconds) override {
        // Sample the live decode bitrate on the engine thread (a lock-free atomic
        // read) and carry it alongside the position to the GUI thread, so the
        // player bar's format line refreshes on the same 100 ms cadence with no
        // extra callback. The bridge is a friend, so m_engine is reachable here.
        const int liveKbps =
            static_cast<int>(m_owner->m_engine.liveBitrateKbps());
        QMetaObject::invokeMethod(
            m_owner,
            [owner = m_owner, seconds, liveKbps] {
                owner->applyPosition(seconds, liveKbps);
            },
            Qt::QueuedConnection);
    }

    void onError(const std::string& message) override {
        const QString m = QString::fromStdString(message);
        QMetaObject::invokeMethod(
            m_owner, [owner = m_owner, m] { owner->applyError(m); },
            Qt::QueuedConnection);
    }

    void onInfo(const std::string& message) override {
        const QString m = QString::fromStdString(message);
        QMetaObject::invokeMethod(
            m_owner, [owner = m_owner, m] { owner->applyInfo(m); },
            Qt::QueuedConnection);
    }

    AudioController* m_owner;
};

// ===========================================================================
// Construction / teardown
// ===========================================================================

AudioController::AudioController(QObject* parent) : QObject(parent) {
    // Build the output device and hand ownership to the engine. open() is
    // deferred to the first play(), so constructing the sink here is cheap and
    // touches no hardware. The rate policy is a Settings > Playback
    // boolean: bit-perfect when the device can clock the track (with
    // the nearest-best fallback when it cannot) by default, or never-touch-the-
    // device resampling when unchecked; loadPlaybackSettings() below restores
    // the saved choice and pushes it to the engine before the first play().
    // Device selection rides the same engine command path.
#if RAWFORM_HAVE_COREAUDIO
    auto platformSink = std::make_unique<rawform::audio::CoreAudioSink>();
#elif RAWFORM_HAVE_PIPEWIRE
    auto platformSink = std::make_unique<rawform::audio::PipeWireSink>();
#else
    auto platformSink = std::make_unique<rawform::audio::NullSink>();
#endif

    // Crash recovery, BEFORE the sink is injected and before any open.
    // A rate_ledger.yaml on disk means a previous session died holding a rate
    // debt; hand the triple to the fresh sink, which repays iff the device
    // still sits at the borrowed rate, and delete the file either way (repaid,
    // moot, or the device is gone: in every case the ledger is spent). The
    // console line is deferred one event-loop turn so the window wiring that
    // routes infoOccurred exists by the time it fires.
    {
        const QString ledger = rateLedgerPath();
        if (QFile::exists(ledger)) {
            QString msg;
            try {
                const YAML::Node root = YAML::LoadFile(ledger.toStdString());
                const std::string id =
                    root["device_id"] ? root["device_id"].as<std::string>()
                                      : std::string{};
                const std::uint32_t orig =
                    root["original_rate"] ? root["original_rate"].as<std::uint32_t>()
                                          : 0;
                const std::uint32_t borrowed =
                    root["borrowed_rate"] ? root["borrowed_rate"].as<std::uint32_t>()
                                          : 0;
                const bool repaid =
                    platformSink->repayStaleRateDebt(id, orig, borrowed);
                msg = repaid
                          ? QStringLiteral(
                                "crash recovery: restored the output device to "
                                "%1 Hz (a previous session left it at %2 Hz)")
                                .arg(orig)
                                .arg(borrowed)
                          : QStringLiteral(
                                "crash recovery: stale rate ledger discarded "
                                "(device absent, moved, or debt moot)");
            } catch (...) {
                msg = QStringLiteral(
                    "crash recovery: malformed rate ledger discarded");
            }
            QFile::remove(ledger);
            QTimer::singleShot(0, this, [this, msg] { emit infoOccurred(msg); });
        }
    }
    m_engine.setSink(std::move(platformSink));

    m_bridge = std::make_unique<EngineListenerBridge>(this);
    m_engine.setListener(m_bridge.get());

    // Lookahead rebuild coalescing (see the member comment). Zero-interval
    // single-shot: started N times in one event-loop turn, fires once after it.
    m_lookaheadCoalesce.setSingleShot(true);
    m_lookaheadCoalesce.setInterval(0);
    connect(&m_lookaheadCoalesce, &QTimer::timeout,
            this, &AudioController::recomputeLookahead);

    // Master volume. The persist timer coalesces a drag's rapid setVolume
    // calls into a single write a short while after the last change; it is
    // single-shot and re-armed on each change. Then load any saved volume/mute and
    // push the resulting gain to the engine BEFORE the first play(), so playback
    // starts at the user's level (and at unity, a true passthrough, when nothing
    // was saved). loadPlaybackSettings ends by calling pushGainToEngine.
    m_volumePersistTimer.setSingleShot(true);
    m_volumePersistTimer.setInterval(kPersistDebounceMs);
    connect(&m_volumePersistTimer, &QTimer::timeout,
            this, &AudioController::persistPlaybackSettingsNow);
    loadPlaybackSettings();

    // Relative seeking (see seekBy). The throttle's timeout is the trailing
    // edge: if presses accumulated past the committed target during the window,
    // commit once more (which opens the next window); otherwise the window
    // simply closes and the next press commits immediately again. The backstop
    // drops a latch whose landing never showed up, but only when no commit is
    // still owed (the throttle is shorter, so an owed commit always runs first
    // and re-arms this).
    m_seekThrottle.setSingleShot(true);
    m_seekThrottle.setInterval(kSeekRepeatThrottleMs);
    connect(&m_seekThrottle, &QTimer::timeout, this, [this] {
        if (m_seekOwed) {
            commitSeek();
        }
    });
    m_seekLandBackstop.setSingleShot(true);
    m_seekLandBackstop.setInterval(kSeekLandBackstopMs);
    connect(&m_seekLandBackstop, &QTimer::timeout, this, [this] {
        if (!m_seekOwed) {
            resetSeekLatch();
        }
    });
}

// Out-of-line so the unique_ptr to the (here-complete) EngineListenerBridge can
// be destroyed. Member order in the header guarantees m_engine is torn down
// (thread joined) before m_bridge. We flush any pending volume write first, while
// every member is still alive, so a level change made within the debounce window
// just before quitting is not lost (the timer would never have elapsed).
AudioController::~AudioController() {
    flushPlaybackSettings();
}

void AudioController::setPlaylistTabs(PlaylistTabs* tabs) { m_tabs = tabs; }

// Visualization tap forwarders (for SpectrumProvider). Both relay straight to the
// engine: copyScopeMono returns the latest mono output window from the lock-free
// tap, and setScopeSource selects which side of the master gain that tap captures
// (0 == post-gain/Output, the default; 1 == pre-gain/Source). The int maps
// value-for-value onto rawform::audio::ScopeSource, with a guard so a stray value
// falls back to the as-heard default rather than reaching the engine malformed.
std::size_t AudioController::copyScopeMono(float* out, std::size_t frames) const {
    return m_engine.copyScope(out, frames);
}

void AudioController::setScopeSource(int source) {
    const rawform::audio::ScopeSource s = (source == 1)
                                              ? rawform::audio::ScopeSource::PreGain
                                              : rawform::audio::ScopeSource::PostGain;
    m_engine.setScopeSource(s);
}

// ===========================================================================
// Property reads with logic
// ===========================================================================

double AudioController::progress() const {
    if (m_durationSeconds <= 0.0) {
        return 0.0;
    }
    const double p = m_positionSeconds / m_durationSeconds;
    return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
}

int AudioController::playingRow() const {
    // -1 while Stopped so the playlist drops its green now-playing marker and the
    // bar's prev/next disable, even though the cursor itself is kept (Stop is a
    // rewind: play() restarts THIS track). The cursor is the restart target and
    // the internal navigation anchor; this getter is only the VIEW of it, and the
    // view says "nothing playing" while Stopped.
    if (isStopped()) {
        return -1;
    }
    return m_cursor.isValid() ? m_cursor.row() : -1;
}

int AudioController::volumePercent() const {
    // The integer the readout shows. Rounds the 0..1 fraction to a whole percent;
    // shares volume's NOTIFY so the label and the bar never disagree.
    return qRound(m_volume * 100.0);
}

qreal AudioController::volumeTaperExponent() const {
    // The single source of truth for the curve, handed to the slider so its dB
    // tooltip reports the gain the engine actually applies.
    return kVolumeTaperExponent;
}

QString AudioController::formatSummary() const {
    if (!m_hasTrack) {
        return QString();
    }
    QString ch;
    if (m_channels == 1) {
        ch = QStringLiteral("Mono");
    } else if (m_channels == 2) {
        ch = QStringLiteral("Stereo");
    } else {
        ch = QStringLiteral("%1 ch").arg(m_channels);
    }
    // e.g. "FLAC 961kbps 44100Hz Stereo" (the player-bar status line). The bitrate is the
    // live, moment-to-moment figure while a track is being decoded (Feature A), frozen at
    // its last value on pause; it falls back to the nominal/average when stopped or
    // before the first position tick. displayBitrateKbps() owns that live-or-nominal
    // choice so the isolated digits the bar renders and this full string can never
    // disagree. The device-outcome suffix comes from outputSuffix() below, the single
    // source the player bar's split rendering also consumes, so the two surfaces cannot
    // disagree.
    const QString suffix = outputSuffix();
    return suffix.isEmpty()
               ? QStringLiteral("%1 %2kbps %3Hz %4")
                     .arg(m_codecName)
                     .arg(displayBitrateKbps())
                     .arg(m_sampleRateHz)
                     .arg(ch)
               : QStringLiteral("%1 %2kbps %3Hz %4 %5")
                     .arg(m_codecName)
                     .arg(displayBitrateKbps())
                     .arg(m_sampleRateHz)
                     .arg(ch)
                     .arg(suffix);
}

// The device-outcome suffix on its own (gain-truthful by design):
// "(Bit Perfect)" when the hardware is clocking the source rate
// AND the composed engine gain is exactly unity (the fast-path passthrough, so
// the delivered bytes really are the decoded bytes). A rate-matched device
// with a non-unity gain names the CAUSE, deliberately verbose so the remedy is
// obvious: "(Muted)", "(Volume Adjusted)", "(ReplayGain Adjusted)", or
// "(Volume + ReplayGain)". "(Resampled to N Hz)" with the actual device rate
// on a rate mismatch WINS over the gain state, because the resample is the
// bigger alteration and naming both would bury it. EMPTY while no device is
// open (Stopped), because there is no outcome to report then. Measured-first
// honesty comes from the engine's publication, so this never claims
// bit-perfect on a misclocking device; gain honesty comes from
// m_engineGainState, which pushGainToEngine classifies from the value it
// actually pushed and its components. No leading space: each consumer owns
// its own joining (formatSummary above, the player bar's format row tail).
QString AudioController::outputSuffix() const {
    if (m_deviceRateHz <= 0) {
        return QString();
    }
    if (!m_outputBitPerfect) {
        return QStringLiteral("(Resampled to %1Hz)").arg(m_deviceRateHz);
    }
    switch (m_engineGainState) {
        case GainState::Unity:
            return QStringLiteral("(Bit Perfect)");
        case GainState::Muted:
            return QStringLiteral("(Muted)");
        case GainState::Volume:
            return QStringLiteral("(Volume Adjusted)");
        case GainState::ReplayGain:
            return QStringLiteral("(ReplayGain Adjusted)");
        case GainState::VolumeAndReplayGain:
            return QStringLiteral("(Volume + ReplayGain)");
    }
    return QString();  // unreachable with the exhaustive switch above; keeps
                       // every compiler's missing-return analysis satisfied
}

// Display name for the engine's Codec tag. Mirrors the CLI's codecName so the two
// front ends agree; the engine keeps this mapping out of its zero-Qt library on
// purpose.
QString AudioController::codecToName(int codec) {
    switch (static_cast<rawform::audio::Codec>(codec)) {
        case rawform::audio::Codec::Pcm:    return QStringLiteral("PCM");
        case rawform::audio::Codec::Flac:   return QStringLiteral("FLAC");
        case rawform::audio::Codec::Mp3:    return QStringLiteral("MP3");
        case rawform::audio::Codec::Aac:    return QStringLiteral("AAC");
        case rawform::audio::Codec::Ac3:    return QStringLiteral("AC3");
        case rawform::audio::Codec::Dts:    return QStringLiteral("DTS");
        case rawform::audio::Codec::Opus:   return QStringLiteral("Opus");
        case rawform::audio::Codec::Vorbis: return QStringLiteral("Vorbis");
        case rawform::audio::Codec::Alac:   return QStringLiteral("ALAC");
        case rawform::audio::Codec::Wma:    return QStringLiteral("WMA");
        case rawform::audio::Codec::Other:  return QStringLiteral("Other");
        case rawform::audio::Codec::Unknown: break;
    }
    return QStringLiteral("Unknown");
}

// ===========================================================================
// GUI-thread appliers (queued from the bridge)
// ===========================================================================

void AudioController::applyState(int s) {
    if (s == m_state) {
        return;
    }
    // Whether this transition enters or leaves Stopped. The now-playing getters
    // (hasTrack, durationSeconds, nowPlayingTitle/Artist, and playingRow) all
    // suppress to "nothing playing" while Stopped, so crossing that boundary must
    // re-evaluate them. Captured against the OLD state, before the assignment.
    const bool crossesStopped = (s == Stopped) || (m_state == Stopped);

    m_state = s;
    if (s == Stopped) {
        // stop is a rewind; settle the readout to 0 so the progress bar empties
        // even though the engine won't push a position tick while Stopped.
        m_positionSeconds = 0.0;
        emit positionChanged();
        // Nothing to land on any more: a keyboard seek in flight is void.
        resetSeekLatch();
        // Drop the live bitrate so the format line reverts to the nominal while
        // stopped (and shows the nominal, not a stale live value, on the next
        // resume until the first tick refreshes it). The device outcome drops
        // with it: the device is released on Stopped, so the
        // bit-perfect/resampled suffix would be a claim about nothing.
        m_liveBitrateKbps  = 0;
        m_deviceRateHz     = 0;
        m_outputBitPerfect = false;
        emit formatSummaryChanged();
    }
    if (crossesStopped) {
        // Re-fire the now-playing facts so the Stopped-gated getters re-read.
        // trackChanged drives hasTrack/durationSeconds/title/artist; playingChanged
        // drives playingRow (the playlist's green marker and prev/next enablement).
        // The cursor and the stored facts are untouched: entering Stopped only
        // HIDES them, and leaving Stopped (a resume from Pause, or a restart that
        // also pushes a fresh track) reveals them again.
        emit trackChanged();
        emit playingChanged();
    }
    emit stateChanged();
}

void AudioController::applyTrack(const EngineTrackFacts& f) {
    // A start is confirmed only when the engine reports the very path we were
    // waiting on as current. Matching on the path (not merely "any track
    // changed") matters: on a failed open the engine may push an empty/other
    // track here, and clearing the guard on that would let the subsequent
    // applyError mistake the failure for a mid-playback error and leave the dead
    // cursor in place.
    if (!m_pendingPath.isEmpty() && f.path == m_pendingPath) {
        m_pendingPath.clear();
    }

    m_hasTrack        = !f.path.isEmpty();
    m_path            = f.path;
    m_codecName       = codecToName(f.codec);
    m_bitrateKbps     = f.bitrateKbps;
    m_sampleRateHz    = f.sampleRateHz;
    m_channels        = f.channels;
    m_seekable        = f.seekable;
    m_durationSeconds = f.durationSeconds;
    m_positionSeconds = 0.0;  // a fresh track starts at 0 until the first tick
    resetSeekLatch();         // a keyboard seek targeted the PREVIOUS track
    m_liveBitrateKbps = 0;    // show the new track's nominal until the first tick
    m_deviceRateHz     = f.deviceRateHz;   // device outcome, sampled with this track
    m_outputBitPerfect = f.bitPerfect;

    // Move the cursor onto the track that just became current. This handles both
    // the playAt-initiated start (the cursor is already here) and a gapless
    // organic advance (the cursor moves one row on), and refreshes the title/
    // artist from the new row. It is the authoritative confirmation of the
    // optimistic cursor playAt set.
    resolveCursorForPath(f.path);

    emit trackChanged();
    emit formatSummaryChanged();  // format line's NOTIFY is now separate from trackChanged
    emit positionChanged();  // progress depends on the new duration; reset
}

void AudioController::applyPosition(double seconds, int liveBitrateKbps) {
    m_positionSeconds = seconds;
    emit positionChanged();

    // The keyboard-seek landing: the committed target has been reached and no
    // further commit is owed, so the next step may measure from the live
    // position again. A report still owed a commit keeps the latch (the
    // accumulated target is ahead of where this landed).
    if (m_seekTarget && !m_seekOwed
        && std::abs(seconds - m_seekCommitted) <= kSeekLandToleranceSeconds) {
        resetSeekLatch();
    }

    // Refresh the format line only when the live figure actually moves,
    // so a steady CBR/PCM stream does not re-evaluate the binding every tick.
    if (liveBitrateKbps != m_liveBitrateKbps) {
        m_liveBitrateKbps = liveBitrateKbps;
        emit formatSummaryChanged();
    }
}

void AudioController::applyInfo(const QString& message) {
    // Informational only: no transport or cursor consequences, unlike
    // applyError below. Straight out to the console via MainWindow's routing.
    emit infoOccurred(message);
}

void AudioController::applyError(const QString& message) {
    qWarning("rawform audio: %s", qUtf8Printable(message));

    // A failed START (an explicit playAt whose open never confirmed via
    // applyTrack) must not leave the now-playing cursor parked on a track that is
    // not playing: that row would otherwise keep reading as playing and prev/next
    // would navigate from a phantom position until something else played. So when
    // a start was pending, detach the cursor here. m_pendingPath is the guard: it
    // is non-empty only between an attempted start and its confirmation, so a
    // mid-playback error (no pending start) leaves the cursor and the current
    // track untouched.
    //
    // We deliberately do NOT touch the engine queue or m_state: if the engine
    // auto-skips the bad track to the next queued one, the next applyTrack
    // re-attaches the cursor to whatever actually starts, and organic advance
    // keeps working. Only the visual cursor resets here.
    if (!m_pendingPath.isEmpty()) {
        m_pendingPath.clear();
        if (m_cursor.isValid()) {
            // Detaches the persistent index, clears the now-playing title/artist,
            // and emits playingChanged (playingRow -> -1, so the row drops its
            // green/marker and prev/next disable).
            setCursorRow(-1);
            // title/artist are NOTIFY trackChanged; setCursorRow only emitted
            // playingChanged, so emit trackChanged for those to clear in the UI.
            emit trackChanged();
        }
    }

    emit errorOccurred(message);
}

// ===========================================================================
// Cursor / organic advance
// ===========================================================================

void AudioController::setPlayingModel(PlaylistModel* model) {
    if (m_playingModel.data() == model) {
        return;
    }
    for (const QMetaObject::Connection& c : m_playingConnections) {
        QObject::disconnect(c);
    }
    m_playingConnections.clear();

    m_playingModel = model;

    if (model) {
        // Structural edits to the playing playlist: the cursor self-heals via its
        // persistent index, so we only re-derive the lookahead and refresh the
        // highlight. A reset or destruction detaches the cursor and drains the
        // queue (current track plays out, then Stopped); neither touches the
        // current track.
        m_playingConnections << connect(model, &QAbstractItemModel::rowsInserted,
                                        this, &AudioController::onPlayingRowsChanged);
        m_playingConnections << connect(model, &QAbstractItemModel::rowsRemoved,
                                        this, &AudioController::onPlayingRowsChanged);
        m_playingConnections << connect(model, &QAbstractItemModel::rowsMoved,
                                        this, &AudioController::onPlayingRowsChanged);
        // Column removals (the header menu's granular hideColumn) can
        // invalidate the cursor's column-0 anchor without touching its row;
        // re-pin it across the pair. Column inserts never invalidate a live
        // persistent index, so they need no connection.
        m_playingConnections << connect(model, &QAbstractItemModel::columnsAboutToBeRemoved,
                                        this, &AudioController::onPlayingColumnsAboutToBeRemoved);
        m_playingConnections << connect(model, &QAbstractItemModel::columnsRemoved,
                                        this, &AudioController::onPlayingColumnsRemoved);
        m_playingConnections << connect(model, &QAbstractItemModel::modelReset,
                                        this, &AudioController::onPlayingModelReset);
        m_playingConnections << connect(model, &QObject::destroyed,
                                        this, &AudioController::onPlayingModelDestroyed);
        // An in-place row refresh (refreshTrack, e.g. after a ReplayGain edit or a
        // "Reload info from file(s)") bumps dataRevision. The playing track's tags
        // may have changed under us, so recompute its RG factor and re-push the
        // composed gain. Cheap and idempotent; unrelated-row refreshes just recompute
        // the same value.
        m_playingConnections << connect(model, &PlaylistModel::dataRevisionChanged,
                                        this, &AudioController::refreshReplayGain);
    }
    emit playingChanged();
}

void AudioController::setCursorRow(int row) {
    PlaylistModel* m = m_playingModel.data();
    m_cursor = (m && row >= 0) ? QPersistentModelIndex(m->index(row, 0))
                               : QPersistentModelIndex();
    refreshNowPlayingMeta();
    emit playingChanged();
}

void AudioController::resolveCursorForPath(const QString& path) {
    PlaylistModel* m = m_playingModel.data();
    if (!m) {
        setCursorRow(-1);
        return;
    }

    const int cur = m_cursor.isValid() ? m_cursor.row() : -1;

    // 1) Already on this track: the playAt-initiated start, or a re-fire. Keeps
    //    the right row even when the same file appears twice in the playlist.
    if (cur >= 0 && m->trackFilePath(cur) == path) {
        setCursorRow(cur);
        return;
    }
    // 2) Organic advance to the immediate next row (the gapless common case).
    if (cur >= 0 && cur + 1 < m->rowCount() && m->trackFilePath(cur + 1) == path) {
        setCursorRow(cur + 1);
        return;
    }
    // 3) Fall back to the first row whose path matches.
    const int n = m->rowCount();
    for (int r = 0; r < n; ++r) {
        if (m->trackFilePath(r) == path) {
            setCursorRow(r);
            return;
        }
    }
    // 4) Not in this playlist (the cursor row was edited away): detach the
    //    highlight.
    setCursorRow(-1);
}

void AudioController::recomputeLookahead() {
    // A direct call supersedes any pending coalesced one: the rebuild about
    // to happen makes the scheduled rebuild redundant, and stopping the timer
    // here is what lets the ordering-critical callers stay synchronous without
    // ever double-posting the queue.
    m_lookaheadCoalesce.stop();

    std::vector<std::string> seq;

    // Queue-prefix ++ organic-tail (see the header): the prefix is empty by
    // contract, so the sequence is the organic tail alone.

    PlaylistModel* m = m_playingModel.data();
    if (m && m_cursor.isValid()) {
        const int start = m_cursor.row() + 1;
        const int n     = m->rowCount();
        if (n > start) {
            seq.reserve(static_cast<std::size_t>(n - start));
        }
        for (int r = start; r < n; ++r) {
            const QString p = m->trackFilePath(r);
            if (!p.isEmpty()) {
                seq.push_back(p.toStdString());
            }
        }
    }
    // setQueue replaces the pending queue atomically and never disturbs the
    // current track; an empty vector clears it (current finishes, then Stopped).
    m_engine.setQueue(std::move(seq));
}

void AudioController::refreshNowPlayingMeta() {
    QString title;
    QString artist;
    PlaylistModel* m = m_playingModel.data();
    if (m && m_cursor.isValid()) {
        if (const TrackData* td = m->trackAt(m_cursor.row())) {
            title  = td->title;
            artist = td->artists.isEmpty() ? QString()
                                           : td->artists.join(QStringLiteral(", "));
        }
    }
    m_nowPlayingTitle  = title;
    m_nowPlayingArtist = artist;
    // The playing track may have changed (or cleared), so recompute its RG factor
    // and re-push the composed gain. Cheap, and it keeps RG correct at every track
    // boundary including gapless seams (album mode is constant across an album, so
    // no audible step there; per-track seam skew is sub-perceptual by design).
    refreshReplayGain();
    // The caller emits trackChanged (these are NOTIFY trackChanged).
}

void AudioController::onPlayingRowsChanged() {
    // The persistent cursor already followed the edit; re-derive the tail so
    // what-plays-next tracks the new order, and refresh the highlight/meta in case
    // the cursor row shifted. The current track is untouched. The tail rebuild is
    // SCHEDULED, not run inline: a batch of structural signals in one turn
    // then costs one rebuild and one setQueue instead of one per signal. The
    // cheap meta refresh and the notifications stay immediate, so the UI reacts
    // on this very signal; only the O(n) queue projection defers to end of turn,
    // a window in which the engine cannot advance past its current track anyway
    // (an end-of-track seam is processed by the engine thread against the queue
    // it already holds, which this rebuild is about to replace either way).
    m_lookaheadCoalesce.start();
    refreshNowPlayingMeta();
    emit playingChanged();
    emit trackChanged();
}

void AudioController::onPlayingModelReset() {
    // A reset invalidates the persistent index. Detach the cursor and drain the
    // queue; the current track finishes and the engine settles to Stopped.
    m_cursor = QPersistentModelIndex();
    recomputeLookahead();  // empty tail -> setQueue([])
    refreshNowPlayingMeta();
    emit playingChanged();
    emit trackChanged();
}

void AudioController::onPlayingColumnsAboutToBeRemoved() {
    // Snapshot the cursor's row before columns go: removing the cursor's anchor
    // column (visual column 0, e.g. hiding the leftmost column from the header
    // menu) invalidates the persistent index even though the ROW is untouched.
    // Rows cannot change inside the begin/endRemoveColumns pair, so the raw int
    // is safe until onPlayingColumnsRemoved.
    m_cursorRowAcrossColumnOp = m_cursor.isValid() ? m_cursor.row() : -1;
}

void AudioController::onPlayingColumnsRemoved() {
    const int row = m_cursorRowAcrossColumnOp;
    m_cursorRowAcrossColumnOp = -1;
    if (row < 0 || m_cursor.isValid())
        return; // no cursor, or it survived (the removed column wasn't its anchor)

    PlaylistModel* m = m_playingModel.data();
    if (!m || row >= m->rowCount())
        return; // model gone under us; the destroyed/reset paths own that case

    // Re-pin to (row, column 0). Pure bookkeeping, not a state transition: the
    // row playback consumes never observably changed, so no playingChanged and
    // no lookahead rebuild (the tail is a row-space projection, column-blind).
    m_cursor = QPersistentModelIndex(m->index(row, 0));
}

void AudioController::onPlayingModelDestroyed() {
    // The playing tab was closed. The current track is NEVER interrupted (the
    // engine holds its own decoder); we only detach the highlight and drain the
    // queue so playback stops after the current track unless something is queued.
    m_playingModel = nullptr;
    m_cursor       = QPersistentModelIndex();
    m_playingConnections.clear();  // Qt already dropped them on destruction
    recomputeLookahead();          // no model -> setQueue([])
    refreshNowPlayingMeta();
    emit playingChanged();
    emit trackChanged();
}

// ===========================================================================
// Transport
// ===========================================================================

void AudioController::playAt(PlaylistModel* model, int row) {
    if (!model || row < 0 || row >= model->rowCount()) {
        return;
    }
    const QString path = model->trackFilePath(row);
    if (path.isEmpty()) {
        return;
    }

    setPlayingModel(model);  // switch organic source + rewire structural signals
    setCursorRow(row);       // optimistic; onTrackChanged confirms it
    recomputeLookahead();    // seed engine pending = organic tail after this row

    // Mark this as a start awaiting confirmation. applyTrack clears it when the
    // engine reports this exact path current; applyError reads it to know a
    // failed open should detach the (optimistic) cursor we just set.
    m_pendingPath = path;

    // setQueue (above) and playNow drain in the same engine-thread command pass,
    // so the pair is atomic with respect to the engine's own advance.
    m_engine.playNow(path.toStdString());
}

void AudioController::play() {
    if (m_state == Playing) {
        return;
    }
    if (m_state == Paused) {
        m_engine.play();  // resume in place, no flush
        return;
    }
    // Stopped. A live cursor means a track is loaded/remembered: restart it from
    // 0 deterministically through playAt (stop is a rewind, so 0 is correct, and
    // this re-seeds the tail rather than leaning on the engine's remembered-track
    // replay).
    if (m_playingModel && m_cursor.isValid()) {
        playAt(m_playingModel.data(), m_cursor.row());
        return;
    }
    // Nothing ever played: start the active tab from its current selection, else
    // its first row.
    if (m_tabs) {
        PlaylistModel* m = m_tabs->activeModel();
        if (m && m->rowCount() > 0) {
            int row = 0;
            if (QItemSelectionModel* sel = m_tabs->activeSelection()) {
                const QModelIndex ci = sel->currentIndex();
                if (ci.isValid()) {
                    row = ci.row();
                }
            }
            playAt(m, row);
        }
    }
}

void AudioController::pause() { m_engine.pause(); }

void AudioController::playPauseToggle() {
    if (m_state == Playing) {
        pause();
    } else {
        play();
    }
}

void AudioController::stop() {
    // Rewind and remember; the cursor stays so play() restarts this track and the
    // now-playing highlight remains visible while Stopped.
    m_engine.stop();
}

bool AudioController::stopIfPlayingAny(const QStringList& paths) {
    // See the header for the full contract and caveats. m_hasTrack covers
    // both Playing and Paused, exactly the states in which the engine's decoder
    // holds the file handle; while Stopped there is no decoder and a write is
    // unconditionally safe.
    if (!m_hasTrack || m_path.isEmpty() || !paths.contains(m_path)) {
        return false;
    }
    const QString name = QFileInfo(m_path).fileName();
    stop();
    emit warningOccurred(
        QStringLiteral("Stopped playback: writing tags to the playing track (%1)")
            .arg(name));
    return true;
}

void AudioController::next() {
    if (!m_playingModel || !m_cursor.isValid()) {
        return;
    }
    const int target = m_cursor.row() + 1;
    if (target < m_playingModel->rowCount()) {
        playAt(m_playingModel.data(), target);
    } else {
        // Past the end: stop.
        m_engine.stop();
    }
}

void AudioController::previous() {
    if (!m_playingModel || !m_cursor.isValid()) {
        return;
    }
    const int row = m_cursor.row();

    // At or past the threshold into the track, Previous rewinds the CURRENT track
    // to 0 rather than leaving it; within the first few seconds it steps to the
    // previous row. The decision keys off the playback position (m_positionSeconds,
    // the latest tick), so a seek forward past the threshold also makes Previous
    // restart, matching how most players behave.
    if (m_positionSeconds >= kPreviousRestartThresholdSeconds) {
        playAt(m_playingModel.data(), row);  // restart current from 0
        return;
    }

    // Within the threshold: step back one row. Clamp at the first row, where there
    // is nothing earlier, so Previous there simply restarts row 0.
    playAt(m_playingModel.data(), row > 0 ? row - 1 : 0);
}

void AudioController::seekSeconds(double seconds) {
    resetSeekLatch();  // the scrubber overrides any keyboard target in flight
    m_engine.seek(seconds);
}

void AudioController::seekBy(double deltaSeconds) {
    // The engine ignores a seek while Stopped and refuses one on an unseekable
    // source; bail here so no latch is ever armed for a seek that cannot land.
    if (m_state == Stopped || !m_seekable) {
        return;
    }

    // Step from the target in flight when there is one (the position readout
    // is stale until it lands), else from the live position. Clamp to the
    // track; a target at the end is a legitimate seek that lets the engine's
    // finished path advance, exactly like a scrub to the end.
    const double base   = m_seekTarget ? *m_seekTarget : m_positionSeconds;
    double       target = base + deltaSeconds;
    if (target < 0.0) {
        target = 0.0;
    }
    if (m_durationSeconds > 0.0 && target > m_durationSeconds) {
        target = m_durationSeconds;
    }

    // A press the clamp swallowed whole moves nothing: no latch, no engine seek
    // (a flush and re-prime to land where we already are), no badge.
    const double effective = target - base;
    if (qFuzzyIsNull(effective)) {
        return;
    }
    m_seekTarget = target;
    m_seekOwed   = true;
    emit seekStepped(effective);

    // Leading edge: nothing throttling, so this press seeks now. Inside a
    // window the target just accumulated; the window's timeout commits it.
    if (!m_seekThrottle.isActive()) {
        commitSeek();
    }
}

void AudioController::commitSeek() {
    m_seekCommitted = *m_seekTarget;
    m_seekOwed      = false;
    m_engine.seek(m_seekCommitted);
    m_seekThrottle.start();
    m_seekLandBackstop.start();
}

// Deliberately leaves the throttle running: it spaces PRESSES, so a seek that
// lands inside its window must not reopen the leading edge for the next
// auto-repeat, or a fast-landing engine would be back to one seek per repeat.
void AudioController::resetSeekLatch() {
    m_seekTarget.reset();
    m_seekOwed = false;
    m_seekLandBackstop.stop();
}

// ===========================================================================
// Master volume
// ===========================================================================

void AudioController::setVolume(qreal value) {
    const qreal v = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);

    // qFuzzyCompare via the +1.0 trick so it is reliable near 0; a stream of
    // identical drag ticks should not churn the gain push, notify, and persist.
    const bool levelMoved = !qFuzzyCompare(v + 1.0, m_volume + 1.0);
    const bool wasMuted   = m_muted;
    if (!levelMoved && !wasMuted) {
        return;
    }

    m_volume = v;

    // Interacting with the slider clears mute: the user is choosing a level to
    // hear, so staying silent would be surprising. Mute otherwise stays an
    // explicit toggle on the readout; this is the single implicit unmute.
    if (wasMuted) {
        m_muted = false;
    }

    pushGainToEngine();
    if (levelMoved) {
        emit volumeChanged();
    }
    if (wasMuted) {
        emit mutedChanged();
    }
    schedulePlaybackPersist();
}

void AudioController::setMuted(bool muted) {
    if (muted == m_muted) {
        return;
    }
    m_muted = muted;
    pushGainToEngine();  // mute forces the engine gain to 0; unmute restores the taper
    emit mutedChanged();
    schedulePlaybackPersist();
}

void AudioController::toggleMute() { setMuted(!m_muted); }

// ===========================================================================
// ReplayGain
// ===========================================================================

int   AudioController::replayGainModeDefault() const { return kRgModeDefault; }
qreal AudioController::replayGainPreampDbDefault() const { return kRgPreampDbDefault; }
qreal AudioController::replayGainUntaggedPreampDbDefault() const { return kRgUntaggedPreampDbDefault; }
bool  AudioController::replayGainClipPreventionDefault() const { return kRgClipPreventionDefault; }
bool  AudioController::replayGainScanSkipExistingDefault() const { return kRgScanSkipExistingDefault; }

void AudioController::setReplayGainMode(int mode) {
    const int clamped = (mode < 0) ? 0 : (mode > 2 ? 2 : mode);
    if (clamped == m_replayGainMode)
        return;
    m_replayGainMode = clamped;
    emit replayGainSettingsChanged();
    refreshReplayGain();        // recompute the factor and re-push the composed gain
    schedulePlaybackPersist();
}

void AudioController::setReplayGainPreampDb(qreal db) {
    if (qFuzzyCompare(db + 1.0, m_replayGainPreampDb + 1.0))
        return;
    m_replayGainPreampDb = db;
    emit replayGainSettingsChanged();
    refreshReplayGain();
    schedulePlaybackPersist();
}

void AudioController::setReplayGainUntaggedPreampDb(qreal db) {
    if (qFuzzyCompare(db + 1.0, m_replayGainUntaggedPreampDb + 1.0))
        return;
    m_replayGainUntaggedPreampDb = db;
    emit replayGainSettingsChanged();
    refreshReplayGain();
    schedulePlaybackPersist();
}

void AudioController::setReplayGainClipPrevention(bool on) {
    if (on == m_replayGainClipPrevention)
        return;
    m_replayGainClipPrevention = on;
    emit replayGainSettingsChanged();
    refreshReplayGain();
    schedulePlaybackPersist();
}

void AudioController::setReplayGainScanSkipExisting(bool on) {
    if (on == m_replayGainScanSkipExisting)
        return;
    m_replayGainScanSkipExisting = on;
    emit replayGainSettingsChanged();
    schedulePlaybackPersist();
    // Deliberately no refreshReplayGain / pushGainToEngine: this is a scan-time
    // preference and contributes nothing to the playing track's gain.
}

bool AudioController::bitPerfectDefault() const { return kBitPerfectDefault; }

void AudioController::setBitPerfect(bool on) {
    if (on == m_bitPerfect)
        return;
    m_bitPerfect = on;
    pushRateModeToEngine();
    emit bitPerfectChanged();
    schedulePlaybackPersist();
    // Deliberately no sink reopen: setRateMode is an atomic
    // the engine thread reads at each track start, so a playing track finishes
    // under the policy it started with and the next one obeys the new choice.
}

// ---------------------------------------------------------------------------
// Output device.

void AudioController::refreshOutputDevices() {
    // A command; the reply lands in applyOutputDevices via the bridge.
    m_engine.requestOutputDevices();
}

void AudioController::setOutputDeviceId(const QString& id) {
    if (id == m_outputDeviceId)
        return;
    m_outputDeviceId = id;
    // Capture the friendly name from the enumeration the picker showed. A
    // stale intent applied without the device present keeps the previously
    // remembered name (better than degrading to the raw id); the empty pin
    // clears it.
    if (id.isEmpty()) {
        m_outputDeviceName.clear();
    } else {
        for (const QVariant& v : m_outputDevices) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("id")).toString() == id) {
                m_outputDeviceName = m.value(QStringLiteral("name")).toString();
                break;
            }
        }
    }
    // The engine applies it live (reopen-at-position) when something is
    // playing, or parks it for the next open. A refusal (the device vanished
    // between enumeration and Apply) is logged by the engine and the previous
    // resolution stays in force; the INTENT above is remembered regardless
    // (see the header's Q_PROPERTY comment).
    m_engine.selectOutputDevice(id.toStdString());
    emit outputDeviceIdChanged();
    schedulePlaybackPersist();
}

// The ledger file. A cleared debt deletes it; an active one writes it
// whole through QSaveFile (the crash cannot half-write the very file that
// exists for crashes). GUI thread, tiny file, immediate by design.
void AudioController::applyRateDebt(const QString& deviceId,
                                    quint32 originalRateHz,
                                    quint32 borrowedRateHz) {
    const QString path = rateLedgerPath();
    if (deviceId.isEmpty() || originalRateHz == 0) {
        if (QFile::exists(path)) {
            QFile::remove(path);
        }
        return;
    }
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "device_id" << YAML::Value << deviceId.toStdString();
    out << YAML::Key << "original_rate" << YAML::Value << originalRateHz;
    out << YAML::Key << "borrowed_rate" << YAML::Value << borrowedRateHz;
    out << YAML::EndMap;

    QByteArray content;
    content += "# rawform rate ledger. This file exists while the audio device\n";
    content += "# sits at a borrowed sample rate; it is deleted when the debt is\n";
    content += "# repaid or forgiven. Found at launch, it means a previous\n";
    content += "# session crashed: rawform restores the original rate iff the\n";
    content += "# device still sits at the borrowed one.\n";
    content += out.c_str();
    content += '\n';

    yamlfile::writeAtomically(path, content);
}

// The mid-track suffix update. Guarded on an outcome being shown at
// all: a republication racing a stop (applyTrack or the stop path already
// cleared the suffix) must not resurrect one for a track that is gone.
void AudioController::applyDeviceOutcome(int deviceRateHz, bool bitPerfect) {
    if (m_deviceRateHz <= 0 || deviceRateHz <= 0) {
        return;
    }
    if (m_deviceRateHz == deviceRateHz && m_outputBitPerfect == bitPerfect) {
        return;  // no visible change
    }
    m_deviceRateHz     = deviceRateHz;
    m_outputBitPerfect = bitPerfect;
    emit formatSummaryChanged();
}

void AudioController::applyOutputDevices(const QVariantList& devices) {
    m_outputDevices = devices;
    // Opportunistic name refresh: if the pinned device is in this enumeration
    // under a changed display name, follow it (and persist lazily with the
    // next settings write).
    if (!m_outputDeviceId.isEmpty()) {
        for (const QVariant& v : devices) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("id")).toString() == m_outputDeviceId) {
                const QString name = m.value(QStringLiteral("name")).toString();
                if (!name.isEmpty() && name != m_outputDeviceName) {
                    m_outputDeviceName = name;
                    emit outputDeviceIdChanged();
                    schedulePlaybackPersist();
                }
                break;
            }
        }
    }
    emit outputDevicesChanged();
}

void AudioController::pushOutputDeviceToEngine() {
    if (!m_outputDeviceId.isEmpty()) {
        m_engine.selectOutputDevice(m_outputDeviceId.toStdString());
    }
}

void AudioController::pushRateModeToEngine() {
    // The whole boolean-to-enum mapping: true is the engine's
    // default bit-perfect-with-fallback policy, false is never-touch-the-device.
    // ForceDeviceRate is unreachable from the UI by design; it is a garbled-
    // output diagnostic that only the CLI exposes.
    m_engine.setRateMode(m_bitPerfect
                             ? rawform::audio::RateMode::BitPerfectWhenAvailable
                             : rawform::audio::RateMode::AlwaysResample);
}

void AudioController::refreshReplayGain() {
    // Resolve the PLAYING track (not the active-tab selection) and compute its RG
    // factor under the current settings. Unity when Off, when nothing is playing,
    // or when the track is untagged with a 0 dB untagged pre-amp, which is exactly
    // the set of cases that must stay bit-perfect at master 100%.
    //
    // Detachment survival: the playlist row is only a VIEW of the
    // playing track; the engine owns the decode. So a successful row lookup
    // snapshots the parsed RG values keyed by the row's file path, and a FAILED
    // lookup (tab closed, model reset, playing row deleted) recomputes from that
    // snapshot as long as the engine's confirmed current track still matches the
    // key, instead of audibly stripping the factor from a track mid-flight. The
    // snapshot is taken REGARDLESS of mode, so turning RG on while detached
    // still knows the playing track's tags. The path key is the staleness guard:
    // once m_path moves on to a track the snapshot was not read for, the
    // fallback yields unity exactly as an unknown track should, and the next
    // successful lookup overwrites the snapshot. During the optimistic start
    // window the cursor already points at the incoming row, so the live branch
    // computes the incoming track's factor there, exactly as before this fix
    // (see the gapless seam note in refreshNowPlayingMeta).
    bool resolved = false;
    if (PlaylistModel* m = m_playingModel.data(); m && m_cursor.isValid()) {
        if (const TrackData* td = m->trackAt(m_cursor.row())) {
            m_playingRgValues = replaygain::read(td->extraTags);
            m_playingRgPath   = td->filePath;
            resolved          = true;
        }
    }
    float linear = 1.0f;
    if (m_replayGainMode != ReplayGainOff &&
        (resolved ||
         (!m_playingRgPath.isEmpty() && m_playingRgPath == m_path))) {
        linear = computeReplayGainLinear(m_playingRgValues, m_replayGainMode,
                                         m_replayGainPreampDb,
                                         m_replayGainUntaggedPreampDb,
                                         m_replayGainClipPrevention);
    }
    m_replayGainLinear = linear;
    pushGainToEngine();
}

void AudioController::pushGainToEngine() {
    // The composed engine gain: the master taper times the cached ReplayGain
    // factor. A linear slider feels wrong (the audible change bunches at the bottom
    // of the travel), so the master fraction maps through a power curve, gain =
    // fraction ^ kVolumeTaperExponent (see the constant for the curve). The RG
    // factor is 1.0 whenever RG is Off or the playing track contributes nothing
    // (refreshReplayGain owns it), so when both are unity at master 100% the engine
    // sees exactly 1.0f and skips its multiply: the bit-perfect passthrough is
    // intact unless the user has actually asked for attenuation or normalization.
    // Mute is a hard override to 0 that leaves both stored states untouched.
    const double v     = m_volume;
    const auto taper = static_cast<float>(std::pow(v, kVolumeTaperExponent));
    const auto gain  = m_muted ? 0.0f : (taper * m_replayGainLinear);
    m_engine.setVolumeGain(gain);

    // Gain truthfulness: classify the value just pushed so the
    // suffix can name WHY the output is not bit-perfect, and republish on a
    // classification CHANGE only, not on every drag tick (a drag inside one
    // state changes the gain constantly but never the claim, and
    // formatSummaryChanged at drag rate would churn the format line's bindings
    // for nothing). Order matters: the actual pushed value governs Unity, so
    // the exotic composition where an RG boost and a volume attenuation cancel
    // to exactly 1.0f honestly claims bit-perfect (the engine skips the
    // multiply; the bytes are the decoder's). Mute is checked next because a
    // muted push is 0.0f regardless of the stored components. The remaining
    // three states split on WHICH component departs from unity; both-unity
    // cannot reach them (1.0f * 1.0f is exactly 1.0f, so it classified Unity
    // above). Exact float equality throughout is deliberate: it is the same
    // test the engine's pull path uses to skip the multiply (std::pow(1.0, e)
    // is exactly 1.0, so full-volume-unmuted-unity-RG lands on Unity
    // reliably). The emission is gated on an outcome being on display,
    // mirroring applyDeviceOutcome: while Stopped there is no claim to
    // correct, and the state silently carried here is read fresh by
    // outputSuffix() when the next track's applyTrack fires the NOTIFY anyway.
    const bool volumeUnity = (taper == 1.0f);
    const bool rgUnity     = (m_replayGainLinear == 1.0f);
    GainState  state       = GainState::Unity;
    if (gain != 1.0f) {
        if (m_muted) {
            state = GainState::Muted;
        } else if (!volumeUnity && !rgUnity) {
            state = GainState::VolumeAndReplayGain;
        } else if (!volumeUnity) {
            state = GainState::Volume;
        } else {
            state = GainState::ReplayGain;
        }
    }
    if (state != m_engineGainState) {
        m_engineGainState = state;
        if (m_deviceRateHz > 0) {
            emit formatSummaryChanged();
        }
    }
}

void AudioController::loadPlaybackSettings() {
    // A missing or unreadable file is the normal first-run case: the defaults
    // (full volume, unmuted) simply stand. A malformed file is logged and ignored,
    // again falling back to defaults rather than failing.
    const auto text = yamlfile::readAll(playbackSettingsPath());
    if (text) {
        try {
            const YAML::Node root = YAML::Load(*text);
            if (root["volume"]) {
                const auto saved = root["volume"].as<double>();
                m_volume = saved < 0.0 ? 0.0 : (saved > 1.0 ? 1.0 : saved);
            }
            if (root["muted"]) {
                m_muted = root["muted"].as<bool>();
            }
            if (root["replaygain_mode"]) {
                const int md = root["replaygain_mode"].as<int>();
                m_replayGainMode = (md < 0) ? 0 : (md > 2 ? 2 : md);
            }
            if (root["replaygain_preamp_db"])
                m_replayGainPreampDb = root["replaygain_preamp_db"].as<double>();
            if (root["replaygain_preamp_untagged_db"])
                m_replayGainUntaggedPreampDb = root["replaygain_preamp_untagged_db"].as<double>();
            if (root["replaygain_clip_prevention"])
                m_replayGainClipPrevention = root["replaygain_clip_prevention"].as<bool>();
            if (root["replaygain_scan_skip_existing"])
                m_replayGainScanSkipExisting = root["replaygain_scan_skip_existing"].as<bool>();
            if (root["bit_perfect"])
                m_bitPerfect = root["bit_perfect"].as<bool>();
            if (root["output_device"])
                m_outputDeviceId =
                    QString::fromStdString(root["output_device"].as<std::string>());
            if (root["output_device_name"])
                m_outputDeviceName = QString::fromStdString(
                    root["output_device_name"].as<std::string>());
        } catch (const YAML::Exception& e) {
            qWarning("rawform: ignoring malformed %s (%s)",
                     qUtf8Printable(playbackSettingsPath()), e.what());
            m_volume = 1.0;
            m_muted  = false;
            m_replayGainMode             = kRgModeDefault;
            m_replayGainPreampDb         = kRgPreampDbDefault;
            m_replayGainUntaggedPreampDb = kRgUntaggedPreampDbDefault;
            m_replayGainClipPrevention   = kRgClipPreventionDefault;
            m_replayGainScanSkipExisting = kRgScanSkipExistingDefault;
            m_bitPerfect                 = kBitPerfectDefault;
            m_outputDeviceId.clear();
            m_outputDeviceName.clear();
        }
    }
    // Apply the loaded (or default) state to the engine before the first play().
    // No notify here: this runs in the ctor before QML binds, and the property
    // reads return the members directly.
    pushGainToEngine();
    pushRateModeToEngine();
    pushOutputDeviceToEngine();
}

void AudioController::schedulePlaybackPersist() {
    // (Re)arm the single-shot debounce. A drag of many ticks keeps restarting it,
    // so only the final value, kPersistDebounceMs after the last change, is
    // written. The timer timeout and flushPlaybackSettings (teardown) are the two
    // ways persistPlaybackSettingsNow runs.
    m_volumePersistTimer.start();
}

void AudioController::flushPlaybackSettings() {
    // Force the pending write now (the timer is armed but has not elapsed) so a
    // change made just before quitting is not lost. A no-op when nothing pends.
    if (m_volumePersistTimer.isActive()) {
        m_volumePersistTimer.stop();
        persistPlaybackSettingsNow();
    }
}

void AudioController::persistPlaybackSettingsNow() {
    // Mirrors the column-config writer: ensure the dir, emit YAML, write through a
    // QSaveFile so a crash mid-write cannot truncate the existing file.
    const QString   path = playbackSettingsPath();

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "volume" << YAML::Value << m_volume;
    out << YAML::Key << "muted"  << YAML::Value << m_muted;
    out << YAML::Key << "replaygain_mode" << YAML::Value << m_replayGainMode;
    out << YAML::Key << "replaygain_preamp_db" << YAML::Value << m_replayGainPreampDb;
    out << YAML::Key << "replaygain_preamp_untagged_db" << YAML::Value << m_replayGainUntaggedPreampDb;
    out << YAML::Key << "replaygain_clip_prevention" << YAML::Value << m_replayGainClipPrevention;
    out << YAML::Key << "replaygain_scan_skip_existing" << YAML::Value << m_replayGainScanSkipExisting;
    out << YAML::Key << "bit_perfect" << YAML::Value << m_bitPerfect;
    out << YAML::Key << "output_device" << YAML::Value
        << m_outputDeviceId.toStdString();
    out << YAML::Key << "output_device_name" << YAML::Value
        << m_outputDeviceName.toStdString();
    out << YAML::EndMap;

    QByteArray content;
    content += "# rawform - playback settings (generated by the app).\n";
    content += "# volume: master level, 0.0..1.0 (the slider fraction, not a gain).\n";
    content += "# muted:  true silences output while remembering volume.\n";
    content += "# replaygain_mode: 0 off, 1 track, 2 album.\n";
    content += "# replaygain_preamp_db:          dB added to tagged tracks.\n";
    content += "# replaygain_preamp_untagged_db: dB applied to tracks with no RG info.\n";
    content += "# replaygain_clip_prevention:    cap the RG factor at 1/peak.\n";
    content += "# output_device: persistent device id to pin output to; empty follows the system default.\n";
    content += "# output_device_name: the pinned device's display name, for showing it while disconnected.\n";
    content += "# replaygain_scan_skip_existing: skip files/albums with RG info when scanning.\n";
    content += out.c_str();
    content += '\n';

    yamlfile::writeAtomically(path, content);
}

}  // namespace rawform
