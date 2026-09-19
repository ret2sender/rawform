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

// SpectrumProvider.h
//
// The Qt face of the spectrum viewer, and the third piece of the feature after
// the engine's RT tap (ScopeTap, in Engine::copyScope) and the zero-Qt transform
// (SpectrumAnalyzer). It is a small, focused QObject in the same standalone shape
// as ReplayGainScanController and MetadataReloader rather than more weight on the
// already large AudioController: it owns the visualizer's timer, its DSP instance,
// its temporal smoothing, and its one persisted preference (the analyzer source),
// and it exposes the bar values QML draws.
//
// WHAT IT DOES, on each timer tick (about 60 Hz):
//   - While playing: pull the most recent mono window from the engine tap through
//     AudioController::copyScopeMono, run it through the SpectrumAnalyzer, and fold
//     the instantaneous bars into the published bars with an attack/decay envelope
//     (instant rise, eased fall), the standard analyzer feel.
//   - While not playing: feed nothing and let the same decay walk every bar down to
//     flat, then PARK the timer once the display has settled, so a stopped or
//     paused player spends no cycles on a frozen or idle spectrum.
// The analyzer is reconfigured whenever the playing track's sample rate changes
// (the band-to-bin mapping depends on it), which AudioController surfaces through
// its sampleRateHz / trackChanged.
//
// WHY THE PROVIDER OWNS THE SOURCE PREFERENCE. The pre/post-gain choice is a
// spectrum-view preference, so it lives here with the rest of the spectrum state
// rather than smeared into the playback settings: the Settings window's "Spectrum
// view" section binds to this object, any spectrum-view knob belongs beside it
// here, and it persists in its own small spectrum.yaml beside the other rawform
// config. The provider is only the POLICY and the
// persistence; the actual tap-position switch is an engine mechanism, so on a
// change the provider pushes the choice to the engine through
// AudioController::setScopeSource. The applied value is this object's `source`
// property; the Settings window stages edits and calls setSource only on Apply/OK,
// exactly as the ReplayGain page stages against the controller.
//
// THREADING. Pure GUI thread. The timer slot, the analyzer, the smoothing, and the
// property emits all run on the thread QML lives on. The only cross-thread contact
// is copyScopeMono, which reads the engine's lock-free tap; that is safe by the
// tap's own contract and needs nothing here.

#pragma once

#include "rawform/audio/SpectrumAnalyzer.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>  // QML_ELEMENT / QML_UNCREATABLE
#include <QTimer>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rawform {

class AudioController;

class SpectrumProvider : public QObject {
    Q_OBJECT

    /// Registered so QML can NAME the type (typed property declarations, and
    /// qmllint member checking on them); never instantiated from QML. The one
    /// instance is created in main.cpp and exposed as a context property.
    QML_ELEMENT
    QML_UNCREATABLE("SpectrumProvider is created in C++ and exposed as a context property")

    /// The bar magnitudes QML draws: barCount values, each 0..1, low frequency
    /// first. Republished on every visible tick while the display is live (and as it
    /// decays to flat after playback stops). A fixed-length list, so a bound Repeater
    /// keys off barCount and only the delegate heights re-evaluate.
    Q_PROPERTY(QList<qreal> bars READ bars NOTIFY barsChanged)

    /// The number of bars (the length of `bars`). Effectively constant for a given
    /// configuration; it carries a NOTIFY only so a reconfiguration that
    /// changes the count keeps QML correct. The view binds its Repeater model to
    /// this so the delegate set is stable across the per-tick bars updates.
    Q_PROPERTY(int barCount READ barCount NOTIFY barCountChanged)

    /// The analyzer source: 0 == Output (post master gain, follows the volume; the
    /// default), 1 == Source (pre master gain, ignores the volume). Exposed as a
    /// plain int (like AudioController's replayGainMode) so the Settings selector and
    /// the persisted value round-trip without QML touching the enum. The WRITE is the
    /// commit: it pushes the choice to the engine and persists, so the Settings page
    /// stages locally and calls it only on Apply/OK.
    Q_PROPERTY(int source READ source WRITE setSource NOTIFY sourceChanged)

    /// The factory default source (Output), exposed read-only so the Settings page
    /// can flag a non-default value and reset it from one source of truth, matching
    /// the ReplayGain *Default reads.
    Q_PROPERTY(int sourceDefault READ sourceDefault CONSTANT)

public:
    /// The analyzer source, mirroring rawform::audio::ScopeSource value-for-value so
    /// the int that crosses to the engine needs no remap. Kept as a Q_ENUM for C++
    /// self-documentation; QML uses the integer values (0 / 1), as the ReplayGain
    /// page does for its mode.
    enum ScopeSource { Output = 0, Source = 1 };
    Q_ENUM(ScopeSource)

    /// The controller is the bridge to the engine tap and the now-playing facts
    /// (isPlaying, sampleRateHz). Non-owning; it must outlive the provider, which is
    /// guaranteed by the composition order in main.cpp (the controller is constructed
    /// first and destroyed last).
    explicit SpectrumProvider(AudioController* controller, QObject* parent = nullptr);
    ~SpectrumProvider() override;

    SpectrumProvider(const SpectrumProvider&)            = delete;
    SpectrumProvider& operator=(const SpectrumProvider&) = delete;

    [[nodiscard]] QList<qreal> bars() const { return m_bars; }
    [[nodiscard]] int          barCount() const { return static_cast<int>(m_bars.size()); }
    [[nodiscard]] int          source() const { return m_source; }
    [[nodiscard]] int          sourceDefault() const;

public slots:
    /// Commit the analyzer source (0 Output / 1 Source). Clamps to the valid pair,
    /// and on an actual change pushes the choice to the engine, persists it, and
    /// emits sourceChanged. A no-op when unchanged. Called by the Settings window on
    /// Apply/OK; safe to call from QML.
    void setSource(int source);

signals:
    void barsChanged();
    void barCountChanged();
    void sourceChanged();

private:
    // --- the per-frame pipeline --------------------------------------------
    void onTick();              ///< the timer slot: sample -> analyze -> smooth, or decay
    void onStateChanged();      ///< start the timer when playback begins
    void onTrackChanged();      ///< reconfigure the analyzer on a sample-rate change
    void reconfigureFor(std::uint32_t sampleRate);  ///< (re)size analyzer + scratch

    // --- persistence -------------------------------------------------------
    void loadSettings();        ///< read spectrum.yaml (absent == defaults)
    void persistSettings();     ///< write spectrum.yaml through the shared atomic writer

    QPointer<AudioController> m_controller;  ///< non-owning bridge to the engine tap

    QTimer                    m_timer;       ///< ~60 Hz visualizer clock
    audio::SpectrumAnalyzer   m_analyzer;    ///< the DSP transform (GUI-thread only)
    std::vector<float>        m_scratch;     ///< mono window copied from the tap, fftSize long
    std::uint32_t             m_configuredRate = 0;  ///< the rate the analyzer is built for

    QList<qreal>              m_bars;        ///< the published, smoothed bar values
    int                       m_source = Output;  ///< applied analyzer source (default Output)
};

}  // namespace rawform
