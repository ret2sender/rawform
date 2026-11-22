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

// PerfLog.h
//
// The "rawform.perf" logging category: timing lines around the operations
// whose cost scales with the playlist (bulk removals and moves, the scanner's
// enumeration and reads, the reloader's result drain, the search haystack
// build), plus the album-art decode, the audio sink's construction, the
// spectrum's park and resume, and each heap trim with the resident set before
// and after it (HeapTrim.h). Off by default; the lines cost one category
// check when disabled. Enable with
//
//     QT_LOGGING_RULES="rawform.perf.debug=true"
//
// (and QT_LOGGING_TO_CONSOLE=1 when stderr is not a terminal, or the lines go
// to the journal). Each timing line names the operation and the elapsed time,
// with a count or size where one applies, so a before/after pair reads off one
// grep.

#pragma once

#include <QLoggingCategory>

namespace rawform {

Q_DECLARE_LOGGING_CATEGORY(lcPerf)

} // namespace rawform
