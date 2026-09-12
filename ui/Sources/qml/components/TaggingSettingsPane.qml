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

import QtQuick
import QtQuick.Layouts


// =============================================================================
// TaggingSettingsPane.qml
//
// The "Tagging" ROOT page in the Settings window. The Settings nav supports
// sub-sections, and the MP3/ID3 write preferences live on
// Mp3TaggingSettingsPane (Tagging > MP3), since every one of them is
// MP3-specific. This page is the selectable parent: it stays in the nav as
// its own page, the place for format-agnostic tagging preferences, of which
// there are none; it carries the framing text below.
//
// The `settings` / `uiFont` properties are declared even though `settings` is
// unused here, so SettingsWindow instantiates every pane uniformly and a
// format-agnostic knob lands here without touching the call site.
// =============================================================================
Item {
    id: pane

    // The window's staged edit object; unused here (see header), declared so
    // the pane is instantiated like its siblings.
    property StagedSettings settings: null
    property string uiFont: Theme.uiFont

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Text {
            text: "Tagging"
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

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDim
            font.family: pane.uiFont
            font.pixelSize: 11
            text: "Format-specific tagging preferences live in the sub-sections "
                + "on the left; MP3 is the only one so far. Options that apply "
                + "to every format will appear on this page."
        }

        Item { Layout.fillHeight: true }  // push content to the top
    }
}
