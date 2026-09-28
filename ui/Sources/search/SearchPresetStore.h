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

// SearchPresetStore.h
//
// Saved Find filters for Edit > Find: named { name, pattern } presets where
// the pattern is a Filter-box expression (see search/PlaylistSearchFilter.h
// for the grammar), persisted to search_filters.yaml under userConfigDir().
// A preset stores the Filter text only, never the String box: a preset is a
// search SCOPE, not a query. The whole contract (tolerant load, name-keyed
// upsert, last-used, atomic write-through) is NamedPatternStore's; this type
// only binds the file name and the banner, and is the QML_ELEMENT the Find
// dialog instantiates.

#pragma once

#include "presets/NamedPatternStore.h"

#include <QObject>
#include <QQmlEngine> // QML_ELEMENT

namespace rawform {

class SearchPresetStore : public NamedPatternStore {
    Q_OBJECT
    QML_ELEMENT

public:
    /// Loads search_filters.yaml immediately (see NamedPatternStore).
    explicit SearchPresetStore(QObject* parent = nullptr);
    ~SearchPresetStore() override = default;
};

} // namespace rawform
