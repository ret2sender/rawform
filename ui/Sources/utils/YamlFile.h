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

// YamlFile.h
//
// The shared file-IO half of the app's YAML persistence idiom. Each store
// (SettingsStore, WindowGeometryStore, RenamePatternStore,
// CustomColumnRegistry, SpectrumProvider, AudioController) keeps its own
// schema: what to emit, the banner comment, and the tolerant per-key parse.
// What is shared is only the IO discipline, so every store gets the same
// first-run and crash-safety behavior without restating it:
//
//   READ:  whole-file into a std::string for yaml-cpp; absent or unreadable is
//          the normal first-run case, reported as nullopt with no warning (the
//          caller's defaults stand).
//   WRITE: ensure the directory, then write through a QSaveFile so a crash
//          mid-write can never truncate the file; a qWarning on each failure
//          mode (dir, open, short write, commit), never an exception.
//
// Free functions, no state, GUI-thread only like every caller.

#pragma once

#include <QByteArray>
#include <QString>

#include <optional>
#include <string>

namespace rawform::yamlfile {

/// Read `path` fully as text, for yaml-cpp's YAML::Load. nullopt when the file
/// is absent or unreadable, which callers treat as first-run defaults.
[[nodiscard]] std::optional<std::string> readAll(const QString& path);

/// Atomically replace `path` with `content` (banner + emitted YAML, trailing
/// newline included by the caller): mkpath the parent, then QSaveFile
/// open/write/commit. Warns and returns false on any failure; on false the
/// previous file content, if any, is untouched.
bool writeAtomically(const QString& path, const QByteArray& content);

}  // namespace rawform::yamlfile
