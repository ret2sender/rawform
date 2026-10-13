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

#include "metadata/PropertiesApplyController.h"

#include <QSet>
#include <QVariantMap>

namespace rawform {

namespace {

/// The rows as the two QList<int>-taking models want them.
QList<int> toIntRows(const QVariantList& rows) {
    QList<int> out;
    out.reserve(rows.size());
    for (const QVariant& v : rows)
        out.push_back(v.toInt());
    return out;
}

}  // namespace

PropertiesApplyController::PropertiesApplyController(QObject* parent) : QObject(parent) {}

void PropertiesApplyController::setModels(PropertiesMetadataModel* metaModel,
                                          ReplayGainRowsModel* rgModel,
                                          MetadataModel* detailsModel) {
    m_metaModel    = metaModel;
    m_rgModel      = rgModel;
    m_detailsModel = detailsModel;
}

void PropertiesApplyController::setEditors(MetadataWritePort* metaEditor,
                                           ReplayGainWritePort* rgEditor) {
    if (m_metaEditor)
        disconnect(m_metaEditor, nullptr, this, nullptr);
    if (m_rgEditor)
        disconnect(m_rgEditor, nullptr, this, nullptr);
    m_metaEditor = metaEditor;
    m_rgEditor   = rgEditor;
    if (m_metaEditor) {
        connect(m_metaEditor, &MetadataWritePort::applied, this,
                [this](int okCount, const QStringList& failedPaths) {
                    m_metaFailed = failedPaths;
                    if (okCount > 0)
                        m_anyWrote = true;
                    applyRgPhase();
                });
    }
    if (m_rgEditor) {
        connect(m_rgEditor, &ReplayGainWritePort::applied, this,
                [this](int okCount, const QStringList& failedPaths) {
                    m_rgFailed = failedPaths;
                    if (okCount > 0)
                        m_anyWrote = true;
                    reloadPhase();
                });
    }
}

void PropertiesApplyController::setReloader(ReloadPort* reloader) {
    if (m_reloader)
        disconnect(m_reloader, nullptr, this, nullptr);
    m_reloader = reloader;
    // If the reloader dies while a pass waits, its DESTRUCTOR flushes the
    // pending tokens through this same handler (the port's contract), so
    // both sequencers always finish; the connection itself is cut only
    // afterwards, by ~QObject.
    if (m_reloader)
        connect(m_reloader, &ReloadPort::pathsReloaded, this,
                &PropertiesApplyController::onPathsReloaded);
}

void PropertiesApplyController::setPlaylist(PlaylistModel* playlist,
                                            const QVariantList& rows) {
    m_playlist = playlist;
    m_rows     = rows;
    // The durable identity snapshot: everything after this moment addresses
    // these tracks by (path, subsong), never by playlist position.
    m_keys = playlist != nullptr ? playlist->trackKeys(rows) : QVariantList{};
    // The RG model ingests through replayGainRows C++-to-C++: one reset.
    // The two read-only tabs copy the TrackData out, so all three stay valid
    // if the playlist selection changes while the window is open.
    if (playlist != nullptr) {
        if (m_rgModel)
            m_rgModel->setSelection(playlist, rows);
        if (m_metaModel)
            m_metaModel->setSelection(playlist, toIntRows(rows));
        if (m_detailsModel)
            m_detailsModel->setSelection(playlist, toIntRows(rows));
    }
    emit selectionChanged();
}

QStringList PropertiesApplyController::selectionPaths() const {
    QSet<QString> seen;
    QStringList   paths;
    for (const QVariant& key : m_keys) {
        const QString p = key.toMap().value(QStringLiteral("path")).toString();
        if (!p.isEmpty() && !seen.contains(p)) {
            seen.insert(p);
            paths.push_back(p);
        }
    }
    return paths;
}

void PropertiesApplyController::setStatus(const QString& message, const QString& level) {
    m_statusText  = message;
    m_statusLevel = level;
    emit statusChanged();
}

void PropertiesApplyController::setBusy(bool busy, const QString& text) {
    m_busy     = busy;
    m_busyText = text;
    emit busyChanged();
}

void PropertiesApplyController::stageStripAll() {
    if (m_metaModel)
        m_metaModel->removeAllFields();
    if (m_rgModel)
        m_rgModel->clearAllReplayGain();
    if (!m_stripStaged) {
        m_stripStaged = true;
        emit stripStagedChanged();
    }
    setStatus(QStringLiteral("Remove tags staged; Apply / OK writes it."),
              QStringLiteral("warning"));
}

// --- the apply sequencer ---------------------------------------------------

void PropertiesApplyController::apply(bool closeAfter) {
    if (m_busy)
        return;
    const QVariantList metaEdits =
        m_metaModel ? m_metaModel->collectEdits() : QVariantList{};
    const QVariantList rgEdits = m_rgModel ? m_rgModel->collectEdits() : QVariantList{};
    // The staged Remove tags resolves to the selection's unique paths HERE,
    // at commit time, so the strip and the reload see the same set.
    const QStringList stripPaths = m_stripStaged ? selectionPaths() : QStringList{};
    if (metaEdits.isEmpty() && rgEdits.isEmpty() && stripPaths.isEmpty()) {
        // Nothing staged anywhere.
        if (closeAfter)
            emit finished(true);
        return;
    }
    // The union of file paths this pass will actually write: both editors'
    // edit maps carry the target path, and every strip path is written by
    // definition, so the reload phase can re-read exactly what changed
    // instead of force-parsing the whole selection.
    QSet<QString> touched;
    for (const QVariant& e : metaEdits)
        touched.insert(e.toMap().value(QStringLiteral("path")).toString());
    for (const QVariant& e : rgEdits)
        touched.insert(e.toMap().value(QStringLiteral("path")).toString());
    for (const QString& p : stripPaths)
        touched.insert(p);
    m_touchedPaths = QStringList(touched.cbegin(), touched.cend());

    m_closeAfter = closeAfter;
    setStatus(QString());
    setBusy(true, QStringLiteral("Applying…"));
    m_metaFailed.clear();
    m_rgFailed.clear();
    m_anyWrote          = false;
    m_pendingRgEdits    = rgEdits;
    m_pendingStripPaths = stripPaths;
    if ((!metaEdits.isEmpty() || !stripPaths.isEmpty()) && m_metaEditor)
        m_metaEditor->apply(metaEdits, stripPaths); // -> applied -> applyRgPhase
    else
        applyRgPhase();
}

void PropertiesApplyController::applyRgPhase() {
    if (!m_pendingRgEdits.isEmpty() && m_rgEditor)
        m_rgEditor->apply(m_pendingRgEdits); // -> applied -> reloadPhase
    else
        reloadPhase();
}

void PropertiesApplyController::reloadPhase() {
    // Re-read only the files this pass wrote. reloadPaths hands back a
    // token and signals pathsReloaded(token) when exactly that pass is done
    // (an empty resolution still runs as an empty pass), so the sequencer
    // can neither hang on a pass that never runs nor finish early on an
    // unrelated pass's refreshed(). The null test matters: the owning tab
    // (and its reloader) may have died since open.
    if (m_anyWrote && m_reloader)
        m_reloadToken = m_reloader->reloadPaths(m_touchedPaths);
    else
        finishApply();
}

bool PropertiesApplyController::repullSelection() {
    // Remap the open-time identity keys to CURRENT rows: positions may have
    // shifted since open, and the model itself may be dead (the owning tab
    // closed, which reads as null here). Tracks that left the playlist drop
    // out of the mapping.
    const QVariantList cur =
        m_playlist ? m_playlist->rowsForKeys(m_keys) : QVariantList{};
    if (cur.isEmpty())
        return false;
    m_rows = cur;
    m_keys = m_playlist->trackKeys(cur);
    if (m_rgModel)
        m_rgModel->setSelection(m_playlist, cur);
    if (m_metaModel)
        m_metaModel->setSelection(m_playlist, toIntRows(cur));
    if (m_detailsModel)
        m_detailsModel->setSelection(m_playlist, toIntRows(cur));
    emit selectionChanged();
    return true;
}

void PropertiesApplyController::finishApply() {
    m_reloadToken = -1;
    // Re-pull fresh on-disk values into all three tabs ONLY when something
    // actually landed. setSelection clears the metadata staging, so on a
    // total write failure we deliberately skip it: the staged edits survive
    // and the user can retry. (On partial success the written files refresh
    // and the failed ones keep their prior values; the status reports the
    // count.)
    if (m_anyWrote) {
        if (!repullSelection()) {
            // Nothing left to re-pull from (the tracks, or the whole tab,
            // are gone), but the writes landed on disk regardless. Fold the
            // staged values into the panes' baselines so the dirty markers
            // clear and the window shows what is on disk now; without this,
            // Apply looks like it did nothing (staging dots persist) even
            // though the files were written. Failed paths revert instead,
            // matching what the re-pull path shows for a failed file.
            if (m_metaModel)
                m_metaModel->adoptEdits(m_metaFailed);
            if (m_rgModel)
                m_rgModel->adoptEdits(m_rgFailed);
        }
        // A write marks the playlist tab dirty (dataChanged -> markDirty ->
        // debounced autosave); the window forces that write now.
        emit filesWritten();
    }

    const int failed = static_cast<int>(m_metaFailed.size() + m_rgFailed.size());
    if (failed > 0)
        setStatus(QStringLiteral("%1 file(s) could not be written.").arg(failed),
                  QStringLiteral("error"));
    // The strip intent is consumed by the pass that carried it, success or
    // not; a failure is reported above and Remove tags can be re-invoked.
    // (The METADATA staging's survival on total failure is a separate
    // mechanism and unchanged: setSelection was skipped above.)
    if (!m_pendingStripPaths.isEmpty()) {
        m_pendingStripPaths.clear();
        m_stripStaged = false;
        emit stripStagedChanged();
    }
    m_anyWrote = false;
    const bool closeRequested = m_closeAfter && failed == 0;
    m_closeAfter = false;
    setBusy(false, QString());
    emit finished(closeRequested);
}

// --- Tools > Reload info ---------------------------------------------------

void PropertiesApplyController::reloadInfo() {
    if (m_busy)
        return;
    if (!m_reloader || !m_playlist) {
        setStatus(QStringLiteral("Owning playlist is gone; nothing to reload."),
                  QStringLiteral("warning"));
        return;
    }
    // Unique file paths from the durable identity keys (the reloader is
    // path-directed, so dedupe).
    const QStringList paths = selectionPaths();
    if (paths.isEmpty())
        return;
    // The window discards any half-open cell editor first, like a tab
    // switch does; the data underneath is about to be replaced. The staged
    // Remove tags is discarded with the rest of the staging: reload is the
    // take-disk-truth action.
    emit reloadStarting();
    if (m_stripStaged) {
        m_stripStaged = false;
        emit stripStagedChanged();
    }
    setStatus(QString());
    setBusy(true, QStringLiteral("Reloading…"));
    m_infoReloadToken = m_reloader->reloadPaths(paths);
}

void PropertiesApplyController::finishReload() {
    m_infoReloadToken = -1;
    // Same remap as finishApply: positions may have shifted since open, and
    // the model may have died while the pass ran. Nothing left to re-pull
    // from: the panes keep showing the open-time snapshot, staged edits
    // included.
    if (!repullSelection())
        setStatus(QStringLiteral("Tracks are no longer in the playlist."),
                  QStringLiteral("warning"));
    setBusy(false, QString());
}

void PropertiesApplyController::onPathsReloaded(int token) {
    // Only OUR pass completes a sequencer, and each token routes to its own
    // finisher (apply vs Tools reload; the tokens come from the same reloader
    // counter, so they can never collide). refreshed() is not used here on
    // purpose: it fires for any pass that changed something and stays silent
    // for one that did not (see the banner).
    if (m_busy && token == m_reloadToken)
        finishApply();
    else if (m_busy && token == m_infoReloadToken)
        finishReload();
}

}  // namespace rawform
