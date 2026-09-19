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

// PlayerBar.qml
//
// The transport row, four regions left to right on one RowLayout
// (playerBarMainRow): the transport buttons (playbackButtonsRow), the
// now-playing column (currentTrackColumn: format line and timing readout over
// the SeekBar), the master-volume column (volumeColumn), and the SpectrumView.
// Everything here is wiring over the AudioController and SpectrumProvider
// context properties; the bars and the spectrum are dumb reusable components
// that know nothing about either.
//
//   - Transport enablement. Each button gates input from controller state:
//     stop is inert when already stopped, previous/next are inert with no live
//     cursor, play/pause is always live (its from-Stopped fallback lives in the
//     controller). The dimmed look is opacity on the button root (see TpBtn);
//     an inert button also loses its pointing-hand cursor.
//
//   - The scrubber. SeekBar owns the scrub state machine: click to seek there
//     on release, press-drag to scrub with the live tick suppressed, release
//     to commit exactly one seekSeconds. This file wires the controller's
//     position/duration/seekable in and the committed seek out.
//
//   - Errors are not surfaced here. A failed open is reported to the log
//     console (and status line); the format slot stays blank when no track is
//     loaded rather than flashing the reason in place of the format line.
//
// Master volume: a two-line column, left of the spectrum, whose percentage readout sits
// on the format/timing baseline and whose VolumeSlider sits on the SeekBar baseline
// (bottomMargin and spacing mirror currentTrackColumn so the columns share one vertical
// rhythm). The slider is a dumb, reusable bar like SeekBar; it applies live (volume is a
// free, flush-less atomic store at the engine's pull chokepoint, so unlike a seek every
// drag tick can be heard) and supports wheel-to-nudge. The percentage label doubles as
// the mute toggle, since this design carries no dedicated mute glyph. All policy (the
// power-law percentage->gain taper, mute, and persistence) lives in the AudioController;
// this file only binds level/muted in and the chosen level out, through the controller's
// volume / volumePercent / muted plus setVolume / toggleMute. Transport icons: the SVG
// set in icons/app/track_controls, rendered through AppIcon so they stay crisp across DPI
// boundaries. TpBtn is plateless: no background rectangle in any state; hover and
// disabled feedback are pure opacity on the button root. The play/pause button swaps its
// SVG source on isPlaying.

// qmllint disable unqualified
// Wiring layer: this file reaches the C++ context properties (audioController,
// spectrumProvider), which qmllint cannot see, so the unqualified-access
// category is disabled file-wide. Components stay fully linted; keep global
// wiring in the views so they can. Cost: a typo'd global name here surfaces at
// runtime, not at lint.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

Item {
    id: root
    implicitHeight: 90

    // Channel-count label for the format line. Mirrors AudioController::formatSummary
    // (Mono / Stereo / N ch) so the bar's split-out line reads identically to the
    // single-string version; this is pure presentation of the already-exposed
    // channels count, which is why it sits here rather than adding a C++ property.
    function channelLabel(n) {
        return n === 1 ? "Mono" : (n === 2 ? "Stereo" : n + " ch")
    }

    // Shared text style for the format/status line: the warm-green token in the
    // Inter face at the bar's 12px bold. The codec, the live kbps, and the
    // sample-rate/channel tail all use it so they read as one continuous line while
    // living in separate items, which is what lets the kbps occupy its own
    // fixed-width slot. Errors never use it; they go to the log surfaces (see
    // the file top).
    component FmtText: Text {
        color: Theme.success
        font.family: Theme.uiFont
        font.pixelSize: 12
        font.weight: Font.Bold
    }

    // -----------------------------------------------------------------------
    // Transport button. An icon-only hit target: the 24x24 SVG glyph rendered
    // via AppIcon (whose explicit sourceSize keeps it crisp when the window
    // crosses DPI boundaries), with no background plate in any state. All
    // feedback is opacity on the root: rest 0.85, hover 1.0, disabled 0.35.
    // Rest sits below 1.0 precisely so hover has headroom to lift, and the
    // lift IS the hover affordance since there is no plate tint. `enabled`
    // propagates to the MouseArea and HoverHandler, so
    // an inert button also loses the pointing-hand cursor, and a disabled
    // Item's handlers never see events, which is why hovered can never be
    // stuck true while disabled; the ternary tests `enabled` first anyway to
    // keep the precedence explicit rather than incidental.
    // -----------------------------------------------------------------------
    component TpBtn: Item {
        id: btn
        property url icon
        signal activated()

        width: 24
        height: 24
        opacity: !btn.enabled ? 0.35 : (btnHover.hovered ? 1.0 : 0.85)

        AppIcon {
            anchors.centerIn: parent
            source: btn.icon
            iconSize: 24
        }

        HoverHandler { id: btnHover }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.activated()
        }
    }

    RowLayout {
        id: playerBarMainRow
        anchors.fill: parent
        // 16 px between items so the volume control sits 16 px from its
        // neighbors on both sides. The wider transport-to-track gap (30) is
        // restored with currentTrackColumn's leftMargin below, so only the
        // volume's two gaps tighten.
        spacing: 16

        // Transport, left to right: stop, previous, play/pause, next. The middle
        // button toggles icon and action on the playing state. On top sits the
        // enablement gates; the controller's verbs are still no-ops when
        // inappropriate, so this is belt-and-suspenders plus the right cursor and
        // the dimmed disabled look.
        Row {
            id: playbackButtonsRow
            spacing: 22

            TpBtn {
                icon: Qt.resolvedUrl("../../icons/app/track_controls/stop.svg")
                // Nothing to stop when already stopped.
                enabled: !audioController.isStopped
                onActivated: audioController.stop()
            }
            TpBtn {
                icon: Qt.resolvedUrl("../../icons/app/track_controls/previous.svg")
                // Organic navigation needs a live cursor to move from.
                enabled: audioController.playingRow >= 0
                onActivated: audioController.previous()
            }
            TpBtn {
                // Single toggling button: the SVG source swaps on the playing
                // state.
                icon: Qt.resolvedUrl(audioController.isPlaying
                    ? "../../icons/app/track_controls/pause.svg"
                    : "../../icons/app/track_controls/play.svg")
                // Always live: play has the from-Stopped fallback to the active tab.
                onActivated: audioController.playPauseToggle()
            }
            TpBtn {
                icon: Qt.resolvedUrl("../../icons/app/track_controls/next.svg")
                enabled: audioController.playingRow >= 0
                onActivated: audioController.next()
            }
        }

        ColumnLayout {
            id: currentTrackColumn
            // 14 + the row's 16 spacing = the original 30 px gap to the transport
            // buttons, kept while the volume's own gaps tightened to 16.
            Layout.leftMargin: 14
            Layout.bottomMargin: 22
            spacing: 6

            RowLayout {
                id: currentTrackRow
                spacing: 12

                Item {
                    id: currentTrackDataSpacer
                    Layout.fillWidth: true
                }

                // The format slot. It carries the engine-derived format line
                // (green) when a track is loaded, and is blank otherwise (a failed
                // open loads nothing, so this stays empty and the reason goes to
                // the log console). The line is split into three items so the live
                // kbps can sit in a fixed-width slot: a 3<->4 digit swing then
                // changes only the digits in place instead of reflowing the codec
                // name, the sample-rate tail, and the timing beside it.
                Row {
                    id: currentTrackFormat
                    // Hidden via OPACITY, not visible: a visible:false child is
                    // dropped from the RowLayout, which would collapse this row to
                    // zero height and let the seek bar below slide up. Opacity does
                    // not affect layout, so the row keeps its one-line height with
                    // or without a loaded track and the seek bar never moves. Goes
                    // transparent when nothing is playing (false while Stopped, so
                    // Stop clears the green line; true while Playing or Paused, so a
                    // pause still shows it).
                    opacity: audioController.hasTrack ? 1 : 0
                    spacing: 0

                    FmtText { text: audioController.codecName + " " }

                    // Fixed-width slot for the live kbps. The number right-aligns
                    // against the trailing "kbps", which stays put; the slot is the
                    // wider of a four-digit reference and the current value, so it
                    // never shrinks mid-track on a VBR stream and an unusually wide
                    // constant rate (e.g. hi-res PCM at five digits) still fits
                    // without overrunning the codec name to its left.
                    Item {
                        height: kbpsRefMetrics.height
                        width: Math.max(kbpsRefMetrics.width, kbpsNowMetrics.width)
                        FmtText {
                            anchors.right: parent.right
                            text: "\u200A" + audioController.displayBitrateKbps
                        }
                    }

                    FmtText {
                        text: "\u2009kbps " + audioController.sampleRateHz + "Hz "
                              + root.channelLabel(audioController.channels)
                              // The device-outcome suffix:
                              // "(Bit Perfect)" / "(Resampled to N Hz)", empty
                              // while Stopped. Wording owned by the controller's
                              // outputSuffix so this split rendering and the
                              // canonical formatSummary string cannot disagree.
                              + (audioController.outputSuffix.length > 0
                                 ? " " + audioController.outputSuffix : "")
                    }
                }

                // Off-screen metrics that size the kbps slot (non-visual, ignored by
                // the layout). kbpsRefMetrics is the four-digit floor; kbpsNowMetrics
                // tracks the current value so a five-digit rate widens the slot
                // rather than overflowing it.
                TextMetrics {
                    id: kbpsRefMetrics
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    text: "8888"
                }
                TextMetrics {
                    id: kbpsNowMetrics
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    text: "" + audioController.displayBitrateKbps
                }

                Text {
                    id: currentTrackTiming
                    // Transparent (not visible:false) when nothing is playing, for
                    // the same layout reason as currentTrackFormat above: hiding it
                    // with visible would collapse the row and shift the seek bar.
                    // Opacity keeps its height reserved, so Stop clears the whole
                    // now-playing readout (this timing line together with the green
                    // format Row) without the timeline moving. A pause still shows
                    // the frozen timing.
                    opacity: audioController.hasTrack ? 1 : 0
                    color: Theme.textPrimary
                    font.family: Theme.monoFont
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    text: Format.mmss(audioController.positionSeconds)
                          + " / " + Format.mmss(audioController.durationSeconds)
                }
            }

            // The progress track is the interactive scrubber: SeekBar owns the
            // click/drag/release state machine. We feed it the live
            // position/duration/seekable (its hover bubble defaults to Theme.monoFont on
            // its own) and commit its one-shot seekRequested straight to the controller.
            // seekSeconds is the only engine call here, and it already existed: no new
            // controller surface.
            SeekBar {
                id: seekBar
                Layout.fillWidth: true

                positionSeconds: audioController.positionSeconds
                durationSeconds: audioController.durationSeconds
                seekable: audioController.seekable

                onSeekRequested: function (seconds) {
                    audioController.seekSeconds(seconds)
                }
            }
        }

        // Master volume, left of the spectrum. Two lines that line up with the track
        // column beside it: the percentage readout on the format/timing baseline (top),
        // the slider on the SeekBar baseline (bottom). bottomMargin and spacing mirror
        // currentTrackColumn so the two columns share a vertical rhythm; they are the
        // obvious tunables if a baseline needs nudging. The column has no fillWidth, so
        // it takes the slider's preferred width and leaves the surrounding layout
        // (including the SeekBar's stretch) undisturbed.
        ColumnLayout {
            id: volumeColumn
            // Pinned to a fixed width on purpose. A ColumnLayout is a Layout type,
            // and Layout types default Layout.fillWidth to TRUE (the scenario
            // this prevents: left at the default it competes with
            // currentTrackColumn, also a Layout, for the row's leftover space,
            // and the split shifts the instant the format line appears on play,
            // so the bar visibly resizes). Disabling fill and clamping min ==
            // preferred == max holds it at 150 in every state and hands all the
            // stretch to currentTrackColumn (the seek bar).
            Layout.fillWidth: false
            Layout.minimumWidth: 150
            Layout.preferredWidth: 150
            Layout.maximumWidth: 150
            Layout.bottomMargin: 22
            spacing: 6

            // The percentage readout, right-aligned over the slider. It doubles as
            // the mute control (this design has no dedicated mute glyph): a click
            // toggles mute, the text dims while muted, and an oblique slash is
            // struck across the digits as an unmistakable muted mark. The readout is
            // a container Item so the slash can sit OVER the glyphs at full strength
            // (a child of the Text would inherit its dimmed opacity) and the click
            // target can cover the whole row.
            Item {
                id: volumeReadout
                Layout.fillWidth: true
                implicitHeight: volumePercentText.implicitHeight

                Text {
                    id: volumePercentText
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: audioController.volumePercent + "%"
                    color: Theme.textPrimary
                    opacity: audioController.muted ? 0.55 : 1.0
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    font.weight: Font.Bold
                }

                // The oblique mute slash: a thin bar across just the digits (sized
                // to the painted glyph width, not the full column), rotated to an
                // oblique angle. Full strength so it reads clearly over the dimmed
                // number. transformOrigin defaults to the item center, so it pivots
                // in place.
                Rectangle {
                    visible: audioController.muted
                    anchors.horizontalCenter: volumePercentText.horizontalCenter
                    anchors.verticalCenter: volumePercentText.verticalCenter
                    width: volumePercentText.contentWidth
                    height: 1.5
                    radius: 1
                    color: Theme.textPrimary
                    opacity: 0.40
                    rotation: -20
                    antialiasing: true
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: audioController.toggleMute()
                }
            }

            // The slider fills the column's fixed 150 px width (the column, not the
            // slider, sets the width). Continuous live application and
            // wheel-to-nudge are owned by the component; level/muted bind in,
            // moved() commits straight to the controller, which owns the perceptual
            // taper, mute, and persistence.
            VolumeSlider {
                id: volumeSlider
                Layout.fillWidth: true
                level: audioController.volume
                muted: audioController.muted
                // The exponent drives the dB bubble so it matches the engine gain;
                // its mono family defaults to Theme.monoFont, same as the SeekBar.
                taperExponent: audioController.volumeTaperExponent
                onMoved: function (level) { audioController.setVolume(level) }
            }
        }

        // Spectrum view: the live analyzer. A dumb SpectrumView fed the smoothed
        // bar values and the stable bar count from the SpectrumProvider, which
        // owns the 60 Hz timer, the FFT, the attack/decay smoothing, and the
        // analyzer-source preference (Settings > Spectrum view). Its geometry
        // and bottom margin are what the surrounding bar layout is built around.
        SpectrumView {
            id: spectrumView
            Layout.bottomMargin: 26
            Layout.preferredWidth: 120
            Layout.preferredHeight: 40
            values: spectrumProvider.bars
            count: spectrumProvider.barCount
        }
    }
}
