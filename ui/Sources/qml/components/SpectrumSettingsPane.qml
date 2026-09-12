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

// Bound component behavior: nested components and delegates resolve outer
// document ids statically instead of through dynamic context lookup. The
// flip side is that views no longer inject model data into delegates via
// context; every delegate in this file declares what it consumes as
// `required property`, which is both the contract and the qmllint proof.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import com.rawform.app


// =============================================================================
// SpectrumSettingsPane.qml
//
// The "Spectrum view" page inside the Settings window, the sibling of
// ReplayGainSettingsPane and built to the same rules: it reads and writes the
// window's staged object (passed in as `settings`), never the provider directly, so
// nothing applies until Apply/OK. The one setting is the ANALYZER SOURCE: does
// the spectrum follow the volume fader (Output, the signal as heard, the default) or
// ignore it (Source, the decoded signal at full scale). The factory default comes
// from the bound provider (one source of truth, see `provider` below) and
// drives both the modified-from-default accent dot and the right-click reset,
// exactly as the ReplayGain rows do.
//
// The selector and the row chrome are local inline components styled to the app, so
// the whole page stays one file and matches the ReplayGain page visually.
// =============================================================================
Item {
    id: pane

    // The window's staged edit object; this page uses settings.spectrumSource.
    property StagedSettings settings: null

    // The SpectrumProvider whose sourceDefault is the reset target and the
    // modified-from-default reference. Declared and bound by the host rather
    // than read as an ambient global.
    property SpectrumProvider provider: null
    property string uiFont: Theme.uiFont

    // Shared right-click reset menu, same shape as the ReplayGain pane.
    ThemedMenu {
        id: resetMenu
        property var doReset: null
        Action {
            text: "Reset to default"
            // The var slot is the mechanism: each row assigns its own reset
            // closure before popping the menu, and a declared function could
            // not be reassigned per row.
            onTriggered: if (resetMenu.doReset) resetMenu.doReset()  // qmllint disable use-proper-function
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Text {
            text: "Spectrum view"
            color: Theme.textPrimary
            font.family: pane.uiFont
            font.pixelSize: 16
            font.weight: Font.Bold
        }

        Rectangle {  // divider under the title
            Layout.fillWidth: true
            Layout.topMargin: 10
            Layout.bottomMargin: 6
            Layout.preferredHeight: 1
            color: Theme.separatorStrong
        }

        // ----- Analyzer source ----------------------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 46

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.spectrumSource = pane.provider.sourceDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLabel {
                text: "Analyzer source"
                modified: pane.settings && pane.settings.spectrumSource !== pane.provider.sourceDefault
            }

            SourceSelect {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                value: pane.settings ? pane.settings.spectrumSource : 0
                onPicked: function (v) { if (pane.settings) pane.settings.spectrumSource = v }
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
            font.pixelSize: 11
            text: (pane.settings && pane.settings.spectrumSource === 1)
                  ? "Source: the decoded signal at full scale, unaffected by the volume."
                  : "Output: the signal as heard, so the bars follow the volume fader."
        }

        Item { Layout.fillHeight: true }  // push rows to the top
    }

    // -------------------------------------------------------------------------
    // Label plus modified-from-default dot, matching the ReplayGain pane.
    // -------------------------------------------------------------------------
    component RowLabel: Row {
        property string text: ""
        property bool modified: false
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 7

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: parent.text
            color: Theme.textSecondary
            font.family: pane.uiFont
            font.pixelSize: 12
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            visible: parent.modified
            width: 6
            height: 6
            radius: 3
            color: Theme.accentSoft
        }
    }

    // -------------------------------------------------------------------------
    // Segmented Output / Source selector, the two-choice sibling of the ReplayGain
    // Off / Track / Album control. Values 0 (Output) and 1 (Source) match
    // SpectrumProvider's source ints, so the picked value maps straight through.
    // -------------------------------------------------------------------------
    component SourceSelect: Row {
        id: ss
        property int value: 0
        signal picked(int value)
        spacing: 0

        Repeater {
            model: ["Output", "Source"]
            delegate: Rectangle {
                id: seg
                required property int index
                required property string modelData
                width: 72
                height: 26
                color: ss.value === seg.index ? Theme.accentSoft
                     : segHover.hovered ? Theme.surfaceControlHover : Theme.surfaceControl
                border.color: Theme.border
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: seg.modelData
                    color: ss.value === seg.index ? Theme.textOnAccent : Theme.textSecondary
                    font.family: pane.uiFont
                    font.pixelSize: 11
                    font.weight: ss.value === seg.index ? Font.Bold : Font.Normal
                }
                HoverHandler { id: segHover }
                TapHandler { onTapped: ss.picked(seg.index) }
            }
        }
    }
}
