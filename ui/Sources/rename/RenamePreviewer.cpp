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

// RenamePreviewer.cpp
//
// See the header for the contract and status vocabulary. Two passes: resolve
// every key to a current row (own resolution, preserving correspondence for
// missing tracks), compute per-row names and statuses; then a cross-row pass
// upgrades same-target collisions to conflict_batch on BOTH rows (the first
// occurrence is as guilty as the second; flagging only the later one would
// point the user at the wrong row half the time).

#include "rename/RenamePreviewer.h"

#include "media/TrackData.h"
#include "rename/RenameSanitizer.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QLatin1Char>
#include <QString>
#include <QVariantMap>

namespace rawform {
namespace {

/// Hash key for (path, subsong) identity; '\n' cannot occur in either half.
QString identityKey(const QString& path, int subsong) {
    return path + QLatin1Char('\n') + QString::number(subsong);
}

QVariantMap makeRow(const QString& path, const QString& oldName,
                    const QString& newName, const char* status,
                    const QString& note = {}) {
    QVariantMap m;
    m.insert(QStringLiteral("path"), path);
    m.insert(QStringLiteral("oldName"), oldName);
    m.insert(QStringLiteral("newName"), newName);
    m.insert(QStringLiteral("status"), QString::fromLatin1(status));
    m.insert(QStringLiteral("note"), note);
    return m;
}

} // namespace

RenamePreviewer::RenamePreviewer(QObject* parent) : QObject(parent) {}

QVariantList RenamePreviewer::preview(rawform::PlaylistModel* model,
                                      const QVariantList& keys,
                                      const QString& pattern) const {
    QVariantList out;
    if (model == nullptr || keys.isEmpty() || pattern.isEmpty())
        return out;

    // Index the model once: identity -> current rows holding it, ascending.
    // Consumed greedily below so N duplicate keys map to N duplicate rows
    // (rowsForKeys semantics), and a key with nothing left to consume is
    // "gone". This full-scan-once beats per-key scans and, unlike
    // rowsForKeys, keeps a result slot for every key.
    QHash<QString, QList<int>> rowsByIdentity;
    const int rows = model->rowCount();
    for (int row = 0; row < rows; ++row) {
        const TrackData* t = model->trackAt(row);
        if (t != nullptr)
            rowsByIdentity[identityKey(t->filePath, t->subsongIndex)].push_back(row);
    }

    QSet<QString> seenSources;            // identities already previewed
    QHash<QString, QList<int>> byTarget;  // target path -> out indices

    for (const QVariant& kv : keys) {
        const QVariantMap key = kv.toMap();
        const QString path = key.value(QStringLiteral("path")).toString();
        const int subsong = key.value(QStringLiteral("subsong")).toInt();
        const QString id = identityKey(path, subsong);
        const QString oldName = QFileInfo(path).fileName();

        // Resolve to a current row (greedy consume) or report the track gone.
        QList<int>& avail = rowsByIdentity[id];
        if (avail.isEmpty()) {
            out.append(makeRow(path, oldName, {}, "gone",
                               QStringLiteral("no longer in the playlist")));
            continue;
        }
        const int row = avail.takeFirst();
        const TrackData* t = model->trackAt(row);
        if (t == nullptr) {
            out.append(makeRow(path, oldName, {}, "gone",
                               QStringLiteral("no longer in the playlist")));
            continue;
        }

        // Subsong siblings share one physical file.
        if (t->subsongIndex != 0) {
            out.append(makeRow(path, oldName, {}, "subsong",
                               QStringLiteral("part of a multi-track file")));
            continue;
        }

        // Duplicate playlist entries of one physical file: first wins, the
        // rest gray out (one file, one rename).
        if (seenSources.contains(id)) {
            out.append(makeRow(path, oldName, {}, "duplicate_entry",
                               QStringLiteral("same file selected above")));
            continue;
        }
        seenSources.insert(id);

        // The naming pipeline end to end, then the last-gate verdict.
        const QString stem = buildRenameStem(*t, pattern);
        const QString newName = composeRenameFileName(stem, t->fileName);
        const QString problem = renameFileNameProblem(newName);
        if (!problem.isEmpty()) {
            out.append(makeRow(path, oldName, newName, "invalid", problem));
            continue;
        }

        if (newName == t->fileName) {
            out.append(makeRow(path, oldName, newName, "identity",
                               QStringLiteral("already named this")));
            continue;
        }

        const QString target =
            QFileInfo(path).absolutePath() + QLatin1Char('/') + newName;

        // Disk check, advisory (FileRenamer's no-clobber rename is the
        // enforcement). The same-file exception keeps a case-only rename
        // legal on a case-insensitive filesystem: exists() answers true for
        // the file's own other-cased name there, and QFileInfo equality
        // recognizes it as this very file.
        if (QFile::exists(target)
            && !(QFileInfo(target) == QFileInfo(path))) {
            out.append(makeRow(path, oldName, newName, "conflict_disk",
                               QStringLiteral("a file with this name exists")));
            continue;
        }

        byTarget[target].push_back(static_cast<int>(out.size()));
        out.append(makeRow(path, oldName, newName, "ok"));
    }

    // Cross-row pass: any target produced by more than one row flags ALL its
    // rows (see the file comment for why not just the later ones).
    for (auto it = byTarget.constBegin(); it != byTarget.constEnd(); ++it) {
        if (it.value().size() < 2)
            continue;
        for (const int idx : it.value()) {
            QVariantMap m = out.at(idx).toMap();
            m.insert(QStringLiteral("status"), QStringLiteral("conflict_batch"));
            m.insert(QStringLiteral("note"),
                     QStringLiteral("two selected tracks produce this name"));
            out[idx] = m;
        }
    }

    return out;
}

}  // namespace rawform
