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

// SpectrumProvider.cpp
//
// Implementation of the spectrum viewer's Qt provider. See the header for the
// shape and the ownership rationale; this file is the per-frame pipeline, the
// attack/decay envelope, the expose gate, and the small spectrum.yaml persistence.

#include "playback/SpectrumProvider.h"

#include "utils/PerfLog.h"
#include "utils/YamlFile.h"

#include "playback/AudioController.h"
#include "paths/Paths.h"  // userConfigDir() for spectrum.yaml

#include <QByteArray>
#include <QEvent>
#include <QFile>
#include <QIODevice>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>  // std::pow: the decay rescaled to the tick interval
#include <optional>
#include <string>

namespace rawform {

namespace {

// The two refresh rates the preference offers. 60 is the reference cadence:
// fast enough to read as fluid motion, cheap enough that one FFT plus twenty
// bands per tick is nothing. 30 halves the ticks, the publications and the
// frames for a display that still reads as live, for a laptop on battery or a
// player left in a corner of the screen.
constexpr int kRefreshHzDefault = 60;
constexpr int kRefreshHzLow     = 30;

// The tick interval the reference rate ran at; the decay below is tuned
// against it, and applyRefreshHz rescales that decay to whatever interval the
// chosen rate gives, so the settle time is the same at both.
constexpr double kReferenceIntervalMs = 16.0;

// Per-frame fall multiplier for a bar that is not being pushed up this tick, at
// the reference interval. The envelope is instant-attack, eased-decay: a bar
// jumps straight to a louder value but eases down from a quieter one, which
// reads as lively peaks with a natural settle rather than a flickering raw
// spectrum. 0.82 per 16 ms frame falls to roughly a tenth in about 120 ms.
constexpr float kDecayPerReferenceTick = 0.82f;

// Below this a decaying bar is snapped to exactly 0, both so the idle display is
// truly flat and so the not-playing path can tell when every bar has settled and
// the timer can be parked.
constexpr float kSilenceEpsilon = 0.001f;

// spectrum.yaml lives beside playback.yaml and the column config under
// userConfigDir(). It holds only the spectrum-view preferences: the analyzer
// source and the refresh rate.
QString spectrumSettingsPath() {
    return userConfigDir() + QStringLiteral("/spectrum.yaml");
}

}  // namespace

// ---------------------------------------------------------------------------
SpectrumProvider::SpectrumProvider(AudioController* controller, QObject* parent)
    : QObject(parent), m_controller(controller) {
    // Build the analyzer for a sensible default rate up front; the first track
    // change reconfigures it to the real rate if it differs. The scratch window and
    // the bar vector are sized to match.
    reconfigureFor(44100);

    // The visualizer clock. It is started on demand when playback begins and
    // parks itself once the display has decayed to flat, so a stopped player burns
    // no cycles here. Its interval and the matching decay come from the refresh
    // rate, applied once the persisted value is in (loadSettings below).
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &SpectrumProvider::onTick);

    // Follow the controller: start the clock when playback begins, and rebuild the
    // bands when a track of a different sample rate starts.
    if (m_controller) {
        connect(m_controller, &AudioController::stateChanged,
                this, &SpectrumProvider::onStateChanged);
        connect(m_controller, &AudioController::trackChanged,
                this, &SpectrumProvider::onTrackChanged);
    }

    // Load the persisted preferences; push the source to the engine before
    // anything plays, so the very first analyzed block already comes from the
    // chosen tap point, and time the clock to the chosen rate.
    loadSettings();
    if (m_controller) {
        m_controller->setScopeSource(m_source);
    }
    applyRefreshHz();

    // If we were constructed while already playing (not the normal path, but cheap
    // to honor), get the clock going.
    updateTimer();
}

SpectrumProvider::~SpectrumProvider() = default;

// ---------------------------------------------------------------------------
int SpectrumProvider::sourceDefault() const { return Output; }
int SpectrumProvider::refreshHzDefault() const { return kRefreshHzDefault; }

// ---------------------------------------------------------------------------
void SpectrumProvider::setDisplayWindow(QWindow* window) {
    if (m_window) {
        m_window->removeEventFilter(this);
    }
    m_window = window;
    if (m_window) {
        m_window->installEventFilter(this);
        m_exposed = m_window->isExposed();
    } else {
        m_exposed = true;  // nothing to gate on
    }
    updateTimer();
}

// The one event that matters is Expose: the platform sends it when the window
// becomes visible on screen or stops being so (minimized, hidden, or covered
// on a compositor that withholds frame callbacks from covered surfaces), and
// isExposed() carries the state. Nothing is consumed; the window still gets
// the event.
bool SpectrumProvider::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_window && event->type() == QEvent::Expose && m_window) {
        const bool exposed = m_window->isExposed();
        if (exposed != m_exposed) {
            m_exposed = exposed;
            qCDebug(lcPerf, "spectrum: %s",
                    exposed ? "resumed (window exposed)" : "parked (window not exposed)");
            updateTimer();
        }
    }
    return QObject::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// The clock runs when there is something to draw and somewhere to draw it: a
// live signal, or bars still settling toward flat, and an exposed window. The
// one caller-independent decision, so every trigger (playback state, expose,
// the first settle) lands on the same rule.
void SpectrumProvider::updateTimer() {
    const bool playing = m_controller && m_controller->isPlaying();
    const bool wanted  = (playing || anyBarAlive()) && m_exposed;
    if (wanted && !m_timer.isActive()) {
        m_timer.start();
    } else if (!wanted && m_timer.isActive()) {
        m_timer.stop();
    }
}

bool SpectrumProvider::anyBarAlive() const {
    return std::any_of(m_bars.begin(), m_bars.end(),
                       [](qreal v) { return v > 0.0; });
}

// The interval for the chosen rate, and the decay rescaled to it so a bar
// falls at the same speed per second at 30 Hz as at 60 (the per-tick factor
// is the reference factor raised to the ratio of the intervals).
void SpectrumProvider::applyRefreshHz() {
    const int intervalMs = 1000 / m_refreshHz;
    m_timer.setInterval(intervalMs);
    m_decay = static_cast<float>(std::pow(
        static_cast<double>(kDecayPerReferenceTick),
        static_cast<double>(intervalMs) / kReferenceIntervalMs));
}

// ---------------------------------------------------------------------------
void SpectrumProvider::reconfigureFor(std::uint32_t sampleRate) {
    if (sampleRate == 0) {
        sampleRate = 44100;
    }
    m_analyzer.configure(sampleRate);  // analyzer defaults: 2048 / 20 bars / 40..18000 Hz
    m_configuredRate = sampleRate;

    // Size the mono scratch to one analysis window, allocated once here (never on
    // the tick path).
    m_scratch.assign(m_analyzer.fftSize(), 0.0f);

    // Match the published bar vector to the analyzer's band count. Reset to flat on
    // a (re)configure; the next live tick fills it. Emit barCountChanged only when
    // the count actually changes, so a same-count reconfigure is silent to QML.
    const int newCount = static_cast<int>(m_analyzer.barCount());
    if (newCount != m_bars.size()) {
        m_bars = QList<qreal>(newCount, 0.0);
        emit barCountChanged();
    } else {
        std::fill(m_bars.begin(), m_bars.end(), 0.0);
    }
    emit barsChanged();
}

// ---------------------------------------------------------------------------
void SpectrumProvider::onStateChanged() {
    // Playback began or ended: re-decide the clock. While paused or stopped it
    // keeps running just long enough for onTick to decay the bars to flat, then
    // parks itself (updateTimer never stops a clock with bars still alive).
    updateTimer();
}

// ---------------------------------------------------------------------------
void SpectrumProvider::onTrackChanged() {
    // The band-to-bin mapping depends on the sample rate, so rebuild the analyzer
    // when a track of a different rate becomes current (the bit-perfect device-rate
    // path means this really does change between songs). A zero rate (Stopped view)
    // is ignored; the existing configuration stays valid for the next play.
    if (!m_controller) {
        return;
    }
    const auto rate = static_cast<std::uint32_t>(m_controller->sampleRateHz());
    if (rate != 0 && rate != m_configuredRate) {
        reconfigureFor(rate);
    }
}

// ---------------------------------------------------------------------------
void SpectrumProvider::onTick() {
    const bool playing = m_controller && m_controller->isPlaying();

    if (playing) {
        // Pull the latest mono window from the engine tap and analyze it. copyScope
        // Mono zero-pads when little has been produced yet, so the early frames are
        // valid too.
        const std::size_t got =
            m_controller->copyScopeMono(m_scratch.data(), m_scratch.size());
        const std::vector<float>& raw = m_analyzer.analyze(m_scratch.data(), got);

        // Instant-attack, eased-decay envelope: jump up to a louder value, ease down
        // from a quieter one. new = max(target, current * decay).
        for (int i = 0; i < m_bars.size(); ++i) {
            const float target  = (i < static_cast<int>(raw.size())) ? raw[i] : 0.0f;
            const float decayed = static_cast<float>(m_bars[i]) * m_decay;
            m_bars[i] = std::max(target, decayed);
        }
        emit barsChanged();
        return;
    }

    // Not playing: no signal to analyze, so let every bar decay toward flat. Once
    // they have all settled, snap to exactly 0 and PARK the timer; the next play()
    // restarts it through onStateChanged.
    bool anyAlive = false;
    for (int i = 0; i < m_bars.size(); ++i) {
        float v = static_cast<float>(m_bars[i]) * m_decay;
        if (v < kSilenceEpsilon) {
            v = 0.0f;
        } else {
            anyAlive = true;
        }
        m_bars[i] = v;
    }
    emit barsChanged();
    if (!anyAlive) {
        m_timer.stop();
    }
}

// ---------------------------------------------------------------------------
void SpectrumProvider::setSource(int source) {
    // Clamp to the valid pair; anything else falls back to the default rather than
    // reaching the engine as a bad mode.
    const int clamped = (source == Source) ? Source : Output;
    if (clamped == m_source) {
        return;
    }
    m_source = clamped;
    if (m_controller) {
        m_controller->setScopeSource(m_source);  // push the tap-position switch to the engine
    }
    persistSettings();
    emit sourceChanged();
}

// ---------------------------------------------------------------------------
void SpectrumProvider::setRefreshHz(int hz) {
    // The two offered rates only; anything else is the default rather than an
    // arbitrary cadence the decay was never tuned for.
    const int clamped = (hz == kRefreshHzLow) ? kRefreshHzLow : kRefreshHzDefault;
    if (clamped == m_refreshHz) {
        return;
    }
    m_refreshHz = clamped;
    applyRefreshHz();  // a running QTimer picks the new interval up at its next start
    if (m_timer.isActive()) {
        m_timer.start();  // restart so the new interval applies now, not after a stop
    }
    persistSettings();
    emit refreshHzChanged();
}

// ---------------------------------------------------------------------------
void SpectrumProvider::loadSettings() {
    // Absent or unreadable is the normal first-run case: the default (Output)
    // stands. A malformed file is logged and ignored, again falling back to the
    // default.
    const auto text = yamlfile::readAll(spectrumSettingsPath());
    if (!text) {
        return;
    }
    try {
        const YAML::Node root = YAML::Load(*text);
        if (root["scope_source"]) {
            const int s = root["scope_source"].as<int>();
            m_source = (s == Source) ? Source : Output;
        }
        if (root["refresh_hz"]) {
            const int hz = root["refresh_hz"].as<int>();
            m_refreshHz = (hz == kRefreshHzLow) ? kRefreshHzLow : kRefreshHzDefault;
        }
    } catch (const YAML::Exception& e) {
        qWarning("rawform: ignoring malformed %s (%s)",
                 qUtf8Printable(spectrumSettingsPath()), e.what());
        m_source    = Output;
        m_refreshHz = kRefreshHzDefault;
    }
}

// ---------------------------------------------------------------------------
void SpectrumProvider::persistSettings() {
    // Emit the preferences and hand them to the shared atomic writer
    // (utils/YamlFile.h). There is no debounce here because, unlike a volume
    // drag, these change only on a deliberate Settings commit.
    const QString   path = spectrumSettingsPath();

    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "scope_source" << YAML::Value << m_source;
    out << YAML::Key << "refresh_hz" << YAML::Value << m_refreshHz;
    out << YAML::EndMap;

    QByteArray content;
    content += "# rawform - spectrum view settings (generated by the app).\n";
    content += "# scope_source: analyzer tap, 0 output (follows volume), 1 source (ignores volume).\n";
    content += "# refresh_hz: display refresh rate, 60 or 30.\n";
    content += out.c_str();
    content += '\n';

    yamlfile::writeAtomically(path, content);
}

}  // namespace rawform
