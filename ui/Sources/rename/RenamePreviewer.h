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

#pragma once

#include "playlist/PlaylistModel.h" // complete type: preview() takes the
                                    // pointer (same marshaling rule as
                                    // MetadataModel::setSelection; a typed
                                    // QObject pointer, not a var round-trip,
                                    // so a destroyed model arrives as null
                                    // instead of a dangling wrapper)

#include <QObject>
#include <QQmlEngine>  // QML_ELEMENT
#include <QString>
#include <QVariantList>

namespace rawform {

/**
 * @brief The rename dialog's preview engine: evaluates a %token% pattern
 *        over a snapshot of track identities and classifies every row per the
 *        skip rules, so the dialog can render old-vs-new with flags and gate
 *        Apply. Pure computation plus read-only filesystem existence checks;
 *        it never renames anything (FileRenamer does) and never mutates the
 *        model.
 *
 * One entry point: preview(model, keys, pattern), where keys are the dialog's
 * open-time snapshot from PlaylistModel::trackKeys ({ path, subsong } maps).
 * Resolution back to rows is done here, NOT via rowsForKeys: that helper
 * compacts missing tracks away, losing the key<->result correspondence the
 * preview table needs (every snapshotted row must render SOMETHING, including
 * "left the playlist"). Duplicate keys consume duplicate playlist rows
 * greedily in ascending order, same semantics as rowsForKeys.
 *
 * Each result is a map: { path, oldName, newName, status, note }.
 * status values and their meaning to the dialog:
 *  - "ok"              rename this file (contributes a job)
 *  - "identity"        new name equals current name; grayed, skipped
 *  - "subsong"         subsongIndex != 0; grayed, skipped (siblings share
 *                      one physical file, per-subsong names are ambiguous)
 *  - "duplicate_entry" the same physical file appeared earlier in the
 *                      selection (duplicate playlist entries); grayed, skipped
 *  - "gone"            the track left the playlist since the dialog opened;
 *                      grayed, skipped
 *  - "invalid"         the pattern yields no usable name for this track
 *                      (empty/overlong; note carries renameFileNameProblem's
 *                      text); BLOCKS Apply
 *  - "conflict_batch"  two rows in this batch produce the same target name;
 *                      both flagged; BLOCKS Apply
 *  - "conflict_disk"   the target exists on disk and is not this same file
 *                      (the same-file check is what keeps case-only renames
 *                      legal on macOS); BLOCKS Apply
 *
 * The blocking rule: grayed statuses degrade gracefully, blocking ones
 * disable Apply/OK until the pattern changes, no auto-suffixing. The disk
 * check is advisory (the world can change between preview and Apply);
 * FileRenamer's no-clobber QFile::rename remains the enforcement.
 */
class RenamePreviewer : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit RenamePreviewer(QObject* parent = nullptr);
    ~RenamePreviewer() override = default;

    /// See the class comment. A null model, empty keys, or empty pattern
    /// yields an empty list (the dialog renders its own empty states).
    [[nodiscard]] Q_INVOKABLE QVariantList preview(rawform::PlaylistModel* model,
                                                   const QVariantList& keys,
                                                   const QString& pattern) const;
};

}  // namespace rawform
