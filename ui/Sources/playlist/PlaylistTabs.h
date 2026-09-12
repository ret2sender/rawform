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

#include "columns/ColumnSchema.h"
#include "media/TrackData.h"

#include <QAbstractListModel>
#include <QFuture>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QQmlEngine> // QML_ELEMENT
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class QItemSelectionModel;
class QTimer;

namespace rawform {

class PlaylistModel;
class MetadataReloader;
class CustomColumnRegistry;
class TrackScanner;
struct PlaylistDocument; // defined in PlaylistFile.h; returned by snapshotTab()

/**
 * @brief The playlist tab session manager: the C++ owner of every open tab and
 *        the single seam that decides which tab a scan, load, or selection
 *        change acts on.
 *
 * Where the models multiply. The scanner is model-free, the custom-column
 * registry is app-global, and PlaylistView swaps the model underneath it; this
 * class owns the per-tab models those pieces were written toward.
 *
 * OWNERSHIP. Each tab owns, as a private subtree:
 *   - PlaylistModel:       the rows + the column order (m_columns);
 *   - QItemSelectionModel: that tab's selection (parented to its model);
 *   - MetadataReloader:    that tab's freshness pass (parented to its model).
 *                          One reloader PER tab, never re-pointed: its async
 *                          QFutureWatcher/pending state stays bound to one model
 *                          for life, so a pass in flight can't apply onto the
 *                          wrong playlist after a tab switch.
 *   - an autosave QTimer:  debounces writes of this tab's live .rwfpl.
 * Column WIDTHS are the one piece that can't live per-model: there is a single
 * HorizontalHeaderView, so the active tab's widths live there and inactive tabs
 * park theirs in Tab::widths, which QML keeps current via stashActiveWidths()
 * (on resize and just before a switch-away).
 *
 * ACTIVE POINTERS. activeModel()/activeSelection()/activeReloader() expose the
 * current tab's objects; QML binds to them and they change (activeChanged) on
 * every switch. activeSelectionChanged() fires for a selection change OR a
 * switch, so the metadata pane re-aggregates uniformly.
 *
 * SCANNER SEAM. setScanner() wires the scanner here.
 * scanIntoActive() captures the active model+selection into a FIFO at call time;
 * scanStarted pops it, so a scan launched while tab A was active lands in A even
 * if the user switches to B mid-scan. Closed-tab targets are QPointer-guarded.
 *
 * LIVE PLAYLISTS. Every tab is backed by an auto-saved
 * userConfigDir()/live_playlist/<uuid>.rwfpl, a working file until the tab is
 * closed (file deleted) or saved elsewhere. Opening a .rwfpl (openInNewTab)
 * creates a fresh live copy; the original is untouched. restoreSession() re-opens
 * the live files on startup, or seeds one empty tab.
 *
 * Registered via QML_ELEMENT into com.rawform.app; a QML context property.
 */
class PlaylistTabs : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex WRITE setCurrentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(PlaylistModel* activeModel READ activeModel NOTIFY activeChanged)
    Q_PROPERTY(QItemSelectionModel* activeSelection READ activeSelection NOTIFY activeChanged)
    Q_PROPERTY(MetadataReloader* activeReloader READ activeReloader NOTIFY activeChanged)

public:
    /// Roles for the tab-bar list delegate.
    enum Roles {
        TitleRole = Qt::UserRole + 1, ///< QString: the tab label.
        DirtyRole,                    ///< bool: has unsaved-vs-autosave changes (served; no delegate binds it).
        ScanningRole,                 ///< bool: a scan is inserting into this tab right now.
        ScanProgressRole,             ///< double 0..1: that scan's progress (files processed / files total).
    };

    explicit PlaylistTabs(QObject* parent = nullptr);
    ~PlaylistTabs() override;

    // --- One-time dependency injection (call BEFORE creating any tab) -----

    /// The schema each new tab's model is seeded with (copied per model). Pass
    /// the ColumnSchema::load() result main.cpp holds.
    void setSchemaPrototype(ColumnSchema schema);

    /// The app-global custom-column registry, injected READ-ONLY into every
    /// tab's model (one shared instance, as the model header requires).
    void setCustomColumns(const CustomColumnRegistry* registry);

    /// Wire the (single, model-free) scanner. After this, route ALL playlist
    /// ingestion through scanIntoActive() rather than scanner->scan() directly,
    /// so results land in the tab that was active when the scan was launched.
    void setScanner(TrackScanner* scanner);

    /// The column arrangement a brand-new (empty) tab adopts: the user's saved
    /// .rwftp "Save Column Layout" preset. Opened/restored tabs ignore this (they
    /// carry their own saved layout). Empty/unset -> new tabs use schema defaults.
    /// Q_INVOKABLE so "Save Column Layout" can refresh it mid-session (otherwise
    /// the change would only take effect after a restart re-reads the preset).
    Q_INVOKABLE void setDefaultLayout(QStringList fieldIds, QVariantList widths);

    // --- QAbstractListModel ----------------------------------------------
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // --- Active tab ------------------------------------------------------
    [[nodiscard]] int count() const { return static_cast<int>(m_tabs.size()); }
    [[nodiscard]] int currentIndex() const { return m_current; }
    void setCurrentIndex(int index);

    [[nodiscard]] PlaylistModel*       activeModel() const;
    [[nodiscard]] QItemSelectionModel* activeSelection() const;
    [[nodiscard]] MetadataReloader*    activeReloader() const;

    // --- Lifecycle (QML) -------------------------------------------------

    /// Create a tab and make it active. @p title empty -> a unique generic
    /// "New Playlist" / "New Playlist N". Returns the new tab's index. The tab
    /// gets a fresh live .rwfpl path immediately (empty until first autosave).
    Q_INVOKABLE int newPlaylist(const QString& title = {});

    /// Close @p index: stop its autosave, delete its live .rwfpl, destroy its
    /// model subtree. Never leaves zero tabs; closing the last one spawns a
    /// fresh empty "New Playlist".
    Q_INVOKABLE void closeTab(int index);

    /// Reorder tabs: move the tab at @p from so it lands at @p to. A
    /// begin/endMoveRows MOVE (keeps currentIndex pinned to the same tab).
    Q_INVOKABLE bool moveTab(int from, int to);

    /// Open the playlist at @p fileUrl in a NEW tab. Title = the file's base
    /// name; async either way. Routes by suffix:
    ///   .m3u / .m3u8  -> a new tab (default column preset), contents expanded
    ///                    through the scanner, since an M3U is file references
    ///                    only and readPlaylist would reject it on magic;
    ///   anything else -> read as a .rwfpl into a fresh live copy (the source
    ///                    file is not adopted). An unrecognized suffix is
    ///                    attempted as .rwfpl on purpose: format identity
    ///                    travels with the MAGIC, not the extension.
    Q_INVOKABLE void openInNewTab(const QUrl& fileUrl);

    /// Concatenate the tracks of the .rwfpl at @p fileUrl into the ACTIVE tab,
    /// inserting at row @p atRow (clamped/appended out of range). The body
    /// drop path for .rwfpl files. Async; captures the active tab at call time.
    /// Internal to the drop path (dropUrlsIntoActive); no QML caller.
    void concatFileIntoActive(const QUrl& fileUrl, int atRow);

    /// Ingest @p urls into the ACTIVE tab via the scanner, inserting at @p at
    /// (current rowCount() / -1 to append). Captures the active tab as the scan
    /// target NOW, so a later tab switch can't misfile the results.
    Q_INVOKABLE void scanIntoActive(const QList<QUrl>& urls, int at = -1);

    // --- Drop routing (QML hands the raw drop payload here in ONE call) ---
    //
    // These exist for performance, not convenience. QQuickDropEvent's
    // `urls` is a live getter that re-decodes the entire text/uri-list mime
    // payload on every access, and a QML sequence value is a REFERENCE back to
    // that getter (urls[i] re-invokes it), so any per-element JS access over a
    // mass-file drop is O(N^2) on the GUI thread. Passing drop.urls straight
    // into an invokable converts it exactly once; the .rwfpl/other split then
    // runs here over a plain QList.

    /// The playlist-body drop: each .rwfpl URL is concatenated into the ACTIVE
    /// playlist at row @p at (concatFileIntoActive); everything else (audio,
    /// folders, .m3u) goes to the scanner in one scanIntoActive(rest, at).
    /// Returns the total URL count, for the caller's status-log line.
    Q_INVOKABLE int dropUrlsIntoActive(const QList<QUrl>& urls, int at);

    /// The tab-strip drop: each .rwfpl URL opens in its own new tab
    /// (openInNewTab); everything else feeds ONE tab, created fresh and
    /// smart-named via suggestTabName() unless @p proactiveTabOpen says the
    /// strip's 2 s hover already popped the (empty, active) destination tab.
    /// Returns the total URL count, for the caller's status-log line.
    Q_INVOKABLE int dropUrlsIntoNewTab(const QList<QUrl>& urls, bool proactiveTabOpen);

    /// Suggest a tab name for a drop/Add-Folder, by this precedence:
    ///   single .m3u/.m3u8        -> that file's base name
    ///   single directory         -> first top-level .m3u/.m3u8 inside, else dir name
    ///   anything else            -> "" (caller falls back to a generic name)
    Q_INVOKABLE [[nodiscard]] QString suggestTabName(const QList<QUrl>& urls) const;

    // --- Active-tab column layout (widths live in the QML header) ---------

    /// Park the active tab's current column widths (visual order). QML calls
    /// this on a header resize and just before a switch, so Tab::widths is the
    /// authority for autosave and for re-seeding the header on re-activation.
    Q_INVOKABLE void stashActiveWidths(const QVariantList& widths);

    /// The active tab's parked widths (visual order), for QML to seed the header
    /// after a switch. Empty if there is no active tab.
    Q_INVOKABLE [[nodiscard]] QVariantList activeWidths() const;

    /// Park / read the active tab's scroll position as its
    /// first visible row. The stash is called from the SAME
    /// onActiveAboutToChange hook as the selection detach, while the
    /// active pointers still read the OUTGOING tab (the stashActiveWidths
    /// convention), and by MainWindow.onClosing for the active tab at quit;
    /// the read happens after a switch, against the INCOMING tab. Note: a
    /// changed value dirties the tab so autosave / the shutdown flush persist
    /// it to the SCRL chunk; an unchanged re-assert never arms autosave.
    Q_INVOKABLE void stashActiveScrollRow(int row);
    Q_INVOKABLE [[nodiscard]] int activeScrollRow() const;

    /// Rename a tab (the inline double-click edit in the tab bar). An empty/
    /// whitespace title is ignored. Persists via the tab's autosave.
    Q_INVOKABLE void renameTab(int index, const QString& title);

    /// The tab index owning @p model, -1 when no open tab does (null model, or
    /// the owning tab was closed). Lets QML resolve which tab to switch
    /// to for the Ctrl+P playing-track reveal, from
    /// audioController.playingModel. Pointer identity only; this object is the
    /// one holder of all models, so a live model is found iff its tab is open.
    Q_INVOKABLE [[nodiscard]] int indexOfModel(PlaylistModel* model) const;
    // --- File operations (File Operations > Rename To) ----------------------

    /// Patch the in-memory identity of every row whose filePath is a KEY of
    /// @p renames (old absolute path -> new absolute path), across EVERY open
    /// tab, after FileRenamer moved the files on disk. Lives here because this
    /// object is the one holder of all models: a file renamed from one tab may
    /// be listed in others, and patching only the invoking tab would leave
    /// those rows pointing at a path that no longer exists.
    ///
    /// Each hit is a copy-patch of filePath + fileName pushed through
    /// PlaylistModel::refreshTrack: an in-place dataChanged, never a reset, so
    /// selection, columns, the metadata pane and the art frame all update
    /// undisturbed; the dataChanged -> markDirty connection then
    /// persists the new paths via each tab's normal autosave. folderName,
    /// mtime and size are untouched by a same-directory rename, so no disk
    /// re-read is needed (and the reloader's stat-compare will agree).
    /// Duplicate rows referencing one file are all patched. Identity and
    /// empty-value entries are skipped.
    Q_INVOKABLE void applyPathRenames(const QVariantMap& renames);

    // --- Startup ---------------------------------------------------------

    /// Restore the previous session: open every userConfigDir()/live_playlist/
    /// *.rwfpl as a tab (titles from their saved file names). If none exist,
    /// seed one empty "New Playlist". Call once, after the dependencies are set.
    void restoreSession();

    // --- Shutdown --------------------------------------------------------

    /// Synchronously write every dirty tab's live file, NOW, on the calling
    /// thread. The shutdown flush.
    ///
    /// Autosave is a kAutosaveDebounceMs (1500 ms) single-shot debounce whose
    /// write is fire-and-forget on a worker (writeTabNow). Nothing else drains
    /// that at close: the destructor only deletes tabs, and a fire-and-forget
    /// worker write would not survive process exit anyway (the scenario this
    /// prevents: a column resize, or any edit, made inside the debounce window
    /// of quitting is silently lost). QML calls this from
    /// ApplicationWindow.onClosing, AFTER pushing the
    /// live header widths into the active tab via stashActiveWidths (the header
    /// is QML-owned; C++ cannot read it), so the on-screen widths are captured.
    /// The destructor calls it too, as a belt-and-suspenders for quit paths that
    /// bypass the window close. Idempotent: a second call writes nothing because
    /// the first cleared every dirty flag.
    Q_INVOKABLE void flushPendingWrites();

    /// Liveness probe for QML. The non-modal Properties windows hold VALUE
    /// captures of a tab's model and reloader, and the tab can die under them;
    /// a QML `var` wrapping a destroyed QObject is not reliably null/falsy in
    /// every JS context (a dangling wrapper can stay truthy and then throw on
    /// the method call). Marshaling it to a QObject* parameter IS reliable:
    /// the engine's own deletion tracking passes nullptr for a dead object.
    /// Lives here because this object owns the tabs whose lifetimes are being
    /// probed, and it is already a context property every window can reach.
    Q_INVOKABLE [[nodiscard]] bool objectAlive(QObject* object) const {
        return object != nullptr;
    }

signals:
    void countChanged();
    void currentIndexChanged();
    /// Emitted BEFORE the active tab flips (m_current still points at the
    /// OUTGOING tab). The view detaches the TableView's selection model on
    /// this, because TableView clears whichever selection model is attached
    /// when its data model swaps (the scenario this prevents: without the
    /// detach every tab switch wipes the outgoing tab's range selection in
    /// C++, and the background tab reads selectedRows=0 right after a switch).
    void activeAboutToChange();
    /// The active tab changed (model/selection/reloader pointers differ). QML
    /// re-binds; main.cpp re-pushes the metadata selection.
    void activeChanged();
    /// The active tab's selection changed OR the active tab switched. The single
    /// trigger the metadata pane listens to.
    void activeSelectionChanged();
    /// The active tab's COLUMN LAYOUT changed from C++ (an async .rwfpl load
    /// applied its saved order/widths to the active model). QML re-seeds the
    /// header widths from activeWidths(), since the C++ apply can't touch it.
    void activeLayoutChanged();

    /// An asynchronous playlist read finished for the tab that is
    /// CURRENTLY ACTIVE (session restore, or an opened file). Emitted after
    /// activeLayoutChanged, once the tracks, the CURR focus row, and the SCRL
    /// scroll parking are all in place. The view parks its position restore
    /// on this: reads are async, so at launch the view initializes against a
    /// still-loading model whose pointer never changes when the tracks land,
    /// and no property-change signal exists to tell it the restorable state
    /// has arrived.
    void activeTabLoadCompleted();

private:
    /// One open playlist. The QObjects are parented into the model's subtree so
    /// closeTab() can tear the whole tab down with model->deleteLater(); the raw
    /// pointers here are non-owning views into that subtree.
    struct Tab {
        PlaylistModel*       model = nullptr;
        QItemSelectionModel* selection = nullptr;
        MetadataReloader*    reloader = nullptr;
        QTimer*              autosave = nullptr;
        QString              livePath;          ///< the live_playlist/<uuid>.rwfpl
        QString              title;
        QList<int>           widths;            ///< parked column widths (visual order)
        /// The parked scroll position, as the FIRST VISIBLE ROW
        /// when the tab was last switched away from (or at quit, for the
        /// active tab). Persisted to the SCRL chunk; -1 means
        /// "never parked" and the view leaves the rebuild's natural position
        /// (launch then falls back to centering the CURR focus row).
        /// Row-quantized on purpose: a row realigns exactly after any edit,
        /// while a pixel offset would drift with content-size timing.
        int                  scrollRow = -1;
        /// Transient scan-progress state driving the tab bar's
        /// underline-as-progress-bar. True/advancing only while a scan whose
        /// captured target is THIS tab is running; cleared at scanFinished.
        /// Never persisted.
        bool                 scanning = false;
        double               scanProgress = 0.0;
        /// The tab's most recent DEBOUNCED write, so the shutdown
        /// flush can drain it before its own synchronous write. The debounced
        /// write is fire-and-forget on a worker with an atomic
        /// last-COMMIT-wins QSaveFile underneath, so without the drain a
        /// worker still serializing an older snapshot could commit AFTER the
        /// shutdown write and clobber it; for the ACTIVE tab that older
        /// snapshot carries a stale parked scrollRow by definition (the live
        /// position exists only in QML until the quit-time stash), so the
        /// clobber would lose the active tab's position across relaunch. A
        /// default-constructed future waits as a no-op.
        QFuture<void>        lastWrite;
        bool                 dirty = false;     ///< unsaved-vs-autosave; arms the debounce
        bool                 suppressAutosave = false; ///< true during a load (avoid re-writing what we just read)
    };

    // Construction / teardown helpers.
    Tab* makeTab(const QString& title);   ///< build model+sel+reloader+timer, wire them, NOT yet inserted
    void insertTab(Tab* tab, int at);     ///< begin/endInsertRows + own it
    void destroyTab(int index);           ///< low-level: remove + delete (no last-tab guard)

    // Autosave.
    void markDirty(Tab* tab);             ///< flag + (re)arm the debounce
    void writeTabNow(Tab* tab);           ///< snapshot + off-thread atomic write of the live file
    void writeTabSync(Tab* tab);          ///< snapshot + SYNCHRONOUS atomic write (shutdown flush)

    /// Build the serializable document from a tab's live state (tracks, column
    /// order from the model, parked widths, title). Shared by the async and
    /// synchronous write paths so the two can never drift in what they capture.
    [[nodiscard]] PlaylistDocument snapshotTab(Tab* tab) const;

    /// Clear a tab's dirty flag and notify the tab strip. Shared by both write
    /// paths (a successful write, by either route, settles the tab).
    void clearDirtyFlag(Tab* tab);

    // Loading.
    void loadInto(Tab* tab, const QString& localPath,
                  bool materializeCopy, bool adoptStoredTitle); ///< async read -> setTracks + layout + validate
    void applyLayoutToModel(Tab* tab, const QStringList& fieldIds,
                            const QVariantList& widths); ///< model order + parked widths

    // Naming / paths.
    [[nodiscard]] QString liveDir() const;            ///< userConfigDir()/live_playlist
    [[nodiscard]] QString makeLivePath() const;       ///< liveDir()/<uuid>.rwfpl
    [[nodiscard]] QString uniqueGenericTitle() const; ///< "New Playlist" / "New Playlist 2" / ...

    // Session manifest: the ordered list of live-file basenames + the active
    // index, so tab ORDER and the active tab survive a restart (the live files
    // themselves are uuid-named, so their on-disk order is meaningless). Written
    // (atomically) on every order/active change; a no-op while m_restoring.
    void                      writeSessionManifest() const;
    [[nodiscard]] QStringList readSessionManifest(int* activeIndex) const;

    // Tab lookup by its (stable) model pointer, for async completions that must
    // tolerate the tab having been closed meanwhile. Returns -1 if gone.
    [[nodiscard]] int indexOfModel(const PlaylistModel* model) const;

    // Scanner seam.
    void onScanStarted(int requestedAt);
    void onScanProgress(int done, int total); ///< Forwards to the target tab's roles.
    void onTracksReady(const QList<TrackData>& batch);
    void onScanFinished(int added, int skipped);
    /// Write scanning/scanProgress onto the CAPTURED scan target's tab
    /// and emit dataChanged for the two roles. The row is resolved through
    /// indexOfModel on EVERY call, so a tab-bar reorder mid-scan can never
    /// misroute the update; a closed target (null QPointer) is a no-op.
    void updateScanTabState(bool scanning, double progress);

    QList<Tab*>                 m_tabs;
    int                         m_current = -1;
    ColumnSchema                m_schemaProto;
    const CustomColumnRegistry* m_customColumns = nullptr;
    TrackScanner*               m_scanner = nullptr;
    QStringList                 m_defaultFieldIds; ///< new-tab preset order (empty -> schema defaults)
    QVariantList                m_defaultWidths;   ///< new-tab preset widths (parallel to ids)
    bool                        m_restoring = false; ///< true during restoreSession (suppresses manifest churn)

    // Scanner targets: captured at scanIntoActive() time, popped on scanStarted.
    // QPointer so a closed-tab target degrades to a safe no-op.
    struct ScanTarget {
        QPointer<PlaylistModel>       model;
        QPointer<QItemSelectionModel> selection;
    };
    QList<ScanTarget> m_scanTargets;  ///< FIFO of queued scan destinations
    ScanTarget        m_scanTarget;   ///< the destination of the scan now running
    int               m_insertCursor = 0;  ///< advancing insert position for the running scan
    int               m_insertedFirst = 0; ///< first row inserted (for select-on-finish)
    int               m_insertedCount = 0; ///< rows inserted so far this scan

    static constexpr int kAutosaveDebounceMs = 1500;
};

} // namespace rawform
