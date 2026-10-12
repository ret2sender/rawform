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
// The main window's menu bar: File, Edit, View, Playback and Help titles in
// a Row (MenuTitle: the label, the hover tint, the Alt+letter toggle and the
// click), each declaring its themed drop-down (ThemedMenu) in place, so the
// items and the signals they raise read top to bottom in this one file.
//
// Everything the bar can DO is a signal (addFilesRequested, settingsRequested,
// aboutRequested, ...); the host owns the doing. The bar holds no reference
// to dialogs, stores, views or windows, so it instantiates anywhere and the
// wiring reads in one place at the host. The only host member reached is
// openMenu, which MenuTitle resolves through the Window attached property.
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

    // Everything this menu bar can DO is a signal; the host owns the doing.
    // The bar renders labels, opens/closes its menus, and emits. It holds no
    // reference to dialogs, stores, views, or windows, so it instantiates
    // anywhere and the wiring is readable in one place at the host. The two
    // entries that read state take it DOWN through the declared properties
    // below, mirroring how these requests go UP through signals.

    // The Playback Queue tab is in the strip (Playback > Show / Hide
    // Playback Queue flips its verb on this).
    property bool queueVisible: false
    // The ACTIVE tab is the Playback Queue (View > Save Column Layout names
    // the queue preset on this; the host routes the save accordingly).
    property bool queueTabActive: false

    signal addFilesRequested()
    signal addFolderRequested()
    signal newPlaylistRequested()
    signal openPlaylistRequested()
    signal savePlaylistRequested()
    signal closePlaylistRequested()
    signal findRequested()
    signal reloadSelectedRequested()
    signal removeUnavailableRequested()
    signal settingsRequested()
    signal saveColumnLayoutRequested()
    signal playPauseRequested()
    signal stopRequested()
    signal previousRequested()
    signal nextRequested()
    // The playback queue: append the selection, put it next in line, empty
    // the queue, and show or hide the queue tab.
    signal addToQueueRequested()
    signal playNextRequested()
    signal clearQueueRequested()
    signal toggleQueueRequested()
    signal aboutRequested()

    Row {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 20
        Accessible.role: Accessible.MenuBar

        MenuTitle {
            label: "<u>F</u>ile"
            mnemonic: "Alt+F"
            menu: ThemedMenu {
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

        MenuTitle {
            label: "<u>E</u>dit"
            mnemonic: "Alt+E"
            menu: ThemedMenu {
                id: editMenu

                ThemedMenuItem {
                    text: "Find\u2026"
                    shortcut: "Ctrl+F"
                    onTriggered: root.findRequested()
                }
                MenuSeparator {}
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

        MenuTitle {
            label: "<u>V</u>iew"
            mnemonic: "Alt+V"
            menu: ThemedMenu {
                id: viewMenu

                ThemedMenuItem {
                    // One entry, two targets: the new-tab default, or the
                    // queue's own preset while the queue tab is active. The
                    // label says which, the host does the routing.
                    text: root.queueTabActive ? "Save Playback Queue Column Layout"
                                              : "Save Column Layout"
                    onTriggered: root.saveColumnLayoutRequested()
                }
            }
        }

        MenuTitle {
            label: "<u>P</u>layback"
            mnemonic: "Alt+P"
            menu: ThemedMenu {
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
                MenuSeparator {}
                // The playback queue. Bare letters, like Space above: a
                // focused text field keeps them (it accepts the shortcut
                // override), so Q types a q in the rename editor and queues
                // the selection everywhere else. The host no-ops both when
                // the active tab is the queue itself.
                ThemedMenuItem {
                    text: "Add to Playback Queue"
                    shortcut: "Q"
                    onTriggered: root.addToQueueRequested()
                }
                ThemedMenuItem {
                    // The front of the queue, in playlist order.
                    text: "Play Next"
                    shortcut: "Shift+Q"
                    onTriggered: root.playNextRequested()
                }
                ThemedMenuItem {
                    text: "Clear Playback Queue"
                    onTriggered: root.clearQueueRequested()
                }
                MenuSeparator {}
                ThemedMenuItem {
                    // A verb that flips rather than a check mark: a checkable
                    // MenuItem toggles its own `checked` on trigger, which
                    // would break a binding to the tab's real state.
                    text: root.queueVisible ? "Hide Playback Queue"
                                            : "Show Playback Queue"
                    shortcut: "Ctrl+Shift+Q"
                    onTriggered: root.toggleQueueRequested()
                }
            }
        }

        MenuTitle {
            label: "<u>H</u>elp"
            mnemonic: "Alt+H"
            menu: ThemedMenu {
                id: helpMenu

                ThemedMenuItem {
                    text: "About rawform"
                    onTriggered: root.aboutRequested()
                }
            }
        }
    }
}
