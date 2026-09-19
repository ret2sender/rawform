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

// RenamePatternStore.h
//
// Saved rename patterns for File Operations > Rename To: named { name, pattern } presets,
// persisted to rename_patterns.yaml under userConfigDir().
//
// CustomColumnRegistry's conventions: wholesale write-through through the
// shared YAML idiom (utils/YamlFile.h: atomic write), tolerant load (a
// missing or malformed file yields zero presets and never throws, so a
// hand-edit typo cannot wedge the dialog), user order preserved in the file.
//
// One deliberate divergence from the registry: records are keyed by their
// user-facing NAME, not a generated id. Nothing references a preset from
// another file (unlike columns, whose ids live inside .rwfpl headers), so an
// id would be dead weight, and name-keying gives save() the semantics the
// dialog's Save button wants: saving under an existing name replaces that
// preset in place, keeping its position in the list.
//
// QML_ELEMENT (unlike the injected registry): the rename dialog instantiates
// its own store, and the constructor loads from disk, so a fresh dialog
// always reflects the file, including edits made by another window's dialog
// earlier in the session (write-through means the file is always the truth).
//
// Ships EMPTY by design: no seed presets.

#pragma once

#include <QList>
#include <QObject>
#include <QQmlEngine>  // QML_ELEMENT
#include <QString>
#include <QVariantList>

namespace rawform {

class RenamePatternStore : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    /// Loads rename_patterns.yaml immediately (see class comment for why the
    /// constructor, unlike the registry's explicit load(): every instance is
    /// dialog-local and wants disk truth at birth). Safe: no observers exist
    /// yet, so nothing is emitted.
    explicit RenamePatternStore(QObject* parent = nullptr);
    ~RenamePatternStore() override = default;

    /// One saved preset.
    struct Preset {
        QString name;
        QString pattern;
    };

    /// Re-read the file, replacing the in-memory list. Tolerant; malformed
    /// logs a warning and yields empty. Emits patternsChanged() so a bound
    /// dropdown refreshes (the constructor's initial load goes through here
    /// too; the emission is harmless with no connections yet).
    Q_INVOKABLE void load();

    /// The presets as QML-friendly maps { name, pattern }, in user order.
    /// Drives the dialog's dropdown.
    [[nodiscard]] Q_INVOKABLE QVariantList catalog() const;

    /// The pattern saved under @p name, or "" if there is none. "" is
    /// unambiguous here: save() refuses empty patterns, so no real preset
    /// can hold one.
    [[nodiscard]] Q_INVOKABLE QString patternFor(const QString& name) const;

    /// Upsert: replace the pattern of the existing preset named @p name (kept
    /// in place), or append a new preset. @p name is trimmed first; an empty
    /// trimmed name or an empty pattern is a no-op (the dialog gates these
    /// too; the store must not rely on it). Persists and emits
    /// patternsChanged() on any real change.
    Q_INVOKABLE void save(const QString& name, const QString& pattern);

    /// Remove the preset named @p name. No-op for an unknown name. Persists
    /// and emits patternsChanged().
    Q_INVOKABLE void remove(const QString& name);

    // --- last-used preset (dialog convenience) -----------------------------
    // Persisted in the same file (top-level `last_used:` key) so a freshly
    // opened dialog can preselect and preload what the person used last.
    // Stored as given (trimmed); the dialog guards through patternFor, so a
    // name whose preset was since deleted simply preselects nothing.

    /// The last name passed to setLastUsed, "" when none was ever set.
    [[nodiscard]] Q_INVOKABLE QString lastUsed() const { return m_lastUsed; }

    /// Record @p name (trimmed) as the last-used preset and persist. No-op
    /// on an unchanged or empty trimmed value. Does not emit: the preset
    /// LIST is unchanged, and the value is only read at dialog open.
    Q_INVOKABLE void setLastUsed(const QString& name);

signals:
    /// The preset list changed (load/save/remove). The dialog re-reads
    /// catalog().
    void patternsChanged();

private:
    /// Index of the preset named @p name (exact match), or -1.
    [[nodiscard]] int indexOf(const QString& name) const;

    /// Write the whole list to rename_patterns.yaml (atomic; a failed write
    /// logs a warning and keeps the in-memory state). User order preserved.
    void persist() const;

    QList<Preset> m_presets;
    QString m_lastUsed;
};

}  // namespace rawform
