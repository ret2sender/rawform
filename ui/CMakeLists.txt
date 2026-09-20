# This file is part of rawform.
# Copyright (C) 2026 Etienne Fleurant
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 4.2)

# ---------------------------------------------------------------------------
# Project version
# ---------------------------------------------------------------------------
# ui/VERSION.txt is the single source of truth for the application version.
# project() consumes it here, and scripts/macdeploy.sh reads the same file to
# name the .dmg, so a release is identifiable in Finder, in the bundle's
# Info.plist (see the if(APPLE) block after qt_add_executable), and in the
# disk image name without ever repeating the number.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" RAWFORM_VERSION)
string(STRIP "${RAWFORM_VERSION}" RAWFORM_VERSION)
if(NOT RAWFORM_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "ui/VERSION.txt must contain a semantic version (got "
        "'${RAWFORM_VERSION}').")
endif()

# Re-run the configure step when ui/VERSION.txt changes; file(READ) records
# no dependency by itself, so without this a version bump would keep the old
# RAWFORM_VERSION_STR baked into the binary until some unrelated CMake edit
# happened to force a reconfigure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt"
)

project(rawform VERSION ${RAWFORM_VERSION} LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global C++ standard
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Sanitizer profiles (mirrors audio/CMakeLists.txt; see its profile block).
# ---------------------------------------------------------------------------
# POSITION IS LOAD-BEARING, in the SCOPE dimension this time: add_compile_
# options/add_link_options are DIRECTORY-scoped and propagate DOWN into
# add_subdirectory, never up. The engine's own sanitize block therefore covers
# only the engine's targets: with -DRAWFORM_SANITIZE=thread and no block here,
# librawform_audio.a came back instrumented while the rawform target got
# neither the compile flags nor the link flag, and the link died on undefined
# ___tsan_* (no runtime). Observed on macOS/AppleClang under
# -DRAWFORM_SANITIZE=thread. And link failure is the GOOD outcome of that
# mistake: a partially instrumented build that links fabricates deterministic
# races, and
# TagWriteLock is header-only code compiled into THESE TUs, so the app side is
# exactly the half that must be instrumented for the rename/tag-write drills
# to mean anything. This block sits under project() and BEFORE the engine
# add_subdirectory: it covers every app TU and flows down into the engine,
# whose identical directory-local flags are harmless duplication.
set(RAWFORM_SANITIZE "off" CACHE STRING
    "Sanitizer profile: off, thread, or address (ASan+UBSan)")
if(RAWFORM_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
    message(STATUS "rawform: ThreadSanitizer profile")
elseif(RAWFORM_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "rawform: AddressSanitizer+UBSan profile")
elseif(NOT RAWFORM_SANITIZE STREQUAL "off")
    message(WARNING "rawform: unknown RAWFORM_SANITIZE '${RAWFORM_SANITIZE}' (off, thread, address)")
endif()

# ---------------------------------------------------------------------------
# rawform_audio engine (the standalone, zero-Qt static library).
# ---------------------------------------------------------------------------
# The engine is a sibling CMake project at ../audio. Pull it in as a subdirectory
# so the app builds against the live source rather than a prebuilt artifact.
#   - EXCLUDE_FROM_ALL: only the rawform_audio static lib is built (it is pulled
#     in by the link below); the engine's own CLI and test targets are configured
#     but not built as part of the app.
#   - The lib's PUBLIC include (../audio/include) and its RAWFORM_HAVE_COREAUDIO /
#     RAWFORM_HAVE_FFMPEG capability defines arrive transitively through the link.
#     For a STATIC lib, CMake also threads the codec/framework link deps onto our
#     link line, so we name none of them here.
# Added BEFORE CMAKE_AUTOMOC is switched on below, so the engine's (Qt-free)
# targets are created with AUTOMOC off and never get a needless moc pass.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../audio
                 ${CMAKE_BINARY_DIR}/rawform_audio EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Qt 6
# ---------------------------------------------------------------------------
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Concurrent Core Gui Quick QuickControls2 Svg Widgets)

# ---------------------------------------------------------------------------
# yaml-cpp: the format of every per-user store under userConfigDir()
# (settings.yaml, window.yaml, playback.yaml, rate_ledger.yaml, spectrum.yaml,
# rename_patterns.yaml, playlist_custom_columns.yaml), read and written through
# the shared utils/YamlFile.h pair.
# ---------------------------------------------------------------------------
# Provided by Homebrew (`brew install yaml-cpp`) and most Linux distros
# (`apt install libyaml-cpp-dev`, `dnf install yaml-cpp-devel`). The CONFIG
# package exports the namespaced target yaml-cpp::yaml-cpp.
find_package(yaml-cpp CONFIG REQUIRED)
# If you would rather vendor it (reproducible, no system install), comment the
# line above and use FetchContent instead:
#   include(FetchContent)
#   FetchContent_Declare(yaml-cpp
#       GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
#       GIT_TAG 0.8.0)
#   set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
#   FetchContent_MakeAvailable(yaml-cpp)
# Very old yaml-cpp (< 0.7) exports a bare `yaml-cpp` target rather than the
# namespaced one; swap the link name below if you hit that.

# ---------------------------------------------------------------------------
# TagLib 2.x: reads tags + audio properties + embedded cover art.
# ---------------------------------------------------------------------------
# macOS dev:  brew install taglib
# Linux:      apt install libtag1-dev   /    dnf install taglib-devel
# We require 2.x deliberately: it exports a proper CMake config target
# (TagLib::tag) and the complexProperties() API used for embedded-art
# extraction: the modern path, no FindTagLib/pkg-config glue.
find_package(TagLib 2.0 CONFIG REQUIRED)
# To vendor instead (reproducible, no system install), comment the line above
# and use FetchContent: mirrors the yaml-cpp option:
#   include(FetchContent)
#   FetchContent_Declare(taglib
#       GIT_REPOSITORY https://github.com/taglib/taglib.git
#       GIT_TAG v2.0.2)
#   set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
#   FetchContent_MakeAvailable(taglib)

# Opt into modern Qt 6.8+ QML resource and module policies.
# QTP0001: embed QML files under :/qt/qml/ instead of :/ to avoid resource collisions.
# QTP0004: require qmldir files for QML subdirectories, enforcing explicit module
# structure.
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

# ---------------------------------------------------------------------------
# App sources
# ---------------------------------------------------------------------------
#
# Each QML-registered type's .cpp/.h is listed as an adjacent pair (header
# included here, not in a separate variable) so AUTOMOC sees the pair and the
# generated rawform_qmltyperegistrations.cpp resolves the moc-recorded header.
# Plain headers (e.g. TrackData.h) carry no registered type but are listed for
# IDE indexing.
set(APP_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/main.cpp

        # utils/: platform glue. The macOS window styling implementation is
        # Objective-C++ and is added to the target separately, only on Apple
        # (see the if(APPLE) block after qt_add_executable). The header is plain
        # C++ and is listed here for IDE indexing.
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/MacOSStyling.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/Clipboard.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/AppInfo.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/AppInfo.cpp
        # Shared YAML file IO (atomic QSaveFile persist + tolerant read),
        # used by every YAML-backed store.
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/YamlFile.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/YamlFile.cpp
        # Shared display formatters (duration, file size) used by the playlist
        # columns and the metadata pane alike.
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/Formats.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/Formats.cpp
        # Baked-in sanitizer default suppressions (TSan races, LSan leaks);
        # empty TU in non-sanitizer builds.
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/SanitizerSuppressions.cpp

        # paths/: shared filesystem-location helpers (XDG config dir)
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/paths/Paths.cpp

        # window/: window geometry persistence (window.yaml)
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/window/WindowGeometryStore.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/window/WindowGeometryStore.cpp
        # window/: strict focus-window probe for the Escape shortcut gates;
        # header-only, listed so the QML type registrar scans it.
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/window/WindowFocus.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/paths/Paths.h

        # media/: file ingestion, tag reading, album art
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/TrackData.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/ReplayGainTags.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/TagWriteLock.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/ReplayGainEditor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/ReplayGainEditor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/MetadataEditor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/MetadataEditor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/ReplayGainScanController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/ReplayGainScanController.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/TrackReader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/TrackReader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/TrackScanner.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/TrackScanner.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/AlbumArtProvider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media/AlbumArtProvider.h

        # columns/: column schema, %pattern% evaluator, custom-column registry
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/ColumnSchema.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/ColumnSchema.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/PatternEvaluator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/PatternEvaluator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/CustomColumnRegistry.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/CustomColumnRegistry.h

        # rename/: File Operations > Rename To. The pure pattern-to-name
        # pipeline (per-value scene sanitizer + stem finalize + verdict), the
        # off-thread on-disk renamer (QML_ELEMENT; the dialog's write half),
        # the previewer, and the named-preset store (QML_ELEMENT;
        # rename_patterns.yaml).
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/RenameSanitizer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/RenameSanitizer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/FileRenamer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/FileRenamer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/RenamePatternStore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/RenamePatternStore.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/RenamePreviewer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/RenamePreviewer.h

        # metadata/: metadata-pane model + on-disk freshness pass
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata/MetadataModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata/MetadataModel.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata/PropertiesMetadataModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata/PropertiesMetadataModel.h
        # The C++ backing model for the ReplayGain Properties pane
        # (QML_ELEMENT).
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata/ReplayGainRowsModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata/ReplayGainRowsModel.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata/MetadataReloader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata/MetadataReloader.h

        # playlist/: playlist model, .rwfpl / .m3u file+store, tab session manager
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistModel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistModel.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistFile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistFile.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistFormat.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/M3uFile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/M3uFile.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistStore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistStore.h
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistTabs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistTabs.h

        # playback/: Qt AudioController wrapping the rawform_audio Engine
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playback/AudioController.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playback/AudioController.h

        # playback/: spectrum viewer provider (timer + FFT drive + smoothing)
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playback/SpectrumProvider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playback/SpectrumProvider.h

        # settings/: general application settings store (settings.yaml)
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/settings/SettingsStore.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/settings/SettingsStore.h
)

qt_add_executable(rawform
        WIN32
        ${APP_SOURCES}
)

# ---------------------------------------------------------------------------
# macOS bundle metadata
# ---------------------------------------------------------------------------
# qt_add_executable does NOT produce a .app bundle by itself; without the
# MACOSX_BUNDLE property the target is a bare Mach-O binary. The bundle is required
# by macdeployqt and for distribution, so switch it on here, and stamp the
# version so releases are distinguishable in Finder's Get Info and in
# Gatekeeper logs. SHORT_VERSION_STRING is the user-visible version;
# BUNDLE_VERSION is the build-number field, kept equal to it since there is
# no separate build counter. Both flow from ui/VERSION.txt via project().
# CLion keeps working unchanged: it resolves bundle targets to the inner
# executable (rawform.app/Contents/MacOS/rawform) when running and debugging.
if(APPLE)
    set_target_properties(rawform PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "rawform"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.rawform.app"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            # CFBundleIconFile. The value is the file NAME inside the bundle's
            # Resources directory, not a path; the file itself is packaged by
            # the target_sources + MACOSX_PACKAGE_LOCATION pair below. This is
            # the whole macOS icon story: Dock, Finder, Cmd-Tab, and Gatekeeper
            # dialogs all read the .icns from the bundle. main.cpp deliberately
            # does NOT call setWindowIcon on macOS, because at runtime that
            # would override the Dock icon with a PNG and the .icns must stay
            # authoritative (it carries the full 16 through 1024 ladder,
            # including the 2x representations).
            MACOSX_BUNDLE_ICON_FILE "rawform.icns")

    # Package the .icns into rawform.app/Contents/Resources/. The file is a
    # target source only so CMake copies it; it never enters the qrc (the qrc
    # icon set below is the Linux/Windows window-icon path). Apple-only on
    # purpose: on other platforms the file would be a meaningless source entry.
    target_sources(rawform PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/icons/app/rawform.icns)
    set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/icons/app/rawform.icns
            PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

# ---------------------------------------------------------------------------
# QML module
# ---------------------------------------------------------------------------
# Theme.qml and TitleBarStyle.qml are module singletons (one instance per
# engine, resolvable by type name from any document in the module, including
# other top-level Windows). The property below is the CMake half of the
# contract; each file's `pragma Singleton` is the QML half. Both halves are
# required: with the pragma alone the generated qmldir does not declare the
# type as a singleton, and every `TitleBarStyle.foo` access silently yields
# undefined. Must be set BEFORE qt_add_qml_module.
set_source_files_properties(
        Sources/qml/theme/Theme.qml
        Sources/qml/components/TitleBarStyle.qml
        Sources/qml/components/Format.qml
        PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

# ---------------------------------------------------------------------------
# QML sources
# ---------------------------------------------------------------------------
# Listed once, here, because two things consume the list: the module
# declaration below and the qmlcachegen-output rule after the warnings block,
# which derives the generated file names from these paths.
set(RAWFORM_QML_FILES
        # theme/: the Theme singleton (fonts and palette)
        Sources/qml/theme/Theme.qml
        # views/: top-level app regions bound to models
        Sources/qml/views/AboutWindow.qml
        Sources/qml/views/CustomColumnsWindow.qml
        Sources/qml/views/MainWindow.qml
        Sources/qml/views/MetadataView.qml
        Sources/qml/views/PlayerBar.qml
        Sources/qml/views/PlaylistView.qml
        Sources/qml/views/PropertiesWindow.qml
        Sources/qml/views/RenameFilesDialog.qml
        Sources/qml/views/SettingsWindow.qml
        # components/: reusable themed widgets
        Sources/qml/components/AppIcon.qml
        Sources/qml/components/HeaderCell.qml
        Sources/qml/components/EditValueDialog.qml
        Sources/qml/components/LogConsole.qml
        Sources/qml/components/LogStore.qml
        Sources/qml/components/MetadataPropertiesPane.qml
        Sources/qml/components/Mp3TaggingSettingsPane.qml
        Sources/qml/components/PlaybackSettingsPane.qml
        Sources/qml/components/PlaylistDialogs.qml
        Sources/qml/components/PlaylistTabBar.qml
        Sources/qml/components/PropertiesLogBar.qml
        Sources/qml/components/ReplayGainPropertiesPane.qml
        Sources/qml/components/ReplayGainScanProgress.qml
        Sources/qml/components/ReplayGainSettingsPane.qml
        Sources/qml/components/Format.qml
        Sources/qml/components/SeekBar.qml
        Sources/qml/components/SpectrumSettingsPane.qml
        Sources/qml/components/SpectrumView.qml
        Sources/qml/components/StagedSettings.qml
        Sources/qml/components/StatusLogBar.qml
        Sources/qml/components/TaggingSettingsPane.qml
        Sources/qml/components/ThemedMenu.qml
        Sources/qml/components/ThemedMenuBar.qml
        Sources/qml/components/ThemedMenuItem.qml
        Sources/qml/components/ThemedSplitView.qml
        Sources/qml/components/TitleBar.qml
        Sources/qml/components/ToolDialogCloseButton.qml
        Sources/qml/components/VolumeSlider.qml
        Sources/qml/components/WindowResizeGrips.qml
        Sources/qml/components/TitleBarControls.qml
        Sources/qml/components/TitleBarStyle.qml
)

# ---------------------------------------------------------------------------
# QML lint (encapsulation hygiene)
# ---------------------------------------------------------------------------
# qt_add_qml_module below auto-generates lint targets; no custom target is
# needed (adding one would just duplicate Qt's). Run either of:
#   cmake --build <dir> --target rawform_qmllint    (this module only)
#   cmake --build <dir> --target all_qmllint        (every module in the tree)
# The warning class that motivates this is "unqualified access": a QML file
# reaching an id or context property it does not declare, i.e. the
# non-self-contained pattern. Every QML file carries
# `pragma ComponentBehavior: Bound`, so delegates and other nested components
# resolve outer document ids statically (checked by qmllint) and declare what
# they consume as `required property`. Every view that reaches a C++ context
# property carries a file-wide `qmllint disable unqualified` for exactly that
# reach; the components declare what they consume (their few directives cover
# other categories, each at its site), so any warning is a regression.
qt_add_qml_module(rawform
        URI com.rawform.app
        VERSION 1.0
        QML_FILES ${RAWFORM_QML_FILES}
        RESOURCES
        Sources/fonts/Inter-VariableFont_opsz,wght.ttf
        Sources/fonts/Inter-Italic-VariableFont_opsz,wght.ttf
        Sources/fonts/JetBrainsMono-VariableFont_wght.ttf
        Sources/fonts/JetBrainsMono-Italic-VariableFont_wght.ttf
        Sources/icons/app/dialogs/menu_arrow.svg
        Sources/icons/app/dialogs/tool_dialog_close_x.svg
        Sources/icons/app/track_controls/stop.svg
        Sources/icons/app/track_controls/previous.svg
        Sources/icons/app/track_controls/play.svg
        Sources/icons/app/track_controls/pause.svg
        Sources/icons/app/track_controls/next.svg
        Sources/icons/default/control_close.svg
        Sources/icons/default/control_maximize.svg
        Sources/icons/default/control_minimize.svg
        Sources/icons/default/control_restore.svg
        Sources/icons/breeze/control_close.svg
        Sources/icons/breeze/control_maximize.svg
        Sources/icons/breeze/control_minimize.svg
        Sources/icons/breeze/control_restore.svg
        Sources/icons/breeze/control_close_hover.svg
        Sources/icons/breeze/control_maximize_hover.svg
        Sources/icons/breeze/control_minimize_hover.svg
        Sources/icons/breeze/control_restore_hover.svg
        Sources/icons/windows/control_close.svg
        Sources/icons/windows/control_maximize.svg
        Sources/icons/windows/control_minimize.svg
        Sources/icons/windows/control_restore.svg
        # icons/app/: the application icon for the Linux/Windows window icon.
        # A single scalable SVG rather than a 16..256 PNG ladder:
        # main.cpp hands it to QIcon and Qt's svg icon engine rasterizes at
        # whatever size any surface requests (taskbar, Alt-Tab large-icon
        # switcher, pager), so no size is ever an upscale of a smaller bitmap.
        # This requires the qsvgicon imageformat plugin at runtime; linking
        # Qt6::Svg below is what guarantees windeployqt/macdeployqt ship it.
        # macOS does not read this file; it uses the bundled .icns (APPLE
        # block above). QTP0001 places this at
        # :/qt/qml/com/rawform/app/Sources/icons/app/rawform.svg and
        # main.cpp's load path must match that prefix exactly.
        Sources/icons/app/rawform.svg
        Sources/images/rawform_logo.png
        Sources/images/rawform_logo_simple.png
)

# ---------------------------------------------------------------------------
# Native macOS window styling (Objective-C++)
# ---------------------------------------------------------------------------
# MacOSStyling.mm reaches into AppKit to surface the native traffic-light
# controls and a transparent, full-size-content title bar. It is Objective-C++,
# and must only ever reach the Apple toolchain: MSVC cannot compile .mm at all,
# and there is no AppKit off macOS. We therefore add the implementation to the
# target only on APPLE and link the AppKit/Foundation frameworks there. main.cpp
# guards the single call site with Q_OS_MAC, so a non-Apple build never
# references the symbol and links fine without this translation unit.
if(APPLE)
    target_sources(rawform PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/MacOSStyling.mm)
    target_link_libraries(rawform PRIVATE
            "-framework AppKit"
            "-framework Foundation")
endif()

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------
target_link_libraries(rawform PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Concurrent
        # Qt6::Svg is not called by any translation unit; it is linked so the
        # deployment tools (windeployqt, macdeployqt) treat the module as used
        # and ship the qsvgicon imageformat plugin, which the window-icon
        # QIcon in main.cpp needs at runtime to rasterize the app SVG. Without
        # this the dev build works (plugin found in the Qt install) and the
        # deployed build silently shows no window icon.
        Qt6::Svg
        Qt6::Widgets
        yaml-cpp::yaml-cpp
        TagLib::tag
        rawform_audio
)

# ---------------------------------------------------------------------------
# Version compile definition
# ---------------------------------------------------------------------------
# PROJECT_VERSION is populated from ui/VERSION.txt (see the top of this
# file). It reaches the application through this compile definition and is
# surfaced in QML by the AppInfo singleton; scripts/macdeploy.sh reads the
# same ui/VERSION.txt file to name the .dmg. Without this definition,
# AppInfo.h falls back to "0.0.0-unversioned", making the misconfiguration
# obvious wherever the version is displayed.
target_compile_definitions(rawform PRIVATE
        RAWFORM_VERSION_STR="${PROJECT_VERSION}"
)

if (MSVC)
    target_compile_options(rawform PRIVATE /utf-8)
endif()

target_include_directories(rawform PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources
        # The hand-written sources include headers by their qualified path
        # (e.g. "media/TrackData.h"), resolved via Sources above. The module
        # subdirs are also listed so the generated QML type-registration TU can
        # resolve a moved QML_ELEMENT header by the bare name moc may record.
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/metadata
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playback
        # Every subdir holding a QML_ELEMENT header must be here (the scenario
        # this prevents: for a subdir that is missing, qmltyperegistrar
        # silently omits the header include and the registration TU fails
        # with "no member named <Type> in namespace rawform").
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/settings
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/window
        ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename
        # rawform_audio's PUBLIC include (../audio/include) arrives transitively
        # through the linked target. Its private headers live under src/, so add
        # that path explicitly to reach the sinks (sinks/CoreAudioSink.h,
        # sinks/PipeWireSink.h, sinks/NullSink.h) from AudioController and
        # DecoderFactory.h from ReplayGainScanController, the same way the
        # engine's own CLI does.
        ${CMAKE_CURRENT_SOURCE_DIR}/../audio/src
)

# Warnings (errors in Debug builds only).
target_compile_options(rawform PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Debug>:-Werror>
        >
)

# qmlcachegen output is Qt's code, not ours. qt_add_qml_module compiles every
# file in RAWFORM_QML_FILES ahead of time into one .cpp under .rcc/qmlcache/
# and adds it to this target, so those sources inherit the -Werror above. What
# they contain is a function of the Qt version that generated them: the same
# QML has produced entries with unused lambda parameters on one Qt and clean
# ones on the next, so a CI image on a different Qt than the workstation
# failed on code nobody in this tree wrote. Warnings there stay warnings. The
# paths mirror Qt's own naming (<target>_<relative path>_qml.cpp, plus the
# module loader); a source property on a file that does not exist yet is
# fine, it attaches when Qt adds the file to the target.
set(_rawform_qmlcache_dir "${CMAKE_CURRENT_BINARY_DIR}/.rcc/qmlcache")
set(_rawform_qmlcache_sources
        "${_rawform_qmlcache_dir}/rawform_qmlcache_loader.cpp")
foreach(_qml IN LISTS RAWFORM_QML_FILES)
    cmake_path(REMOVE_EXTENSION _qml LAST_ONLY OUTPUT_VARIABLE _stem)
    list(APPEND _rawform_qmlcache_sources
            "${_rawform_qmlcache_dir}/rawform_${_stem}_qml.cpp")
endforeach()
set_source_files_properties(${_rawform_qmlcache_sources} PROPERTIES
        COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-error>"
)
unset(_rawform_qmlcache_dir)
unset(_rawform_qmlcache_sources)

# ---------------------------------------------------------------------------
# Optional unit tests (OFF by default; the normal app build is unaffected).
# Enable with -DRAWFORM_BUILD_TESTS=ON, then:
#   cmake --build <dir> --target rawform_pattern_test
#   ctest --test-dir <dir>
#
# The pattern-evaluator test is Quick-free: it links only Qt6::Core + yaml-cpp
# (pulled in by ColumnSchema.cpp) and the source files under test (plus the
# shared Formats helpers they call), so it builds and runs without standing
# up the GUI.
# ---------------------------------------------------------------------------
option(RAWFORM_BUILD_TESTS "Build rawform unit tests" OFF)
if(RAWFORM_BUILD_TESTS)
    enable_testing()

    add_executable(rawform_pattern_test
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/tests/PatternEvaluatorTest.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/PatternEvaluator.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/ColumnSchema.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/Formats.cpp
    )
    target_include_directories(rawform_pattern_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media
    )
    target_link_libraries(rawform_pattern_test PRIVATE
            Qt6::Core
            yaml-cpp::yaml-cpp
    )
    add_test(NAME pattern_evaluator COMMAND rawform_pattern_test)

    # Same recipe for the rename-name pipeline: Quick-free, no TagLib,
    # no filesystem. RenameSanitizer.cpp pulls in PatternEvaluator (the
    # transform hook) which pulls in ColumnSchema (fieldFromString), hence
    # the same three-source spine plus the unit under test.
    add_executable(rawform_rename_test
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/tests/RenameSanitizerTest.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename/RenameSanitizer.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/PatternEvaluator.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns/ColumnSchema.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/utils/Formats.cpp
    )
    target_include_directories(rawform_rename_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/columns
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/media
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/rename
    )
    target_link_libraries(rawform_rename_test PRIVATE
            Qt6::Core
            yaml-cpp::yaml-cpp
    )
    add_test(NAME rename_sanitizer COMMAND rawform_rename_test)

    # ------------------------------------------------------------------
    # The .rwfpl playlist-format test: round-trip fidelity, the full
    # PlaylistIoError taxonomy, unknown-chunk and short-chunk tolerance,
    # and truncation/bit-flip sweeps over a real written file (run it
    # under ASan/UBSan and the sweeps double as a cheap fuzz of the
    # bounds checks). Filesystem via QTemporaryDir; still Qt-Quick-free.
    # ------------------------------------------------------------------
    add_executable(rawform_playlist_test
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/tests/PlaylistFileTest.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources/playlist/PlaylistFile.cpp
    )
    target_include_directories(rawform_playlist_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Sources
    )
    target_link_libraries(rawform_playlist_test PRIVATE
            Qt6::Core
    )
    add_test(NAME playlist_file COMMAND rawform_playlist_test)
endif()

# ---------------------------------------------------------------------------
# Windows deployment (windeployqt)
# ---------------------------------------------------------------------------
if(WIN32)
    find_program(WINDEPLOYQT windeployqt HINTS "${CMAKE_PREFIX_PATH}/bin")

    if(WINDEPLOYQT)
        add_custom_command(TARGET rawform POST_BUILD
                COMMAND "${WINDEPLOYQT}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/Sources/qml"
                "$<TARGET_FILE:rawform>"
                COMMENT "Running windeployqt"
        )
    else()
        message(WARNING "windeployqt not found: Qt DLLs won't be copied to the build directory. "
                "Set CMAKE_PREFIX_PATH to your Qt installation (e.g. C:/Qt/6.11.0/msvc2022_64).")
    endif()
endif()
