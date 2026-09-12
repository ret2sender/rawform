#!/usr/bin/env bash
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

#
# rawform - macOS bundle deployment.
#
# Copies the Qt frameworks, QML modules and third-party dylibs the app links
# against into rawform.app, rewrites their install names, re-signs the bundle,
# and reports anything still pointing outside the bundle.
#
# rawform links a longer media chain than most Qt apps (libsndfile, libFLAC,
# libmpg123, libebur128, TagLib, yaml-cpp, and optionally four FFmpeg
# libraries), and macdeployqt handles non-Qt dylibs less reliably than Qt's
# own, so the verification report at the end is the pass criterion here, not
# the exit code.
#
# Ordering guarantee: with --dmg, the disk image is created by this script
# as the LAST step, from the pruned, re-signed, verified bundle, never by
# macdeployqt's own -dmg flag. macdeployqt would snapshot the image before
# the pruning and before the final re-sign, and that re-sign is load
# bearing: Homebrew bottle dylibs carry plain ad-hoc signatures (not
# linker signatures), so macdeployqt's install-name rewrites invalidate
# them without repair, and a bundle shipped in that state is killed at
# launch by dyld with CODESIGNING: Invalid Page the moment one of them is
# mapped (the scenario this ordering prevents: an image snapshotted before
# the re-sign ships exactly such a bundle). The ordering also makes the
# shipped image byte-identical to the bundle the reports and codesign
# --verify actually inspected.
#
# Usage:
#   scripts/macdeploy.sh [path/to/rawform.app] [--dmg]
#
# Environment:
#   QT_PREFIX           Qt installation prefix (default: `brew --prefix qt`,
#                       falling back to the newest ~/Qt/6.x.x/macos)
#   QMLDIR              QML source directory to scan (default: ui/Sources/qml)
#   APPSTORE_COMPLIANT  Pass -appstore-compliant to macdeployqt, skipping
#                       plugins that use private API (default: 1; set 0 to
#                       deploy them)
#   PRUNE_PLUGINS       Space-separated plugin directories to delete after
#                       deployment (default: sqldrivers; set empty to keep all)
#   PRUNE_STYLES        Qt Quick Controls styles to delete (default: all but
#                       Basic, which is the style rawform loads; set empty to
#                       keep all)
#   BUILD_TYPE          Build type of the bundle being deployed. Anything
#                       other than Release adds a lowercase suffix to the
#                       .dmg name (e.g. -debug), so a Debug image can never
#                       masquerade as a release. The Makefile passes this
#                       automatically; unset means no suffix.
#

set -euo pipefail

BUNDLE="${1:-build/app-release/rawform.app}"
QMLDIR="${QMLDIR:-ui/Sources/qml}"
MAKE_DMG=0

for arg in "$@"; do
    [[ "$arg" == "--dmg" ]] && MAKE_DMG=1
done

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "error: this script only runs on macOS." >&2
    exit 1
fi

if [[ ! -d "$BUNDLE" ]]; then
    echo "error: bundle not found: $BUNDLE" >&2
    echo "       build it first (e.g. 'make BUILD_TYPE=Release app')." >&2
    exit 1
fi

if [[ ! -d "$QMLDIR" ]]; then
    echo "error: QML directory not found: $QMLDIR" >&2
    echo "       run this from the repository root, or set QMLDIR." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Project version
#
# ui/VERSION.txt is the single source of truth (ui/CMakeLists.txt feeds
# project() and the bundle's Info.plist from the same file); the .dmg is
# named after it so a release image is identifiable without mounting it. The
# QMLDIR check above already guarantees this script runs from the repository
# root.
# ---------------------------------------------------------------------------
VERSION="$(tr -d '[:space:]' < ui/VERSION.txt 2>/dev/null || true)"
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "warning: ui/VERSION.txt does not contain a semantic version;"
    echo "         the .dmg will be named 'unversioned'."
    VERSION="unversioned"
fi

# ---------------------------------------------------------------------------
# Locate macdeployqt
#
# Same resolution order as the Makefile's Qt discovery: explicit QT_PREFIX,
# then PATH, then Homebrew, then the Qt online installer layout. A candidate
# only counts if the binary is actually executable there.
# ---------------------------------------------------------------------------
MACDEPLOYQT=""
if [[ -n "${QT_PREFIX:-}" && -x "$QT_PREFIX/bin/macdeployqt" ]]; then
    MACDEPLOYQT="$QT_PREFIX/bin/macdeployqt"
elif command -v macdeployqt >/dev/null 2>&1; then
    MACDEPLOYQT="$(command -v macdeployqt)"
elif command -v brew >/dev/null 2>&1 && [[ -x "$(brew --prefix qt 2>/dev/null)/bin/macdeployqt" ]]; then
    MACDEPLOYQT="$(brew --prefix qt)/bin/macdeployqt"
else
    for d in "$HOME"/Qt/6.*/macos; do
        [[ -x "$d/bin/macdeployqt" ]] && MACDEPLOYQT="$d/bin/macdeployqt"
    done
fi

if [[ -z "$MACDEPLOYQT" ]]; then
    echo "error: macdeployqt not found." >&2
    echo "       Set QT_PREFIX to your Qt installation, e.g.:" >&2
    echo "         QT_PREFIX=\$HOME/Qt/6.11.1/macos scripts/macdeploy.sh" >&2
    exit 1
fi

echo "==> macdeployqt : $MACDEPLOYQT"
echo "==> bundle      : $BUNDLE"
echo "==> qml sources : $QMLDIR"
echo

# ---------------------------------------------------------------------------
# Warn if the bundle looks already-deployed
#
# Running macdeployqt twice over the same bundle can mangle install names,
# and macdeployqt only ever adds files, never removes them. A fresh build is
# always the safe input.
# ---------------------------------------------------------------------------
if [[ -d "$BUNDLE/Contents/Frameworks" ]]; then
    echo "warning: $BUNDLE already contains Contents/Frameworks, so it looks"
    echo "         deployed. Re-running macdeployqt over a deployed bundle can"
    echo "         corrupt install names. Prefer a clean rebuild:"
    echo "             make distclean && make BUILD_TYPE=Release app bundle"
    echo
fi

# ---------------------------------------------------------------------------
# Deploy
#
# -appstore-compliant skips the plugins Qt knows use private API (SQL drivers
# for ODBC/PostgreSQL). rawform links no QtSql itself, but plugin deployment
# is category-driven, so the flag and the pruning below are kept as
# belt-and-suspenders: each is a no-op if the category never arrives.
# ---------------------------------------------------------------------------
APPSTORE_COMPLIANT="${APPSTORE_COMPLIANT:-1}"

DEPLOY_ARGS=("$BUNDLE" "-qmldir=$QMLDIR" "-always-overwrite" "-verbose=1")

if [[ "$APPSTORE_COMPLIANT" != "0" ]]; then
    DEPLOY_ARGS+=("-appstore-compliant")
fi

# Deliberately NOT forwarding --dmg to macdeployqt: the image is created
# at the end of this script from the finished bundle (see the ordering
# guarantee in the header).

echo "==> Running macdeployqt..."
echo "    flags: ${DEPLOY_ARGS[*]:1}"
"$MACDEPLOYQT" "${DEPLOY_ARGS[@]}"
echo

# ---------------------------------------------------------------------------
# Prune unused plugins and styles
#
# PRUNING IS FOR PLUGINS ONLY. The two mechanisms are opposites:
#
#   * PlugIns/ content is copied by CATEGORY, over-inclusively; nothing in the
#     app's load commands references it (plugins are dlopen candidates), so
#     deleting an unused one is safe.
#
#   * Contents/Frameworks content is copied by walking the actual Mach-O LOAD
#     COMMANDS, so that set is by construction exactly what dyld demands at
#     launch. The video-codec dylibs (x264, x265, SVT-AV1, vpx, dav1d) are
#     hard dependencies of Homebrew's libavcodec itself, and OpenSSL of
#     libavformat; delete any of them and the app is killed at startup with
#     "Library not loaded". Never hand-prune this directory. The right way to
#     shed the video chain is linking a minimal audio-only FFmpeg build, which
#     is outside this script's scope, not deleting files here.
#
# macdeployqt copies plugins by category, tied to the frameworks in use rather
# than to what the application actually loads. Every Qt Quick Controls style
# arrives because QtQuickControls2 is in use; rawform's own QML imports
# QtQuick.Controls.Basic directly (compile-time style selection), and
# main.cpp additionally keeps QQuickStyle::setStyle("Basic") because
# QtQuick.Dialogs builds its chrome from the RUNTIME-selected style, which
# without the call would default to the native macOS style, one of the very
# styles pruned below. So Basic is the only style the app can ever ask for,
# and the rest are dead weight. Note the pruning is still required either
# way: style deployment is category-driven and is not prevented by the
# direct imports.
#
# Each style exists as up to three pieces - a framework pair, a plugin pair,
# and a QML module directory - which must be removed together or the survivors
# reference something that is no longer there.
#
# This must happen before signing: deleting files afterwards invalidates the
# signature.
#
#   PRUNE_PLUGINS  plugin directories to delete       (default: sqldrivers)
#   PRUNE_STYLES   Quick Controls styles to delete    (default: all but Basic)
#
# Set either to "" to keep everything. If the application ever calls
# QQuickStyle::setStyle() with something other than Basic, adjust PRUNE_STYLES
# to match or the chosen style will be missing at runtime.
# ---------------------------------------------------------------------------
PRUNE_PLUGINS="${PRUNE_PLUGINS-sqldrivers}"
PRUNE_STYLES="${PRUNE_STYLES-Material Fusion Imagine Universal FluentWinUI3 IOS MacOS}"

shopt -s nullglob

if [[ -n "$PRUNE_PLUGINS" || -n "$PRUNE_STYLES" ]]; then
    echo "==> Pruning unused components..."
    pruned=0

    for d in $PRUNE_PLUGINS; do
        target="$BUNDLE/Contents/PlugIns/$d"
        if [[ -d "$target" ]]; then
            rm -rf "$target"
            echo "    plugins    PlugIns/$d"
            pruned=$((pruned + 1))
        fi
    done

    for S in $PRUNE_STYLES; do
        s="$(echo "$S" | tr '[:upper:]' '[:lower:]')"

        for fw in "$BUNDLE"/Contents/Frameworks/QtQuickControls2"$S"*.framework; do
            rm -rf "$fw"
            echo "    framework  $(basename "$fw")"
            pruned=$((pruned + 1))
        done

        for pl in "$BUNDLE"/Contents/PlugIns/quick/libqtquickcontrols2"$s"style*plugin.dylib; do
            rm -f "$pl"
            echo "    plugin     $(basename "$pl")"
            pruned=$((pruned + 1))
        done

        # QML directory names differ in case from the module names
        # (iOS vs IOS, macOS vs MacOS), so match case-insensitively.
        qmlroot="$BUNDLE/Contents/Resources/qml/QtQuick/Controls"
        if [[ -d "$qmlroot" ]]; then
            while IFS= read -r d; do
                [[ -n "$d" ]] || continue
                rm -rf "$d"
                echo "    qml        QtQuick/Controls/${d##*/}"
                pruned=$((pruned + 1))
            done < <(find "$qmlroot" -maxdepth 1 -type d -iname "$S" 2>/dev/null || true)
        fi
    done

    echo "    ($pruned items removed)"
    echo
fi

# ---------------------------------------------------------------------------
# Ad-hoc code signature
#
# Rewriting install names invalidates any existing signature. On Apple
# Silicon every binary must carry at least an ad-hoc signature or the app
# is killed at launch, so re-sign the whole bundle.
# ---------------------------------------------------------------------------
echo "==> Re-signing (ad-hoc)..."
codesign --force --deep --sign - "$BUNDLE"
# Hard gate, not annotation: the dmg below ships exactly this bundle, so a
# signature that does not verify must abort the build (set -e). --strict
# additionally rejects the modified-after-signing states dyld kills on.
codesign --verify --deep --strict --verbose=2 "$BUNDLE" 2>&1 | sed 's/^/    /'
echo

# ---------------------------------------------------------------------------
# Inventory the bundled third-party dylibs
#
# The media chain is the reason this report exists: eyeball that libsndfile,
# FLAC, mpg123, ebur128, TagLib, yaml-cpp and (if enabled) the four libav*
# libraries all actually landed. A library missing from this list but linked
# by the app will also show up in the external-reference check below, but
# this positive listing is easier to read.
# ---------------------------------------------------------------------------
echo "==> Bundled non-Qt dylibs:"
found_dylib=0
for f in "$BUNDLE"/Contents/Frameworks/*.dylib; do
    echo "    $(basename "$f")"
    found_dylib=1
done
[[ "$found_dylib" -eq 0 ]] && echo "    (none)"
echo

# ---------------------------------------------------------------------------
# Verify self-containment
#
# Walk every Mach-O file in the bundle and report load commands still
# resolving to Homebrew or other prefixes outside the bundle.
# ---------------------------------------------------------------------------
echo "==> Checking for dependencies outside the bundle..."

EXTERNAL="$(
    find "$BUNDLE/Contents" -type f -print0 |
    while IFS= read -r -d '' f; do
        file -b "$f" 2>/dev/null | grep -q 'Mach-O' || continue
        otool -L "$f" 2>/dev/null | tail -n +2 |
            grep -E '/opt/homebrew|/usr/local/opt|/usr/local/Cellar|/opt/local' |
            sed "s|^[[:space:]]*|    ${f#"$BUNDLE"/}: |"
    done || true
)"

if [[ -n "$EXTERNAL" ]]; then
    echo
    echo "  The following still reference libraries outside the bundle:"
    echo "$EXTERNAL"
    echo
    echo "  The app will run on this machine but not on one without those"
    echo "  libraries installed. Each can be fixed with install_name_tool,"
    echo "  or by passing the library to macdeployqt explicitly."
    echo
    echo "  (System libraries under /usr/lib and /System are expected and"
    echo "  are not reported here.)"
else
    echo "    None - every linked library resolves inside the bundle or to"
    echo "    system paths. The bundle looks self-contained."
fi

echo
echo "==> Checking for references to removed frameworks..."

DANGLING="$(
    find "$BUNDLE/Contents" -type f -print0 |
    while IFS= read -r -d '' f; do
        file -b "$f" 2>/dev/null | grep -q 'Mach-O' || continue
        otool -L "$f" 2>/dev/null | tail -n +2 |
            grep -o '@rpath/[A-Za-z0-9_.]*\.framework' | sort -u |
            while IFS= read -r ref; do
                name="${ref#@rpath/}"
                if [[ ! -d "$BUNDLE/Contents/Frameworks/$name" ]]; then
                    echo "    ${f#"$BUNDLE"/} -> $name"
                fi
            done
    done | sort -u || true
)"

if [[ -n "$DANGLING" ]]; then
    echo
    echo "  These link against frameworks that are not in the bundle:"
    echo "$DANGLING"
    echo
    echo "  Pruning removed something still in use. Re-run with the relevant"
    echo "  style restored, e.g.:"
    echo "      PRUNE_STYLES=\"Material Fusion\" make BUILD_TYPE=Release bundle"
    echo "  or PRUNE_STYLES=\"\" to keep them all."
else
    echo "    None - every framework reference resolves inside the bundle."
fi

echo
echo "==> Checking for unresolved dylib references..."

# The framework check above cannot see these: a bare dylib is referenced as
# @rpath/libfoo.dylib, @loader_path/libfoo.dylib or
# @executable_path/../Frameworks/libfoo.dylib, all of which macdeployqt
# resolves to Contents/Frameworks. A reference whose target is absent there
# means either Contents/Frameworks was hand-pruned (see the pruning header:
# do not) or macdeployqt missed a nested dependency; both are killed-at-launch
# conditions that a clean exit code and the framework check would miss.
DANGLING_DYLIBS="$(
    find "$BUNDLE/Contents" -type f -print0 |
    while IFS= read -r -d '' f; do
        file -b "$f" 2>/dev/null | grep -q 'Mach-O' || continue
        otool -L "$f" 2>/dev/null | tail -n +2 |
            grep -oE '@(rpath|loader_path|executable_path)[^ ]*\.dylib' | sort -u |
            while IFS= read -r ref; do
                name="${ref##*/}"
                if [[ ! -f "$BUNDLE/Contents/Frameworks/$name" ]]; then
                    echo "    ${f#"$BUNDLE"/} -> $name"
                fi
            done
    done | sort -u || true
)"

if [[ -n "$DANGLING_DYLIBS" ]]; then
    echo
    echo "  These reference dylibs that are not in Contents/Frameworks:"
    echo "$DANGLING_DYLIBS"
    echo
    echo "  If Contents/Frameworks was pruned by hand, restore from a clean"
    echo "  'make BUILD_TYPE=Release app bundle'; those files are resolved"
    echo "  from load commands and are all required at launch. Otherwise"
    echo "  macdeployqt missed a nested dependency: copy it in and rewrite"
    echo "  its install name with install_name_tool, then re-sign."
else
    echo "    None - every dylib reference resolves inside the bundle."
fi

echo
echo "==> Done: $BUNDLE"

# ---------------------------------------------------------------------------
# Create the disk image (LAST, from the finished bundle)
#
# Created here rather than by macdeployqt so the image contains the pruned,
# re-signed, verified bundle the reports above inspected; see the ordering
# guarantee in the header. Named from ui/VERSION.txt and the architecture
# (arm64 on Apple Silicon) so a release image is identifiable without
# mounting it.
# ---------------------------------------------------------------------------
if [[ "$MAKE_DMG" -eq 1 ]]; then
    # Release images carry no build-type suffix; anything else is marked.
    TYPE_TAG=""
    if [[ -n "${BUILD_TYPE:-}" && "${BUILD_TYPE}" != "Release" ]]; then
        TYPE_TAG="-$(echo "$BUILD_TYPE" | tr '[:upper:]' '[:lower:]')"
    fi

    DMG_DST="$(dirname "$BUNDLE")/rawform-$VERSION-macos-$(uname -m)$TYPE_TAG.dmg"
    rm -f "$DMG_DST"
    hdiutil create -volname "rawform" -srcfolder "$BUNDLE" \
        -ov -format UDZO "$DMG_DST"
    echo "==> Disk image: $DMG_DST"
fi
