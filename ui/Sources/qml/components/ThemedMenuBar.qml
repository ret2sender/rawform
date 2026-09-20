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

// ThemedMenuBar.qml
//
// The main window's menu bar: File, Edit, View, Playback and Help labels in
// a Row, each with a themed drop-down (ThemedMenu) opened through the host
// window's openMenu so the popup is positioned under its label, and an
// Alt+letter shortcut that toggles it.
//
// Everything the bar can DO is a signal (addFilesRequested, settingsRequested,
// aboutRequested, ...); the host owns the doing. The bar holds no reference
// to dialogs, stores, views or windows, so it instantiates anywhere and the
// wiring reads in one place at the host. The only host member it touches is
// openMenu, resolved through the Window attached property rather than an
// ancestor id.
//
// A command's keyboard shortcut is declared on its menu item (see
// ThemedMenuItem's `shortcut`): the item owns the trigger and the hint, so a
// key that reaches the host does so through the same signal as a click on the
// row. Keys with no menu entry (the playlist cursor and reveal keys) live in
// PlaylistView instead.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Rectangle {
    id: root
    color: "transparent"
    implicitHeight: 24

    // The host window, resolved via the Window attached property rather than
    // an ancestor id. openMenu is declared on MainWindow's ApplicationWindow
    // root, and Window.window IS that object, so the member resolves. It is
    // the only host-window member this file touches.
    readonly property var hostWindow: root.Window.window

    // Everything this menu bar can DO is a signal; the host owns the doing.
    // The bar renders labels, opens/closes its menus, and emits. It holds no
    // reference to dialogs, stores, views, or windows, so it instantiates
    // anywhere and the wiring is readable in one place at the host. None of
    // the actions below need an enabled/checked binding; if one ever
    // does, the state comes DOWN through a declared property, mirroring how
    // these requests go UP through signals.
    signal addFilesRequested()
    signal addFolderRequested()
    signal newPlaylistRequested()
    signal openPlaylistRequested()
    signal savePlaylistRequested()
    signal closePlaylistRequested()
    signal reloadSelectedRequested()
    signal removeUnavailableRequested()
    signal settingsRequested()
    signal saveColumnLayoutRequested()
    signal playPauseRequested()
    signal stopRequested()
    signal previousRequested()
    signal nextRequested()
    signal aboutRequested()

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 20

        Text {
            id: fileMenuLabel
            color: fileMenuHover.hovered ? Theme.accentHover : Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
            text: "<u>F</u>ile"

            HoverHandler { id: fileMenuHover }

            Shortcut {
                context: Qt.ApplicationShortcut
                sequence: "Alt+F"

                onActivated: fileMenu.visible
                    ? fileMenu.close()
                    : root.hostWindow.openMenu(fileMenuLabel, fileMenu)
            }

            TapHandler {
                property bool wasOpenOnPress: false

                onPressedChanged: {
                    if (pressed)
                        wasOpenOnPress = fileMenu.visible
                }

                onTapped: {
                    if (wasOpenOnPress)
                        fileMenu.close()
                    else
                        root.hostWindow.openMenu(fileMenuLabel, fileMenu)
                }
            }

            ThemedMenu {
                id: fileMenu

                ThemedMenuItem {
                    text: "Add files…"
                    onTriggered: root.addFilesRequested()
                }
                ThemedMenuItem {
                    text: "Add folder…"
                    onTriggered: root.addFolderRequested()
                }
                MenuSeparator {}
                ThemedMenuItem {
                    text: "New Playlist"
                    shortcut: "Ctrl+N"
                    onTriggered: root.newPlaylistRequested()
                }
                ThemedMenuItem {
                    text: "Open playlist…"
                    shortcut: "Ctrl+O"
                    onTriggered: root.openPlaylistRequested()
                }
                ThemedMenuItem {
                    text: "Save playlist…"
                    shortcut: "Ctrl+S"
                    onTriggered: root.savePlaylistRequested()
                }
                ThemedMenuItem {
                    // Closes the active tab; the host owns the tab manager,
                    // which never leaves zero tabs.
                    text: "Close playlist"
                    shortcut: "Ctrl+W"
                    onTriggered: root.closePlaylistRequested()
                }
            }
        }

        Text {
            id: editMenuLabel
            color: editMenuHover.hovered ? Theme.accentHover : Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
            text: "<u>E</u>dit"

            HoverHandler { id: editMenuHover }

            Shortcut {
                context: Qt.ApplicationShortcut
                sequence: "Alt+E"

                onActivated: editMenu.visible
                    ? editMenu.close()
                    : root.hostWindow.openMenu(editMenuLabel, editMenu)
            }

            TapHandler {
                property bool wasOpenOnPress: false

                onPressedChanged: {
                    if (pressed)
                        wasOpenOnPress = editMenu.visible
                }

                onTapped: {
                    if (wasOpenOnPress)
                        editMenu.close()
                    else
                        root.hostWindow.openMenu(editMenuLabel, editMenu)
                }
            }

            ThemedMenu {
                id: editMenu

                ThemedMenuItem {
                    // Semantics (selection vs whole playlist) live at the
                    // host handler, which owns the reloader.
                    text: "Reload info from file(s)"
                    onTriggered: root.reloadSelectedRequested()
                }
                ThemedMenuItem {
                    // Drops the grayed-out rows; the host handler owns the
                    // model and the guard.
                    text: "Remove missing tracks"
                    onTriggered: root.removeUnavailableRequested()
                }
                MenuSeparator {}
                ThemedMenuItem {
                    text: "Settings\u2026"
                    shortcut: "Ctrl+P"
                    onTriggered: root.settingsRequested()
                }
            }
        }

        Text {
            id: viewMenuLabel
            color: viewMenuHover.hovered ? Theme.accentHover : Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
            text: "<u>V</u>iew"

            HoverHandler { id: viewMenuHover }

            Shortcut {
                // Application context, not the default
                // window context: while the menu is open
                // it holds the keyboard focus scope, and a
                // window-context shortcut would go dormant
                // there, so the second Alt+V would never
                // fire and the menu wouldn't toggle closed.
                context: Qt.ApplicationShortcut
                sequence: "Alt+V"

                onActivated: viewMenu.visible
                    ? viewMenu.close()
                    : root.hostWindow.openMenu(viewMenuLabel, viewMenu)
            }

            TapHandler {
                // The menu closes itself on a press outside it
                // (including a press on this label), so by the
                // time onTapped fires it may already be closing.
                // Capture whether it was up at press time, before
                // that reaction, so a click on "View" while open
                // closes it rather than immediately reopening.
                property bool wasOpenOnPress: false

                onPressedChanged: {
                    if (pressed)
                        wasOpenOnPress = viewMenu.visible
                }

                onTapped: {
                    if (wasOpenOnPress)
                        viewMenu.close()
                    else
                        root.hostWindow.openMenu(viewMenuLabel, viewMenu)
                }
            }

            ThemedMenu {
                id: viewMenu

                ThemedMenuItem {
                    text: "Save Column Layout"
                    onTriggered: root.saveColumnLayoutRequested()
                }
            }
        }

        Text {
            id: playbackMenuLabel
            color: playbackMenuHover.hovered ? Theme.accentHover : Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
            text: "<u>P</u>layback"

            HoverHandler { id: playbackMenuHover }

            Shortcut {
                context: Qt.ApplicationShortcut
                sequence: "Alt+P"

                onActivated: playbackMenu.visible
                    ? playbackMenu.close()
                    : root.hostWindow.openMenu(playbackMenuLabel, playbackMenu)
            }

            TapHandler {
                property bool wasOpenOnPress: false

                onPressedChanged: {
                    if (pressed)
                        wasOpenOnPress = playbackMenu.visible
                }

                onTapped: {
                    if (wasOpenOnPress)
                        playbackMenu.close()
                    else
                        root.hostWindow.openMenu(playbackMenuLabel, playbackMenu)
                }
            }

            ThemedMenu {
                id: playbackMenu

                // The transport commands, each a twin of a PlayerBar button.
                // None needs an enabled binding: the controller treats every
                // one as a state-aware no-op where it does not apply.
                ThemedMenuItem {
                    text: "Play / Pause"
                    shortcut: "Space"
                    onTriggered: root.playPauseRequested()
                }
                ThemedMenuItem {
                    // The backtick key left of 1 on a US layout; the alias
                    // makes the shifted tilde on the same key work too.
                    text: "Stop"
                    shortcut: "Ctrl+`"
                    shortcutAliases: ["Ctrl+~"]
                    onTriggered: root.stopRequested()
                }
                MenuSeparator {}
                ThemedMenuItem {
                    // The < and > keycaps are the shifted comma and period
                    // on a US layout; the aliases accept the unshifted key
                    // so Ctrl plus the keycap works with or without Shift.
                    text: "Previous"
                    shortcut: "Ctrl+<"
                    shortcutAliases: ["Ctrl+,"]
                    onTriggered: root.previousRequested()
                }
                ThemedMenuItem {
                    text: "Next"
                    shortcut: "Ctrl+>"
                    shortcutAliases: ["Ctrl+."]
                    onTriggered: root.nextRequested()
                }
            }
        }

        Text {
            id: helpMenuLabel
            color: helpMenuHover.hovered ? Theme.accentHover : Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.weight: Font.Bold
            text: "<u>H</u>elp"

            HoverHandler { id: helpMenuHover }

            Shortcut {
                context: Qt.ApplicationShortcut
                sequence: "Alt+H"

                onActivated: helpMenu.visible
                    ? helpMenu.close()
                    : root.hostWindow.openMenu(helpMenuLabel, helpMenu)
            }

            TapHandler {
                property bool wasOpenOnPress: false

                onPressedChanged: {
                    if (pressed)
                        wasOpenOnPress = helpMenu.visible
                }

                onTapped: {
                    if (wasOpenOnPress)
                        helpMenu.close()
                    else
                        root.hostWindow.openMenu(helpMenuLabel, helpMenu)
                }
            }

            ThemedMenu {
                id: helpMenu

                ThemedMenuItem {
                    text: "About rawform"
                    onTriggered: root.aboutRequested()
                }
            }
        }
    }
}
