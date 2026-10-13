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

// PropertiesApplyController.h
//
// The Properties window's apply and reload sequencer, one instance per
// window (created in PropertiesWindow.qml). It owns everything about the
// selection and the write passes that is not a widget: the captured
// playlist and the open-time rows, the durable { path, subsong } identity
// keys, the touched-path set of the pass in flight, the Remove-tags staging,
// the per-editor failure lists, the two reload tokens, the busy gate and the
// window status. The window keeps the UI: tab switching, the Edit Value
// overlay, the Tools menu, Escape, and the cell editors it closes or commits
// around a pass (reloadStarting(), and commitOpenEditor before apply()).
//
// Track identity is PATH-BASED throughout (plus subsong for the re-pull
// keys): every write targets the file captured at open, and the re-pull
// remaps the open-time keys to CURRENT rows via rowsForKeys. Playlist row
// numbers are positions, not identities; a reorder, sort, or removal between
// opening the window and hitting Apply must never redirect a write.
//
// Lifetime of the captured world: the playlist model and the reloader belong
// to the owning tab, which can close while the window is open. Both are held
// through QPointer, so a dead one reads null: a dead model or reloader skips
// its phase, and a reload pass orphaned by a dying reloader is completed by
// the reloader's destructor token flush (ReloadPort's contract). The file
// writes are path-based and land regardless; when nothing is left to re-pull
// from, the panes adopt the staged values as their new baseline instead
// (see finishApply).
//
//   OK     : apply(true), the window closes on finished(true) (stays open
//            if any file failed to write)
//   Apply  : apply(false), stay open
//
// Sequence: apply -> [meta write] -> applyRgPhase -> [rg write] ->
// reloadPhase (reloadPaths of the TOUCHED paths only, the union of what the
// two editors actually targeted) -> pathsReloaded(our token) -> finishApply
// (re-pull + finished). The token matters: the reloader's refreshed() only
// fires when something changed and can belong to an unrelated conditional
// pass, so waiting on it could hang (written tracks all left the playlist:
// empty pass, no signal) or finish early (a revalidation pass completing
// first). `busy` gates the footer AND the titlebar X so a pass cannot be
// double-fired and the window cannot be destroyed mid-write from the UI (the
// editors' destructors wait out an in-flight pass as the backstop for
// OS-initiated closes).
//
// Tools > Reload info (reloadInfo) force-re-reads the selection's files from
// disk through the owning tab's reloader and re-pulls all three tabs,
// DISCARDING staged edits by design (it is the take-disk-truth action). It
// runs under the same busy gate as apply but with its own token, so the
// apply and reload sequencers can never complete each other through the
// shared pathsReloaded handler.
//
// Remove tags (stageStripAll) is STAGED like everything else: the window
// blanks the panes' editors, this blanks every metadata field row
// (removeAllFields), stages RG clears, and raises stripStaged; apply then
// hands the selection's file paths to the metadata writer as stripAllPaths.
// The flag clears when the pass that carried it finishes, success or not
// (re-invoke Remove tags to retry a failure), and on reloadInfo.
//
// Status: one message and its level (info | warning | error, the log bar's
// vocabulary), always set together through setStatus so the color can never
// lag the text. The window's log bar composes it with the pane hints.
//
// The collaborators arrive through the ApplyPorts bases, so the unit test
// (PropertiesApplyControllerTest) drives every phase on stand-ins that touch
// no file; the models are the real ones.

#pragma once

#include "metadata/ApplyPorts.h"
#include "metadata/MetadataModel.h"
#include "metadata/PropertiesMetadataModel.h"
#include "metadata/ReplayGainRowsModel.h"
#include "playlist/PlaylistModel.h"

#include <QObject>
#include <QPointer>
#include <QQmlEngine> // QML_ELEMENT
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace rawform {

class PropertiesApplyController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    /// True while a write or reload pass runs: the window locks its footer,
    /// titlebar X, panes and Escape on it.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    /// What the log bar shows while busy: "Applying..." or "Reloading...".
    Q_PROPERTY(QString busyText READ busyText NOTIFY busyChanged)
    /// The window status and its level (info | warning | error).
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString statusLevel READ statusLevel NOTIFY statusChanged)
    /// Remove tags is staged: an Apply-enabler in its own right, since
    /// blanking already-blank fields stages nothing in the models.
    Q_PROPERTY(bool stripStaged READ stripStaged NOTIFY stripStagedChanged)
    /// The current row count of the selection (the window title).
    Q_PROPERTY(int rowCount READ rowCount NOTIFY selectionChanged)

public:
    explicit PropertiesApplyController(QObject* parent = nullptr);

    /// The three pane models this feeds on open and on every re-pull.
    Q_INVOKABLE void setModels(rawform::PropertiesMetadataModel* metaModel,
                               rawform::ReplayGainRowsModel* rgModel,
                               rawform::MetadataModel* detailsModel);
    /// The two writers; their applied() signals advance the sequencer.
    Q_INVOKABLE void setEditors(rawform::MetadataWritePort* metaEditor,
                                rawform::ReplayGainWritePort* rgEditor);
    /// The owning tab's reloader, captured once (never a live binding: a tab
    /// switch under the non-modal window must not retarget the reload).
    Q_INVOKABLE void setReloader(rawform::ReloadPort* reloader);
    /// Capture the selection: the playlist and its rows, the identity keys,
    /// and the three models' first fill.
    Q_INVOKABLE void setPlaylist(rawform::PlaylistModel* playlist,
                                 const QVariantList& rows);

    /// Apply / OK. Nothing staged anywhere: finished(closeAfter) at once.
    Q_INVOKABLE void apply(bool closeAfter);
    /// Tools > Reload info. No-op while busy; a dead playlist or reloader
    /// sets a warning status instead.
    Q_INVOKABLE void reloadInfo();
    /// Tools > Remove tags: stage the whole removal for review.
    Q_INVOKABLE void stageStripAll();
    /// Set (or clear, with an empty message) the window status.
    Q_INVOKABLE void setStatus(const QString& message,
                               const QString& level = QStringLiteral("info"));

    [[nodiscard]] bool busy() const { return m_busy; }
    [[nodiscard]] QString busyText() const { return m_busyText; }
    [[nodiscard]] QString statusText() const { return m_statusText; }
    [[nodiscard]] QString statusLevel() const { return m_statusLevel; }
    [[nodiscard]] bool stripStaged() const { return m_stripStaged; }
    [[nodiscard]] int rowCount() const { return static_cast<int>(m_rows.size()); }

    /// Unique file paths of the open-time selection, from the durable
    /// identity keys (subsong siblings share one physical file, so dedupe).
    [[nodiscard]] QStringList selectionPaths() const;

signals:
    void busyChanged();
    void statusChanged();
    void stripStagedChanged();
    void selectionChanged();
    /// An apply pass ended; @p closeRequested when it was an OK and every
    /// file was written (or there was nothing to write).
    void finished(bool closeRequested);
    /// A reload pass is about to start: the window discards any half-open
    /// cell editor, since the data underneath is about to be replaced.
    void reloadStarting();
    /// Files were written and the models re-fed: the window flushes the
    /// playlist cache so the .rwfpl reflects the edit now, not on a debounce.
    void filesWritten();

private:
    void applyRgPhase();
    void reloadPhase();
    void finishApply();
    void finishReload();
    void onPathsReloaded(int token);
    void setBusy(bool busy, const QString& text);
    /// Remap the open-time identity keys to CURRENT rows and re-feed the
    /// three models; false when nothing is left to re-pull from (the tracks,
    /// or the whole tab, are gone).
    bool repullSelection();

    QPointer<PropertiesMetadataModel> m_metaModel;
    QPointer<ReplayGainRowsModel>     m_rgModel;
    QPointer<MetadataModel>           m_detailsModel;
    QPointer<MetadataWritePort>       m_metaEditor;
    QPointer<ReplayGainWritePort>     m_rgEditor;
    QPointer<ReloadPort>              m_reloader;
    QPointer<PlaylistModel>           m_playlist;

    QVariantList m_rows;   // current positions, a convenience mirror of m_keys
    QVariantList m_keys;   // durable { path, subsong } keys, what survives a mutation

    bool    m_busy = false;
    QString m_busyText;
    QString m_statusText;
    QString m_statusLevel = QStringLiteral("info");
    bool    m_stripStaged = false;

    // The pass in flight.
    QStringList  m_touchedPaths;      // unique paths this pass writes
    QVariantList m_pendingRgEdits;
    QStringList  m_pendingStripPaths; // the strip payload, if this pass carries it
    QStringList  m_metaFailed;
    QStringList  m_rgFailed;
    bool         m_anyWrote = false;
    bool         m_closeAfter = false;
    int          m_reloadToken = -1;     // the apply pass's reload
    int          m_infoReloadToken = -1; // the Tools reload
};

}  // namespace rawform
