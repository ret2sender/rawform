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

// main.cpp
//
// Composition root for rawform.
//
// rawform is a cross-platform audio player in the spirit of foobar2000: a
// fast, keyboard-friendly playlist view backed by its own binary playlist
// format (.rwfpl), driven by the standalone rawform_audio engine (CoreAudio on
// macOS, PipeWire on Linux).
//
// This file owns no UI logic. It constructs the app-global C++ services, wires
// them together once, exposes them to QML, and loads the QML front end. The
// context properties, in registration order (the name QML binds is on the
// left):
//
//   - playlistTabs     : PlaylistTabs, one PlaylistModel + selection +
//                        MetadataReloader per tab, the active-tab pointers,
//                        the scanner seam, the live-playlist autosave/restore
//   - customColumns    : CustomColumnRegistry, shared user-defined %pattern%
//                        columns
//   - metadataModel    : MetadataModel, the dock pane, following the active tab
//   - trackScanner     : TrackScanner, off-thread file/tag ingestion
//   - playlistStore    : PlaylistStore, Save / Save As + the column preset
//   - audioController  : AudioController, the engine wrapper and the cursor
//   - spectrumProvider : SpectrumProvider, the spectrum viewer
//   - windowGeometry   : WindowGeometryStore, window.yaml
//   - settingsStore    : SettingsStore, settings.yaml
//
// plus the "rawformart" image provider (AlbumArtProvider, engine-owned). The
// types QML instantiates itself (the Properties window's models, editors, and
// scan controller; FileRenamer, RenamePreviewer, RenamePatternStore; AppInfo,
// Clipboard, WindowFocus) register through QML_ELEMENT and need nothing here.
//
// The one piece of behavior here is the seam that re-aggregates the metadata
// pane whenever the active tab's selection changes or the active tab switches.

#include <QGuiApplication>

#include <ctime>  // tzset

// glibc-only (not ISO C, not POSIX): mallopt, for the allocator tuning below.
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <QDebug>
#include <QIcon>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QVariantMap>

#include "media/AlbumArtProvider.h"
#include "columns/ColumnSchema.h"
#include "columns/CustomColumnRegistry.h"
#include "metadata/MetadataModel.h"
#include "metadata/MetadataReloader.h"
#include "playlist/PlaylistModel.h"
#include "playlist/PlaylistStore.h"
#include "playlist/PlaylistTabs.h"
#include "media/TrackData.h"
#include "media/TrackScanner.h"
#include "playback/AudioController.h"
#include "playback/SpectrumProvider.h"
#include "settings/SettingsStore.h"
#include "utils/MacOSStyling.h"
#include "window/WindowGeometryStore.h"

int main(int argc, char* argv[]) {

    // Warm glibc's timezone cache while the process is still single-threaded
    // (QGuiApplication's construction already spawns the Wayland event
    // thread). Local-time conversions (QFileInfo timestamps, QDateTime) from
    // worker threads would otherwise race to initialize the cache; glibc
    // locks that internally, but initializing it here once removes the
    // concurrent free/strdup pair outright. See the tzset_internal note in
    // utils/SanitizerSuppressions.cpp.
    tzset();

#ifdef __GLIBC__
    // Pin glibc's mmap threshold at 1 MB, which also switches OFF its dynamic
    // adjustment (mallopt(3): setting M_MMAP_THRESHOLD disables the dynamic
    // behavior). Every allocation of 1 MB or more then comes from its own
    // mapping and is returned to the OS the moment it is freed, in every
    // thread. Left at the default, the threshold climbs toward 32 MB as large
    // chunks are freed, after which image-sized transients (a cover decoding
    // on the QML image-reader thread, a picture block TagLib reads on a pool
    // worker) are carved from that thread's heap and stay resident after the
    // free: RSS then grows with every album browsed even though nothing is
    // live (measured: ~3 to 12 MB per album, plateauing around +240 MB after
    // fifty). A 1 MB floor keeps the heap for the small-object traffic the
    // model and the QML engine generate, and only the buffers that would
    // otherwise pin whole arenas take the mmap round trip. Must run while the
    // process is still single-threaded, like tzset above: the setting is
    // global, and the reader and pool threads inherit it. macOS's allocator
    // has no equivalent residue and no mallopt.
    mallopt(M_MMAP_THRESHOLD, 1024 * 1024);
#endif

    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle("Basic");

    // Conventional app identity (the window class). The config paths are
    // resolved explicitly through userConfigDir() and do not depend on it;
    // QStandardPaths and QSettings are unused.
    QCoreApplication::setApplicationName(QStringLiteral("rawform"));

    // Wayland app_id. Qt derives the toplevel's app_id from the desktop file
    // name; without this it falls back to the binary name. Pinned to the same
    // identifier as MACOSX_BUNDLE_GUI_IDENTIFIER, which the Flatpak uses for
    // its .desktop file and exported hicolor icons, so the app_id is the same
    // in dev and installed and never changes underneath saved KDE window rules
    // or taskbar pins. In dev (no installed .desktop file) compositors simply
    // fail the desktop-entry lookup and fall back to the window-icon pixels
    // supplied below, so this costs nothing.
    QGuiApplication::setDesktopFileName(QStringLiteral("com.rawform.app"));

    // -----------------------------------------------------------------------
    // Window icon (Linux/Windows only).
    // -----------------------------------------------------------------------
    // Theme-name-first, with the bundled scalable SVG as fallback. The split
    // exists because of how icons travel on Wayland (xdg-toplevel-icon, Qt
    // 6.8+): a pixel-buffer icon is rendered by Qt into the discrete sizes
    // the compositor advertises and then SCALED by the compositor for any
    // surface that draws larger (KWin's large-icon Alt-Tab switcher), which
    // reads as jagged edges. An icon carrying a theme NAME is forwarded as
    // that name instead, and the compositor rasterizes it from the icon
    // theme at the final draw size, the same path installed apps use.
    //
    // So: once tools/install_linux_icons.sh (or the Flatpak) has placed
    // com.rawform.app in hicolor, fromTheme resolves, the name is what
    // reaches the compositor, and every surface is crisp. On a machine
    // without the install (and on Windows) the lookup fails and the qrc SVG
    // fallback keeps the buffer path: correct in the taskbar, upscaled in
    // large Alt-Tab renders; that is the accepted uninstalled-dev gap.
    //
    // The fallback path carries the full QTP0001 module prefix: RESOURCES
    // entries of the qml module land under :/qt/qml/<URI-as-path>/, the same
    // scheme as the engine.load URL below, NOT under a bare :/icons/. The
    // qsvgicon plugin it needs is deployment-guaranteed by the Qt6::Svg link
    // in CMakeLists.txt.
    //
    // Excluded on macOS on purpose: there setWindowIcon overrides the Dock
    // icon at runtime, and the bundle's rawform.icns (declared in
    // CMakeLists.txt) must stay authoritative.
    #if !defined(Q_OS_MAC)
    QGuiApplication::setWindowIcon(QIcon::fromTheme(
        QStringLiteral("com.rawform.app"),
        QIcon(QStringLiteral(
            ":/qt/qml/com/rawform/app/Sources/icons/app/rawform.svg"))));
    #endif

    // -----------------------------------------------------------------------
    // App-global services shared across every tab.
    // -----------------------------------------------------------------------

    // User-defined %pattern% columns. One shared, read-only instance: every
    // tab's model renders/titles/aligns against this registry and reacts to its
    // edits. Loaded before any tab's model binds so a restored "custom:<id>"
    // arrangement resolves on first layout.
    rawform::CustomColumnRegistry customColumns;
    customColumns.load();

    rawform::MetadataModel metadataModel;

    // Off-thread file ingestion. Model-free; PlaylistTabs owns the seam that
    // decides which tab a scan's results land in (correct across tab switches).
    rawform::TrackScanner trackScanner;

    // -----------------------------------------------------------------------
    // The playlist session. Owns one PlaylistModel + QItemSelectionModel +
    // MetadataReloader per open tab, exposes the active-tab pointers QML binds
    // to, absorbs the scanner seam, and runs the live-playlist autosave/restore.
    // -----------------------------------------------------------------------
    rawform::PlaylistTabs tabs;
    tabs.setSchemaPrototype(rawform::ColumnSchema::load());
    tabs.setCustomColumns(&customColumns);
    tabs.setScanner(&trackScanner);

    // -----------------------------------------------------------------------
    // Save/preset store. Not bound to a fixed model: QML binds its `model` to the
    // active tab (playlistTabs.activeModel), so "Save playlist..." targets the
    // visible playlist. Opening a .rwfpl makes a new tab via PlaylistTabs, so the
    // store handles just Save + the .rwftp column preset.
    // -----------------------------------------------------------------------
    rawform::PlaylistStore playlistStore(nullptr);

    // New (empty) tabs adopt the user's saved column preset rather than bare
    // schema defaults, so a chosen column set/order/widths applies to fresh
    // playlists; opened/restored tabs use their own saved layout. Read the
    // preset ONCE and hand it to the session.
    {
        const QVariantMap preset = playlistStore.loadColumnPreset();
        const QStringList ids = preset.value(QStringLiteral("fieldIds")).toStringList();
        if (!ids.isEmpty())
            tabs.setDefaultLayout(ids, preset.value(QStringLiteral("widths")).toList());
    }

    // -----------------------------------------------------------------------
    // Metadata pane follows the ACTIVE tab. PlaylistTabs surfaces both a
    // selection change AND a tab switch as activeSelectionChanged, so this one
    // connection covers both: re-aggregate from the active model's selected rows
    // (or clear when there is no active tab / nothing selected).
    // -----------------------------------------------------------------------
    auto pushSelectionToMetadata = [&tabs, &metadataModel]() {
        rawform::PlaylistModel* model = tabs.activeModel();
        QItemSelectionModel* selection = tabs.activeSelection();
        if (!model || !selection) {
            metadataModel.setSelection(nullptr, {});
            return;
        }
        const QModelIndexList rows = selection->selectedRows(0);
        QList<int> rowNumbers;
        rowNumbers.reserve(rows.size());
        for (const QModelIndex& idx : rows)
            rowNumbers.push_back(idx.row());
        metadataModel.setSelection(model, rowNumbers);
    };

    QObject::connect(&tabs, &rawform::PlaylistTabs::activeSelectionChanged,
                     &metadataModel,
                     [pushSelectionToMetadata]() { pushSelectionToMetadata(); });

    // Seed the session: re-open the previous live tabs, or one empty tab. Done
    // AFTER the metadata wiring so the initial active tab pushes its (empty)
    // selection, and BEFORE the engine loads so QML binds to a live active tab.
    tabs.restoreSession();

    // -----------------------------------------------------------------------
    // Save outcome logging (the store reports save success/failure).
    // -----------------------------------------------------------------------
    QObject::connect(&playlistStore, &rawform::PlaylistStore::saved,
                     &playlistStore, [](bool ok, const QString& message) {
                         if (ok)
                             qInfo("rawform: %s", qUtf8Printable(message));
                         else
                             qWarning("rawform: playlist save failed: %s",
                                      qUtf8Printable(message));
                     });

    // -----------------------------------------------------------------------
    // Audio playback. Wraps the standalone, zero-Qt rawform_audio Engine in
    // a QObject: signals from the engine's Listener (marshaled to this thread),
    // Q_INVOKABLE transport, and Q_PROPERTY state/position/duration/now-playing
    // for QML to bind. It owns the cursor that makes playback follow a playlist
    // automatically; the persistent playlist itself stays in PlaylistModel. The
    // tabs pointer is the fallback source when play() is hit from Stopped with
    // nothing ever played (start the active tab).
    // -----------------------------------------------------------------------
    rawform::AudioController audioController;
    audioController.setPlaylistTabs(&tabs);

    // -----------------------------------------------------------------------
    // Spectrum viewer. A focused QObject that drives the PlayerBar's spectrum: a
    // ~60 Hz timer drains the engine's lock-free output tap (through the controller),
    // runs the zero-Qt SpectrumAnalyzer, applies attack/decay smoothing, and
    // publishes the bar values QML draws. It also owns the one spectrum preference,
    // the analyzer source (Settings > Spectrum view), persisted in spectrum.yaml and
    // pushed to the engine tap. Constructed AFTER the controller (which it
    // references) and before the QML engine, so it outlives the QML that binds to it
    // and is destroyed before the controller it points at.
    rawform::SpectrumProvider spectrumProvider(&audioController);

    // Window geometry persistence (window.yaml). Constructed AFTER the
    // QGuiApplication (its off-screen guard queries QGuiApplication::screens())
    // and registered before engine.load below, so its startup properties are
    // already loaded and valid when MainWindow.qml binds width/height/x/y.
    rawform::WindowGeometryStore windowGeometry;

    // General application settings (settings.yaml). Loads on construction, so
    // the tagging preferences are valid before any QML binds or any write path
    // snapshots them. Constructed here, with the other config-backed services,
    // and reachable from QML-instantiated C++ (MetadataEditor) through
    // SettingsStore::instance(); it must therefore outlive the QML engine,
    // which stack order below guarantees.
    rawform::SettingsStore settingsStore;

    // -----------------------------------------------------------------------
    // QML engine
    // -----------------------------------------------------------------------
    QQmlApplicationEngine engine;

    // Album art on demand, keyed by file path: image://rawformart/<path>.
    // The engine takes ownership of the provider.
    engine.addImageProvider(QStringLiteral("rawformart"), new rawform::AlbumArtProvider);

    // -----------------------------------------------------------------------
    // QML warning gate: silence expected album-art misses, pass everything
    // else through verbatim.
    // -----------------------------------------------------------------------
    // Selecting a track with NO cover art (no sidecar, no embedded picture)
    // makes AlbumArtProvider return a null QImage; that null is the provider's
    // documented "no art" answer, and Image.Error on the QML side is what
    // drives the placeholder fade. Qt, however, narrates every such miss as a
    // warning ("Failed to get image from provider: image://rawformart/..."),
    // one line per art-less selection, which drowns the console in a normal
    // testing session.
    //
    // There is no narrower hook: the message carries no logging category (so
    // QT_LOGGING_RULES cannot isolate it) and QQuickImage offers no
    // per-element opt-out, so the engine's warnings() signal is the only
    // interception point. setOutputWarningsToStandardError(false) discards
    // nothing; it reroutes EVERY QML warning to the handler below, which
    // drops exactly the rawformart art-miss message and re-emits everything
    // else via QQmlError::toString(), reproducing Qt's own formatting
    // (file:line:column prefix included). That re-emit path is load-bearing:
    // mis-filtering here would silently eat real QML warnings, which is why
    // the match is anchored on the full message text PLUS the scheme and
    // provider id, nothing broader.
    //
    // Escape hatch: RAWFORM_LOG_ART_MISSES=1 in the environment (settable per
    // CLion run configuration) lets the art misses through unchanged for
    // diagnostic sessions. Documented in README, Build notes.
    engine.setOutputWarningsToStandardError(false);
    const bool logArtMisses =
        qEnvironmentVariableIntValue("RAWFORM_LOG_ART_MISSES") != 0;
    QObject::connect(
        &engine, &QQmlEngine::warnings, &engine,
        [logArtMisses](const QList<QQmlError>& warnings) {
            static const QString kArtMiss = QStringLiteral(
                "Failed to get image from provider: image://rawformart/");
            for (const QQmlError& e : warnings) {
                if (!logArtMisses && e.description().contains(kArtMiss))
                    continue;
                qWarning().noquote() << e.toString();
            }
        });

    QQmlContext* ctx = engine.rootContext();
    ctx->setContextProperty("playlistTabs", &tabs);
    ctx->setContextProperty("customColumns", &customColumns);
    ctx->setContextProperty("metadataModel", &metadataModel);
    ctx->setContextProperty("trackScanner", &trackScanner);
    ctx->setContextProperty("playlistStore", &playlistStore);
    ctx->setContextProperty("audioController", &audioController);
    ctx->setContextProperty("spectrumProvider", &spectrumProvider);
    ctx->setContextProperty("windowGeometry", &windowGeometry);
    ctx->setContextProperty("settingsStore", &settingsStore);

    // -----------------------------------------------------------------------
    // Load QML
    // -----------------------------------------------------------------------
    engine.load(QUrl(QStringLiteral(
        "qrc:/qt/qml/com/rawform/app/Sources/qml/views/MainWindow.qml")));

    if (engine.rootObjects().isEmpty())
        return -1;

    // -----------------------------------------------------------------------
    // Native macOS window styling. rawform draws its own QML title bar; on macOS
    // we additionally reach into AppKit to surface the genuine traffic-light
    // controls and hide the native title text, so the QML content sits behind a
    // transparent native title bar. This must run AFTER the QML has loaded (the
    // root QQuickWindow has to exist to be styled) and only on macOS. On
    // Linux/Windows the window stays frameless with internal, themed QML
    // TitleBarControls, so there is nothing to do here.
    // -----------------------------------------------------------------------
    #if defined(Q_OS_MAC)
    applyMacOSStyling(&engine);
    #endif

    return QGuiApplication::exec();
}
