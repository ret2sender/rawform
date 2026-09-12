#!/usr/bin/env bash
#
# rawform - insert per-file license headers.
#
# Walks audio/ and ui/, prepending the GPL-3.0-or-later notice stanza to every
# source file that does not already carry one. Comment style follows the file
# type: /* */ for C++, Objective-C++ and QML; # for CMake and shell. Binary
# assets, fonts, license texts, IDE state and build output are never touched.
#
# The script is idempotent: a file whose first lines already contain an
# SPDX-License-Identifier is skipped, so re-running it after adding new files
# stamps only the newcomers.
#
# Usage (from the repository root):
#   scripts/add-license-headers.sh            # apply
#   scripts/add-license-headers.sh --dry-run  # list what would change, change nothing
#   scripts/add-license-headers.sh --check    # exit 1 if any file lacks a header
#
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

set -euo pipefail

HOLDER="Etienne Fleurant"
YEAR="2026"

MODE="apply"
case "${1:-}" in
    --dry-run) MODE="dry" ;;
    --check)   MODE="check" ;;
    "")        ;;
    *) echo "usage: $0 [--dry-run|--check]" >&2; exit 2 ;;
esac

# Run from the repository root: both component directories must be present.
if [[ ! -d audio || ! -d ui ]]; then
    echo "error: run this from the repository root (audio/ and ui/ not found)." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Header texts. The stanza is the FSF's canonical per-file notice; keep its
# wording intact so license scanners match it.
# ---------------------------------------------------------------------------
c_header() {
cat <<EOF
/*
 * This file is part of rawform.
 * Copyright (C) ${YEAR} ${HOLDER}
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

EOF
}

hash_header() {
cat <<EOF
# This file is part of rawform.
# Copyright (C) ${YEAR} ${HOLDER}
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

EOF
}

# ---------------------------------------------------------------------------
# Comment style by file type. Classification is by extension and well-known
# name, never by substring matching on arbitrary filenames.
# ---------------------------------------------------------------------------
style_for() {
    local f="$1"
    local base; base="$(basename "$f")"
    case "$base" in
        CMakeLists.txt) echo hash; return ;;
        Makefile)       echo hash; return ;;
    esac
    case "$f" in
        *.cpp|*.h|*.hpp|*.mm|*.qml) echo c ;;
        *.cmake|*.sh)               echo hash ;;
        *)                          echo skip ;;
    esac
}

# ---------------------------------------------------------------------------
# Insertion. A shebang line must stay on line 1, so the header goes after it;
# everything else (including QML pragma lines, which legally follow comments)
# gets the header prepended.
# ---------------------------------------------------------------------------
insert_header() {
    local f="$1" style="$2"
    local tmp; tmp="$(mktemp)"
    local first; first="$(head -n 1 "$f" || true)"

    if [[ "$first" == '#!'* ]]; then
        {
            printf '%s\n' "$first"
            if [[ "$style" == c ]]; then c_header; else hash_header; fi
            tail -n +2 "$f"
        } > "$tmp"
    else
        {
            if [[ "$style" == c ]]; then c_header; else hash_header; fi
            cat "$f"
        } > "$tmp"
    fi

    # Preserve the original mode (mktemp files are 0600, which would strip
    # the executable bit from shell scripts).
    chmod --reference="$f" "$tmp" 2>/dev/null || chmod "$(stat -f '%Lp' "$f")" "$tmp"
    mv "$tmp" "$f"
}

# ---------------------------------------------------------------------------
# Walk. Prune IDE state, build output and archive noise; scripts/ is included
# so future tooling added there gets stamped too (this script skips itself
# through the idempotency check).
# ---------------------------------------------------------------------------
stamped=0
skipped=0
missing=0

while IFS= read -r -d '' f; do
    style="$(style_for "$f")"
    [[ "$style" == skip ]] && continue

    if head -n 40 "$f" | grep -q 'SPDX-License-Identifier'; then
        skipped=$((skipped + 1))
        continue
    fi

    case "$MODE" in
        dry)
            echo "would stamp ($style): $f"
            missing=$((missing + 1))
            ;;
        check)
            echo "missing header: $f"
            missing=$((missing + 1))
            ;;
        apply)
            insert_header "$f" "$style"
            echo "stamped ($style): $f"
            stamped=$((stamped + 1))
            ;;
    esac
done < <(find audio ui scripts \
            -type d \( -name '.idea' -o -name 'build*' \
                       -o -name 'cmake-build-*' -o -name '__MACOSX' \) -prune \
            -o -type f -print0)

echo
case "$MODE" in
    apply) echo "done: $stamped stamped, $skipped already had headers." ;;
    dry)   echo "dry run: $missing would be stamped, $skipped already have headers." ;;
    check)
        echo "check: $missing missing, $skipped present."
        [[ "$missing" -eq 0 ]] || exit 1
        ;;
esac
