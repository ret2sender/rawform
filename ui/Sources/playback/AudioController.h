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

// AudioController.h
//
// The Qt face of the standalone rawform_audio Engine. The engine is a zero-Qt,
// forward-queue transport with no cursor model; this QObject wraps it so QML can drive
// playback and bind to its state, and it adds the one thing the engine deliberately does
// NOT own, the cursor that makes playback follow a playlist.
//
// THREE THREADS, as the engine header spells out. The engine's Listener fires on
// the engine thread, never the GUI thread. We marshal every callback to the GUI
// thread through a small private bridge (EngineListenerBridge, defined in the
// .cpp) so the Q_PROPERTY/signal surface is only ever touched on the thread QML
// lives on. The transport calls below are non-blocking (each enqueues an engine
// command and returns), so calling them straight from a QML handler is safe.
//
// THE CURSOR AND ORGANIC ADVANCE. The persistent playlist already lives in
// PlaylistModel (per tab, owned by PlaylistTabs); duplicating it here would make
// two sources of truth that desync on the first edit. So the controller owns
// only the cursor: a QPointer to the PLAYING model (which can differ from the
// active tab, exactly so you can switch tabs and edit other playlists while audio
// keeps playing) plus a QPersistentModelIndex into it. The persistent index rides
// the same begin/endMove|Insert|RemoveRows machinery the selection model rides,
// so the cursor self-heals across edits and invalidates on a reset for free.
//
// The engine's pending queue is then a pure projection: the paths of the rows
// AFTER the cursor in the playing model. We re-derive and re-push it (setQueue,
// which by the engine's contract never disturbs the current track) whenever the
// cursor moves by an explicit gesture or the playing playlist changes
// structurally. That is what makes the expected behaviors fall out: reordering
// follows, clearing the playing playlist stops it after the current track, and
// closing the playing tab never interrupts the current track. The current track
// is untouchable through any of this; only what-plays-next changes.
//
// QUEUE SHAPE. The "what plays next" derivation in recomputeLookahead() is
// queue-prefix ++ organic-tail; an explicit cross-playlist play queue (an "Add
// to queue" command and a queue view) is outside this controller's contract,
// so the prefix is empty and only the organic tail exists.
//
// Master volume lands here: the volume property below is the
// UI source of truth, the power-law percentage->gain taper and the mute live in
// the .cpp, the resulting LINEAR gain is pushed to the engine
// (Engine::setVolumeGain, a lock-free store at the pull chokepoint), and the
// level is persisted under userConfigDir(). ReplayGain is composed on top of
// that master gain in this same controller, and output-device selection is
// forwarded to the engine's device surface. A DeviceVolume mode that delegates
// attenuation to the device and keeps the bit-perfect path is outside the
// engine surface (its header says so) and therefore outside this one. The sink
// itself is built in the .cpp per platform: CoreAudioSink on macOS,
// PipeWireSink on Linux, and a silent NullSink elsewhere so every build links
// and runs.

#pragma once

#include "rawform/audio/Engine.h"  // value member: needs the complete type

// PlaylistModel is exposed as a Q_PROPERTY pointer (playingModel). moc emits
// metatype registration for that pointer, and Qt6 static-asserts the pointed-to
// type is COMPLETE at that point, so a forward declaration is not enough here:
// the full definition must be visible. (PlaylistTabs, used only as a plain
// pointer in no Q_PROPERTY, stays forward-declared below.)
#include "media/ReplayGainTags.h"  // replaygain::Values member (the playing track's RG snapshot)
#include "playlist/PlaylistModel.h"

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QQmlEngine>  // QML_ELEMENT / QML_UNCREATABLE
#include <QPersistentModelIndex>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariant> // QVariantList for the output device model

#include <cstddef>  // copyScopeMono's frame-count type
#include <memory>
#include <optional>  // the in-flight keyboard seek target

namespace rawform {

class PlaylistTabs;

/// Marshals engine-thread Listener callbacks onto the GUI thread. Defined in the
/// .cpp; held by unique_ptr so this header stays free of the bridge's detail, the
/// same pimpl shape the engine and decoders use.
struct EngineListenerBridge;

class AudioController : public QObject {
    Q_OBJECT

    /// Registered so QML can NAME the type (typed property declarations, and
    /// qmllint member checking on them); never instantiated from QML. The one
    /// instance is created in main.cpp and exposed as a context property.
    QML_ELEMENT
    QML_UNCREATABLE("AudioController is created in C++ and exposed as a context property")

    /// Transport state, mirrored from rawform::audio::State as an int plus three
    /// convenience bools so QML never needs the enum to drive a play/pause toggle.
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool isPlaying READ isPlaying NOTIFY stateChanged)
    Q_PROPERTY(bool isPaused READ isPaused NOTIFY stateChanged)
    Q_PROPERTY(bool isStopped READ isStopped NOTIFY stateChanged)

    /// The matched progress-bar pair, plus a precomputed 0..1 ratio for the fill.
    /// position() and duration() are lock-free engine atomics, but we drive these
    /// off the pushed onPositionChanged/onTrackChanged cadence so bindings update
    /// cleanly rather than polling.
    Q_PROPERTY(double positionSeconds READ positionSeconds NOTIFY positionChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY trackChanged)
    Q_PROPERTY(double progress READ progress NOTIFY positionChanged)

    /// Now-playing facts. The format/codec facts come from the engine's TrackInfo
    /// (the status line); the human title/artist come from the playing playlist
    /// row (the engine has no notion of them), resolved through the cursor.
    Q_PROPERTY(bool hasTrack READ hasTrack NOTIFY trackChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY trackChanged)
    Q_PROPERTY(QString codecName READ codecName NOTIFY trackChanged)
    Q_PROPERTY(int bitrateKbps READ bitrateKbps NOTIFY trackChanged)
    Q_PROPERTY(int sampleRateHz READ sampleRateHz NOTIFY trackChanged)
    Q_PROPERTY(int channels READ channels NOTIFY trackChanged)
    Q_PROPERTY(QString formatSummary READ formatSummary NOTIFY formatSummaryChanged)

    /// The device-outcome suffix on its own (gain-truthful by design):
    /// "(Bit Perfect)", "(Muted)", "(Volume Adjusted)",
    /// "(ReplayGain Adjusted)", "(Volume + ReplayGain)", "(Resampled to N Hz)",
    /// or empty while no device is open. The player bar renders the format line
    /// as SPLIT items (so the live kbps sits in a fixed-width slot), so it
    /// cannot consume formatSummary directly; this property is the piece it
    /// appends, and formatSummary composes the SAME string, keeping the two
    /// surfaces incapable of disagreeing (the displayBitrateKbps pattern).
    /// Shares formatSummary's NOTIFY because they change at exactly the same
    /// moments (track change, Stopped, a mid-track outcome republication, and
    /// the gain classification changing; pushGainToEngine fires the NOTIFY for
    /// that last one).
    Q_PROPERTY(QString outputSuffix READ outputSuffix NOTIFY formatSummaryChanged)
    /// The live bitrate the format line shows: the trailing-window figure while
    /// playing, the nominal otherwise. Exposed on its own (separate from the static
    /// bitrateKbps nominal) so the player bar can place the digits in a fixed-width
    /// slot and keep the rest of the line from reflowing as the value crosses a
    /// digit boundary. Shares formatSummary's NOTIFY, so the two update together.
    Q_PROPERTY(int displayBitrateKbps READ displayBitrateKbps NOTIFY formatSummaryChanged)
    Q_PROPERTY(QString nowPlayingTitle READ nowPlayingTitle NOTIFY trackChanged)
    Q_PROPERTY(QString nowPlayingArtist READ nowPlayingArtist NOTIFY trackChanged)

    /// The playing playlist and the cursor row within it, exposed so the playlist
    /// view can draw a now-playing marker. playingRow is -1 when there is no
    /// live cursor (detached or stopped).
    Q_PROPERTY(rawform::PlaylistModel* playingModel READ playingModel NOTIFY playingChanged)
    Q_PROPERTY(int playingRow READ playingRow NOTIFY playingChanged)

    /// Master volume. `volume` is the 0..1 slider FRACTION (the
    /// percentage over 100), the source of truth the slider binds to; volumePercent
    /// is the rounded integer for the readout; muted is an independent flag that
    /// silences output while remembering `volume`. The WRITE/slot path applies the
    /// perceptual taper, pushes the linear gain to the engine, and persists; see
    /// the .cpp. volumePercent shares volume's NOTIFY so the bar and the readout
    /// update together.
    Q_PROPERTY(qreal volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(int volumePercent READ volumePercent NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)

    /// The perceptual taper exponent (gain = fraction ^ exponent), exposed read-only
    /// and CONSTANT so the volume slider's dB tooltip computes the SAME dB the
    /// engine actually applies, from one source of truth (the constant lives in the
    /// .cpp). If the taper family ever changes from a power law, the tooltip's dB
    /// formula changes with it.
    Q_PROPERTY(qreal volumeTaperExponent READ volumeTaperExponent CONSTANT)

    /// ReplayGain. The applied state, composed with master volume into the
    /// single engine gain. Mode is the enum below (Off/Track/Album); the two pre-amps
    /// are in dB (tagged tracks vs tracks with no RG info); clip prevention caps the
    /// RG factor at 1/peak. These are the APPLIED values: the Settings window stages
    /// edits locally and calls the setters only on Apply/OK, so changing one here is
    /// already a commit (it recomputes the playing track's factor and persists). The
    /// *Default reads are the factory values, exposed so the Settings window can mark
    /// a non-default row and reset it from one source of truth. All four share one
    /// NOTIFY since they always travel together through a commit.
    Q_PROPERTY(int   replayGainMode READ replayGainMode WRITE setReplayGainMode NOTIFY replayGainSettingsChanged)
    Q_PROPERTY(qreal replayGainPreampDb READ replayGainPreampDb WRITE setReplayGainPreampDb NOTIFY replayGainSettingsChanged)
    Q_PROPERTY(qreal replayGainUntaggedPreampDb READ replayGainUntaggedPreampDb WRITE setReplayGainUntaggedPreampDb NOTIFY replayGainSettingsChanged)
    Q_PROPERTY(bool  replayGainClipPrevention READ replayGainClipPrevention WRITE setReplayGainClipPrevention NOTIFY replayGainSettingsChanged)
    /// Scan-time preference (not part of the applied gain): when on, a ReplayGain
    /// scan skips tracks (or whole albums) that already carry RG info. Shares the
    /// RG NOTIFY since it commits through the same Settings page.
    Q_PROPERTY(bool  replayGainScanSkipExisting READ replayGainScanSkipExisting WRITE setReplayGainScanSkipExisting NOTIFY replayGainSettingsChanged)
    Q_PROPERTY(int   replayGainModeDefault READ replayGainModeDefault CONSTANT)
    Q_PROPERTY(qreal replayGainPreampDbDefault READ replayGainPreampDbDefault CONSTANT)
    Q_PROPERTY(qreal replayGainUntaggedPreampDbDefault READ replayGainUntaggedPreampDbDefault CONSTANT)
    Q_PROPERTY(bool  replayGainClipPreventionDefault READ replayGainClipPreventionDefault CONSTANT)
    Q_PROPERTY(bool  replayGainScanSkipExistingDefault READ replayGainScanSkipExistingDefault CONSTANT)

    /// Output rate policy: the Settings > Playback boolean. Checked (true, the default)
    /// maps to the engine's BitPerfectWhenAvailable, switching the device to the track's
    /// rate when the hardware can clock it and taking the nearest-best advertised
    /// fallback when it cannot; unchecked maps to AlwaysResample, never touching the
    /// device rate. ForceDeviceRate, the third engine mode, is a CLI-only diagnostic and
    /// deliberately has no UI surface. Persisted in playback.yaml; takes effect at the
    /// next track boundary, because the engine reads its rate mode there (a natural
    /// advance or a manual cut) and Apply never reopens a playing sink. The *Default
    /// CONSTANT drives the pane's modified-from-default marker and right-click reset,
    /// exactly like the RG defaults above.
    Q_PROPERTY(bool bitPerfect READ bitPerfect WRITE setBitPerfect NOTIFY bitPerfectChanged)
    Q_PROPERTY(bool bitPerfectDefault READ bitPerfectDefault CONSTANT)

    /// Output device: the Settings > Playback picker's model and value.
    /// outputDevices is the last enumeration pushed by the engine (a list of
    /// {id, name, isDefault} maps; refreshOutputDevices() requests a fresh one).
    /// outputDeviceId is the applied selection INTENT, persisted in
    /// playback.yaml: the empty string (the default) follows the system's
    /// output choice, a non-empty id pins that device. Intent, because a
    /// remembered device can be unplugged: the engine then refuses (logged to
    /// the console) and keeps the previous resolution, while the intent stays
    /// remembered for when the device returns. Applying a change while playing
    /// moves the audio immediately at the kept position (the engine's
    /// reopen-at-position boundary).
    Q_PROPERTY(QVariantList outputDevices READ outputDevices NOTIFY outputDevicesChanged)
    Q_PROPERTY(QString outputDeviceId READ outputDeviceId NOTIFY outputDeviceIdChanged)
    /// The pinned device's FRIENDLY name, persisted beside the id and refreshed
    /// from each enumeration, so a remembered-but-absent device can be shown by
    /// its human name ("... (not connected)") rather than a raw platform id
    /// whose informative suffix would be elided away. Changes together with the
    /// id, hence the shared NOTIFY.
    Q_PROPERTY(QString outputDeviceName READ outputDeviceName NOTIFY outputDeviceIdChanged)
    Q_PROPERTY(QString outputDeviceIdDefault READ outputDeviceIdDefault CONSTANT)

public:
    /// Mirror of rawform::audio::State for QML. The bools above mean most QML never
    /// touches these names, but Q_ENUM keeps them available and self-documenting.
    enum PlaybackState { Stopped = 0, Playing = 1, Paused = 2 };
    Q_ENUM(PlaybackState)

    /// ReplayGain source mode, exposed to QML for the Settings selector. Off keeps
    /// the RG factor at unity (so master 100% stays bit-perfect); Track uses the
    /// track tags; Album uses the album tags, falling back to the track tags when a
    /// file carries no album gain. Integer-valued so it round-trips through
    /// playback.yaml and binds to a plain int property.
    enum ReplayGainMode { ReplayGainOff = 0, ReplayGainTrack = 1, ReplayGainAlbum = 2 };
    Q_ENUM(ReplayGainMode)

    explicit AudioController(QObject* parent = nullptr);
    ~AudioController() override;

    AudioController(const AudioController&)            = delete;
    AudioController& operator=(const AudioController&) = delete;

    /// The session manager, used only as the fallback source when play() is hit
    /// from Stopped with nothing ever played (start the active tab). Non-owning.
    void setPlaylistTabs(PlaylistTabs* tabs);

    // --- property reads ----------------------------------------------------
    [[nodiscard]] int     state() const { return m_state; }
    [[nodiscard]] bool    isPlaying() const { return m_state == Playing; }
    [[nodiscard]] bool    isPaused() const { return m_state == Paused; }
    [[nodiscard]] bool    isStopped() const { return m_state == Stopped; }
    [[nodiscard]] double  positionSeconds() const { return m_positionSeconds; }
    /// Reports 0 while Stopped. Stop clears the now-playing presentation, so the
    /// duration the seek bar and timing line read goes to 0 (the bar empties, the
    /// hover tooltip and seeking go inert). m_durationSeconds is left intact so a
    /// resume from Pause still has it; only the Stopped VIEW of it is zeroed.
    /// applyState re-fires trackChanged (this getter's NOTIFY) across the Stopped
    /// boundary, so the value re-reads as we enter or leave Stopped.
    [[nodiscard]] double  durationSeconds() const { return isStopped() ? 0.0 : m_durationSeconds; }
    [[nodiscard]] double  progress() const;
    /// False while Stopped even though m_hasTrack (the engine's last-opened fact)
    /// is still set. Stop clears the now-playing presentation, so the green format
    /// line and the timing line beside it (both gated on this) hide, matching the
    /// cleared green row in the playlist. The remembered track stays reachable
    /// through the cursor, so play() restarts it; only the VIEW says "nothing
    /// playing" here.
    [[nodiscard]] bool    hasTrack() const { return !isStopped() && m_hasTrack; }
    [[nodiscard]] bool    seekable() const { return m_seekable; }
    [[nodiscard]] QString codecName() const { return m_codecName; }
    [[nodiscard]] int     bitrateKbps() const { return m_bitrateKbps; }
    /// Live value the format line renders: the moment-to-moment figure once it is
    /// non-zero, else the nominal. Single source for that selection, shared with
    /// formatSummary() so the isolated digits and the full line never disagree.
    [[nodiscard]] int     displayBitrateKbps() const {
        return m_liveBitrateKbps > 0 ? m_liveBitrateKbps : m_bitrateKbps;
    }
    [[nodiscard]] int     sampleRateHz() const { return m_sampleRateHz; }
    [[nodiscard]] int     channels() const { return m_channels; }
    [[nodiscard]] QString formatSummary() const;
    [[nodiscard]] QString outputSuffix() const;
    /// Blank while Stopped, for the same reason as hasTrack: the now-playing
    /// identity is part of the presentation Stop clears. The stored strings stay
    /// (a resume from Pause keeps showing them); only the Stopped VIEW is blanked.
    [[nodiscard]] QString nowPlayingTitle() const { return isStopped() ? QString() : m_nowPlayingTitle; }
    [[nodiscard]] QString nowPlayingArtist() const { return isStopped() ? QString() : m_nowPlayingArtist; }
    [[nodiscard]] PlaylistModel* playingModel() const { return m_playingModel.data(); }
    [[nodiscard]] int     playingRow() const;
    [[nodiscard]] qreal   volume() const { return m_volume; }
    [[nodiscard]] int     volumePercent() const;
    [[nodiscard]] bool    muted() const { return m_muted; }
    [[nodiscard]] qreal   volumeTaperExponent() const;

    [[nodiscard]] int     replayGainMode() const { return m_replayGainMode; }
    [[nodiscard]] qreal   replayGainPreampDb() const { return m_replayGainPreampDb; }
    [[nodiscard]] qreal   replayGainUntaggedPreampDb() const { return m_replayGainUntaggedPreampDb; }
    [[nodiscard]] bool    replayGainClipPrevention() const { return m_replayGainClipPrevention; }
    [[nodiscard]] bool    replayGainScanSkipExisting() const { return m_replayGainScanSkipExisting; }
    [[nodiscard]] int     replayGainModeDefault() const;
    [[nodiscard]] qreal   replayGainPreampDbDefault() const;
    [[nodiscard]] qreal   replayGainUntaggedPreampDbDefault() const;
    [[nodiscard]] bool    replayGainClipPreventionDefault() const;
    [[nodiscard]] bool    replayGainScanSkipExistingDefault() const;
    [[nodiscard]] bool    bitPerfect() const { return m_bitPerfect; }
    [[nodiscard]] bool    bitPerfectDefault() const;

    /// Output device; see the Q_PROPERTY comment above.
    [[nodiscard]] QVariantList outputDevices() const { return m_outputDevices; }
    [[nodiscard]] QString      outputDeviceId() const { return m_outputDeviceId; }
    [[nodiscard]] QString      outputDeviceName() const { return m_outputDeviceName; }
    [[nodiscard]] QString      outputDeviceIdDefault() const { return QString(); }
    Q_INVOKABLE void           refreshOutputDevices();
    Q_INVOKABLE void           setOutputDeviceId(const QString& id);

    // --- visualization tap (for SpectrumProvider) --------------------------
    /// Two thin forwarders to the engine's spectrum tap, used by the standalone
    /// SpectrumProvider rather than QML, so they are plain C++ methods, not
    /// Q_PROPERTY or slots. copyScopeMono hands through the engine's lock-free
    /// copyScope (the latest mono output window); setScopeSource pushes the tap
    /// position (0 == post-gain/Output, 1 == pre-gain/Source) the provider owns and
    /// persists. The provider holds the source POLICY; this only relays the choice
    /// to the engine mechanism.
    [[nodiscard]] std::size_t copyScopeMono(float* out, std::size_t frames) const;
    void                      setScopeSource(int source);

public slots:
    // --- transport (invokable from QML) ------------------------------------

    /// Make @p model the playing playlist and @p row its current track, and play
    /// that track immediately (the double-click / Enter gesture). Seeds the engine
    /// with the organic tail after @p row, then cuts/starts via playNow. A no-op
    /// for a null model or an out-of-range/empty-path row.
    void playAt(rawform::PlaylistModel* model, int row);

    /// Start or resume. Playing: no-op. Paused: resume in place. Stopped with a
    /// live cursor: restart the cursor track from 0 (stop is a rewind). Stopped
    /// with nothing ever played: start the active tab from its current selection,
    /// else its first row.
    void play();

    /// Pause without flushing (instant resume). Toggle picks play vs pause.
    void pause();
    void playPauseToggle();

    /// Halt and rewind, remembering the cursor so play() restarts it.
    void stop();

    /// The tag-write interlock. If a track is loaded (playing OR paused; a
    /// paused decoder still holds the file handle) and its path is in @p paths,
    /// stop playback, post a warningOccurred line naming the file, and return
    /// true; otherwise return false with no side effects. Called by the write
    /// editors with the file set they are about to rewrite, because TagLib
    /// rewriting a file the engine is decoding can shift the audio bytes under
    /// the reader. Two honest caveats, both fine: the guard is per-path, not
    /// per-outcome (whether a save would have been padding-only is unknowable
    /// before TagLib::save runs, so any write to the loaded file stops), and
    /// stop() is an asynchronous posted command, so a write can begin while the
    /// engine thread is still tearing down; that overlap cannot harm the written
    /// file (a concurrent reader never endangers the writer), it only means the
    /// dying decoder may read torn bytes it will never play. Since stop rewinds
    /// and keeps the cursor, play() afterwards restarts the same track from the
    /// top, freshly reopened with the new tags.
    bool stopIfPlayingAny(const QStringList& paths);

    /// Organic navigation, computed from the cursor (mirrors the CLI's controller,
    /// not engine.next(), so the cursor stays authoritative). next past the end
    /// stops; previous restarts the current track from 0
    /// once the playback position is at or past the threshold (see the .cpp), and
    /// steps to the previous row (clamped at row 0) before that.
    void next();
    void previous();

    /// Reposition the current track. The scrubber that drives this interactively
    /// is the view's concern; the engine call lands here. An absolute seek also
    /// drops any relative seek still in flight (see seekBy), so the next step
    /// key measures from where the scrubber put playback, not from a stale
    /// keyboard target.
    void seekSeconds(double seconds);

    /// Reposition RELATIVE to where playback is heading, for the keyboard step
    /// keys. Two facts make this more than positionSeconds + delta. The engine
    /// seek is asynchronous, so positionSeconds keeps reporting the OLD spot
    /// until the engine has processed the command; and a held key auto-repeats
    /// at 25 to 30 presses a second, each of which would otherwise compute the
    /// same stale target and cost a full ring flush and re-prime. So the
    /// controller keeps the target of the seek in flight and steps from THAT
    /// while it has not landed (the latch clears when a position report lands
    /// within tolerance of the committed target, or on a backstop timer), and
    /// commits at most one engine seek per throttle window: the first press
    /// seeks at once, presses inside the window accumulate into the target,
    /// and the window's end commits the accumulated target if it moved. The
    /// target clamps to [0, duration]; reaching the end lets the engine's own
    /// finished path run, as a scrub to the end does. A no-op while Stopped or
    /// on an unseekable source. Playing or Paused both work; a Paused seek stays
    /// Paused at the new spot. Every step that moves the target emits
    /// seekStepped with the EFFECTIVE delta (after clamping), for the view's
    /// step badge.
    void seekBy(double deltaSeconds);

    /// Master volume. setVolume takes the 0..1 slider fraction, clamps it, applies
    /// the power-law taper, pushes the resulting linear gain to the engine, and
    /// schedules a debounced persist. setMuted/toggleMute silence or restore output
    /// without losing the stored level. All three update live and are invokable
    /// from QML (the slider's moved -> setVolume, the readout's click -> toggleMute,
    /// the wheel -> setVolume).
    void setVolume(qreal value);
    void setMuted(bool muted);
    void toggleMute();

    /// ReplayGain settings. Called by the Settings window on Apply/OK (not live).
    /// Each validates, stores, recomputes the playing track's RG factor, recomposes
    /// and pushes the engine gain, persists, and emits replayGainSettingsChanged.
    void setReplayGainMode(int mode);
    void setReplayGainPreampDb(qreal db);
    void setReplayGainUntaggedPreampDb(qreal db);
    void setReplayGainClipPrevention(bool on);
    /// Scan-time preference; stores, emits, and persists, but does NOT touch the
    /// engine gain (it never affects playback, only what a future scan measures).
    void setReplayGainScanSkipExisting(bool on);

    /// Output rate policy. Stores, maps the boolean onto the
    /// engine's RateMode (true -> BitPerfectWhenAvailable, false ->
    /// AlwaysResample), pushes it through Engine::setRateMode (an atomic store
    /// the engine thread reads at the next track start, so this is safe in any
    /// state including mid-playback), emits, and persists. No sink is reopened
    /// here: the running track finishes under the old policy.
    void setBitPerfect(bool on);

signals:
    void stateChanged();
    void positionChanged();
    void trackChanged();    ///< any now-playing fact changed (format, duration, title)
    /// The format line alone changed: fires on the live-bitrate cadence
    /// and on track/state changes. Kept SEPARATE from trackChanged on purpose: the
    /// player bar wipes its transient error text on trackChanged, so driving the
    /// 10 Hz live-bitrate refresh through trackChanged would erase errors instantly.
    void formatSummaryChanged();
    void playingChanged();  ///< playing model or cursor row changed
    /// A keyboard step (seekBy) moved the seek target by @p deltaSeconds, the
    /// effective amount after clamping to the track, so a view can show what
    /// the press actually did ("+5", or "+3" against the end). Not emitted for
    /// a press that moves nothing (Left at 0:00, Right at the end).
    void seekStepped(double deltaSeconds);
    void errorOccurred(const QString& message);
    /// A notable non-error event worth a line in the log console: stopping
    /// playback because a tag write targets the loaded file (stopIfPlayingAny,
    /// the one emitter). MainWindow routes it to LogStore at "warning" level,
    /// parallel to errorOccurred's "error".
    void warningOccurred(const QString& message);
    /// An informational engine/sink line for the log console: the output
    /// sink's device-negotiation diagnostics (rate switches, measured
    /// bit-perfect outcomes, close-time restores) and the ctor's crash-recovery
    /// notice for a stale rate ledger. MainWindow routes it to LogStore at
    /// "info" level, parallel to warningOccurred's "warning" and
    /// errorOccurred's "error".
    void infoOccurred(const QString& message);
    void volumeChanged();   ///< volume (and so volumePercent) changed
    void mutedChanged();    ///< mute flag toggled
    void replayGainSettingsChanged();  ///< any RG setting committed
    void bitPerfectChanged();          ///< output rate policy toggled
    void outputDevicesChanged();       ///< a fresh device enumeration arrived
    void outputDeviceIdChanged();      ///< the applied device selection changed

private:
    friend struct EngineListenerBridge;

    /// The engine-thread facts for one track, snapshotted to GUI-thread-safe value
    /// types at the bridge boundary (no std::string crosses the queued call). The
    /// bridge (a friend) builds this; applyTrack consumes it on the GUI thread.
    struct EngineTrackFacts {
        QString  path;
        int      codec          = 0;  ///< rawform::audio::Codec as int
        int      bitrateKbps    = 0;
        int      sampleRateHz   = 0;
        int      channels       = 0;
        quint64  totalFrames    = 0;
        bool     seekable       = false;
        double   durationSeconds = 0.0;
        /// The device outcome, sampled by the bridge from
        /// the engine's lock-free observers inside onTrackChanged (the same
        /// engine-thread-sampling pattern the live bitrate rides in
        /// onPositionChanged), so the pair is coherent with this very track.
        /// deviceRateHz 0 means "no open device"; the format line then shows no
        /// suffix.
        int      deviceRateHz   = 0;
        bool     bitPerfect     = false;
    };

    // --- GUI-thread appliers (invoked queued from the bridge) --------------
    void applyState(int state);
    void applyTrack(const EngineTrackFacts& facts);
    void applyPosition(double seconds, int liveBitrateKbps);
    void applyError(const QString& message);
    void applyInfo(const QString& message);
    void applyOutputDevices(const QVariantList& devices);
    void applyDeviceOutcome(int deviceRateHz, bool bitPerfect);
    void applyRateDebt(const QString& deviceId, quint32 originalRateHz,
                       quint32 borrowedRateHz); ///< Persist/clear the ledger

    // --- cursor / organic advance ------------------------------------------
    void setPlayingModel(rawform::PlaylistModel* model);  ///< swap source + rewire signals
    void setCursorRow(int row);                           ///< set cursor + refresh highlight/meta
    void resolveCursorForPath(const QString& path);       ///< predict-then-verify on advance
    void recomputeLookahead();                            ///< setQueue(organic tail after cursor)
    void refreshNowPlayingMeta();                         ///< title/artist from the cursor row

    // --- master volume + ReplayGain / persistence --------------------------
    /// Resolve the effective LINEAR gain and push it to the engine: the power-law taper
    /// of m_volume (kVolumeTaperExponent in the .cpp, the one definition of the curve),
    /// forced to 0 while muted, multiplied by the cached ReplayGain factor
    /// m_replayGainLinear. The single point that talks to Engine::setVolumeGain; called
    /// on every volume/mute/RG change and once at construction after the saved values
    /// load. Also the single point that CLASSIFIES the pushed gain: it derives
    /// m_engineGainState from the composed value and its components and fires
    /// formatSummaryChanged when the classification changes while a device outcome is on
    /// display, so the suffix follows the slider and the RG settings live.
    void pushGainToEngine();

    /// The single point that maps m_bitPerfect onto rawform::audio::RateMode and
    /// hands it to Engine::setRateMode; called once from loadPlaybackSettings()
    /// (before the first play(), so the very first track obeys the saved policy)
    /// and from setBitPerfect() on every committed change.
    void pushRateModeToEngine();

    /// Push the persisted device selection to the engine; called once
    /// from loadPlaybackSettings() before the first play(), so the very first
    /// open resolves the remembered device. A refusal (device not present) is
    /// logged by the engine and the intent stays remembered; see the
    /// Q_PROPERTY comment.
    void pushOutputDeviceToEngine();
    /// Recompute m_replayGainLinear from the PLAYING track's tags under the current
    /// RG settings (unity when mode is Off, there is no playing track, or the track
    /// is untagged with a 0 dB untagged pre-amp), then push the composed gain.
    /// Called on track change (via refreshNowPlayingMeta) and on any RG setting.
    /// Detachment-safe: every successful row lookup snapshots the track's
    /// parsed RG values keyed by its file path; when the row is gone (tab closed,
    /// model reset, playing row deleted) the factor is recomputed from the snapshot
    /// as long as the engine's current track still matches it, so closing the tab
    /// never audibly strips ReplayGain from a track that is still playing.
    void refreshReplayGain();
    /// Load and apply the persisted playback settings (volume, mute, ReplayGain,
    /// rate policy, output device) from userConfigDir()/playback.yaml (no file
    /// == defaults). Called once in the ctor.
    void loadPlaybackSettings();
    /// Write the playback settings to playback.yaml now (synchronous, through the
    /// shared atomic writer). The debounce coalesces rapid drag writes onto
    /// this; flush forces it on teardown.
    void persistPlaybackSettingsNow();
    void schedulePlaybackPersist();   ///< (re)arm the debounce timer
    void flushPlaybackSettings();     ///< write immediately if a persist is pending

    /// Relative-seek plumbing (see seekBy). commitSeek sends the accumulated
    /// target to the engine and opens a throttle window; resetSeekLatch drops the
    /// whole in-flight state (track change, Stopped, an absolute seek).
    void commitSeek();
    void resetSeekLatch();

    /// Playing-model structural reactions (cursor self-heals; we re-derive the
    /// tail and refresh the highlight).
    void onPlayingRowsChanged();
    void onPlayingModelReset();
    void onPlayingModelDestroyed();

    /// Column-op cursor re-anchor. The cursor is a QPersistentModelIndex pinned
    /// to COLUMN 0; hiding the leftmost visual column (a granular
    /// begin/endRemoveColumns from the header menu) invalidates it even though
    /// the ROW, the only coordinate playback consumes, is untouched. Snapshot
    /// the row before the removal and re-pin to (row, 0) after, only when the
    /// index actually died (removing a non-anchor column leaves it valid).
    /// Column INSERTS never invalidate a live index and need no handling.
    void onPlayingColumnsAboutToBeRemoved();
    void onPlayingColumnsRemoved();

    static QString codecToName(int codec);

    /// Declaration order matters for teardown: the bridge is declared FIRST so it
    /// is destroyed LAST, and the engine is declared AFTER so it is destroyed
    /// FIRST. ~Engine joins the engine thread, so no bridge callback can fire after
    /// the engine is gone, and the bridge it points at is still alive while that
    /// happens. The sink is owned by the engine (moved in), so it dies with it.
    std::unique_ptr<EngineListenerBridge> m_bridge;
    rawform::audio::Engine                m_engine;

    QPointer<PlaylistTabs>  m_tabs;

    QPointer<PlaylistModel> m_playingModel;
    QPersistentModelIndex   m_cursor;            ///< column 0 of m_playingModel
    /// Cursor row snapshot across a column removal (see onPlayingColumns*).
    /// Rows cannot change inside a begin/endRemoveColumns pair, so a raw int is
    /// safe between the two signals. -1 outside a column op / no cursor.
    int                     m_cursorRowAcrossColumnOp = -1;
    QList<QMetaObject::Connection> m_playingConnections;

    /// Lookahead rebuild coalescing. A burst of structural edits to the
    /// playing playlist (the scanner streams inserts in batches, one rowsInserted
    /// per batch) would otherwise rebuild the entire remaining tail and post a
    /// full setQueue per signal, quadratic over a large scan. Single-shot at
    /// 0 ms: every schedule within one event-loop turn collapses to a single
    /// rebuild right after the turn. Ordering-critical paths (playAt, model reset
    /// / destruction) still call recomputeLookahead() directly, which cancels any
    /// pending coalesce so nothing double-posts.
    QTimer m_lookaheadCoalesce;

    /// Relative-seek state (see seekBy). m_seekTarget is the accumulated target
    /// while a keyboard seek is in flight, the base the next step adds to;
    /// nullopt when nothing is pending, so a step measures from the live
    /// position. m_seekOwed says the target moved since the last commit (a
    /// flag, not a value comparison: a Right then Left inside one window nets
    /// back to the committed value and must still commit). m_seekCommitted is
    /// the value most recently sent to the engine, the one a position report
    /// can land on. The throttle timer is the commit window and outlives a
    /// landing on purpose (it rate-limits presses, not seeks); the backstop
    /// clears a latch whose landing was never observed.
    std::optional<double> m_seekTarget;
    bool                  m_seekOwed      = false;
    double                m_seekCommitted = 0.0;
    QTimer                m_seekThrottle;
    QTimer                m_seekLandBackstop;

    /// Published now-playing snapshot (GUI thread only).
    int     m_state            = Stopped;
    double  m_positionSeconds  = 0.0;
    double  m_durationSeconds  = 0.0;
    bool    m_hasTrack         = false;
    bool    m_seekable         = false;
    QString m_codecName;
    int     m_bitrateKbps      = 0;   ///< nominal/average, fixed at track open
    int     m_liveBitrateKbps  = 0;   ///< moment-to-moment, 0 == use nominal
    int     m_sampleRateHz     = 0;
    int     m_channels         = 0;
    QString m_path;
    /// Path of an explicit start (playAt) whose open the engine has not yet
    /// confirmed. Set in playAt before playNow, cleared in applyTrack only when
    /// the engine reports THAT path as current. It is therefore non-empty only in
    /// the window between an attempted start and its confirmation, which is how
    /// applyError tells a failed open (detach the cursor) from a mid-playback
    /// error (leave the playing track alone).
    QString m_pendingPath;
    QString m_nowPlayingTitle;
    QString m_nowPlayingArtist;

    /// The device outcome for the current open: the rate the output device is actually
    /// running at and whether that equals the source rate, refreshed from
    /// EngineTrackFacts on every track change and cleared on entering Stopped (device
    /// released). outputSuffix() renders them as the "(Bit Perfect)" / "(Resampled to N
    /// Hz)" suffix (with the bit-perfect leg further gated on the gain classification;
    /// see m_engineGainState below); deviceRateHz 0 means no suffix.
    int  m_deviceRateHz     = 0;
    bool m_outputBitPerfect = false;

    /// Master volume state (GUI thread only). m_volume is the 0..1 slider fraction;
    /// it defaults to 1.0 so the engine starts at unity (a byte-identical
    /// passthrough) until loadPlaybackSettings() applies any saved value. m_muted
    /// is independent and remembers m_volume across a silence. The persist timer is
    /// a single-shot debounce: every change re-arms it, so a drag of many ticks
    /// costs one coalesced write ~300 ms after the last change, with a synchronous
    /// flush on teardown catching a change made inside that window before quitting.
    qreal  m_volume = 1.0;
    bool   m_muted  = false;
    QTimer m_volumePersistTimer;

    /// Gain truthfulness classification. WHY the composed gain
    /// last pushed to the engine is or is not exactly 1.0f, so the suffix can
    /// name the cause and the remedy is obvious at a glance (the slider for
    /// Volume, the Settings RG page for ReplayGain, the readout for Muted).
    /// Unity means the engine's fast path is in effect and the delivered
    /// samples are a byte-identical passthrough; outputSuffix() renders it as
    /// "(Bit Perfect)" on a rate-matched device and the other states as their
    /// named tokens. pushGainToEngine() owns the classification and its flips.
    /// Unity is keyed off the ACTUAL pushed value rather than the settings, so
    /// a ReplayGain configuration that happens to compose to exactly unity
    /// still counts as bit-perfect (the multiply is skipped; the claim and the
    /// fast path share one definition and cannot disagree). Mute is its own
    /// state, not a volume level: it agrees with the readout's slash and is
    /// neither the slider's position nor RG. Exact float equality throughout is
    /// deliberate: it is the engine's own test. Defaults Unity to match the
    /// startup state (volume 1.0, unmuted, RG unity).
    enum class GainState { Unity, Muted, Volume, ReplayGain, VolumeAndReplayGain };
    GainState m_engineGainState = GainState::Unity;

    /// ReplayGain applied state (GUI thread only). The settings default to a no-op:
    /// mode Off, 0 dB pre-amps, clip prevention on (harmless while Off). m_replayGain
    /// Linear is the cached factor for the PLAYING track under these settings,
    /// multiplied into the engine gain by pushGainToEngine; it stays 1.0 whenever RG
    /// is Off or the track contributes no adjustment, which is what preserves the
    /// bit-perfect path.
    int    m_replayGainMode               = ReplayGainOff;
    qreal  m_replayGainPreampDb           = 0.0;
    qreal  m_replayGainUntaggedPreampDb   = 0.0;
    bool   m_replayGainClipPrevention     = true;
    float  m_replayGainLinear             = 1.0f;

    /// RG snapshot of the playing track (GUI thread only). The parsed RG
    /// values of the last row refreshReplayGain successfully resolved, keyed by
    /// that row's file path. The playlist row is a VIEW of the playing track, not
    /// its owner (the engine holds its own decoder), so when the view goes away
    /// (tab closed, model reset, playing row deleted) the factor must survive for
    /// as long as the engine still plays that path; the path key is the guard
    /// that a stale snapshot can never leak onto a different track. VALUES are
    /// cached rather than the computed linear so an RG settings change while
    /// detached still recomputes correctly under the new settings. Never needs
    /// explicit clearing: a path mismatch simply yields unity, and the next
    /// successful lookup overwrites it.
    replaygain::Values m_playingRgValues;
    QString            m_playingRgPath;

    /// ReplayGain scan setting (GUI thread only). Unlike the applied state above
    /// this never reaches the engine: it only tells a ReplayGain scan whether to
    /// skip tracks (or albums) that already carry RG info. Persisted alongside the
    /// other RG settings in playback.yaml and read by the Properties pane before a
    /// scan. Default off, so a fresh scan measures everything.
    bool   m_replayGainScanSkipExisting   = false;

    /// Output rate policy (GUI thread only). True is the
    /// engine's BitPerfectWhenAvailable, the factory default and the engine's own
    /// default, so a fresh install behaves identically with or without a saved
    /// playback.yaml. Persisted there as `bit_perfect` and pushed to the engine
    /// in loadPlaybackSettings() before the first play() and in setBitPerfect()
    /// on every committed change.
    bool   m_bitPerfect                   = true;

    /// The last device enumeration (GUI-thread copy of the engine's
    /// push) and the applied selection intent (empty = system default).
    QVariantList m_outputDevices;
    QString      m_outputDeviceId;
    QString      m_outputDeviceName;  ///< friendly companion of the id; see Q_PROPERTY
};

}  // namespace rawform
