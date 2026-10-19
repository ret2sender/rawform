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

// SpectrumSettingsPane.qml
//
// The "Spectrum view" page inside the Settings window, the sibling of
// ReplayGainSettingsPane and built to the same rules: it reads and writes the
// window's staged object (passed in as `settings`), never the provider directly, so
// nothing applies until Apply/OK. Two settings. The ANALYZER SOURCE: does the
// spectrum follow the volume fader (Output, the signal as heard, the default) or
// ignore it (Source, the decoded signal at full scale). The REFRESH RATE: 60 Hz
// (the default) or 30, which halves the analysis ticks and the frames the bars
// cost while a track plays. The factory defaults come from the bound provider
// (one source of truth, see `provider` below) and drive both the
// modified-from-default accent dot and the right-click reset, exactly as the
// ReplayGain rows do.
//
// Each row is a SettingsRow with the shared SettingsResetMenu, as on the sibling
// panes. The selectors are the shared SegmentSelect; the source values 0 (Output)
// and 1 (Source) match SpectrumProvider's source ints and map straight through,
// the rate segments map index 0 to 60 Hz and 1 to 30 Hz.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import com.rawform.app

Item {
    id: pane

    // The window's staged edit object; this page uses settings.spectrumSource
    // and settings.spectrumRefreshHz.
    property StagedSettings settings: null

    // The SpectrumProvider whose sourceDefault and refreshHzDefault are the
    // reset targets and the modified-from-default references. Declared and
    // bound by the host rather than read as an ambient global.
    property SpectrumProvider provider: null
    property string uiFont: Theme.uiFont

    // Shared label column so both selectors start at the same x.
    readonly property real labelColumnWidth: Math.max(sourceRow.labelImplicitWidth,
                                                      refreshRow.labelImplicitWidth)

    SettingsResetMenu { id: resetMenu }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Text {
            text: "Spectrum view"
            color: Theme.textPrimary
            font.family: pane.uiFont
            font.pixelSize: Theme.fontSizeHeading
            font.weight: Font.Bold
        }

        Rectangle {  // divider under the title
            Layout.fillWidth: true
            Layout.topMargin: 10
            Layout.bottomMargin: 6
            Layout.preferredHeight: 1
            color: Theme.separatorStrong
        }

        // ----- Analyzer source ---------------------------------------------
        SettingsRow {
            id: sourceRow
            Layout.fillWidth: true
            text: "Analyzer source"
            modified: pane.settings && pane.settings.spectrumSource !== pane.provider.sourceDefault
            labelWidth: pane.labelColumnWidth
            resetMenu: resetMenu
            onReset: if (pane.settings) pane.settings.spectrumSource = pane.provider.sourceDefault

            SegmentSelect {
                labels: ["Output", "Source"]
                segWidth: 72
                value: pane.settings ? pane.settings.spectrumSource : 0
                onPicked: function (v) {
                    if (pane.settings) pane.settings.spectrumSource = v
                }
            }
        }

        // A subdued one-line explanation of the two choices, so the labels are not
        // cryptic. Updates with the staged value.
        Text {
            Layout.fillWidth: true
            Layout.topMargin: 2
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: Theme.fontSizeSmall
            text: (pane.settings && pane.settings.spectrumSource === 1)
                  ? "Source: the decoded signal at full scale, unaffected by the volume."
                  : "Output: the signal as heard, so the bars follow the volume fader."
        }

        // ----- Refresh rate ------------------------------------------------
        SettingsRow {
            id: refreshRow
            Layout.fillWidth: true
            Layout.topMargin: 10
            text: "Refresh rate"
            modified: pane.settings
                      && pane.settings.spectrumRefreshHz !== pane.provider.refreshHzDefault
            labelWidth: pane.labelColumnWidth
            resetMenu: resetMenu
            onReset: {
                if (pane.settings)
                    pane.settings.spectrumRefreshHz = pane.provider.refreshHzDefault
            }

            SegmentSelect {
                labels: ["60 Hz", "30 Hz"]
                segWidth: 72
                value: (pane.settings && pane.settings.spectrumRefreshHz === 30) ? 1 : 0
                onPicked: function (v) {
                    if (pane.settings) pane.settings.spectrumRefreshHz = (v === 1) ? 30 : 60
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 2
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: Theme.fontSizeSmall
            text: "How often the bars update while a track plays. 30 Hz halves the "
                  + "work the display costs; the bars settle at the same speed."
        }

        Item { Layout.fillHeight: true }  // push rows to the top
    }
}
