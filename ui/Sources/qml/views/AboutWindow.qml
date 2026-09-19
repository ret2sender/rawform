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

// AboutWindow.qml
//
// The About window, opened from Help > About rawform. A non-modal, frameless
// window reusing the main window's chrome: transparent Window, rounded
// Theme.surfacePage body at radius 8 with a 1 px border, a slim custom title
// bar with window-drag and a close button (the SettingsWindow idiom).
//
// CONTENT: the square app logo, a click-to-copy version line, the Qt runtime
// version, the copyright and license lines, and two external links (the
// GitHub repository and the third-party notices file on it). All metadata
// comes from the AppInfo singleton, whose version string flows from
// ui/VERSION.txt through RAWFORM_VERSION_STR; nothing here repeats the
// number.
//
// Purely informational: no staged state, no Apply/OK footer. The close
// button and Escape both simply hide the window, so reopening is cheap and
// the instance shares MainWindow's lifetime.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import com.rawform.app

Window {
    id: aboutWindow

    title: "About rawform"
    width: 420
    height: 470
    // Informational and laid out for exactly this content: fix the size on
    // both axes rather than exposing a resize surface nothing benefits from.
    minimumWidth: width
    maximumWidth: width
    minimumHeight: height
    maximumHeight: height
    color: "transparent"
    flags: Qt.Tool | Qt.FramelessWindowHint
    modality: Qt.NonModal

    // The window to center over on open. Set by the instantiation site
    // (MainWindow); a plain screen-relative fallback keeps openAbout()
    // harmless if it is ever left unset.
    property var hostWindow: null

    // Help > About entry point: center over the host, then show + raise.
    function openAbout() {
        if (aboutWindow.hostWindow) {
            aboutWindow.x = aboutWindow.hostWindow.x
                + (aboutWindow.hostWindow.width - aboutWindow.width) / 2
            aboutWindow.y = aboutWindow.hostWindow.y
                + (aboutWindow.hostWindow.height - aboutWindow.height) / 2
        }
        show()
        raise()
        requestActivate()
    }

    // Escape closes. Window-context (the default), but context alone is NOT exclusivity:
    // this is a hide()-reused instance, so the Shortcut object lives forever, and a
    // still-enabled shortcut in a HIDDEN window keeps participating in Qt's shortcut map.
    // Two enabled matches on one press are ambiguous, and Qt then fires neither's
    // onActivated (it rotates activatedAmbiguously across the candidates), which surfaced
    // as dead or double-press Escape in OTHER windows after this one had been shown and
    // hidden. The gate ungrabs the shortcut outside this window's turn. NOT
    // Window.active: that is QWindow::isActive(), a transient-GROUP activation every
    // visible secondary window reports together, which kept two-open-window presses
    // ambiguous. The strict WindowFocus.focusWindow identity is singular by definition,
    // so at most one secondary-window Escape shortcut is grabbed at any instant, and a
    // window that can receive the Escape key IS the focus window, so the gate never
    // starves a legitimate press. The plural `sequences` form: StandardKey.Cancel expands
    // to several platform bindings, and the singular `sequence` property binds only the
    // first of them (and warns about the rest at load).
    Shortcut {
        sequences: [StandardKey.Cancel]
        enabled: aboutWindow.visible
                 && WindowFocus.focusWindow === aboutWindow
        onActivated: aboutWindow.hide()
    }

    // Clipboard access for the click-to-copy version line; QML has no
    // direct clipboard API of its own.
    Clipboard { id: clip }

    // -----------------------------------------------------------------------
    // Body: rounded frame matching the main window.
    // -----------------------------------------------------------------------
    Rectangle {
        id: windowBody
        anchors.fill: parent
        color: Theme.surfacePage
        radius: 8
        border.color: Theme.separatorStrong
        border.width: 1

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1   // sit inside the 1 px border
            spacing: 0

            // ----- title bar: drag + close ---------------------------------
            Item {
                id: titleBar
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                // Drag the window from the title bar (frameless, so manual).
                MouseArea {
                    anchors.fill: parent
                    onPressed: aboutWindow.startSystemMove()
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "About"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    font.weight: Font.Bold
                }

                // Close: just hide
                ToolDialogCloseButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: aboutWindow.hide()
                }
            }

            // ----- content -------------------------------------------------
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 0

                    // The square logo. A plain Image, deliberately NOT AppIcon:
                    // AppIcon sets sourceSize, which is the right call for SVG
                    // (rasterize at logical size x devicePixelRatio) but wrong
                    // for a raster asset; it would decode the 512 px PNG down
                    // to 160 px once and then upscale on high-DPI screens,
                    // blurring it. Loading at native size and letting the
                    // scenegraph scale down keeps it crisp at any ratio up to
                    // 3.2x (512 / 160).
                    Image {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 160
                        Layout.preferredHeight: 160
                        source: "../../images/rawform_logo_simple.png"
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        mipmap: true
                    }

                    // Version line. Clicking copies the full build info, handy
                    // for bug reports, with brief inline feedback.
                    Text {
                        id: versionText

                        property bool copied: false

                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 14
                        color: versionText.copied ? Theme.accent : Theme.textPrimary
                        font.family: Theme.monoFont
                        font.pixelSize: 13
                        text: versionText.copied
                              ? "Copied!"
                              : "Version " + AppInfo.version

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }

                        TapHandler {
                            onTapped: {
                                clip.setText(
                                    "rawform " + AppInfo.version
                                    + " (Qt " + AppInfo.qtVersion + ", "
                                    + Qt.platform.os + ")")
                                versionText.copied = true
                                copiedResetTimer.restart()
                            }
                        }

                        Timer {
                            id: copiedResetTimer
                            interval: 1200
                            onTriggered: versionText.copied = false
                        }
                    }

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 8
                        color: Theme.textMuted
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        text: "Built with Qt " + AppInfo.qtVersion
                    }

                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 12
                        color: Theme.textMuted
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        text: "Copyright \u00A9 2026 Etienne Fleurant\n"
                              + "Released under the GNU GPL v3.0 or later"
                    }

                    // External links. Plain Texts styled as links rather than
                    // Text.RichText anchors: hover state and cursor come from
                    // the same handler pattern as everything else in the app,
                    // and the URL lives in one obvious place per link.
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 16
                        spacing: 24

                        Text {
                            id: githubLink
                            color: githubHover.hovered ? Theme.accentHover : Theme.accentSoft
                            font.family: Theme.uiFont
                            font.pixelSize: 12
                            font.underline: true
                            text: "GitHub"

                            HoverHandler {
                                id: githubHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: Qt.openUrlExternally(
                                    "https://github.com/ret2sender/rawform")
                            }
                        }

                        Text {
                            id: noticesLink
                            color: noticesHover.hovered ? Theme.accentHover : Theme.accentSoft
                            font.family: Theme.uiFont
                            font.pixelSize: 12
                            font.underline: true
                            text: "Third-party notices"

                            HoverHandler {
                                id: noticesHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: Qt.openUrlExternally(
                                    "https://github.com/ret2sender/rawform/blob/main/THIRD-PARTY-NOTICES.md")
                            }
                        }
                    }
                }
            }
        }
    }
}
