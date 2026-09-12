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
import QtQuick.Dialogs
import com.rawform.app

/*
 * PlaylistDialogs.qml
 *
 * The four native file dialogs (Add files, Add folder, Save playlist, Open
 * playlist) and the consequences of accepting them, behind one controller.
 * MainWindow only instantiates this and forwards ThemedMenuBar's request
 * signals to the open* functions below; the menu holds no reference to a
 * dialog and MainWindow holds no dialog ids.
 *
 * Dependencies are declared and typed, never ambient:
 *  - tabs: the PlaylistTabs registry (scan targets, tab creation, open)
 *  - store: the PlaylistStore (.rwfpl / .m3u / .m3u8 save)
 *  - columnSource: the PlaylistView whose live column order/widths a save
 *    snapshots (the C++ store cannot see the QML-owned header widths)
 *
 * The root is a zero-size, invisible Item rather than a QtObject: FileDialog
 * needs an Item ancestry to find its transient parent window, which is what
 * centers the dialogs on the main window.
 */
Item {
    id: host
    visible: false

    property PlaylistTabs tabs: null
    property PlaylistStore store: null
    property PlaylistView columnSource: null

    // One function per menu action; the host wires ThemedMenuBar's request
    // signals straight to these.
    function openAddFiles() { addFilesDialog.open() }
    function openAddFolder() { addFolderDialog.open() }
    function openSavePlaylist() { savePlaylistDialog.open() }
    function openOpenPlaylist() { openPlaylistDialog.open() }

    // Native "Add files" / "Add folder" dialogs.
    //  - Add files: append into the ACTIVE playlist (-1 = append), routed through
    //    PlaylistTabs so the scan lands in the tab active at launch time.
    //  - Add folder: opens the folder as a NEW tab, named by the same precedence
    //    as a tab-strip drop (a top-level .m3u inside the folder, else the folder
    //    name), then scans the folder into that new (now-active) tab.
    FileDialog {
        id: addFilesDialog
        fileMode: FileDialog.OpenFiles
        nameFilters: [
            "Audio & playlists (*.flac *.mp3 *.wav *.ogg *.oga *.opus *.m4a *.mp4 *.aac *.wv *.ape *.wma *.aiff *.aif *.mpc *.tta *.ac3 *.dts *.m3u *.m3u8)",
            "All files (*)"
        ]
        title: "Add files"

        onAccepted: host.tabs.scanIntoActive(selectedFiles, -1)
    }

    FolderDialog {
        id: addFolderDialog
        title: "Add folder"

        onAccepted: {
            host.tabs.newPlaylist(host.tabs.suggestTabName([selectedFolder]))
            host.tabs.scanIntoActive([selectedFolder], -1)
        }
    }

    // Save the ACTIVE playlist (a user-chosen permanent copy, distinct from the
    // per-tab live autosave). The store snapshots from whatever model is set on
    // it, so point it at the active tab first.
    //
    // THREE FORMATS, M3U DEFAULT. Qt picks nameFilters[0] as the initial filter,
    // so putting M3U first IS the default. There is deliberately no
    // defaultSuffix: it can only ever append one fixed extension, so it cannot
    // follow a filter change, and it would fight the C++ resolution below. All
    // suffix policy lives in PlaylistFormat.h; this dialog only reports which
    // filter the user had selected.
    FileDialog {
        id: savePlaylistDialog
        fileMode: FileDialog.SaveFile
        nameFilters: [
            "M3U playlist (*.m3u)",
            "M3U8 playlist (*.m3u8)",
            "rawform playlist (*.rwfpl)"
        ]
        title: "Save playlist"

        // Parallel to nameFilters above, index for index: the stable format id
        // C++ understands. Declared adjacent to the filters on purpose, since
        // the two lists must be edited together or not at all.
        readonly property var formatIds: ["m3u", "m3u8", "rwfpl"]

        onAccepted: {
            // A native panel can report no usable filter index; fall back to the
            // first id, which is the same default the C++ side would choose.
            const filterIndex = savePlaylistDialog.selectedNameFilter.index
            const formatHint =
                (filterIndex >= 0 && filterIndex < savePlaylistDialog.formatIds.length)
                    ? savePlaylistDialog.formatIds[filterIndex]
                    : savePlaylistDialog.formatIds[0]

            // The linter cannot resolve activeModel's PlaylistModel return
            // type here even though the type is registered; runtime verified.
            // (Do not start this comment with the word qmllint: any comment
            // leading with it is parsed as a lint directive.)
            host.store.model = host.tabs.activeModel  // qmllint disable unresolved-type
            // The position snapshot rides along, so the saved.rwfpl
            // embeds the live focus row and scroll position and restores them
            // when opened. Read from the view for the same reason the layout
            // is: the store can't see either.
            host.store.save(selectedFile,
                            host.columnSource.currentColumnOrder(),
                            host.columnSource.currentColumnWidths(),
                            formatHint,
                            host.columnSource.currentFocusRow(),
                            host.columnSource.currentScrollRow())
        }
    }

    // Open hands the file to PlaylistTabs, which decides by suffix: a .rwfpl is
    // read into a NEW tab as a fresh live copy, an .m3u/.m3u8 opens a new tab
    // and is expanded through the scanner (it stores file references only).
    FileDialog {
        id: openPlaylistDialog
        fileMode: FileDialog.OpenFile
        nameFilters: [
            "Playlists (*.rwfpl *.m3u *.m3u8)",
            "rawform playlist (*.rwfpl)",
            "M3U playlist (*.m3u *.m3u8)",
            "All files (*)"
        ]
        title: "Open playlist"

        onAccepted: host.tabs.openInNewTab(selectedFile)
    }
}
