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

// MetadataView.qml
//
// Metadata pane: a two-column Name/Value table, grouped into sections
// ("Metadata", "Location") with purple section-header rows, matching the
// Figma design. Driven by MetadataModel, which is populated in C++ whenever
// the playlist's current (anchor) row changes.
//
// Section rows are flagged via the isSection role; the delegate renders those
// as a full-width purple label and the field rows as Name | Value.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import com.rawform.app

Item {
    id: root

    property MetadataModel model

    // Font family for all text. Defaults to the Theme singleton's UI family,
    // which resolves in any window (a separate Window is outside MainWindow's
    // id scope, so a FontLoader id there would be unreachable; the singleton
    // sidesteps that). The property remains as an override surface for hosts
    // that want a different family.
    property string uiFont: Theme.uiFont

    // Default width of the Name column. The column is user-resizable (the
    // header owns the live width via explicitColumnWidth); this is the fallback
    // shown until the first drag. Value takes whatever is left, or grows.
    readonly property int nameColumnWidth: 120

    // Single source of truth for the two column widths, read by BOTH the header
    // and the body providers so the two can never drift out of alignment.
    //  - Name:  the user's resized width if the header has one, else the default.
    //  - Value: fill the leftover viewport when everything fits (so the zebra
    //           striping spans the pane), else grow to the widest value so the
    //           table overflows and the horizontal scrollbar engages.
    function _nameWidth() {
        var w = header.explicitColumnWidth(0) // -1 until the user drags it
        return w >= 0 ? w : root.nameColumnWidth
    }
    function _valueWidth(viewportW) {
        var fill = viewportW - _nameWidth()
        var content = root._valueContentWidth + 16 + 2 // 8+8 px margins, +2 safety
        return Math.max(80, Math.max(fill, content))
    }

    // Natural pixel width of the WIDEST Value cell across all rows. Drives the
    // Value column width below: when this exceeds the leftover viewport, the
    // Value column grows past the pane and the horizontal scrollbar engages.
    // Recomputed off the model's dataChanged() (which fires on every selection),
    // not from the delegates, so it stays correct even when a row is scrolled
    // out of the loaded set (delegate-reported widths would not).
    property int _valueContentWidth: 0

    // Off-screen ruler for the Value text. Must mirror the Value cell's font
    // (uiFont, 12px) so the measurement matches what actually paints.
    TextMetrics {
        id: valueMetrics
        font.family: root.uiFont
        font.pixelSize: 12
    }

    // Measure every row's Value string and keep the widest. metaTable.rows is
    // the model's row count; valueAt() returns "" for section rows (zero width),
    // so they never inflate the result. forceLayout() is the caller's job after
    // this, since TableView caches columnWidthProvider results.
    function _recomputeValueWidth() {
        if (!root.model) {
            root._valueContentWidth = 0
            return
        }
        var maxW = 0
        var n = metaTable.rows
        for (var r = 0; r < n; ++r) {
            valueMetrics.text = root.model.valueAt(r)
            if (valueMetrics.advanceWidth > maxW)
                maxW = valueMetrics.advanceWidth
        }
        root._valueContentWidth = Math.ceil(maxW)
    }

    // Re-measure the widest Value and re-run both providers. dataChanged covers
    // the common case (same shape, values changed); modelReset covers a
    // structural change (a conditional row appeared or disappeared).
    function _remeasure() {
        _recomputeValueWidth()
        header.forceLayout()
        metaTable.forceLayout()
    }

    Connections {
        target: root.model
        function onDataChanged() { root._remeasure() }
        function onModelReset()  { root._remeasure() }
    }

    // Initial pass (values are empty at startup, so this measures to 0 and the
    // Value column simply fills the pane until the first selection arrives).
    Component.onCompleted: _remeasure()

    // Hover over the whole pane drives the frame stroke (same as PlaylistView).
    HoverHandler {
        id: paneHover
    }

    // Rounded, clipped content. Carries the pane background so the corners
    // show #1B1B1B. The stroke is drawn separately on top (below).
    Rectangle {
        id: contentClip
        anchors.fill: parent
        color: Theme.surfacePage
        radius: 4
        clip: true

        Column {
            anchors.fill: parent
            spacing: 0

        // Header row (Name / Value). INDEPENDENT (not synced to the body): bound
        // straight to the model so it always has realized columns, which is what
        // makes the built-in column resize work. This mirrors PlaylistView; here
        // it gives a draggable Name/Value boundary, so the Name column resizes.
        HorizontalHeaderView {
            id: header
            clip: true
            model: root.model
            width: parent.width
            interactive: false
            // No syncView, so mirror the body's horizontal scroll by hand to keep
            // the header columns aligned over the body when the Value overflows.
            contentX: metaTable.contentX
            resizableColumns: true

            // The header owns the widths; the body reads the SAME source (the two
            // _*Width helpers on root), so header and body always agree.
            columnWidthProvider: function (column) {
                return column === 0 ? root._nameWidth()
                                    : root._valueWidth(header.width)
            }

            // A resize changes the header's contentWidth; re-run the body so the
            // Value column tracks the new Name width. A pane-width change re-runs
            // the header itself (the body has its own onWidthChanged below).
            onContentWidthChanged: metaTable.forceLayout()
            onWidthChanged: forceLayout()

            delegate: HeaderCell {}
        }

        TableView {
            id: metaTable
            width: parent.width
            height: parent.height - header.height
            clip: true

            model: root.model

            columnSpacing: 0
            rowSpacing: 0
            boundsBehavior: Flickable.StopAtBounds

            columnWidthProvider: function (column) {
                // Read the SAME width source as the header (above), so the two
                // stay aligned. Name = the header's resized/default width; Value
                // = fill-or-grow. See _nameWidth/_valueWidth on root for the rule.
                return column === 0 ? root._nameWidth()
                                    : root._valueWidth(metaTable.width)
            }
            rowHeightProvider: function (row) {
                // Field rows are 19px. Section headers are 26px, plus 3px extra
                // for every section after the first; that extra space sits above
                // the title, separating it from the previous section. Row 0 gets
                // no top gap (nothing above it).
                if (!root.model || !root.model.isSectionRow(row))
                    return 19
                return row === 0 ? 26 : 29
            }

            // Re-layout when the pane width changes so the Value column tracks.
            onWidthChanged: forceLayout()

            ScrollBar.vertical: ScrollBar {}
            ScrollBar.horizontal: ScrollBar {}

            delegate: Item {
                id: mcell

                required property int row
                required property int column
                required property var display
                required property bool isSection

                // --- Section header row ------------------------------------
                // #232323 band, 26px visual height. For sections after the
                // first, the row is 3px taller and the band is pushed down by
                // that amount, so the extra space lands ABOVE the title.
                Rectangle {
                    visible: mcell.isSection
                    anchors.fill: parent
                    anchors.topMargin: mcell.row === 0 ? 2 : 5
                    anchors.bottomMargin: 2
                    color: Theme.surfacePanel

                    Text {
                        visible: mcell.column === 0
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        color: Theme.textAccent
                        text: mcell.display
                        font.family: root.uiFont
                        font.pixelSize: root.model && root.model.isSectionRow(mcell.row) ? 16 : 12
                        font.weight: Font.Bold
                    }
                }

                // --- Field row ---------------------------------------------
                // Alternating background, restarting at #232323 for the first
                // field of each section (parity from fieldIndexInSection).
                Rectangle {
                    visible: !mcell.isSection
                    anchors.fill: parent
                    color: {
                        if (!root.model)
                            return Theme.surfacePanel
                        var i = root.model.fieldIndexInSection(mcell.row)
                        return (i % 2 === 0) ? Theme.rowEven : Theme.rowOdd
                    }

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        text: mcell.display
                        // Name column dimmer than the value, matching the design.
                        color: mcell.column === 0 ? Theme.textMuted : Theme.textPrimary
                        font.family: root.uiFont
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }
    }

    // Stroke overlay: drawn last, on top of the content, so the rounded border
    // is never erased at the corners. Transparent fill; hover animates the
    // alpha 10% -> 100%, matching PlaylistView.
    Rectangle {
        id: strokeOverlay
        anchors.fill: parent
        color: "transparent"
        radius: 4
        border.width: 2

        property real strokeOpacity: paneHover.hovered ? 0.25 : 0.10
        Behavior on strokeOpacity {
            NumberAnimation { duration: 150; easing.type: Easing.InOutQuad }
        }
        border.color: Qt.rgba(0xBD / 255, 0xB2 / 255, 0xFF / 255, strokeOpacity)
    }
}
