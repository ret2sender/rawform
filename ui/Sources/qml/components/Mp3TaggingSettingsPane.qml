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

// Mp3TaggingSettingsPane.qml
//
// The "MP3" sub-page under Tagging in the Settings window, holding the
// MP3/ID3 write preferences. Built to the same rules as the sibling panes: it
// reads and writes the window's staged object (passed in as `settings`),
// never SettingsStore directly, so nothing applies until Apply/OK. Factory
// defaults come from the bound store's *Default constants (one source of
// truth, see `store` below) and drive the modified-from-default accent dot
// and the right-click "Reset to default", exactly as the sibling panes do.
//
// Each row is a label and a control in a RowLayout; every label takes the width
// of the widest one (labelColumnWidth), so the controls line up in a second
// column while each row stays one item with its own right-click reset area.
//
// The four rows are consumed by MetadataEditor. They only affect
// MP3 files; every other format stays on the generic tag write path, which is
// why they live on a format-named sub-page rather than the Tagging root.
// Enum-int mapping (must match SettingsStore's Q_ENUMs; segment order below is
// deliberately the enum order so the picked index maps straight through, the
// same trick as the Spectrum pane's Output/Source pair):
//
//   id3v2Version : 0 ID3v2.3, 1 ID3v2.4
//   id3v1Mode    : 0 Write, 1 Preserve, 2 Strip
//   apeMode      : 0 Preserve, 1 Strip
//   id3v2Encoding: 0 Latin-1, 1 UTF-16, 2 UTF-8
//
// The subdued hint lines under the non-obvious rows update with the staged
// value (the Spectrum pane's pattern). The encoding hint also carries the one
// real footgun, UTF-8 being an ID3v2.4-only encoding (TagLib downgrades it to
// UTF-16 when rendering 2.3 frames, so the combination degrades safely);
// that specific staged combination paints the hint in the app's warning
// amber (Theme.warning, the StatusLogBar/LogStore warning-level color) instead of
// the subdued gray.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import com.rawform.app

Item {
    id: pane

    // The window's staged edit object; this page uses settings.id3v2Version,
    // settings.id3v1Mode, settings.apeMode, and settings.id3v2Encoding.
    property StagedSettings settings: null

    // The SettingsStore whose *Default constants are the reset targets and the
    // modified-from-default reference. Declared and bound by the host rather
    // than read as an ambient global, so this pane states its dependencies
    // and instantiates anywhere.
    property SettingsStore store: null
    property string uiFont: Theme.uiFont

    // The label column is as wide as the widest label, so every row's control
    // starts at the same x. A label's implicit width is constant (its modified
    // dot hides by opacity and keeps its slot), so the column does not move when
    // a dot appears.
    readonly property real labelColumnWidth: Math.max(id3v2VersionLabel.implicitWidth,
                                                      id3v1Label.implicitWidth,
                                                      apeLabel.implicitWidth,
                                                      encodingLabel.implicitWidth)

    // The gap between the label column and the control column.
    readonly property int columnGutter: 24

    // Shared right-click reset menu, same shape as the sibling panes.
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
            text: "Tagging / MP3"
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

        // ----- ID3v2 version -----------------------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 42

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.id3v2Version = pane.store.id3v2VersionDefault
                    }
                    resetMenu.popup()
                }
            }

            // Fills the row rather than taking only a width, so the layout's
            // default vertical centering works against the full row height.
            RowLayout {
                anchors.fill: parent
                spacing: pane.columnGutter

                SettingsRowLabel {
                    id: id3v2VersionLabel
                    Layout.preferredWidth: pane.labelColumnWidth
                    text: "ID3v2 version"
                    modified: pane.settings && pane.settings.id3v2Version !== pane.store.id3v2VersionDefault
                }

                SegmentSelect {
                    labels: ["ID3v2.3", "ID3v2.4"]
                    segWidth: 72
                    value: pane.settings ? pane.settings.id3v2Version : 0
                    onPicked: function (v) {
                        if (pane.settings) pane.settings.id3v2Version = v
                    }
                }

                Item { Layout.fillWidth: true }  // keeps the row's content packed left
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: 11
            text: (pane.settings && pane.settings.id3v2Version === 1)
                  ? "Newer revision, enables UTF-8; old software may not read it."
                  : "The most widely compatible revision."
        }

        // ----- ID3v1 -------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.topMargin: 8
            implicitHeight: 42

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.id3v1Mode = pane.store.id3v1ModeDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLayout {
                anchors.fill: parent
                spacing: pane.columnGutter

                SettingsRowLabel {
                    id: id3v1Label
                    Layout.preferredWidth: pane.labelColumnWidth
                    text: "ID3v1 tag"
                    modified: pane.settings && pane.settings.id3v1Mode !== pane.store.id3v1ModeDefault
                }

                SegmentSelect {
                    labels: ["Write", "Preserve", "Strip"]
                    segWidth: 68
                    value: pane.settings ? pane.settings.id3v1Mode : 0
                    onPicked: function (v) {
                        if (pane.settings) pane.settings.id3v1Mode = v
                    }
                }

                Item { Layout.fillWidth: true }  // keeps the row's content packed left
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: 11
            text: {
                if (!pane.settings) return ""
                switch (pane.settings.id3v1Mode) {
                case 0:  return "Always emit a legacy ID3v1 block alongside ID3v2."
                case 2:  return "Remove any ID3v1 block on save."
                default: return "Keep an existing ID3v1 block updated; never add one."
                }
            }
        }

        // ----- APEv2 -------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.topMargin: 8
            implicitHeight: 42

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.apeMode = pane.store.apeModeDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLayout {
                anchors.fill: parent
                spacing: pane.columnGutter

                SettingsRowLabel {
                    id: apeLabel
                    Layout.preferredWidth: pane.labelColumnWidth
                    text: "APEv2 tag"
                    modified: pane.settings && pane.settings.apeMode !== pane.store.apeModeDefault
                }

                SegmentSelect {
                    labels: ["Preserve", "Strip"]
                    segWidth: 68
                    value: pane.settings ? pane.settings.apeMode : 0
                    onPicked: function (v) {
                        if (pane.settings) pane.settings.apeMode = v
                    }
                }

                Item { Layout.fillWidth: true }  // keeps the row's content packed left
            }
        }

        // ----- ID3v2 text encoding -----------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.topMargin: 8
            implicitHeight: 42

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.id3v2Encoding = pane.store.id3v2EncodingDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLayout {
                anchors.fill: parent
                spacing: pane.columnGutter

                SettingsRowLabel {
                    id: encodingLabel
                    Layout.preferredWidth: pane.labelColumnWidth
                    text: "Text encoding"
                    modified: pane.settings && pane.settings.id3v2Encoding !== pane.store.id3v2EncodingDefault
                }

                SegmentSelect {
                    labels: ["Latin-1", "UTF-16", "UTF-8"]
                    segWidth: 62
                    value: pane.settings ? pane.settings.id3v2Encoding : 1
                    onPicked: function (v) {
                        if (pane.settings) pane.settings.id3v2Encoding = v
                    }
                }

                Item { Layout.fillWidth: true }  // keeps the row's content packed left
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            // The UTF-8-under-2.3 line is a real conflict warning, not a
            // neutral hint, so it takes the app's warning-level amber (the
            // StatusLogBar/LogStore color). Both bindings track the staged
            // values, so flipping either segment updates text and color live.
            color: (pane.settings
                    && pane.settings.id3v2Encoding === 2
                    && pane.settings.id3v2Version === 0)
                   ? Theme.warning : Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: 11
            text: {
                if (!pane.settings) return ""
                if (pane.settings.id3v2Encoding === 2 && pane.settings.id3v2Version === 0)
                    return "UTF-8 needs ID3v2.4; 2.3 frames fall back to UTF-16."
                switch (pane.settings.id3v2Encoding) {
                case 0:  return "Smallest frames; Western European characters only."
                case 2:  return "Full Unicode, compact; requires ID3v2.4."
                default: return "Full Unicode, readable everywhere."
                }
            }
        }

        Item { Layout.fillHeight: true }  // push rows to the top
    }

    // -----------------------------------------------------------------------
    // Generalized segmented selector: the RG pane's Off/Track/Album control with
    // the labels and segment width as properties, since this page needs four
    // different segment sets. Picked index == enum value by construction (the
    // labels arrays above are in enum order).
    // -----------------------------------------------------------------------
    component SegmentSelect: Row {
        id: ss
        property var labels: []
        property int segWidth: 68
        property int value: 0
        signal picked(int value)
        spacing: 0

        Repeater {
            model: ss.labels
            delegate: Rectangle {
                id: seg
                required property int index
                required property string modelData
                width: ss.segWidth
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
