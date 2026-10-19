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

// MainWindow.qml
//
// The application window and the app's wiring layer. Frameless on Linux and
// Windows (rounded, self-outlined windowBackground, custom TitleBar, resize
// grips); titled on macOS, where applyMacOSStyling makes the native title bar
// transparent and full-size-content so the QML title bar row sits under the
// traffic lights (titleBarOffset pulls the content up by the measured native
// height).
//
// Layout: TitleBar over a ThemedSplitView. The left pane stacks the
// ThemedMenuBar, the album art and the MetadataView; the right pane stacks
// the PlaylistTabBar, the PlaylistView and the StatusLogBar, with the
// LogConsole popping up over the playlist. The PlayerBar spans the bottom.
//
// Wiring: this file binds the C++ singletons (AudioController, PlaybackQueue,
// PlaylistTabs, TrackScanner, PlaylistStore, WindowGeometryStore,
// MetadataPaneModel) to the components, which declare what they consume and
// reach no singleton themselves. It forwards ThemedMenuBar's request signals to
// PlaylistDialogs and the tool windows, feeds the status line (live activity
// while busy, otherwise the latest LogStore entry), logs activity edges and
// playback errors, and persists the windowed geometry on quit. Settings, Find,
// About and the file dialogs are created here on their first use and then
// share this window's lifetime (a Loader each, so none of them exists before
// the first frame); Properties, Custom Columns and Rename Files are created by
// PlaylistView.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: applicationWindow
    color: "transparent"
    // macOS must NOT be frameless: the native title bar, and therefore the
    // traffic-light controls, only exists on a titled window. applyMacOSStyling()
    // then makes that title bar transparent and full-size-content so rawform's
    // QML still paints behind it. Every other platform stays frameless and uses
    // the QML TitleBarControls instead.
    flags: Qt.platform.os === "osx" ? Qt.Window
                                    : (Qt.Window | Qt.FramelessWindowHint)
    // Startup geometry (window.yaml). Size restores from the last windowed quit,
    // else the built-in default, clamped to the primary screen by the store;
    // position restores when the store validated it against the connected
    // screens, else the store centers the window on the primary screen's
    // available area. Bound up front rather than nudged in Component.onCompleted
    // so the window is placed from its very first frame, with no visible jump
    // (the default Qt/AppKit placement lands at the left edge on macOS, which is
    // exactly what this avoids). The four values are constants once the store
    // has loaded: the bindings carry no dependency on the window's own size or
    // on the Screen attached property, so a resize or a move to another monitor
    // never re-evaluates them (on cocoa and xcb that re-centered the window
    // under the user's hands; Wayland ignores setX/setY and never showed it).
    height: WindowGeometryStore.startupHeight
    width: WindowGeometryStore.startupWidth
    // Hard floor under the split constraints: playlistPane asks for half the
    // window and mediaDataSplit for 256 px, so below roughly 900 px the two
    // minimums cannot both hold and the SplitView starts inverting. The
    // floor also clamps a hand-edited window.yaml, and wins over the store's
    // screen clamp on a screen smaller than it.
    minimumWidth: 960
    minimumHeight: 600
    x: WindowGeometryStore.startupX
    y: WindowGeometryStore.startupY
    visible: true

    // Flush column widths (and any other debounced edit) on close. The per-tab
    // autosave is a 1500 ms debounce whose write is fire-and-forget on a worker,
    // and nothing drained it on teardown, so a resize made within that window of
    // quitting was lost: the timer never elapsed. Push the LIVE header widths
    // into the active tab's parked copy first (the header is QML-owned, so C++
    // can't read it), then force a synchronous write of every dirty tab.
    onClosing: {
        // Lift any Find filter FIRST (dismiss hides the dialog, which clears
        // PlaylistTabs.activeSearch and re-filters synchronously), so the
        // scroll row stashed below and the current row the flush persists are
        // unfiltered playlist positions. A Find that was never opened does not
        // exist and has no filter to lift.
        if (applicationWindow._findDialog)
            applicationWindow._findDialog.dismiss()
        PlaylistTabs.stashActiveWidths(playlistView.currentColumnWidths())
        // The scroll twin of the widths line above: push the ACTIVE
        // tab's live position (QML-owned, C++ can't read it) into its parking
        // so the flush below writes it to the SCRL chunk. Background tabs
        // were parked at their switch-away and are already covered.
        playlistView.stashScrollPosition()
        PlaylistTabs.flushPendingWrites()
        // Persist the windowed frame only. Closing while maximized or
        // fullscreen skips the write, so the last good windowed geometry in
        // window.yaml survives (the macOS fullscreen-exit fix already leaves a
        // clean windowed frame behind, which is what makes this guard exact).
        if (applicationWindow.visibility === Window.Windowed)
            WindowGeometryStore.save(applicationWindow.x, applicationWindow.y,
                                applicationWindow.width, applicationWindow.height)
    }

    property Item menuBlurSource: windowBackground

    // Native macOS title bar height, written from C++ (applyMacOSStyling) once
    // the window has been styled. Stays 0 on every other platform. With the
    // full-size content view on macOS, the transparent native title bar overlaps
    // the top strip of the Qt content area; this is how many pixels it occupies.
    property int nativeTitleBarHeight: 0

    // Effective top offset applied to the content. Only macOS needs it: it pulls
    // the QML content up under the transparent native title bar so the QML title
    // bar row aligns with it (and the traffic lights sit cleanly over that row).
    // Elsewhere it is 0 and the layout is untouched.
    readonly property int titleBarOffset:
        Qt.platform.os === "osx" ? nativeTitleBarHeight : 0

    // What a title-bar double-click does on macOS, written from C++
    // (applyMacOSStyling) from the system preference behind System Settings >
    // Desktop & Dock > "Double-click a window's title bar to": "Maximize"
    // (zoom, the default), "Minimize", or "None". Ignored on every other
    // platform, where a double-click always maximizes / restores.
    property string macTitleBarDoubleClickAction: "Maximize"

    // Title-bar double-click. Native behavior on every platform: Windows and
    // Linux maximize / restore; macOS obeys the user's preference above. Only
    // the windowed and maximized states take part: a double-click in
    // fullscreen has no title bar to land on, and a minimized window cannot
    // receive one.
    function _onTitleBarDoubleClicked() {
        var state = applicationWindow.visibility
        if (state !== Window.Windowed && state !== Window.Maximized)
            return
        var action = Qt.platform.os === "osx"
                ? applicationWindow.macTitleBarDoubleClickAction : "Maximize"
        if (action === "None")
            return
        if (action === "Minimize") {
            applicationWindow.showMinimized()
            return
        }
        if (state === Window.Maximized)
            applicationWindow.showNormal()
        else
            applicationWindow.showMaximized()
    }

    // Status line content for the StatusLogBar under the playlist pane. While
    // something is busy, show that live activity; otherwise show the most recent
    // logged event from logStore. The activity strings are shared with the edge
    // logging below so the live line and the retained log entry read identically.
    readonly property string _scanMsg: "Scanning library\u2026"
    readonly property string _saveMsg: "Saving playlist\u2026"
    readonly property string _readMsg: "Reading tags\u2026"
    readonly property string _activityText:
          TrackScanner.scanning ? _scanMsg
        : PlaylistStore.busy ? _saveMsg
        : (PlaylistTabs.activeReloader && PlaylistTabs.activeReloader.busy)
              ? _readMsg
        : ""
    readonly property string _statusText:
        _activityText !== "" ? _activityText : logStore.latestText
    readonly property string _statusLevel:
        _activityText !== "" ? "info" : logStore.latestLevel

    // The three tool windows (Settings, Find, About) and the file dialogs are
    // created on their first use, not with this window: each sits behind a
    // Loader that is inactive until its open function flips it, and never
    // goes inactive again, so from the first open on the instance is the
    // hide()-reused one it always was (the Find texts survive a close, the
    // Settings size restores from window.yaml, the Escape shortcuts stay
    // gated on their own window). What this buys is the first frame: a
    // Settings window alone is several hundred QML objects, and none of the
    // four is on screen at startup. A Window loaded this way has the Loader
    // as its object parent and resolves its transient parent through the
    // Loader's window, the same path a Window declared inside a view takes.
    // The typed accessors below are what the rest of this file reaches the
    // instances through (a Loader's item is an untyped object).

    // The non-modal Settings window (Edit > Settings). Renders as its own
    // top-level window, not inline. Fonts come from the Theme singleton, which
    // resolves in any window, so nothing is passed in.
    Loader {
        id: settingsLoader
        active: false
        sourceComponent: SettingsWindow {}
    }
    readonly property SettingsWindow _settingsWindow:
        settingsLoader.item as SettingsWindow

    function _openSettings() {
        settingsLoader.active = true
        applicationWindow._settingsWindow.openSettings()
    }

    // The non-modal Find dialog (Edit > Find). One instance, so the session's
    // last search text survives a close. It follows the ACTIVE tab through the
    // two bindings, and reveals rows through the playlist view.
    Loader {
        id: findLoader
        active: false
        sourceComponent: FindDialog {
            model: PlaylistTabs.activeModel
            view: PlaylistTabs.activeView
            selection: PlaylistTabs.activeSelection
            onRevealRow: function (row) { playlistView.revealRow(row) }
        }
    }
    readonly property FindDialog _findDialog: findLoader.item as FindDialog

    function _openFind() {
        findLoader.active = true
        applicationWindow._findDialog.open()
    }

    // The non-modal About window (Help > About rawform). hostWindow lets
    // openAbout() center it over this window at open.
    Loader {
        id: aboutLoader
        active: false
        sourceComponent: AboutWindow {
            hostWindow: applicationWindow
        }
    }
    readonly property AboutWindow _aboutWindow: aboutLoader.item as AboutWindow

    function _openAbout() {
        aboutLoader.active = true
        applicationWindow._aboutWindow.openAbout()
    }

    // Session log feeding the StatusLogBar (and the pop-up LogConsole).
    // Playback errors land here as error entries; the bar and the console are
    // their only surfaces (the PlayerBar's format slot stays blank on a failed
    // open).
    LogStore { id: logStore }

    Connections {
        target: AudioController
        function onErrorOccurred(message) { logStore.append("error", message) }
        // Notable non-error events (stopping playback because a tag write
        // targets the loaded file) land in the log at warning level.
        function onWarningOccurred(message) { logStore.append("warning", message) }
        // Sink console: the output sink's device-negotiation diagnostics
        // (rate switches, measured bit-perfect outcomes, close-time restores),
        // forwarded from the engine, land at info level.
        function onInfoOccurred(message) { logStore.append("info", message) }
    }

    // Retain playlist actions in the log, not just live on the bar: log the start
    // of each activity, plus a brief completion so the idle bar reads "done"
    // instead of freezing on the last "in progress" line.
    // A playlist file the session could not read: the tab opens empty and
    // the reason lands here, the one surface the user sees.
    Connections {
        target: PlaylistTabs
        function onWarningOccurred(message) { logStore.append("warning", message) }
    }

    Connections {
        target: TrackScanner
        function onScanningChanged() {
            logStore.append("info", TrackScanner.scanning
                            ? applicationWindow._scanMsg : "Library scan complete")
        }
    }
    Connections {
        target: PlaylistStore
        function onBusyChanged() {
            logStore.append("info", PlaylistStore.busy
                            ? applicationWindow._saveMsg : "Playlist saved")
        }
    }
    // activeReloader swaps per active tab, so this target re-binds on tab switch;
    // busy transitions on the current reloader are logged. (Switching away while a
    // read is in flight can miss that tab's completion, which is acceptable.) If
    // tag reads prove too chatty in the log, remove this block.
    Connections {
        target: PlaylistTabs.activeReloader
        function onBusyChanged() {
            if (PlaylistTabs.activeReloader)
                logStore.append("info", PlaylistTabs.activeReloader.busy
                                ? applicationWindow._readMsg : "Tags read")
        }
    }

    function openMenu(menuLabelId, menuId) {
        menuId.popup(menuLabelId, 0, menuLabelId.height + 4)
    }

    // Cycle the playlist tabs from the keyboard, wrapping at both ends.
    // Ctrl+Tab / Ctrl+Shift+Tab on Linux and Windows, Control+Tab /
    // Control+Shift+Tab on macOS ("Meta" is the Control key in Qt's macOS
    // spelling, since "Ctrl" there is Command, whose Tab is the system app
    // switcher). Explicit sequences rather than StandardKey.NextChild, which
    // expands to Cmd+Tab on macOS and to the Forward media key everywhere.
    function _cycleTab(step) {
        var count = PlaylistTabs.count
        if (count < 2)
            return
        PlaylistTabs.currentIndex = (PlaylistTabs.currentIndex + step + count) % count
    }
    Shortcut {
        sequence: Qt.platform.os === "osx" ? "Meta+Tab" : "Ctrl+Tab"
        onActivated: applicationWindow._cycleTab(1)
    }
    Shortcut {
        // Both spellings: a shifted Tab arrives as Key_Backtab on X11 and
        // Windows (Qt's own PreviousChild binding is Ctrl+Shift+Backtab), and
        // as Shift+Tab on some other keymaps.
        sequences: Qt.platform.os === "osx"
                   ? ["Meta+Shift+Tab", "Meta+Shift+Backtab"]
                   : ["Ctrl+Shift+Tab", "Ctrl+Shift+Backtab"]
        onActivated: applicationWindow._cycleTab(-1)
    }

    // The saved DEFAULT column arrangement is applied per TAB in C++:
    // PlaylistTabs.setDefaultLayout() (fed the preset from main.cpp) seeds each
    // brand-new tab, and restored/opened tabs carry their own saved layout. All
    // that's left here is to seed the header widths of the tab that is active at
    // startup: a brand-new tab's widths are ready synchronously (a restored tab's
    // arrive later and re-seed via the onActiveLayoutChanged handler below).
    Component.onCompleted: {
        playlistView.applyWidths(PlaylistTabs.activeWidths());
    }

    // The four native file dialogs (Add files/folder, Save/Open playlist) and
    // their accept-time consequences live behind PlaylistDialogs; MainWindow
    // only supplies the dependencies and forwards the menu's requests. Loaded
    // on the first request, like the tool windows above; the host Item keeps
    // its Item ancestry (the Loader) for the dialogs' transient parent.
    Loader {
        id: dialogsLoader
        active: false
        sourceComponent: PlaylistDialogs {
            tabs: PlaylistTabs
            store: PlaylistStore
            columnSource: playlistView
        }
    }
    readonly property PlaylistDialogs _playlistDialogs:
        dialogsLoader.item as PlaylistDialogs

    function _dialogs() : PlaylistDialogs {
        dialogsLoader.active = true
        return applicationWindow._playlistDialogs
    }

    // Re-seed the playlist header widths when an async .rwfpl load applies its
    // saved layout to the active tab's model: the C++ apply sets the column order
    // but can't touch the QML-owned header widths, so we push the freshly parked
    // widths on here. (Tab switches are handled in PlaylistView via onModelChanged.)
    Connections {
        target: PlaylistTabs
        function onActiveLayoutChanged() {
            playlistView.applyWidths(PlaylistTabs.activeWidths())
        }
    }

    Rectangle {
        id: windowBackground
        anchors.fill: parent
        // On macOS the transparent native title bar occupies the top strip of
        // the window; pull the content up by exactly its measured height so the
        // QML title bar row aligns under it and the traffic lights overlay that
        // row. A negative top margin extends the content above the default
        // content-area origin. Off macOS titleBarOffset is 0, so this is a no-op.
        anchors.topMargin: -applicationWindow.titleBarOffset
        color: Theme.surfacePage
        // The frameless Linux/Windows window rounds its own corners here. On
        // macOS the real (titled) NSWindow already rounds and shadows the frame,
        // so a rounded windowBackground would double-round and clip oddly; keep
        // it square and let the OS do the rounding.
        radius: Qt.platform.os === "osx" ? 0 : 8
        // The 1 px window outline KWin draws around windows it decorates;
        // frameless opts out of that pass, so the window paints its own (the
        // tool windows already do, via their separatorStrong body borders).
        // Gated exactly like the radius above, and for the same reason: macOS
        // outlines the titled window natively, and a second, square line
        // behind the native rounding would double-draw. The edge-touching
        // children (TitleBar, PlayerBar) are transparent, so nothing overdraws
        // the line.
        border.color: Theme.windowOutline
        border.width: Qt.platform.os === "osx" ? 0 : 1

        ColumnLayout {
            id: mainColumn
            anchors.fill: parent
            spacing: 0

            // Title bar
            TitleBar {
                id: titleBar
                Layout.fillWidth: true

                onMoveRequested: applicationWindow.startSystemMove()
                onMaximizeToggleRequested: applicationWindow._onTitleBarDoubleClicked()
            }

            ThemedSplitView {
                id: mainSplit
                Layout.fillHeight: true
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12

                ThemedSplitView {
                    id: mediaDataSplit
                    implicitWidth: 450
                    orientation: Qt.Vertical
                    SplitView.maximumWidth: applicationWindow.width / 2
                    SplitView.minimumWidth: 256

                    ColumnLayout {
                        id: mainMenuAndAlbumArtLayout
                        spacing: 0
                        SplitView.fillWidth: true
                        SplitView.maximumHeight: 450 * 2
                        SplitView.minimumHeight: 450 / 2

                        // The menu bar emits requests; every consequence is
                        // wired here, where the dialogs, stores, views, and the
                        // Settings window all live. The bar itself holds no
                        // reference to any of them.
                        ThemedMenuBar {
                            id: mainMenu
                            Layout.fillWidth: true

                            // State DOWN for the two entries that read it (the
                            // queue tab's verb, the layout preset's name).
                            queueVisible: PlaylistTabs.queueVisible
                            queueTabActive: PlaylistTabs.activeIsQueue

                            onAddFilesRequested: applicationWindow._dialogs().openAddFiles()
                            onAddFolderRequested: applicationWindow._dialogs().openAddFolder()
                            onNewPlaylistRequested: PlaylistTabs.newPlaylist("")
                            onOpenPlaylistRequested: applicationWindow._dialogs().openOpenPlaylist()
                            onSavePlaylistRequested: applicationWindow._dialogs().openSavePlaylist()
                            // The active tab; closeTab never leaves zero tabs
                            // (closing the last one spawns a fresh empty one).
                            onClosePlaylistRequested:
                                PlaylistTabs.closeTab(PlaylistTabs.currentIndex)
                            // Through close(), so onClosing runs the same
                            // flush and geometry save as the title-bar X.
                            onQuitRequested: applicationWindow.close()

                            // With a selection: force-reload those files. With
                            // nothing selected: refresh the whole playlist
                            // (changed files only). Guarded: a fresh empty tab
                            // has no reloader yet.
                            onReloadSelectedRequested: {
                                if (PlaylistTabs.activeReloader)
                                    PlaylistTabs.activeReloader.reloadSelected()
                            }

                            // Drop every row whose file is gone/unreadable (the
                            // grayed-out entries) from the active playlist.
                            onRemoveUnavailableRequested: {
                                if (PlaylistTabs.activeModel)
                                    PlaylistTabs.activeModel.removeUnavailableTracks()
                            }

                            onFindRequested: applicationWindow._openFind()
                            onSettingsRequested: applicationWindow._openSettings()

                            // Transport, the same controller calls the
                            // PlayerBar buttons make, so the menu, its
                            // shortcuts, and the buttons can never diverge.
                            onPlayPauseRequested: AudioController.playPauseToggle()
                            onStopRequested: AudioController.stop()
                            onPreviousRequested: AudioController.previous()
                            onNextRequested: AudioController.next()

                            // The playback queue. The two enqueue verbs act on
                            // the active playlist's selection through the
                            // view (which owns the selection and logs the
                            // outcome); clear and show/hide go straight to
                            // the queue and the tab manager.
                            onAddToQueueRequested: playlistView.enqueueSelection(false)
                            onPlayNextRequested: playlistView.enqueueSelection(true)
                            onClearQueueRequested: {
                                if (PlaybackQueue.count > 0) {
                                    PlaybackQueue.clear()
                                    logStore.append("info", "Playback queue cleared")
                                }
                            }
                            onToggleQueueRequested: PlaylistTabs.toggleQueueTab()

                            onAboutRequested: applicationWindow._openAbout()

                            // Persist the full arrangement (which columns, order,
                            // widths). For a playlist tab: as the startup default,
                            // also refreshing the live default so new tabs created
                            // later this session use it immediately, rather than
                            // only after a restart re-reads the preset file. For
                            // the Playback Queue tab: as the queue's own preset,
                            // which is what seeds its layout at launch (the queue
                            // has no live file); the new-tab default is untouched.
                            // Both branches report through the log: the store
                            // says whether the preset file was written, and a
                            // failed write is the one thing the user cannot
                            // see otherwise (the live default still updates).
                            onSaveColumnLayoutRequested: {
                                var order = playlistView.currentColumnOrder()
                                var widths = playlistView.currentColumnWidths()
                                var forQueue = PlaylistTabs.activeIsQueue
                                var saved = PlaylistStore.saveColumnPreset(
                                            order, widths, forQueue)
                                if (!forQueue)
                                    PlaylistTabs.setDefaultLayout(order, widths)
                                var what = forQueue ? "Playback queue column layout"
                                                    : "Column layout"
                                if (saved)
                                    logStore.append("info", what + " saved")
                                else
                                    logStore.append("warning", what
                                        + " could not be saved (see the console)")
                            }
                        }

                        Rectangle {
                            id: albumArtFrame
                            clip: true
                            color: Theme.surfaceSunken
                            implicitHeight: 450
                            implicitWidth: 450
                            Layout.fillHeight: true
                            Layout.fillWidth: true
                            radius: 4

                            // Bumped on every selection change AND tab switch
                            // (PlaylistTabs coalesces both into one signal), so the
                            // art binding below re-evaluates against the FULL
                            // selection, not just currentIndex. currentIndex alone
                            // would miss a range that grew or shrank without the
                            // anchor moving; the binding reads this counter to be
                            // driven by the whole selection.
                            property int _selectionRevision: 0
                            Connections {
                                target: PlaylistTabs
                                function onActiveSelectionChanged() {
                                    albumArtFrame._selectionRevision++
                                }
                            }

                            // Art for the current selection, resolved by the C++
                            // model: it returns a cover URL only when every selected
                            // row is the same album, and "" for a cross-album
                            // selection. When NOTHING is selected in the active tab
                            // (a fresh tab, a dropped m3u, nothing clicked yet), the
                            // pane falls back to the PLAYING track's cover instead of
                            // clearing ("prefer selection, fall back to now
                            // playing"). A non-empty selection always wins: clicking
                            // a row shows that row's album, cross-album selections
                            // still clear the pane (deliberately no fallback there:
                            // the user pointed at a mixed set, and the playing cover
                            // would misattribute it), and returning to a tab with a
                            // live selection shows that selection again.
                            readonly property string artSource: {
                                // Touch _selectionRevision so a selection/tab change
                                // re-runs this, and dataRevision so an in-place row
                                // reload (a re-read cover) does too. A binding only
                                // re-evaluates on the properties it actually reads,
                                // and the calls below read neither; the version tag
                                // folded into the returned URL (mtime+size) then makes
                                // the URL differ only when the file truly changed.
                                var selRev = albumArtFrame._selectionRevision
                                // The playing-state reads are HOISTED above the
                                // selection branch on purpose. QML captures a
                                // binding's dependencies from what the LAST
                                // evaluation actually read; if an evaluation
                                // returns from the selection branch without
                                // touching these, the binding holds no playback
                                // dependencies and a later track advance dirties
                                // nothing (the fallback then shows the previous
                                // album until an unrelated selection/tab event).
                                // Reading them unconditionally keeps playingChanged
                                // and the playing model's dataRevision in the
                                // dependency set on every evaluation.
                                var pm = AudioController.playingModel
                                var prow = AudioController.playingRow
                                var prev = pm ? pm.dataRevision : 0
                                var m = PlaylistTabs.activeModel
                                var v = PlaylistTabs.activeView
                                var sel = PlaylistTabs.activeSelection
                                if (m && v && sel) {
                                    var rev = m.dataRevision
                                    // The selection is bound to the filter
                                    // proxy (activeView), which maps its rows
                                    // to the source before resolving; the
                                    // dataRevision read above keeps the source
                                    // model's refreshes in the dependency set.
                                    var fromSelection = v.albumArtSourceForSelection(sel)
                                    if (fromSelection.length > 0)
                                        return fromSelection
                                    // "" is ambiguous: nothing selected, or a
                                    // selection that resolves to no cover (mixed
                                    // albums / missing file). Only the truly-empty
                                    // case falls through to now-playing; the C++
                                    // emptiness rule is mirrored here (no ranges AND
                                    // no current index).
                                    if (sel.hasSelection || sel.currentIndex.valid)
                                        return ""
                                }
                                // Now-playing fallback. playingModel/playingRow both
                                // notify on playingChanged, so track advance, stop
                                // (row -1), and cursor detach all re-run this; the
                                // playing model's dataRevision covers a re-read
                                // cover of the playing track itself.
                                if (!pm || prow < 0)
                                    return ""
                                return pm.albumArtSourceForRow(prow)
                            }

                            // Album art for the current selection. A single image bound
                            // straight to the resolved source: an empty source (nothing
                            // selected, or a missing/unreadable file) clears to the
                            // placeholder, and a real cover fades in once it has decoded.
                            // No manual double-buffering; correctness over a
                            // cover-to-cover crossfade (switching covers briefly shows
                            // the placeholder).
                            Image {
                                id: artImage
                                anchors.fill: parent
                                asynchronous: true
                                // No pixmap cache: the previous cover is released
                                // the moment the next one is bound, so the frame
                                // holds exactly one decoded cover. Navigating back
                                // (A -> B -> A) re-decodes A, which is a few ms on
                                // the image-reader thread at the 1024 bound below
                                // and never blocks the UI (asynchronous above).
                                // Caching was measured to cost ~60 MB more over ten
                                // albums than it saved in decode time; the ?v= tag
                                // (mtime+size) in the URL, which existed to make a
                                // cached entry invalidate on a changed file, still
                                // serves the reloader's re-read path.
                                cache: false
                                fillMode: Image.PreserveAspectFit
                                // The decode bound, passed to AlbumArtProvider as
                                // its requestedSize (which decodes straight to it,
                                // aspect preserved, never upscaling). Without it the
                                // provider decodes covers at full pixel dimensions:
                                // a 3000x3000 embedded JPEG is 36 MB of ARGB32,
                                // retained per album browsed (measured: +110 MB
                                // over ten albums). 1024 covers this frame at a
                                // 2x device pixel ratio with margin, for at most
                                // 4 MB a cover, and it is a FIXED bucket rather
                                // than a width/height-derived one on purpose: a
                                // splitter drag resizes the frame continuously, and
                                // a size-bound sourceSize would re-request (and
                                // re-decode) the cover on every pixel of it.
                                sourceSize: Qt.size(1024, 1024)
                                // Don't gate opacity on Ready: QQuickImage keeps
                                // painting the previous cover while the next loads,
                                // so fading out on Loading is what flashed the
                                // placeholder. Stay opaque whenever we want a cover
                                // (non-empty source, not errored); the old cover
                                // holds until the new one swaps in, and we fade to
                                // the placeholder only for an empty source or error.
                                opacity: (albumArtFrame.artSource !== ""
                                    && status !== Image.Error) ? 1 : 0
                                source: albumArtFrame.artSource

                                // Round the cover's corners to match the 4 px
                                // frame. The parent Rectangle's `radius` only
                                // rounds its OWN fill (which is why the empty
                                // placeholder reads rounded), and `clip: true`
                                // clips children to the bounding BOX, not the
                                // rounded outline, so a square cover painted
                                // edge to edge buries the corners. Render the
                                // cover to its own layer and mask it with a
                                // rounded-rect alpha source (artMask below), so
                                // the painted pixels themselves are rounded.
                                // Opacity still composites correctly: the fade
                                // applies to the masked layer as a whole.
                                layer.enabled: true
                                layer.effect: MultiEffect {
                                    maskEnabled: true
                                    maskSource: artMask
                                }

                                Behavior on opacity {
                                    NumberAnimation { duration: 160; easing.type: Easing.InOutQuad }
                                }
                            }

                            // Alpha mask for the cover: a rounded rectangle at
                            // the frame's own radius. Kept off-screen (visible:
                            // false) so it never paints into the scene, while
                            // layer.enabled keeps its texture live for the
                            // MultiEffect above to sample. antialiasing softens
                            // the 4 px corner; if it ever reads too hard or too
                            // soft on device, nudge the MultiEffect's
                            // maskSpreadAtMin rather than this rectangle.
                            Item {
                                id: artMask
                                anchors.fill: artImage
                                visible: false
                                layer.enabled: true

                                Rectangle {
                                    anchors.fill: parent
                                    radius: albumArtFrame.radius
                                    antialiasing: true
                                }
                            }

                            Label {
                                anchors.centerIn: parent
                                color: Theme.textDisabled
                                font.family: Theme.uiFont
                                text: "Album Art"
                                // Placeholder only when there's truly no cover to
                                // show: nothing selected / missing file (empty
                                // source) or a failed load. Not during a normal
                                // cover-to-cover load, so it never overlays the old
                                // cover still on screen.
                                visible: albumArtFrame.artSource === ""
                                         || artImage.status === Image.Error
                            }
                        }
                    }

                    MetadataView {
                        id: metadataView
                        implicitWidth: 450
                        model: MetadataPaneModel
                        SplitView.minimumHeight: 450 / 2
                    }
                }

                Item {
                    id: playlistPane
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: applicationWindow.width / 2

                    ColumnLayout {
                        id: playlistLayout
                        anchors.fill: parent
                        spacing: 0

                        PlaylistTabBar {
                            id: playlistTabBar
                            Layout.fillWidth: true
                            tabs: PlaylistTabs
                            view: playlistView
                            log: logStore
                        }

                        PlaylistView {
                            id: playlistView
                            Layout.fillHeight: true
                            Layout.fillWidth: true
                            // The filter proxy, not the model: the view and
                            // its selection live in proxy rows (see the
                            // view's `model` note).
                            model: PlaylistTabs.activeView
                            selectionModel: PlaylistTabs.activeSelection
                            reloader: PlaylistTabs.activeReloader
                            tabs: PlaylistTabs
                            log: logStore
                        }

                        // Status / log line, under the playlist pane (8 px gap).
                        // Shows live activity while busy, else the latest logged
                        // event. The `>` prompt toggles the expanded console.
                        StatusLogBar {
                            id: statusLogBar
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            level: applicationWindow._statusLevel
                            text: applicationWindow._statusText
                            consoleOpen: logConsole.open
                            onConsoleRequested: logConsole.open = !logConsole.open
                        }
                    }

                    // Expanded log console, overlaid above the status line over the
                    // bottom of the track list (non-modal, no reflow). Its bottom
                    // sits the status bar's height plus an 8 px gap above the pane
                    // bottom, so it pops up from just above the prompt.
                    LogConsole {
                        id: logConsole
                        anchors.left: playlistLayout.left
                        anchors.right: playlistLayout.right
                        anchors.bottom: playlistLayout.bottom
                        anchors.bottomMargin: statusLogBar.height + 8
                        logText: logStore.logText
                    }
                }
            }

            // Play bar
            PlayerBar {
                id: playerBar
                Layout.fillWidth: true
                Layout.leftMargin: 44
                Layout.rightMargin: 12
            }
        }
    }

    // Resize edges for the frameless platforms. The window is frameless
    // everywhere but macOS, and frameless means the compositor supplies no
    // resize edges, so without these the window would be pinned to its
    // startup size. On macOS the window is titled and AppKit resizes it
    // natively, so the grips stay out of the way there (a second set would
    // fight the native edges). Off while maximized/fullscreen: the window
    // manager owns the frame in those states and a stray edge drag would
    // only fight it. Declared last so the strips sit above the chrome; with
    // topInset 0 the top strip takes the outermost 6 px of the title bar,
    // which is the normal frameless convention (the rest of the bar moves).
    WindowResizeGrips {
        target: applicationWindow
        enabled: Qt.platform.os !== "osx"
                 && applicationWindow.visibility === Window.Windowed
    }
}
