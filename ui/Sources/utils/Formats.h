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

// Formats.h
//
// The shared display formatters for track facts, factored out of their
// carriers so the playlist columns and the Properties panes render the same
// fact the same way (the scenario this prevents: the file-size column and
// the metadata pane each formatting bytes on their own, so one file shows
// two different sizes). One definition, both consumers.
//
// Deliberately small: only formats used by MORE THAN ONE surface live here.
// A single-surface format (the metadata pane's "M:SS.mmm (N samples)"
// duration, its "(N bytes)" size suffix) stays with its surface and composes
// on these where it can. The QML transport clock (m:ss from SECONDS) has its
// own QML-side singleton (qml/components/Format.qml) because its inputs come
// from QML bindings; keep the two in step if either shape ever changes.

#pragma once

#include <QString>
#include <QtGlobal>  // qint64

namespace rawform::formats {

/// "m:ss" from milliseconds (seconds zero-padded, minutes not); empty when
/// unknown (ms <= 0). The playlist Duration column shape.
[[nodiscard]] QString durationText(int ms);

/// Compact human-readable size, e.g. "53.2 MB" (1024-based traditional units,
/// one decimal, locale-formatted); empty when unknown (bytes <= 0).
[[nodiscard]] QString fileSizeText(qint64 bytes);

}  // namespace rawform::formats
