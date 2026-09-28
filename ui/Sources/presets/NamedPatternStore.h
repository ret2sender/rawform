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

// NamedPatternStore.h
//
// The shared store behind every "named preset" dropdown: a user-ordered list
// of { name, pattern } records plus a last-used name, persisted to one YAML
// file under userConfigDir(). RenamePatternStore (rename_patterns.yaml) and
// SearchPresetStore (search_filters.yaml) are this class with a file name
// and a banner; the dialogs talk to the invokable surface below and never see
// the difference.
//
// CustomColumnRegistry's conventions: wholesale write-through through the
// shared YAML idiom (utils/YamlFile.h: atomic write), tolerant load (a
// missing or malformed file yields zero presets and never throws, so a
// hand-edit typo cannot wedge a dialog), user order preserved in the file.
//
// One deliberate divergence from the registry: records are keyed by their
// user-facing NAME, not a generated id. Nothing references a preset from
// another file (unlike columns, whose ids live inside .rwfpl headers), so an
// id would be dead weight, and name-keying gives save() the semantics a
// dialog's Save button wants: saving under an existing name replaces that
// preset in place, keeping its position in the list.
//
// The constructor loads from disk (unlike the registry's explicit load()):
// every instance is dialog-local, so a fresh dialog always reflects the file,
// including edits made by another window's dialog earlier in the session
// (write-through means the file is always the truth). Safe: no observers
// exist yet, so the load's emission reaches nobody.
//
// QML_ANONYMOUS: not creatable from QML (no file name to create it with); the
// subclasses are the QML_ELEMENT types, and their instances expose these
// invokables through the meta-object chain.
//
// Ships EMPTY by design: no seed presets.

#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QQmlEngine> // QML_ANONYMOUS
#include <QString>
#include <QVariantList>

namespace rawform {

class NamedPatternStore : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

public:
    ~NamedPatternStore() override = default;

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
    /// Drives a dialog's dropdown.
    [[nodiscard]] Q_INVOKABLE QVariantList catalog() const;

    /// The pattern saved under @p name, or "" if there is none. "" is
    /// unambiguous here: save() refuses empty patterns, so no real preset
    /// can hold one.
    [[nodiscard]] Q_INVOKABLE QString patternFor(const QString& name) const;

    /// Upsert: replace the pattern of the existing preset named @p name (kept
    /// in place), or append a new preset. @p name is trimmed first; an empty
    /// trimmed name or an empty pattern is a no-op (the dialogs gate these
    /// too; the store must not rely on it). Persists and emits
    /// patternsChanged() on any real change.
    Q_INVOKABLE void save(const QString& name, const QString& pattern);

    /// Remove the preset named @p name. No-op for an unknown name. Persists
    /// and emits patternsChanged().
    Q_INVOKABLE void remove(const QString& name);

    // --- last-used preset (dialog convenience) -----------------------------
    // Persisted in the same file (top-level `last_used:` key) so a freshly
    // opened dialog can preselect and preload what the person used last.
    // Stored as given (trimmed); a dialog guards through patternFor, so a
    // name whose preset was since deleted simply preselects nothing.

    /// The last name passed to setLastUsed, "" when none was ever set.
    [[nodiscard]] Q_INVOKABLE QString lastUsed() const { return m_lastUsed; }

    /// Record @p name (trimmed) as the last-used preset and persist. No-op
    /// on an unchanged or empty trimmed value. Does not emit: the preset
    /// LIST is unchanged, and the value is only read at dialog open.
    Q_INVOKABLE void setLastUsed(const QString& name);

signals:
    /// The preset list changed (load/save/remove). A dialog re-reads
    /// catalog().
    void patternsChanged();

protected:
    /// @p fileName is the file under userConfigDir(); @p banner the comment
    /// block written above the emitted YAML (newline-terminated lines);
    /// @p presetNoun names the records in the invalid-file warning ("rename
    /// presets"). Loads immediately.
    NamedPatternStore(const QString& fileName, QByteArray banner, QString presetNoun,
                      QObject* parent);

private:
    /// Index of the preset named @p name (exact match), or -1.
    [[nodiscard]] int indexOf(const QString& name) const;

    /// Write the whole list to the file (atomic; a failed write logs a
    /// warning and keeps the in-memory state). User order preserved.
    void persist() const;

    QString       m_path;       ///< absolute; may not exist yet
    QByteArray    m_banner;
    QString       m_presetNoun;
    QList<Preset> m_presets;
    QString       m_lastUsed;
};

} // namespace rawform
