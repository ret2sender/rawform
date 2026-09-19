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

// ReplayGainSettingsPane.qml
//
// The ReplayGain page inside the Settings window. Reads and writes the window's
// `staged` object (passed in as `settings`), never the controller directly, so
// nothing applies until the window's Apply/OK. Factory defaults come from the
// controller (the one source of truth) and drive two things per row: a small
// accent dot when the staged value differs from its default, and a right-click
// "Reset to default" that sets it back.
//
// Rows are written out explicitly (mode, two pre-amps, clip prevention, and the
// scan skip-existing toggle) rather than through a slotted
// row component, which keeps the chrome and the injected control cleanly separate
// without fighting QML's default-property rules. The controls themselves
// (segmented mode, dB steppers, a toggle) are local inline components, styled to
// the app rather than native, so the whole page stays one file.
//
// The right-click reset MouseArea is the LOWEST child of each row and accepts only
// the right button, so the control above handles its own left interaction and
// right clicks fall through to reset.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import com.rawform.app

Item {
    id: pane

    // The window's staged edit object: { mode, preampDb, untaggedPreampDb, clip }.
    property StagedSettings settings: null

    // The AudioController whose replayGain*Default constants are the reset
    // targets and the modified-from-default references. Declared and bound by
    // the host rather than read as an ambient global.
    property AudioController controller: null
    property string uiFont: Theme.uiFont

    function _near(a, b) { return Math.abs(a - b) < 1e-9 }

    // Shared right-click reset menu. A row sets doReset to its own closure, then
    // pops this at the cursor.
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
            text: "ReplayGain"
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

        // ----- Mode --------------------------------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 46

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.mode = pane.controller.replayGainModeDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLabel {
                text: "Mode"
                modified: pane.settings && pane.settings.mode !== pane.controller.replayGainModeDefault
            }

            ModeSelect {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                value: pane.settings ? pane.settings.mode : 0
                onPicked: function (v) { if (pane.settings) pane.settings.mode = v }
            }
        }

        // ----- Pre-amp for tagged tracks -----------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 46

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.preampDb = pane.controller.replayGainPreampDbDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLabel {
                text: "Pre-amp (tagged)"
                modified: pane.settings && !pane._near(pane.settings.preampDb,
                                                       pane.controller.replayGainPreampDbDefault)
            }

            DbField {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                value: pane.settings ? pane.settings.preampDb : 0
                onMoved: function (v) { if (pane.settings) pane.settings.preampDb = v }
            }
        }

        // ----- Pre-amp for untagged tracks ---------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 46

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.untaggedPreampDb = pane.controller.replayGainUntaggedPreampDbDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLabel {
                text: "Pre-amp (untagged)"
                modified: pane.settings && !pane._near(pane.settings.untaggedPreampDb,
                                                       pane.controller.replayGainUntaggedPreampDbDefault)
            }

            DbField {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                value: pane.settings ? pane.settings.untaggedPreampDb : 0
                onMoved: function (v) { if (pane.settings) pane.settings.untaggedPreampDb = v }
            }
        }

        // ----- Clip prevention ---------------------------------------------
        Item {
            Layout.fillWidth: true
            implicitHeight: 46

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.clip = pane.controller.replayGainClipPreventionDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLabel {
                text: "Prevent clipping"
                modified: pane.settings && pane.settings.clip !== pane.controller.replayGainClipPreventionDefault
            }

            Toggle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: pane.settings ? pane.settings.clip : true
                onToggled: if (pane.settings) pane.settings.clip = !pane.settings.clip
            }
        }

        // ----- Skip files with existing info (scan-time) -------------------
        // Unlike the rows above this one does not affect playback: it tells a
        // ReplayGain scan to leave tracks (or whole albums) that already carry RG
        // info untouched. The scan path reads it before measuring.
        Item {
            Layout.fillWidth: true
            implicitHeight: 46

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.RightButton
                onClicked: {
                    resetMenu.doReset = function () {
                        if (pane.settings) pane.settings.skipExisting = pane.controller.replayGainScanSkipExistingDefault
                    }
                    resetMenu.popup()
                }
            }

            RowLabel {
                text: "Skip files with existing info when scanning"
                modified: pane.settings && pane.settings.skipExisting !== pane.controller.replayGainScanSkipExistingDefault
            }

            Toggle {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: pane.settings ? pane.settings.skipExisting : false
                onToggled: if (pane.settings) pane.settings.skipExisting = !pane.settings.skipExisting
            }
        }

        Item { Layout.fillHeight: true }  // push rows to the top
    }

    // -----------------------------------------------------------------------
    // The label plus modified-from-default dot, left-aligned in a row.
    // -----------------------------------------------------------------------
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

    // -----------------------------------------------------------------------
    // Segmented Off / Track / Album selector.
    // -----------------------------------------------------------------------
    component ModeSelect: Row {
        id: ms
        property int value: 0
        signal picked(int value)
        spacing: 0

        Repeater {
            model: ["Off", "Track", "Album"]
            delegate: Rectangle {
                id: seg
                required property int index
                required property string modelData
                width: 62
                height: 26
                color: ms.value === seg.index ? Theme.accentSoft
                     : segHover.hovered ? Theme.surfaceControlHover : Theme.surfaceControl
                border.color: Theme.border
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: seg.modelData
                    color: ms.value === seg.index ? Theme.textOnAccent : Theme.textSecondary
                    font.family: pane.uiFont
                    font.pixelSize: 11
                    font.weight: ms.value === seg.index ? Font.Bold : Font.Normal
                }
                HoverHandler { id: segHover }
                TapHandler { onTapped: ms.picked(seg.index) }
            }
        }
    }

    // -----------------------------------------------------------------------
    // A small square stepper button used by the dB field. Hoisted to the pane
    // level because QML does not allow an inline component nested inside another.
    // -----------------------------------------------------------------------
    component StepButton: Rectangle {
        id: sb
        property string glyph: ""
        signal bumped()
        width: 24
        height: 26
        radius: 4
        color: sbHover.hovered ? Theme.surfaceControlHover : Theme.surfaceControl
        border.color: Theme.border
        border.width: 1
        Text {
            anchors.centerIn: parent
            text: sb.glyph
            color: Theme.textSecondary
            font.family: pane.uiFont
            font.pixelSize: 14
            font.weight: Font.Bold
        }
        HoverHandler { id: sbHover }
        TapHandler { onTapped: sb.bumped() }
    }

    // -----------------------------------------------------------------------
    // A dB stepper: minus, an editable value, plus. Emits moved(value). Typing
    // commits on Return or focus-out, then re-binds the text to the live value.
    // -----------------------------------------------------------------------
    component DbField: Row {
        id: dbf
        property real value: 0
        property real minimum: -24
        property real maximum: 24
        property real step: 0.5
        signal moved(real value)
        spacing: 4

        function _commit(v) {
            var c = Math.max(dbf.minimum, Math.min(dbf.maximum, v))
            c = Math.round(c * 10) / 10  // one decimal
            dbf.moved(c)
        }

        StepButton {
            glyph: "\u2212"  // minus sign
            onBumped: dbf._commit(dbf.value - dbf.step)
        }

        Rectangle {
            width: 78
            height: 26
            radius: 4
            color: Theme.surfaceInset
            border.color: valueInput.activeFocus ? Theme.accentSoft : Theme.border
            border.width: 1

            Row {
                anchors.centerIn: parent
                spacing: 3

                TextInput {
                    id: valueInput
                    anchors.verticalCenter: parent.verticalCenter
                    text: dbf.value.toFixed(1)
                    color: Theme.textPrimary
                    font.family: pane.uiFont
                    font.pixelSize: 12
                    horizontalAlignment: TextInput.AlignRight
                    width: 42
                    selectByMouse: true

                    function commit() {
                        var v = parseFloat(valueInput.text.replace(",", "."))
                        if (!isNaN(v))
                            dbf._commit(v)
                        valueInput.text = Qt.binding(function () { return dbf.value.toFixed(1) })
                    }
                    onActiveFocusChanged: if (!activeFocus) commit()
                    Keys.onReturnPressed: commit()
                    Keys.onEnterPressed: commit()
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "dB"
                    color: Theme.textDim
                    font.family: pane.uiFont
                    font.pixelSize: 11
                }
            }
        }

        StepButton {
            glyph: "+"
            onBumped: dbf._commit(dbf.value + dbf.step)
        }
    }

    // -----------------------------------------------------------------------
    // A pill toggle.
    // -----------------------------------------------------------------------
    component Toggle: Rectangle {
        id: tg
        property bool checked: false
        signal toggled()
        width: 42
        height: 22
        radius: 11
        color: checked ? Theme.accentSoft : Theme.border

        Rectangle {
            width: 18
            height: 18
            radius: 9
            color: Theme.surfacePage
            anchors.verticalCenter: parent.verticalCenter
            x: tg.checked ? tg.width - width - 2 : 2
            Behavior on x { NumberAnimation { duration: 90; easing.type: Easing.OutQuad } }
        }
        TapHandler { onTapped: tg.toggled() }
    }
}
