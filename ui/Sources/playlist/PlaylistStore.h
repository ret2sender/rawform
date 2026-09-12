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

#include "playlist/PlaylistFormat.h"

#include <QFutureWatcher>
#include <QObject>
#include <QQmlEngine>  // QML_ELEMENT / QML_UNCREATABLE
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace rawform {

class PlaylistModel;

/**
 * @brief Saves a playlist to a .rwfpl / .m3u / .m3u8 file off the GUI thread,
 *        and persists the column-layout preset.
 *
 * The seam analog of TrackScanner: it owns no policy about which playlist is
 * active beyond the model handed to it, and file I/O runs on the global thread
 * pool so a big save never blocks the UI.
 *
 * SAVE pulls the track list from the model (a snapshot copy, safe to hand a
 * worker) and takes the column order + widths as arguments, because widths live
 * on the QML TableView (explicitColumnWidth), not the model. The QML save action
 * passes playlistView.currentColumnOrder() + currentColumnWidths().
 *
 * FORMAT is resolved here, not in QML (see PlaylistFormat.h for the precedence
 * rule): the chosen path's suffix decides when it is one we recognize, otherwise
 * the dialog's filter hint does and the extension is appended. .rwfpl goes to
 * writePlaylist (the full document); .m3u/.m3u8 go to writeM3u, which carries
 * tracks only. The saved() message says so for an M3U target, since a column
 * layout that silently failed to travel is exactly the kind of loss that should
 * be stated once rather than discovered later.
 *
 * The column-layout PRESET (saveColumnPreset / loadColumnPreset) is a
 * layout-only .rwftp under userConfigDir(), loaded at startup so a user's
 * chosen columns/order/widths survive restarts. It reuses the .rwfpl writer with
 * an empty track list.
 *
 * One operation at a time: a save requested while busy is rejected with a failure
 * signal rather than queued.
 */
class PlaylistStore : public QObject {
    Q_OBJECT

    // Registered so QML can NAME the type (typed property declarations, and
    // qmllint member checking on them); never instantiated from QML. The one
    // instance is created in main.cpp and exposed as a context property.
    QML_ELEMENT
    QML_UNCREATABLE("PlaylistStore is created in C++ and exposed as a context property")

    /// True while a write is in flight. Bound by QML to the status line
    /// (alongside trackScanner.scanning / activeReloader.busy).
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)

    /// The playlist this store saves. With tabs, QML binds it to the active
    /// tab's model (playlistTabs.activeModel) so "Save playlist..." always targets
    /// the visible playlist. save() snapshots from whatever model is set here.
    Q_PROPERTY(PlaylistModel* model READ model WRITE setModel NOTIFY modelChanged)

public:
    explicit PlaylistStore(PlaylistModel* model, QObject* parent = nullptr);
    ~PlaylistStore() override = default;

    [[nodiscard]] bool isBusy() const { return m_busy; }

    [[nodiscard]] PlaylistModel* model() const { return m_model; }
    void setModel(PlaylistModel* m) {
        if (m_model == m)
            return;
        m_model = m;
        emit modelChanged();
    }

    /// Write the current playlist to @p fileUrl. @p fieldIds + @p widths are the
    /// view's visual-order column order and widths (parallel); the track list is
    /// taken from the model. Emits saved() when done.
    ///
    /// @p formatHint is the save dialog's SELECTED NAME FILTER as a stable id
    /// ("m3u", "m3u8", "rwfpl"). It is only consulted when @p fileUrl carries no
    /// recognized suffix, in which case the matching extension is appended to
    /// the path actually written. An empty or unknown hint means M3U, the chosen
    /// default. The layout arguments are still taken for every format; they are
    /// simply dropped by the M3U writer, so the caller never has to branch.
    ///
    /// @p currentRow and @p scrollRow are the CURR focus row and the
    /// SCRL scroll position (first visible row) to embed, supplied by the
    /// caller because this store owns neither the selection nor the view.
    /// Same drop-by-the-M3U-writer contract as the layout arguments. -1 (the
    /// default) writes the chunks as "none", so a Save As that leaves them at
    /// the default produces a .rwfpl that restores neither its position nor
    /// its focus row; the QML save action passes both.
    Q_INVOKABLE void save(const QUrl& fileUrl,
                          const QStringList& fieldIds,
                          const QVariantList& widths,
                          const QString& formatHint = {},
                          int currentRow = -1,
                          int scrollRow = -1);

    /// Save the default column arrangement (order + widths, no tracks) to the
    /// user config preset (columns.rwftp under userConfigDir()), loaded on
    /// startup so the chosen columns/order/widths survive restarts.
    /// Synchronous; returns false on write failure. Reuses the .rwfpl writer
    /// with an empty track list (the preset is just a layout-only playlist
    /// file).
    Q_INVOKABLE bool saveColumnPreset(const QStringList& fieldIds,
                                      const QVariantList& widths);

    /// Load the default column arrangement preset. Returns a map
    /// { fieldIds: QStringList, widths: QVariantList } in visual order, or an
    /// empty map when no preset exists or can't be read (the caller then keeps
    /// the schema defaults). Synchronous; safe to call at startup. Called from
    /// main.cpp only; no QML caller, so not invokable.
    QVariantMap loadColumnPreset();

signals:
    /// A save finished. @p ok false carries a human-readable @p message.
    void saved(bool ok, QString message);

    void busyChanged();
    void modelChanged();

private slots:
    void onSaveFinished();

private:
    void setBusy(bool on);

    PlaylistModel*                    m_model;
    // The write future yields (ok, message); see the result type in the .cpp.
    struct WriteOutcome {
        bool    ok = false;
        QString message;
    };
    QFutureWatcher<WriteOutcome>      m_saveWatcher;
    QString                           m_savePath; ///< for the saved() message
    /// The format the in-flight save resolved to, kept so the completion
    /// message can name the M3U lossiness without re-deriving it from the path.
    PlaylistFormat                    m_saveFormat = PlaylistFormat::Rwfpl;
    bool                              m_busy = false;
};

} // namespace rawform
