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

# ---------------------------------------------------------------------------
# install_linux_icons.sh: per-user desktop integration for dev builds.
# ---------------------------------------------------------------------------
# Installs the two artifacts that make a Linux desktop treat rawform like an
# installed application instead of an anonymous window:
#
#   ~/.local/share/applications/com.rawform.app.desktop
#   ~/.local/share/icons/hicolor/scalable/apps/com.rawform.app.svg
#
# What this buys, and why each piece exists:
#   - main.cpp pins the Wayland app_id to com.rawform.app via
#     setDesktopFileName. With the desktop file installed, KWin resolves the
#     window's icon from the icon theme by that id (the same mechanism as any
#     installed app), so every surface (taskbar, Alt-Tab at any icon size,
#     pager) gets a fresh vector render instead of a scaled protocol bitmap.
#   - The same lookup satisfies xdg-desktop-portal's app registration, which
#     silences the startup console error:
#       "Could not register app ID: App info not found for 'com.rawform.app'"
#   - Scalable-only on purpose: KDE and GNOME both rasterize scalable/apps
#     SVGs at need; no fixed-size PNG ladder to maintain. If some minor
#     environment ever demands fixed sizes, add them then, not now.
#
# This mirrors the Flatpak's own export: the manifest exports these same two
# artifacts under the same id. The desktop
# entry is a COMMITTED asset (packaging/com.rawform.app.desktop) shared by
# both consumers: the manifest installs it verbatim (the exporter rewrites
# Exec= to the sandbox launcher), and this script installs it with Exec=
# substituted for the dev binary, so the two can never drift apart. Run
# --uninstall before validating the Flatpak's own export so the per-user
# files never shadow the sandbox's.
#
# Usage (from the repository root):
#   ui/tools/install_linux_icons.sh [path-to-rawform-binary]
#   ui/tools/install_linux_icons.sh --uninstall
#
# The binary path lands in the desktop file's Exec= line and only matters for
# launcher-menu starts; icon resolution and portal registration key on the
# desktop file's existence and Icon= field, not on Exec=. Defaults to plain
# "rawform" (i.e. found on PATH) when omitted.

set -euo pipefail

APP_ID="com.rawform.app"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_SVG="${SCRIPT_DIR}/../Sources/icons/app/rawform.svg"
REPO_DESKTOP="${SCRIPT_DIR}/../../packaging/${APP_ID}.desktop"

DATA_HOME="${XDG_DATA_HOME:-${HOME}/.local/share}"
DESKTOP_DIR="${DATA_HOME}/applications"
ICON_DIR="${DATA_HOME}/icons/hicolor/scalable/apps"
DESKTOP_FILE="${DESKTOP_DIR}/${APP_ID}.desktop"
ICON_FILE="${ICON_DIR}/${APP_ID}.svg"

refresh_caches() {
    # Best effort, in dependency order; every tool here is optional.
    #   - update-desktop-database: rebuilds the desktop-entry cache portals
    #     and launchers consult.
    #   - touch on the theme root: KIconLoader invalidates on directory
    #     mtime, which is all a per-user hicolor needs (the user dir usually
    #     has no index.theme, so gtk-update-icon-cache would just complain;
    #     it is intentionally not called).
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "${DESKTOP_DIR}" || true
    fi
    if [ -d "${DATA_HOME}/icons/hicolor" ]; then
        touch "${DATA_HOME}/icons/hicolor"
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "${DESKTOP_FILE}" "${ICON_FILE}"
    refresh_caches
    echo "removed ${DESKTOP_FILE}"
    echo "removed ${ICON_FILE}"
    echo "note: log out/in or restart plasmashell if a stale icon lingers"
    exit 0
fi

EXEC_PATH="${1:-rawform}"

if [ ! -f "${REPO_SVG}" ]; then
    echo "error: repo icon not found at ${REPO_SVG}" >&2
    echo "place the scalable app icon at Sources/icons/app/rawform.svg" >&2
    exit 1
fi
if [ ! -f "${REPO_DESKTOP}" ]; then
    echo "error: committed desktop entry not found at ${REPO_DESKTOP}" >&2
    echo "it lives at packaging/${APP_ID}.desktop" >&2
    exit 1
fi

install -Dm644 "${REPO_SVG}" "${ICON_FILE}"

mkdir -p "${DESKTOP_DIR}"
# The committed asset is the single source of truth for the entry's content
# (theme-name Icon=, %F so open-with hands files to the running instance,
# SingleMainWindow). Only Exec= is substituted here: the committed file carries the plain
# "rawform" the Flatpak exporter rewrites, while a dev install needs the
# actual binary path for launcher-menu starts.
sed "s|^Exec=.*|Exec=${EXEC_PATH} %F|" "${REPO_DESKTOP}" > "${DESKTOP_FILE}"
chmod 644 "${DESKTOP_FILE}"

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "${DESKTOP_FILE}"
fi

refresh_caches

echo "installed ${ICON_FILE}"
echo "installed ${DESKTOP_FILE} (Exec=${EXEC_PATH})"
echo "if the window icon does not update, close and relaunch rawform;"
echo "compositors bind the desktop entry at window creation."
