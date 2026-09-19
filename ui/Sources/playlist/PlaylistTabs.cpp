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

// PlaylistTabs.cpp
//
// Implementation of the tab session manager: per-tab model, selection, and
// reloader ownership, the active-tab pointers, the scanner seam with its FIFO of
// scan targets, tab creation, closing, moving, and renaming, the live-playlist
// autosave and restore, and the width and scroll parking that survives a switch.

#include "playlist/PlaylistTabs.h"

#include "columns/ColumnSchema.h"
#include "paths/Paths.h"            // userConfigDir()
#include "metadata/MetadataReloader.h"
#include "playlist/PlaylistFile.h"      // readPlaylist / writePlaylist / PlaylistDocument
#include "playlist/PlaylistFormat.h"    // formatForPath (Open routes by suffix)
#include "playlist/PlaylistModel.h"
#include "media/TrackScanner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QSaveFile>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>
#include <QtGlobal>

#include <algorithm> // std::min (parked-width splice on columnsRemoved)
#include <utility> // std::move

namespace rawform {

PlaylistTabs::PlaylistTabs(QObject* parent)
    : QAbstractListModel(parent) {}

PlaylistTabs::~PlaylistTabs() {
    // Belt-and-suspenders: the QML onClosing handler already flushes on a normal
    // window close, but a quit path that bypasses it (programmatic exit, etc.)
    // would otherwise drop any still-debounced write. Drain before we delete the
    // tabs; harmless (a no-op) if onClosing already settled everything.
    flushPendingWrites();

    // Tab QObjects are parented into their model subtrees and die with `this`;
    // the Tab structs are plain heap records we own, so free them explicitly.
    qDeleteAll(m_tabs);
    m_tabs.clear();
}

// ---------------------------------------------------------------------------
// Dependency injection
// ---------------------------------------------------------------------------

void PlaylistTabs::setSchemaPrototype(ColumnSchema schema) {
    m_schemaProto = std::move(schema);
}

void PlaylistTabs::setCustomColumns(const CustomColumnRegistry* registry) {
    m_customColumns = registry;
}

void PlaylistTabs::setDefaultLayout(QStringList fieldIds, QVariantList widths) {
    m_defaultFieldIds = std::move(fieldIds);
    m_defaultWidths   = std::move(widths);
}

void PlaylistTabs::setScanner(TrackScanner* scanner) {
    if (m_scanner == scanner)
        return;
    if (m_scanner)
        disconnect(m_scanner, nullptr, this, nullptr);
    m_scanner = scanner;
    if (!m_scanner)
        return;
    // The scanner seam: results are routed to the tab captured at
    // scanIntoActive() time, not the live-active tab.
    connect(m_scanner, &TrackScanner::scanStarted, this, &PlaylistTabs::onScanStarted);
    connect(m_scanner, &TrackScanner::scanProgress, this, &PlaylistTabs::onScanProgress);
    connect(m_scanner, &TrackScanner::tracksReady, this, &PlaylistTabs::onTracksReady);
    connect(m_scanner, &TrackScanner::scanFinished, this, &PlaylistTabs::onScanFinished);
}

// ---------------------------------------------------------------------------
// QAbstractListModel
// ---------------------------------------------------------------------------

int PlaylistTabs::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_tabs.size());
}

QVariant PlaylistTabs::data(const QModelIndex& index, int role) const {
    const int r = index.row();
    if (r < 0 || r >= m_tabs.size())
        return {};
    const Tab* t = m_tabs.at(r);
    switch (role) {
    case TitleRole:
        return t->title;
    case DirtyRole:
        return t->dirty;
    case ScanningRole:
        return t->scanning;
    case ScanProgressRole:
        return t->scanProgress;
    default:
        return {};
    }
}

QHash<int, QByteArray> PlaylistTabs::roleNames() const {
    return {
        { TitleRole,        QByteArrayLiteral("title") },
        { DirtyRole,        QByteArrayLiteral("dirty") },
        { ScanningRole,     QByteArrayLiteral("scanning") },
        { ScanProgressRole, QByteArrayLiteral("scanProgress") },
    };
}

// ---------------------------------------------------------------------------
// Active tab
// ---------------------------------------------------------------------------

void PlaylistTabs::setCurrentIndex(int index) {
    if (index == m_current)
        return;
    if (index < 0 || index >= m_tabs.size())
        return;
    // BEFORE the flip: let the view detach the TableView's selection model
    // while every binding still reads the OUTGOING tab, so the model swap's
    // internal selection clear hits nothing (see activeAboutToChange docs).
    emit activeAboutToChange();
    m_current = index;
    emit currentIndexChanged();
    // The bound model/selection/reloader pointers all change together; QML
    // re-binds PlaylistView and the album-art wiring, and the metadata pane
    // re-aggregates the new tab's selection.
    emit activeChanged();
    emit activeSelectionChanged();
    writeSessionManifest();
}

PlaylistModel* PlaylistTabs::activeModel() const {
    if (m_current < 0 || m_current >= m_tabs.size())
        return nullptr;
    return m_tabs.at(m_current)->model;
}

QItemSelectionModel* PlaylistTabs::activeSelection() const {
    if (m_current < 0 || m_current >= m_tabs.size())
        return nullptr;
    return m_tabs.at(m_current)->selection;
}

MetadataReloader* PlaylistTabs::activeReloader() const {
    if (m_current < 0 || m_current >= m_tabs.size())
        return nullptr;
    return m_tabs.at(m_current)->reloader;
}

// ---------------------------------------------------------------------------
// Construction helpers
// ---------------------------------------------------------------------------

PlaylistTabs::Tab* PlaylistTabs::makeTab(const QString& title) {
    // Build the model subtree. The model is parented to `this` so it is owned
    // even before the tab is inserted; the selection, reloader and autosave
    // timer are parented to the model so closeTab()'s model->deleteLater()
    // tears the whole tab down.
    auto* model = new PlaylistModel(this);
    model->setSchema(m_schemaProto);          // a private copy per tab
    model->setCustomColumns(m_customColumns); // one shared, read-only registry

    auto* selection = new QItemSelectionModel(model, model);
    auto* reloader  = new MetadataReloader(model, selection, model);

    auto* autosave = new QTimer(model);
    autosave->setSingleShot(true);
    autosave->setInterval(kAutosaveDebounceMs);

    Tab* tab = new Tab;
    tab->model     = model;
    tab->selection = selection;
    tab->reloader  = reloader;
    tab->autosave  = autosave;
    tab->livePath  = makeLivePath();
    tab->title     = title.isEmpty() ? uniqueGenericTitle() : title;
    // Seed parked widths from the model's schema defaults so a never-resized
    // tab still autosaves sensible widths.
    tab->widths.clear();
    for (const QVariant& w : model->defaultColumnWidths())
        tab->widths << w.toInt();

    // --- Per-tab wiring ----------------------------------------------------
    // Each tab's selection drives ITS OWN reloader (freshness on its rows),
    // independent of which tab is shown.
    connect(selection, &QItemSelectionModel::selectionChanged, reloader,
            [reloader](const QItemSelection&, const QItemSelection&) {
                reloader->scheduleRevalidation();
            });
    // When the ACTIVE tab's selection changes, nudge the metadata pane. A
    // background tab's selection won't change (only the view drives it), so the
    // active-guard is belt-and-suspenders.
    connect(selection, &QItemSelectionModel::selectionChanged, this,
            [this, model](const QItemSelection&, const QItemSelection&) {
                if (activeModel() == model)
                    emit activeSelectionChanged();
            });
    // The CURRENT INDEX is selection state too: the art resolver's
    // "valid current index with an empty set counts as a single-row
    // selection" courtesy, and its representative choice, both read it. A
    // current-only change (clearCurrentIndex from the empty-space click,
    // setCurrentIndex with NoUpdate) emits currentChanged WITHOUT
    // selectionChanged; unforwarded, the art/metadata consumers keep
    // evaluating against a stale current and never see the final state.
    connect(selection, &QItemSelectionModel::currentChanged, this,
            [this, model](const QModelIndex&, const QModelIndex&) {
                if (activeModel() == model)
                    emit activeSelectionChanged();
            });
    // A reloader refresh changed row data in place; re-aggregate the pane if
    // this is the active tab (the metadata model snapshots values at selection
    // time, so it needs the nudge). The art epoch bumps UNCONDITIONALLY (a
    // background tab's reload also invalidates its sidecar covers; its URLs
    // re-resolve on the next switch/eval), only the pane nudge is
    // active-gated.
    connect(reloader, &MetadataReloader::refreshed, this, [this, model]() {
        model->bumpArtEpoch();
        if (activeModel() == model)
            emit activeSelectionChanged();
    });

    // Keep this tab's PARKED widths positionally aligned across granular
    // column removals. The header's explicit widths are the live store only
    // for the ACTIVE tab; a registry-driven custom-column delete hits EVERY
    // tab's model (each connects to the registry), and a background tab's
    // parked widths would otherwise keep the removed column's entry, pairing
    // every later width one position off on the next switch/autosave. The
    // active tab is spliced here too; the view then re-parks the same
    // post-removal values via stashActiveWidths, so the two writes agree.
    // Inserts need no counterpart: only the interactive toggle appends a
    // column, and it is active-tab-only and re-parks explicitly.
    connect(model, &QAbstractItemModel::columnsRemoved, this,
            [this, tab](const QModelIndex&, int first, int last) {
                if (!m_tabs.contains(tab))
                    return;
                const int hi = std::min(last, static_cast<int>(tab->widths.size()) - 1);
                for (int c = hi; c >= first && c >= 0; --c)
                    tab->widths.removeAt(c);
            });

    // Any structural change (rows in/out/moved, a column toggle reset, a column
    // move) or an in-place tag refresh marks the tab dirty and (re)arms autosave.
    const auto dirty = [this, tab]() { markDirty(tab); };
    connect(model, &QAbstractItemModel::rowsInserted, this, dirty);
    connect(model, &QAbstractItemModel::rowsRemoved, this, dirty);
    connect(model, &QAbstractItemModel::rowsMoved, this, dirty);
    connect(model, &QAbstractItemModel::columnsMoved, this, dirty);
    // Granular column toggles (showColumn/hideColumn) and the registry-driven
    // custom-column removal are begin/endInsert/RemoveColumns, NOT resets, so
    // they need their own dirty hooks or a toggled layout never autosaves.
    // (The registry-removal path predates these connects and silently relied
    // on some later edit to trigger the save; this closes that hole too.)
    connect(model, &QAbstractItemModel::columnsInserted, this, dirty);
    connect(model, &QAbstractItemModel::columnsRemoved, this, dirty);
    connect(model, &QAbstractItemModel::modelReset, this, dirty);
    connect(model, &QAbstractItemModel::dataChanged, this, dirty);
    // The CURRENT (focus) row is persisted in the live file, so moving it
    // dirties the tab. Click-frequency, but the autosave debounce coalesces a
    // burst into one deferred QSaveFile write on the pool, and markDirty's
    // suppressAutosave guard keeps the load-time restore from re-writing what
    // was just read.
    connect(selection, &QItemSelectionModel::currentChanged, this, dirty);

    // Debounced write of this tab's live file. Guard against the tab having been
    // closed before the timer fires.
    connect(autosave, &QTimer::timeout, this, [this, tab]() {
        if (m_tabs.contains(tab))
            writeTabNow(tab);
    });

    return tab;
}

void PlaylistTabs::insertTab(Tab* tab, int at) {
    if (at < 0 || at > m_tabs.size())
        at = static_cast<int>(m_tabs.size());
    beginInsertRows(QModelIndex(), at, at);
    m_tabs.insert(at, tab);
    endInsertRows();
    // Inserting before the current tab shifts its index right by one.
    if (m_current >= at)
        ++m_current; // no signal: the active tab object didn't change
    emit countChanged();
    writeSessionManifest();
}

void PlaylistTabs::destroyTab(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;
    Tab* tab = m_tabs.at(index);
    if (tab->autosave)
        tab->autosave->stop();

    // Delete the live working file; these are temporary until kept.
    if (!tab->livePath.isEmpty())
        QFile::remove(tab->livePath);

    beginRemoveRows(QModelIndex(), index, index);
    m_tabs.removeAt(index);
    endRemoveRows();

    if (tab->model)
        tab->model->deleteLater(); // cascades selection + reloader + timer
    delete tab;                    // the plain record
    emit countChanged();
    writeSessionManifest();
}

// ---------------------------------------------------------------------------
// Lifecycle (QML)
// ---------------------------------------------------------------------------

int PlaylistTabs::newPlaylist(const QString& title) {
    Tab* tab = makeTab(title);
    // A brand-new tab adopts the user's saved column preset (if any) so new
    // playlists open with their chosen columns/order/widths; with no preset the
    // model keeps its schema defaults. Opened/restored tabs skip this; their
    // own saved layout is applied in loadInto().
    if (!m_defaultFieldIds.isEmpty())
        applyLayoutToModel(tab, m_defaultFieldIds, m_defaultWidths);
    const int at = static_cast<int>(m_tabs.size());
    insertTab(tab, at);
    setCurrentIndex(at);
    // Materialize the live file immediately so an intentional (even empty) tab
    // survives a restart, rather than only after the first edit.
    writeTabNow(tab);
    return at;
}

void PlaylistTabs::closeTab(int index) {
    if (index < 0 || index >= m_tabs.size())
        return;

    const bool wasCurrent = (index == m_current);
    destroyTab(index);

    if (m_tabs.isEmpty()) {
        // Never zero tabs: spawn a fresh empty one (becomes current).
        m_current = -1;
        newPlaylist();
        return;
    }

    // Re-resolve the current index. destroyTab didn't touch m_current; fix it.
    if (wasCurrent) {
        const int next = qMin(index, static_cast<int>(m_tabs.size()) - 1);
        m_current = -1;        // force setCurrentIndex to treat `next` as a change
        setCurrentIndex(next);
    } else if (index < m_current) {
        --m_current;           // a tab before the current one was removed
        emit currentIndexChanged();
        writeSessionManifest(); // active index shifted (destroyTab's write was pre-shift)
    }
}

bool PlaylistTabs::moveTab(int from, int to) {
    const int n = static_cast<int>(m_tabs.size());
    if (from == to || from < 0 || from >= n || to < 0 || to >= n)
        return false;

    const int dest = (to > from) ? to + 1 : to; // beginMoveRows: pre-move coords
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), dest))
        return false;
    m_tabs.move(from, to);
    endMoveRows();

    // Keep currentIndex pinned to the SAME tab object across the reorder.
    if (m_current == from) {
        m_current = to;
        emit currentIndexChanged();
    } else if (from < m_current && to >= m_current) {
        --m_current;
        emit currentIndexChanged();
    } else if (from > m_current && to <= m_current) {
        ++m_current;
        emit currentIndexChanged();
    }
    writeSessionManifest();
    return true;
}

void PlaylistTabs::openInNewTab(const QUrl& fileUrl) {
    const QString src = fileUrl.toLocalFile();
    if (src.isEmpty())
        return;
    const QString title = QFileInfo(src).completeBaseName();

    // An .m3u/.m3u8 is NOT an RFW1 document: it carries file references only, so
    // handing it to readPlaylist would fail on the magic check and the open
    // would look like a corrupt file. Route it through the scanner instead,
    // where expandM3u already resolves the references in playlist order and each
    // resolved file is TagLib-scanned exactly like any other ingestion. That
    // also means there is only ever ONE M3U parser in the build.
    //
    // The new tab takes the user's default column preset (newPlaylist applies
    // it), which is the right answer here: an M3U stores no layout, so there is
    // nothing of the source's to honor. An unrecognized suffix keeps the
    // pre-existing behavior and is attempted as .rwfpl, since identity travels
    // with the MAGIC and a renamed rawform playlist should still open.
    if (formatForPath(src, PlaylistFormat::Rwfpl) != PlaylistFormat::Rwfpl) {
        newPlaylist(title);              // creates AND activates the destination
        scanIntoActive({ fileUrl }, -1); // captures that tab as the scan target
        return;
    }

    // A freshly opened .rwfpl becomes its OWN live copy (new livePath); the
    // source file is not adopted. Title from the source's base name.
    Tab* tab = makeTab(title);
    const int at = static_cast<int>(m_tabs.size());
    insertTab(tab, at);
    setCurrentIndex(at);
    loadInto(tab, src, /*materializeCopy*/ true, /*adoptStoredTitle*/ false);
}

void PlaylistTabs::concatFileIntoActive(const QUrl& fileUrl, int atRow) {
    PlaylistModel* m = activeModel();
    if (!m)
        return;
    const QString src = fileUrl.toLocalFile();
    if (src.isEmpty())
        return;

    QPointer<PlaylistModel> target = m;
    auto* watcher = new QFutureWatcher<PlaylistReadResult>(this);
    connect(watcher, &QFutureWatcher<PlaylistReadResult>::finished, this,
            [this, watcher, target, atRow]() {
                const PlaylistReadResult res = watcher->result();
                watcher->deleteLater();
                if (!res.ok()) {
                    qWarning("rawform: .rwfpl concat failed: %s",
                             qUtf8Printable(res.message));
                    return;
                }
                if (!target)
                    return; // active tab closed mid-read
                const int idx = indexOfModel(target);
                if (idx < 0)
                    return;
                // Insert (not reset) at the drop point; selection is preserved.
                target->insertTracks(atRow, res.doc.tracks);
                Tab* t = m_tabs.at(idx);
                markDirty(t);
                // The concatenated rows came from a cache; validate to gray any
                // whose files are gone. A whole-playlist sweep: the reloader
                // offers no range-scoped variant, and the conditional pass
                // skips unchanged rows cheaply.
                t->reloader->validateAll();
            });
    watcher->setFuture(QtConcurrent::run([src]() { return readPlaylist(src); }));
}

void PlaylistTabs::scanIntoActive(const QList<QUrl>& urls, int at) {
    if (!m_scanner || urls.isEmpty())
        return;
    // Capture the destination NOW so a tab switch during the scan can't misfile
    // it. QPointer-guarded so a closed target degrades to a no-op.
    ScanTarget tgt;
    tgt.model     = activeModel();
    tgt.selection = activeSelection();
    m_scanTargets.append(tgt);
    m_scanner->scan(urls, at);
}

namespace {

/// The drop split's .rwfpl test: case-insensitive on the URL string, so
/// file:// and plain paths behave alike.
bool isRwfplUrl(const QUrl& url) {
    return url.toString().endsWith(QLatin1String(".rwfpl"), Qt::CaseInsensitive);
}

} // namespace

int PlaylistTabs::dropUrlsIntoActive(const QList<QUrl>& urls, int at) {
    // Same routing order the QML loop had: each .rwfpl concatenated as it is
    // encountered, the rest gathered and dispatched once. Both land at the same
    // drop row; the scanner and the concat reads are all async, so this returns
    // as soon as the split is done.
    QList<QUrl> rest;
    rest.reserve(urls.size());
    for (const QUrl& u : urls) {
        if (isRwfplUrl(u))
            concatFileIntoActive(u, at);
        else
            rest << u;
    }
    if (!rest.isEmpty())
        scanIntoActive(rest, at);
    return static_cast<int>(urls.size());
}

int PlaylistTabs::dropUrlsIntoNewTab(const QList<QUrl>& urls, bool proactiveTabOpen) {
    // .rwfpl playlists each open in their own new tab; everything else feeds
    // one tab. Sequencing: all openInNewTab calls first, THEN the (possibly
    // proactive) destination tab and its scan, so the scan target is the tab
    // created/activated last.
    QList<QUrl> rest;
    rest.reserve(urls.size());
    for (const QUrl& u : urls) {
        if (isRwfplUrl(u))
            openInNewTab(u);
        else
            rest << u;
    }
    if (!rest.isEmpty()) {
        // If the strip's 2 s hover already popped an empty tab, the drop lands
        // in it (generic name). Otherwise this is a quick drop: make a tab
        // smart-named from the content (empty name -> generic).
        if (!proactiveTabOpen)
            newPlaylist(suggestTabName(rest));
        scanIntoActive(rest, -1);
    }
    return static_cast<int>(urls.size());
}

QString PlaylistTabs::suggestTabName(const QList<QUrl>& urls) const {
    // Smart naming only for a single, unambiguous source. Anything else (loose
    // files, multiple items, mixed) returns "" -> caller uses a generic name.
    if (urls.size() != 1)
        return {};
    const QString local = urls.first().toLocalFile();
    if (local.isEmpty())
        return {};

    const QFileInfo fi(local);
    if (fi.isDir()) {
        // Precedence: a top-level .m3u/.m3u8 inside the dir, else the dir name.
        const QStringList m3us =
            QDir(local).entryList({ QStringLiteral("*.m3u"), QStringLiteral("*.m3u8") },
                                  QDir::Files, QDir::Name);
        if (!m3us.isEmpty())
            return QFileInfo(m3us.first()).completeBaseName();
        return fi.fileName(); // directory name
    }

    const QString suffix = fi.suffix().toLower();
    if (suffix == QLatin1String("m3u") || suffix == QLatin1String("m3u8"))
        return fi.completeBaseName();
    return {};
}

// ---------------------------------------------------------------------------
// Active-tab column layout
// ---------------------------------------------------------------------------

void PlaylistTabs::stashActiveWidths(const QVariantList& widths) {
    if (m_current < 0 || m_current >= m_tabs.size())
        return;
    Tab* t = m_tabs.at(m_current);
    QList<int> incoming;
    incoming.reserve(static_cast<int>(widths.size()));
    for (const QVariant& w : widths)
        incoming << w.toInt();
    // A switch/seed re-asserts the already-parked widths (header re-laid out with
    // the same values), so nothing changed and we don't arm autosave. Only a real
    // resize, where the widths actually differ, marks the tab dirty so the new
    // widths get written (markDirty respects suppressAutosave during a load).
    if (incoming == t->widths)
        return;
    t->widths = std::move(incoming);
    markDirty(t);
}

QVariantList PlaylistTabs::activeWidths() const {
    QVariantList out;
    if (m_current < 0 || m_current >= m_tabs.size())
        return out;
    const Tab* t = m_tabs.at(m_current);
    out.reserve(t->widths.size());
    for (int w : t->widths)
        out << w;
    return out;
}

void PlaylistTabs::stashActiveScrollRow(int row) {
    // Targets the tab that is active AT CALL TIME: the QML
    // stash runs in onActiveAboutToChange, before m_current flips, so this
    // parks the OUTGOING tab's position; MainWindow.onClosing calls it for
    // the still-active tab at quit. Stashing into a tab that is about to
    // close is a harmless write to a dying struct.
    // The stashActiveWidths contract: a switch re-asserting the SAME
    // position changes nothing and never arms autosave; a real change marks
    // the tab dirty so the debounced autosave (or the shutdown flush)
    // persists it to the SCRL chunk. markDirty respects suppressAutosave
    // during a load.
    if (m_current < 0 || m_current >= m_tabs.size())
        return;
    Tab* t = m_tabs.at(m_current);
    const int v = row < 0 ? -1 : row;
    if (t->scrollRow == v)
        return;
    t->scrollRow = v;
    markDirty(t);
}

int PlaylistTabs::activeScrollRow() const {
    if (m_current < 0 || m_current >= m_tabs.size())
        return -1;
    return m_tabs.at(m_current)->scrollRow;
}

void PlaylistTabs::renameTab(int idx, const QString& title) {
    if (idx < 0 || idx >= m_tabs.size())
        return;
    const QString clean = title.trimmed();
    if (clean.isEmpty())
        return; // ignore an empty rename; keep the existing title
    Tab* t = m_tabs.at(idx);
    if (t->title == clean)
        return;
    t->title = clean;
    emit dataChanged(index(idx, 0), index(idx, 0), { TitleRole });
    markDirty(t); // persist the new name to the live file
}

int PlaylistTabs::indexOfModel(PlaylistModel* model) const {
    // Pointer identity (the Ctrl+P playing-track reveal). A null model
    // (e.g. audioController.playingModel after its owning tab closed; the
    // controller holds it through a QPointer) falls out naturally: nothing
    // in m_tabs is null-modeled.
    if (!model)
        return -1;
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs.at(i)->model == model)
            return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Autosave
// ---------------------------------------------------------------------------

void PlaylistTabs::markDirty(Tab* tab) {
    if (!tab || tab->suppressAutosave)
        return;
    if (!tab->dirty) {
        tab->dirty = true;
        const int idx = m_tabs.indexOf(tab);
        if (idx >= 0)
            emit dataChanged(index(idx, 0), index(idx, 0), { DirtyRole });
    }
    if (tab->autosave)
        tab->autosave->start(); // (re)arm the debounce
}

PlaylistDocument PlaylistTabs::snapshotTab(Tab* tab) const {
    // Snapshot on the GUI thread; the worker (or the calling thread, for the
    // sync path) only serializes. Order from the model, widths from the parked
    // copy (QML keeps the active tab's parked widths current; on close the
    // onClosing handler pushes the live header widths here first).
    PlaylistDocument doc;
    doc.tracks     = tab->model->snapshotTracks();
    doc.fieldIds   = tab->model->currentColumnOrder();
    doc.widths     = tab->widths;
    doc.title      = tab->title;
    doc.savedAtUtc = QDateTime::currentDateTimeUtc();
    // The focus row (the remembered last-selected track). -1 for none.
    doc.currentRow = (tab->selection && tab->selection->currentIndex().isValid())
                         ? tab->selection->currentIndex().row() : -1;
    // The parked scroll position. The ACTIVE tab's live position is
    // pushed into this parking by MainWindow.onClosing (like the widths line
    // there) before the shutdown flush; during the session it is current as
    // of the last switch-away, which is exactly when this tab's file can be
    // written with it.
    doc.scrollRow = tab->scrollRow;
    return doc;
}

void PlaylistTabs::clearDirtyFlag(Tab* tab) {
    if (!tab->dirty)
        return;
    tab->dirty = false;
    const int idx = m_tabs.indexOf(tab);
    if (idx >= 0)
        emit dataChanged(index(idx, 0), index(idx, 0), { DirtyRole });
}

void PlaylistTabs::writeTabNow(Tab* tab) {
    if (!tab)
        return;
    PlaylistDocument doc = snapshotTab(tab);

    const QString path = tab->livePath;
    // Fire-and-forget: QSaveFile is atomic (temp + rename), so two overlapping
    // autosaves to the same path are last-write-wins, never a torn file;
    // coalescing overlapping writes is outside this path (the debounce is the
    // only throttle). The future is kept on the tab so flushPendingWrites can
    // drain an in-flight write before its own sync write (see Tab::lastWrite).
    tab->lastWrite = QtConcurrent::run([path, doc = std::move(doc)]() {
        QString err;
        if (!writePlaylist(path, doc, &err))
            qWarning("rawform: live autosave failed (%s): %s",
                     qUtf8Printable(path), qUtf8Printable(err));
    });

    clearDirtyFlag(tab);
}

void PlaylistTabs::writeTabSync(Tab* tab) {
    if (!tab || tab->livePath.isEmpty())
        return;
    // The shutdown path: write inline on the calling thread so it actually
    // completes before the process exits (a QtConcurrent write would be
    // abandoned). Same atomic QSaveFile underneath, so it stays torn-write safe
    // even if a debounced worker write for the same tab is still in flight.
    const PlaylistDocument doc = snapshotTab(tab);
    QString err;
    if (!writePlaylist(tab->livePath, doc, &err))
        qWarning("rawform: shutdown flush failed (%s): %s",
                 qUtf8Printable(tab->livePath), qUtf8Printable(err));
    clearDirtyFlag(tab);
}

void PlaylistTabs::flushPendingWrites() {
    // Drain every pending autosave synchronously. Stop each debounce first (so a
    // timer that was about to fire can't double-write or fire post-teardown),
    // then write the tabs that actually have unsaved changes. suppressAutosave
    // tabs are mid-load: their dirty flag was never set, so they are skipped and
    // their on-disk source is left intact.
    for (Tab* tab : m_tabs) {
        if (tab->autosave)
            tab->autosave->stop();
        if (tab->dirty) {
            // An in-flight DEBOUNCED write must complete BEFORE the
            // sync write, so the sync snapshot is provably the FINAL atomic
            // commit; otherwise a worker still serializing an older snapshot
            // (stale scrollRow for the active tab: its live position only
            // reached the parking in the quit-time stash moments ago) could
            // commit after this and win. Waiting on a never-assigned future
            // is a no-op; waiting on a finished one returns immediately.
            tab->lastWrite.waitForFinished();
            writeTabSync(tab);
        }
    }
}

// ---------------------------------------------------------------------------
// File operations
// ---------------------------------------------------------------------------

void PlaylistTabs::applyPathRenames(const QVariantMap& renames) {
    // See the header for the contract. The sweep is linear over every row of
    // every tab with a constFind per row: a rename batch is small and rare,
    // and rows * O(log n) map lookups is nothing next to the disk renames
    // that just happened.
    if (renames.isEmpty())
        return;
    for (Tab* tab : std::as_const(m_tabs)) {
        PlaylistModel* model = tab->model;
        if (model == nullptr)
            continue;
        const int rows = model->rowCount();
        for (int row = 0; row < rows; ++row) {
            const TrackData* t = model->trackAt(row);
            if (t == nullptr)
                continue;
            const auto it = renames.constFind(t->filePath);
            if (it == renames.constEnd())
                continue;
            const QString newPath = it->toString();
            if (newPath.isEmpty() || newPath == t->filePath)
                continue;
            TrackData patched = *t;
            patched.filePath = newPath;
            patched.fileName = QFileInfo(newPath).fileName();
            // refreshTrack emits dataChanged for the whole row; the per-tab
            // dataChanged -> markDirty connection re-arms that tab's autosave,
            // so the patched paths reach the live .rwfpl without any explicit
            // save call here.
            model->refreshTrack(row, std::move(patched));
        }
    }
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void PlaylistTabs::loadInto(Tab* tab, const QString& localPath,
                            bool materializeCopy, bool adoptStoredTitle) {
    // Suppress autosave across the load's resets/refreshes so we don't re-write
    // what we just read (and so a restore doesn't immediately rewrite every
    // live file on startup).
    tab->suppressAutosave = true;

    QPointer<PlaylistModel> target = tab->model;
    auto* watcher = new QFutureWatcher<PlaylistReadResult>(this);
    connect(watcher, &QFutureWatcher<PlaylistReadResult>::finished, this,
            [this, watcher, target, materializeCopy, adoptStoredTitle]() {
                const PlaylistReadResult res = watcher->result();
                watcher->deleteLater();
                if (!target)
                    return; // tab closed mid-load
                const int idx = indexOfModel(target);
                if (idx < 0)
                    return;
                Tab* t = m_tabs.at(idx);

                if (!res.ok()) {
                    qWarning("rawform: live playlist load failed: %s",
                             qUtf8Printable(res.message));
                    t->suppressAutosave = false;
                    return;
                }

                // Tracks first (instant display), then the column layout.
                t->model->setTracks(res.doc.tracks);
                QVariantList w;
                w.reserve(res.doc.widths.size());
                for (int x : res.doc.widths)
                    w << x;
                applyLayoutToModel(t, res.doc.fieldIds, w);

                if (adoptStoredTitle && !res.doc.title.isEmpty()) {
                    t->title = res.doc.title;
                    emit dataChanged(index(idx, 0), index(idx, 0), { TitleRole });
                }

                // Restore the focus row as CURRENT only, never a
                // selection: the remembered last-selected track is the
                // focus outline, not a tint. NoUpdate leaves the (empty)
                // selection untouched. Clamped against the live row count (the
                // file may have been written against a different track list),
                // and done BEFORE suppressAutosave lifts so the resulting
                // currentChanged can't mark the tab dirty and rewrite what was
                // just read.
                const int cr = res.doc.currentRow;
                if (t->selection && cr >= 0 && cr < t->model->rowCount())
                    t->selection->setCurrentIndex(t->model->index(cr, 0),
                                                  QItemSelectionModel::NoUpdate);

                // The persisted scroll position lands in the same parking
                // the in-session switches use, so the QML restore path (launch
                // and switch-in alike) needs no disk awareness. Raw, not
                // clamped: the applier clamps against the live layout. A plain
                // field write, no signals, so autosave suppression is moot.
                t->scrollRow = res.doc.scrollRow;

                t->suppressAutosave = false;

                // The header (single, in QML) can't be seeded from C++, so if this
                // load landed on the ACTIVE tab, tell QML to push the freshly
                // parked widths onto it. (A background tab seeds its header when
                // it later becomes active, via PlaylistView.onModelChanged.)
                // ActiveTabLoadCompleted follows, AFTER the layout
                // signal so any layout-driven model reset settles before the
                // view parks its position restore. This signal exists because
                // playlist reads are ASYNC (QFutureWatcher): at app launch
                // the QML view comes up against a still-loading model, both
                // of its launch parkers find nothing (scrollRow and the CURR
                // focus row apply right here, later), and the model POINTER
                // never changes when the tracks land, so onModelChanged never
                // refires. Without this event the launch restore can never
                // run, whatever the disk says.
                if (activeModel() == t->model) {
                    emit activeLayoutChanged();
                    emit activeTabLoadCompleted();
                }

                // An opened .rwfpl becomes its own live copy on disk now (the
                // restore path skips this: the file already IS the live file).
                if (materializeCopy)
                    writeTabNow(t);

                // Freshness: re-read changed files, gray gone ones.
                t->reloader->validateAll();
            });
    watcher->setFuture(QtConcurrent::run([localPath]() { return readPlaylist(localPath); }));
}

void PlaylistTabs::applyLayoutToModel(Tab* tab, const QStringList& fieldIds,
                                      const QVariantList& widths) {
    // applyColumnLayout reconciles the saved fields against the live schema and
    // returns the widths re-sequenced to the resulting visual order; park those
    // so the header is seeded correctly when this tab is (re)activated.
    const QVariantList aligned = tab->model->applyColumnLayout(fieldIds, widths);
    tab->widths.clear();
    tab->widths.reserve(static_cast<int>(aligned.size()));
    for (const QVariant& v : aligned)
        tab->widths << v.toInt();
}

// ---------------------------------------------------------------------------
// Naming / paths
// ---------------------------------------------------------------------------

QString PlaylistTabs::liveDir() const {
    return userConfigDir() + QStringLiteral("/live_playlist");
}

QString PlaylistTabs::makeLivePath() const {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return liveDir() + QLatin1Char('/') + id + QStringLiteral(".rwfpl");
}

// Session manifest: a tiny text file in liveDir() recording tab ORDER and the
// active index. Line 0 is the active index; each following line is a live-file
// basename in tab order. The live files are uuid-named, so without this their
// on-disk (name-sorted) order is arbitrary; this is what makes tabs reopen in
// the order they were left. Named ".session" so the *.rwfpl enumeration skips it.
void PlaylistTabs::writeSessionManifest() const {
    if (m_restoring)
        return; // one explicit write at the end of restoreSession instead
    QStringList lines;
    lines << QString::number(m_current);
    for (const Tab* t : m_tabs)
        lines << QFileInfo(t->livePath).fileName();

    QSaveFile f(liveDir() + QStringLiteral("/.session"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return; // best-effort: a missing manifest just falls back to name order
    f.write(lines.join(QLatin1Char('\n')).toUtf8());
    f.commit();
}

QStringList PlaylistTabs::readSessionManifest(int* activeIndex) const {
    if (activeIndex)
        *activeIndex = 0;
    QStringList names;
    QFile f(liveDir() + QStringLiteral("/.session"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return names;
    const QList<QByteArray> lines = f.readAll().split('\n');
    if (lines.isEmpty())
        return names;
    bool ok = false;
    const int active = QString::fromUtf8(lines.first()).trimmed().toInt(&ok);
    if (ok && activeIndex)
        *activeIndex = active;
    for (int i = 1; i < lines.size(); ++i) {
        const QString s = QString::fromUtf8(lines.at(i)).trimmed();
        if (!s.isEmpty())
            names << s;
    }
    return names;
}

QString PlaylistTabs::uniqueGenericTitle() const {
    const QString base = QStringLiteral("New Playlist");
    const auto taken = [this](const QString& s) {
        for (const Tab* t : m_tabs)
            if (t->title == s)
                return true;
        return false;
    };
    if (!taken(base))
        return base;
    for (int n = 2;; ++n) {
        const QString candidate = base + QLatin1Char(' ') + QString::number(n);
        if (!taken(candidate))
            return candidate;
    }
}

int PlaylistTabs::indexOfModel(const PlaylistModel* model) const {
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs.at(i)->model == model)
            return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Scanner seam
// ---------------------------------------------------------------------------

void PlaylistTabs::onScanStarted(int requestedAt) {
    m_scanTarget = m_scanTargets.isEmpty() ? ScanTarget{} : m_scanTargets.takeFirst();
    PlaylistModel* m = m_scanTarget.model;
    const int n = m ? m->rowCount() : 0;
    m_insertCursor  = (requestedAt < 0 || requestedAt > n) ? n : requestedAt;
    m_insertedFirst = m_insertCursor;
    m_insertedCount = 0;
    // Arm the target tab's progress bar at zero. During the enumeration
    // walk no total exists, so the bar sits at zero width until the scanner's
    // first (0, total) emission; accepted over an indeterminate mode.
    updateScanTabState(true, 0.0);
}

void PlaylistTabs::onScanProgress(int done, int total) {
    // Files processed over files enumerated; skipped files advance too,
    // so the bar always reaches the end.
    updateScanTabState(true, total > 0 ? double(done) / double(total) : 0.0);
}

void PlaylistTabs::updateScanTabState(bool scanning, double progress) {
    PlaylistModel* m = m_scanTarget.model;
    if (!m)
        return; // target tab closed mid-scan: its state died with it
    const int row = indexOfModel(m); // per-call: reorder-safe
    if (row < 0)
        return;
    Tab* t = m_tabs.at(row);
    if (t->scanning == scanning && qAbs(t->scanProgress - progress) < 1e-9)
        return;
    t->scanning     = scanning;
    t->scanProgress = progress;
    emit dataChanged(index(row, 0), index(row, 0),
                     { ScanningRole, ScanProgressRole });
}

void PlaylistTabs::onTracksReady(const QList<TrackData>& batch) {
    PlaylistModel* m = m_scanTarget.model;
    if (!m)
        return; // target tab closed mid-scan
    m->insertTracks(m_insertCursor, batch);
    m_insertCursor  += static_cast<int>(batch.size());
    m_insertedCount += static_cast<int>(batch.size());
}

void PlaylistTabs::onScanFinished(int added, int skipped) {
    qInfo("rawform: scan finished: added %d, skipped %d", added, skipped);

    PlaylistModel*       m   = m_scanTarget.model;
    QItemSelectionModel* sel = m_scanTarget.selection;
    if (m && sel && m_insertedCount > 0) {
        const int last = m_insertedFirst + m_insertedCount - 1;
        const int cols = m->columnCount();
        const QModelIndex tl = m->index(m_insertedFirst, 0);
        const QModelIndex br = m->index(last, cols > 0 ? cols - 1 : 0);
        sel->select(QItemSelection(tl, br),
                    QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        sel->setCurrentIndex(m->index(m_insertedFirst, 0), QItemSelectionModel::NoUpdate);
    }
    // Retire the bar BEFORE dropping the target (the helper resolves the
    // tab through it). Unfocused tabs hide the underline again; the focused
    // tab's delegate falls back to the full accent underline on its own.
    updateScanTabState(false, 0.0);
    m_scanTarget = ScanTarget{};
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

void PlaylistTabs::restoreSession() {
    m_restoring = true; // suppress per-op manifest writes; one explicit write below
    const QString dir = liveDir();
    QDir().mkpath(dir);

    // Live files actually present on disk.
    const QStringList present =
        QDir(dir).entryList({ QStringLiteral("*.rwfpl") }, QDir::Files, QDir::Name);

    // The order (and active index) the user left, from the manifest.
    int savedActive = 0;
    const QStringList saved = readSessionManifest(&savedActive);

    // Reconstruct the order: manifest entries that still exist, in manifest
    // order, then any present files the manifest didn't list (e.g. a crash
    // between creating a live file and writing the manifest) appended at the end.
    QStringList ordered;
    for (const QString& name : saved)
        if (present.contains(name) && !ordered.contains(name))
            ordered << name;
    for (const QString& name : present)
        if (!ordered.contains(name))
            ordered << name;

    if (ordered.isEmpty()) {
        m_restoring = false;
        newPlaylist(); // one empty tab on a fresh install (writes the manifest)
        return;
    }

    for (const QString& f : ordered) {
        const QString path = dir + QLatin1Char('/') + f;
        Tab* tab = makeTab(QStringLiteral("…")); // provisional; real title from META
        tab->livePath = path;                    // adopt the existing live file
        const int at = static_cast<int>(m_tabs.size());
        insertTab(tab, at);
        loadInto(tab, path, /*materializeCopy*/ false, /*adoptStoredTitle*/ true);
    }
    setCurrentIndex(qBound(0, savedActive, static_cast<int>(m_tabs.size()) - 1));

    m_restoring = false;
    writeSessionManifest(); // settle the manifest to the restored state
}

} // namespace rawform
