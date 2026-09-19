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

// CustomColumnRegistry.h
//
// The app-global registry of user-defined custom columns: the single source of truth for
// their definitions.
//
// One shared instance is injected by pointer into each PlaylistModel via
// setCustomColumns() and exposed to QML for the manager window. A single shared
// registry (rather than a copy per model) means every tab sees the same
// definitions and a manager edit reflects everywhere at once; models only read
// it and react to its signals, never mutate it.
//
// Persistence is wholesale write-through to playlist_custom_columns.yaml under
// userConfigDir(), through the shared YAML idiom (utils/YamlFile.h: atomic
// write). Reads are tolerant: a missing or malformed file yields zero columns
// and never throws, so a hand-edit typo can't wedge startup.
//
// Rendering lives elsewhere (PatternEvaluator, driven by the model); this class
// is pure definition management plus I/O.

#pragma once

#include "columns/ColumnSchema.h" // CustomColumn

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>

namespace rawform {

class CustomColumnRegistry : public QObject {
    Q_OBJECT

public:
    explicit CustomColumnRegistry(QObject* parent = nullptr);
    ~CustomColumnRegistry() override = default;

    /// Load the records from playlist_custom_columns.yaml. Tolerant: a missing or
    /// malformed file leaves the registry empty (a warning is logged when
    /// malformed). Call once at startup, before the model binds. Does not emit;
    /// nothing is observing yet.
    void load();

    // --- Lookup (used by the model) ----------------------------------------

    /// A snapshot copy of every record, in user order (the order shown in the
    /// manager and written to the file).
    [[nodiscard]] QList<CustomColumn> all() const { return m_columns; }

    /// The record with @p id, or nullptr if there is none. The pointer is valid
    /// until the next mutation; callers (the model) copy what they need
    /// per-render rather than holding it.
    [[nodiscard]] const CustomColumn* byId(const QString& id) const;

    /// Whether a record with @p id exists.
    [[nodiscard]] bool contains(const QString& id) const;

    // --- Management (used by the manager window in QML) --------------------
    // Every mutator persists immediately and emits so the model(s) and the
    // manager refresh. add() returns the new id so the manager can select it.

    /// Append a new, blank record (empty name + pattern, left-aligned). Persists,
    /// emits columnsChanged(), and returns the generated id.
    Q_INVOKABLE QString add();

    /// The records as QML-friendly maps: { id, fieldId ("custom:<id>"), name,
    /// align ("left"/"center"/"right"), pattern }, in user order. Drives the
    /// manager table.
    [[nodiscard]] Q_INVOKABLE QVariantList catalog() const;

    /// Remove the record with @p id. Persists, then emits columnRemoved(id) so a
    /// model showing that column drops it (no dangling header). No-op for an
    /// unknown id.
    Q_INVOKABLE void remove(const QString& id);

    /// Set the alignment of @p id from a Qt::Alignment flag value
    /// (Qt::AlignLeft / Qt::AlignHCenter / Qt::AlignRight). Persists + emits.
    Q_INVOKABLE void setAlignment(const QString& id, int alignment);

    /// Set the header label of @p id. Persists + emits columnsChanged() on a real
    /// change. No-op for an unknown id or an unchanged value.
    Q_INVOKABLE void setName(const QString& id, const QString& name);

    /// Set the %token% pattern of @p id. Persists + emits columnsChanged().
    Q_INVOKABLE void setPattern(const QString& id, const QString& pattern);

signals:
    /// The record @p id was removed; the model drops any visual column bound to
    /// it. remove() emits only this (not columnsChanged()) to avoid double work.
    void columnRemoved(QString id);

    /// A record was added or one of its fields (name/alignment/pattern) changed.
    /// The model refreshes any shown custom column's header/cells/alignment; the
    /// manager re-reads the table.
    void columnsChanged();

private:
    /// Index of @p id in m_columns, or -1.
    [[nodiscard]] int indexOf(const QString& id) const;

    /// Write the whole list to playlist_custom_columns.yaml (atomic; a failed
    /// write logs a warning and keeps the in-memory state). User order is
    /// preserved.
    void persist() const;

    QList<CustomColumn> m_columns;
};

} // namespace rawform
