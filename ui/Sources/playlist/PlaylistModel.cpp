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

// PlaylistModel.cpp
//
// Implementation of the playlist table model: the schema-driven column overlay
// (visual columns, reordering, alignment, default widths, layout persistence by
// field id), the track list mutations the tabs and the scanner drive, and the
// per-row accessors QML and the Properties window read.

#include "playlist/PlaylistModel.h"

#include "columns/CustomColumnRegistry.h" // registry lookup + signals
#include "columns/PatternEvaluator.h"     // renderFieldValue + evaluatePattern
#include "media/ReplayGainTags.h"         // shared REPLAYGAIN_* parse + format

#include <QItemSelectionModel> // selectedRows / currentIndex for album-art selection
#include <QLatin1Char>
#include <QPair> // (path, subsong) identity key in rowsForKeys
#include <QSet>
#include <QUrl>                // percent-encode the art path for the provider URL
#include <QVariantMap>
#include <QtGlobal> // qWarning

#include <algorithm> // std::ranges::sort / unique / rotate, std::max
#include <utility> // std::move
#include <vector> // selectedRowList's dedupe/sort bitmap

namespace rawform {

PlaylistModel::PlaylistModel(QObject* parent)
    : QAbstractTableModel(parent) {}

int PlaylistModel::rowCount(const QModelIndex& parent) const {
    // A table model has no hierarchy: child indices have no rows.
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_tracks.size());
}

int PlaylistModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_columns.size());
}

const PlaylistModel::VisualColumn* PlaylistModel::columnAt(int column) const {
    if (column < 0 || column >= m_columns.size())
        return nullptr;
    return &m_columns.at(column);
}

QVariant PlaylistModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};

    const int row = index.row();
    const int col = index.column();
    if (row < 0 || row >= m_tracks.size())
        return {};

    switch (role) {
    case Qt::DisplayRole: {
        const VisualColumn* vc = columnAt(col);
        if (!vc)
            return {};
        if (vc->isCustom) {
            // Resolve the record by id and evaluate its pattern. A record deleted
            // out from under a still-shown column (before onCustomColumnRemoved
            // drops it) renders empty rather than crashing.
            const CustomColumn* rec =
                m_customColumns ? m_customColumns->byId(vc->customId) : nullptr;
            return rec ? evaluatePattern(m_tracks.at(row), rec->pattern) : QVariant{};
        }
        return renderField(m_tracks.at(row), vc->field);
    }
    case RowIndexRole:
        return row;
    case RowAvailableRole:
        return m_tracks.at(row).available;
    case ColumnAlignmentRole:
        return columnAlignment(col); // per-column, row-independent
    default:
        return {};
    }
}

QVariant PlaylistModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (orientation != Qt::Horizontal)
        return {};
    // Alignment as a header ROLE: the header view's proxy maps headerData to
    // its cells' injected role properties and headerDataChanged to their
    // refresh, so the emissions the structural column ops already make (move,
    // hide, registry removal, alignment edit) refresh alignment and title
    // together, atomically per cell.
    if (role == ColumnAlignmentRole)
        return columnAlignment(section);
    if (role != Qt::DisplayRole)
        return {};
    const VisualColumn* vc = columnAt(section);
    if (!vc)
        return {};
    if (vc->isCustom) {
        // A custom column's header IS its record name (no title-override tier).
        const CustomColumn* rec =
            m_customColumns ? m_customColumns->byId(vc->customId) : nullptr;
        return rec ? QVariant(rec->name) : QVariant{};
    }
    return humanTitleFor(vc->field);
}

QHash<int, QByteArray> PlaylistModel::roleNames() const {
    return {
        { DisplayRole,  QByteArrayLiteral("display") },
        { RowIndexRole, QByteArrayLiteral("rowIndex") },
        { RowAvailableRole, QByteArrayLiteral("rowAvailable") },
        { ColumnAlignmentRole, QByteArrayLiteral("columnAlignment") },
    };
}

void PlaylistModel::setSchema(ColumnSchema schema) {
    // Column set changes -> structural reset. The catalog (m_schema) is
    // title/alignment only; which columns show, their order, and their seed
    // widths come from the code defaultColumnArrangement(). We translate that
    // arrangement (field ids) into catalog indices once here, and cache the
    // field->seed-width map for seedWidthForField(). The visible columns reset to
    // the default arrangement (all native; defaults never include custom
    // columns); per-playlist order is layered back on afterwards via
    // applyColumnLayout().
    beginResetModel();
    m_schema = std::move(schema);

    // field id -> catalog index, for translating the code arrangement.
    QHash<int, int> indexOfField;
    indexOfField.reserve(static_cast<int>(m_schema.entries.size()));
    for (int i = 0; i < m_schema.entries.size(); ++i)
        indexOfField.insert(static_cast<int>(m_schema.entries.at(i).field), i);

    m_defaultArrangement.clear();
    m_seedWidths.clear();
    for (const DefaultColumn& dc : defaultColumnArrangement()) {
        m_seedWidths.insert(static_cast<int>(dc.field), dc.seedWidth);
        const int idx = indexOfField.value(static_cast<int>(dc.field), -1);
        if (idx >= 0) // a catalog always contains every field, so this holds
            m_defaultArrangement.append(idx);
    }

    // Seed the visible columns from the default arrangement (native fields).
    m_columns.clear();
    for (const int idx : m_defaultArrangement)
        m_columns.push_back(VisualColumn{ false, m_schema.entries.at(idx).field, {} });
    if (m_columns.isEmpty()) {
        // Defensive: an empty default arrangement shows the whole catalog rather
        // than a headerless table.
        for (const ColumnSchema::Entry& e : m_schema.entries)
            m_columns.push_back(VisualColumn{ false, e.field, {} });
    }
    endResetModel();
}

void PlaylistModel::setCustomColumns(const CustomColumnRegistry* registry) {
    if (m_customColumns == registry)
        return;
    if (m_customColumns)
        disconnect(m_customColumns, nullptr, this, nullptr);

    m_customColumns = registry;

    if (m_customColumns) {
        connect(m_customColumns, &CustomColumnRegistry::columnsChanged,
                this, &PlaylistModel::onCustomColumnsChanged);
        connect(m_customColumns, &CustomColumnRegistry::columnRemoved,
                this, &PlaylistModel::onCustomColumnRemoved);
    }
    // Any custom columns already shown (e.g. set before the registry was wired)
    // refresh against the now-available definitions.
    onCustomColumnsChanged();
}

void PlaylistModel::onCustomColumnsChanged() {
    // A definition changed (name/alignment/pattern) or one was added. Refresh
    // any SHOWN custom column's header + cells, and nudge QML to re-read
    // alignment (an invokable it can't track as a binding dependency).
    if (columnCount() > 0)
        emit headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);

    const int rows = rowCount();
    if (rows > 0) {
        for (int c = 0; c < m_columns.size(); ++c) {
            if (!m_columns.at(c).isCustom)
                continue;
            emit dataChanged(index(0, c), index(rows - 1, c));
        }
    }
    emit columnLayoutChanged();
}

void PlaylistModel::onCustomColumnRemoved(const QString& id) {
    // Count visual columns bound to this id (normally 0 or 1; applyColumnLayout
    // de-dupes custom ids, so >1 shouldn't occur, but we handle it).
    int matches = 0;
    for (const VisualColumn& vc : m_columns)
        if (vc.isCustom && vc.customId == id)
            ++matches;
    if (matches == 0)
        return;

    // Removing them all would leave zero columns, so reset to the default
    // arrangement instead of blanking the header. (Rare: the deleted custom
    // column was the only visible column.)
    if (matches >= m_columns.size()) {
        applyColumnLayout({}, {}); // empty -> defaults; resets the model
        emit columnLayoutChanged();
        return;
    }

    // Otherwise drop each bound column bottom-up so the bound selection model
    // remaps rather than resets.
    for (int c = m_columns.size() - 1; c >= 0; --c) {
        const VisualColumn& vc = m_columns.at(c);
        if (vc.isCustom && vc.customId == id) {
            beginRemoveColumns(QModelIndex(), c, c);
            m_columns.removeAt(c);
            endRemoveColumns();
        }
    }
    // Refresh the surviving header titles (same delegate-reuse staleness as
    // hideColumn; a full-span emit keeps this multi-removal path simple).
    if (!m_columns.isEmpty())
        emit headerDataChanged(Qt::Horizontal, 0,
                               static_cast<int>(m_columns.size()) - 1);
    emit columnLayoutChanged();
}

int PlaylistModel::seedWidthForField(ColumnField field) const {
    const auto it = m_seedWidths.constFind(static_cast<int>(field));
    return it != m_seedWidths.constEnd() ? *it
                                         : ColumnSchema::kDefaultColumnWidth;
}

bool PlaylistModel::moveVisualColumn(int from, int to) {
    const int n = static_cast<int>(m_columns.size());
    if (from == to || from < 0 || from >= n || to < 0 || to >= n)
        return false;

    // beginMoveColumns quirk: the destination is given in PRE-move coordinates,
    // and to land AFTER position `to` when moving right we must pass `to + 1`.
    // Moving left, the destination is `to` as-is. It also rejects a destination
    // inside [from, from+1], which our from==to guard already excludes.
    const int dest = (to > from) ? to + 1 : to;
    if (!beginMoveColumns(QModelIndex(), from, from, QModelIndex(), dest))
        return false;
    m_columns.move(from, to); // QList::move targets the final index
    endMoveColumns();

    // Refresh the shifted header titles. The independent HorizontalHeaderView's
    // cells get their title as an injected `display` property that is only
    // re-injected on a dataChanged; a move permutes the CELLS but leaves the
    // reused delegate items wearing their old titles (stale until something,
    // e.g. a tab switch, rebuilt them). headerDataChanged is the header's
    // documented refresh contract (the custom-column rename path already relies
    // on it); only [min, max] shifted, so only that span is emitted.
    emit headerDataChanged(Qt::Horizontal, std::min(from, to), std::max(from, to));
    return true;
}

int PlaylistModel::columnAlignment(int column) const {
    const VisualColumn* vc = columnAt(column);
    if (!vc)
        return static_cast<int>(Qt::AlignLeft);
    if (vc->isCustom) {
        const CustomColumn* rec =
            m_customColumns ? m_customColumns->byId(vc->customId) : nullptr;
        return static_cast<int>(rec ? rec->alignment : Qt::AlignLeft);
    }
    return static_cast<int>(defaultAlignmentFor(vc->field));
}

QVariantList PlaylistModel::defaultColumnWidths() const {
    // In VISUAL order, so the view can seed explicit widths positionally. Custom
    // columns have no per-field seed; they take the global default.
    QVariantList widths;
    widths.reserve(static_cast<int>(m_columns.size()));
    for (const VisualColumn& vc : m_columns)
        widths.append(vc.isCustom ? ColumnSchema::kDefaultColumnWidth
                                  : seedWidthForField(vc.field));
    return widths;
}

QStringList PlaylistModel::currentColumnOrder() const {
    QStringList order;
    order.reserve(static_cast<int>(m_columns.size()));
    for (const VisualColumn& vc : m_columns)
        order.append(vc.isCustom ? customFieldId(vc.customId)
                                 : stringFromField(vc.field));
    return order;
}

QVariantList PlaylistModel::applyColumnLayout(const QStringList& fieldIds,
                                              const QVariantList& widths) {
    const int n = static_cast<int>(m_schema.entries.size());
    QList<bool>         usedNative(n, false); // native dedup, by catalog index
    QSet<QString>       usedCustom;           // custom dedup, by record id
    QList<VisualColumn> newOrder;
    QVariantList        alignedWidths;
    newOrder.reserve(fieldIds.size());
    alignedWidths.reserve(fieldIds.size());

    // 1. Honor saved fields that still exist, in saved order; duplicates are
    //    consumed left-to-right. A saved width <= 0 (or missing) falls back to
    //    the seed width (field arrangement for native, kDefaultColumnWidth for
    //    custom). A "custom:<id>" whose record no longer exists is dropped, the
    //    same way a retired native field id is.
    for (int k = 0; k < fieldIds.size(); ++k) {
        const QString fid = fieldIds.at(k);
        const int     w   = (k < widths.size()) ? widths.at(k).toInt() : 0;

        if (isCustomFieldId(fid)) {
            const QString id = customIdFromFieldId(fid);
            if (id.isEmpty() || usedCustom.contains(id))
                continue;
            if (!m_customColumns || !m_customColumns->byId(id)) {
                // The saved layout names a custom column whose definition no
                // longer exists: deleted, or (since identity is a generated
                // uuid, not the name) re-created under a fresh id. Drop it, but
                // say so, else a column silently vanishes on load.
                qWarning("rawform: custom column \"%s\" referenced by this layout "
                         "has no matching definition; it will not be shown.",
                         qUtf8Printable(fid));
                continue;
            }
            usedCustom.insert(id);
            newOrder.append(VisualColumn{ true, ColumnField{}, id });
            alignedWidths.append(w > 0 ? w : ColumnSchema::kDefaultColumnWidth);
            continue;
        }

        const std::optional<ColumnField> f = fieldFromString(fid);
        if (!f) {
            // An unrecognized native field id (corrupt or hand-edited file).
            // Drop it, with a warning for parity with the custom case above.
            if (!fid.isEmpty())
                qWarning("rawform: unknown column field id \"%s\" in this layout; "
                         "it will not be shown.",
                         qUtf8Printable(fid));
            continue;
        }
        for (int i = 0; i < n; ++i) {
            if (usedNative.at(i) || m_schema.entries.at(i).field != *f)
                continue;
            usedNative[i] = true;
            newOrder.append(VisualColumn{ false, *f, {} });
            alignedWidths.append(w > 0 ? w : seedWidthForField(*f));
            break;
        }
    }

    // 2. Fall back to the DEFAULT ARRANGEMENT only when no layout was supplied.
    //    An empty list means "take the default columns" (fresh playlist / reset);
    //    a NON-empty list is taken literally: it defines exactly which columns
    //    are visible and in what order, so columns the user removed stay removed,
    //    and a field NOT in the default arrangement is shown only if listed.
    if (fieldIds.isEmpty()) {
        for (const int idx : m_defaultArrangement) {
            const ColumnField f = m_schema.entries.at(idx).field;
            newOrder.append(VisualColumn{ false, f, {} });
            alignedWidths.append(seedWidthForField(f));
        }
    }

    // Safety net: never end up with zero columns (a corrupt save, or every listed
    // field unknown/retired, would otherwise blank the header). Fall back to the
    // default arrangement, or the whole (native) catalog if even that is empty.
    if (newOrder.isEmpty()) {
        alignedWidths.clear();
        for (const int idx : m_defaultArrangement) {
            const ColumnField f = m_schema.entries.at(idx).field;
            newOrder.append(VisualColumn{ false, f, {} });
            alignedWidths.append(seedWidthForField(f));
        }
    }
    if (newOrder.isEmpty()) {
        for (int i = 0; i < n; ++i) {
            const ColumnField f = m_schema.entries.at(i).field;
            newOrder.append(VisualColumn{ false, f, {} });
            alignedWidths.append(seedWidthForField(f));
        }
    }

    // Load-time structural change: reset (there is no live selection to keep
    // across a playlist load, unlike an interactive moveColumn()).
    beginResetModel();
    m_columns = std::move(newOrder);
    endResetModel();

    return alignedWidths;
}

QVariantList PlaylistModel::availableColumns() const {
    // Which NATIVE fields are currently shown. Custom columns are not catalog
    // entries; they are offered separately via customColumnCatalog().
    QSet<int> shownNative;
    for (const VisualColumn& vc : m_columns)
        if (!vc.isCustom)
            shownNative.insert(static_cast<int>(vc.field));

    // Menu order, not schema/enum order: PLAIN fields alphabetically by their
    // display title, then the COMPOSITE presentation fields (see
    // isCompositeField) alphabetically, so the menu can draw a separator
    // between the groups. Locale-aware compare: titles are display strings.
    QList<ColumnSchema::Entry> sorted = m_schema.entries;
    std::ranges::sort(sorted,
              [](const ColumnSchema::Entry& a, const ColumnSchema::Entry& b) {
                  const bool ca = isCompositeField(a.field);
                  const bool cb = isCompositeField(b.field);
                  if (ca != cb)
                      return !ca; // plain group first
                  return QString::localeAwareCompare(humanTitleFor(a.field),
                                                     humanTitleFor(b.field)) < 0;
              });

    QVariantList out;
    out.reserve(static_cast<int>(sorted.size()));
    for (const ColumnSchema::Entry& e : sorted) {
        QVariantMap entry;
        entry.insert(QStringLiteral("fieldId"), stringFromField(e.field));
        entry.insert(QStringLiteral("title"), humanTitleFor(e.field));
        entry.insert(QStringLiteral("width"), seedWidthForField(e.field));
        entry.insert(QStringLiteral("visible"), shownNative.contains(static_cast<int>(e.field)));
        entry.insert(QStringLiteral("composite"), isCompositeField(e.field));
        out.append(entry);
    }
    return out;
}

QVariantList PlaylistModel::customColumnCatalog() const {
    QVariantList out;
    if (!m_customColumns)
        return out;

    // Which custom ids are currently shown.
    QSet<QString> shown;
    for (const VisualColumn& vc : m_columns)
        if (vc.isCustom)
            shown.insert(vc.customId);

    const QList<CustomColumn> all = m_customColumns->all();
    out.reserve(static_cast<int>(all.size()));
    for (const CustomColumn& c : all) {
        QVariantMap entry;
        entry.insert(QStringLiteral("fieldId"), customFieldId(c.id));
        entry.insert(QStringLiteral("title"), c.name);
        entry.insert(QStringLiteral("width"), ColumnSchema::kDefaultColumnWidth);
        entry.insert(QStringLiteral("visible"), shown.contains(c.id));
        out.append(entry);
    }
    return out;
}

QString PlaylistModel::columnFieldId(int column) const {
    const VisualColumn* vc = columnAt(column);
    if (!vc)
        return {};
    return vc->isCustom ? customFieldId(vc->customId) : stringFromField(vc->field);
}

void PlaylistModel::setTracks(QList<TrackData> tracks) {
    beginResetModel();
    m_tracks = std::move(tracks);
    endResetModel();
}

void PlaylistModel::insertTracks(int at, QList<TrackData> tracks) {
    if (tracks.isEmpty())
        return;
    const int n = static_cast<int>(m_tracks.size());
    if (at < 0 || at > n)
        at = n; // clamp / append (also catches a queued drop's stale target)

    const int count = static_cast<int>(tracks.size());
    beginInsertRows(QModelIndex(), at, at + count - 1);
    // Append the whole batch, then rotate it into place: one O(n + count) pass
    // (the scenario this prevents: a per-element insert(at + i, ...) shifts the
    // entire tail of heavy TrackData values once per inserted track, quadratic
    // for a large batch dropped into the middle of a large playlist, and the
    // scanner streams its batches through exactly this entry point). A pure
    // append (at == n, the common streaming case) makes the rotate a no-op
    // over an empty range.
    m_tracks.append(std::move(tracks));
    std::ranges::rotate(m_tracks.begin() + at, m_tracks.begin() + n, m_tracks.end());
    endInsertRows();
}

void PlaylistModel::refreshTrack(int row, TrackData track) {
    if (row < 0 || row >= m_tracks.size())
        return;
    m_tracks[row] = std::move(track);

    // Let bindings that can't observe dataChanged (the album-art source) know.
    ++m_dataRevision;
    emit dataRevisionChanged();

    const int lastCol = columnCount() > 0 ? columnCount() - 1 : 0;
    emit dataChanged(index(row, 0), index(row, lastCol)); // all roles
}

void PlaylistModel::removeTracks(const QVariantList& rows) {
    if (rows.isEmpty() || m_tracks.isEmpty())
        return;

    // Normalize to in-range, unique, ascending row indices.
    QList<int> idx;
    idx.reserve(static_cast<int>(rows.size()));
    for (const QVariant& v : rows) {
        bool ok = false;
        const int r = v.toInt(&ok);
        if (ok && r >= 0 && r < m_tracks.size())
            idx.append(r);
    }
    if (idx.isEmpty())
        return;
    std::ranges::sort(idx);
    const auto dup = std::ranges::unique(idx);
    idx.erase(dup.begin(), dup.end());

    // Delete contiguous runs from the bottom up so lower indices (including
    // those still queued in idx) stay valid as we go. The bracket lets the
    // view re-anchor once for the whole batch (see the signal's note).
    emit bulkRemovalStarted();
    int i = static_cast<int>(idx.size()) - 1;
    while (i >= 0) {
        const int last = idx.at(i);
        int first = last;
        while (i > 0 && idx.at(i - 1) == first - 1) {
            --i;
            first = idx.at(i);
        }
        beginRemoveRows(QModelIndex(), first, last);
        m_tracks.remove(first, last - first + 1);
        endRemoveRows();
        --i;
    }
    emit bulkRemovalFinished();
}

int PlaylistModel::removeUnavailableTracks() {
    if (m_tracks.isEmpty())
        return 0;

    // Collect missing rows in ascending order (m_tracks is already in row order).
    QList<int> idx;
    for (int r = 0; r < m_tracks.size(); ++r)
        if (!m_tracks.at(r).available)
            idx.append(r);
    if (idx.isEmpty())
        return 0;

    const int removed = static_cast<int>(idx.size());
    // Same bottom-up contiguous-run deletion (and bracket) as removeTracks(),
    // so the bound selection model remaps rather than resets.
    emit bulkRemovalStarted();
    int i = removed - 1;
    while (i >= 0) {
        const int last = idx.at(i);
        int first = last;
        while (i > 0 && idx.at(i - 1) == first - 1) {
            --i;
            first = idx.at(i);
        }
        beginRemoveRows(QModelIndex(), first, last);
        m_tracks.remove(first, last - first + 1);
        endRemoveRows();
        --i;
    }
    emit bulkRemovalFinished();
    return removed;
}

bool PlaylistModel::moveTracks(int first, int count, int dest) {
    const int n = static_cast<int>(m_tracks.size());
    const int last = first + count - 1;
    if (count <= 0 || first < 0 || last >= n)
        return false;
    if (dest < 0 || dest > n)
        return false;
    if (dest >= first && dest <= last + 1) // in-place: no move
        return false;

    // beginMoveRows takes the destination in PRE-move coordinates and rejects a
    // destination inside [first, last+1], already excluded above.
    if (!beginMoveRows(QModelIndex(), first, last, QModelIndex(), dest))
        return false;
    const QList<TrackData> block = m_tracks.mid(first, count);
    m_tracks.remove(first, count);
    // After removing the block, a downward move's insertion index shifts left
    // by count; an upward move inserts at dest unchanged.
    const int insertAt = (dest <= first) ? dest : dest - count;
    for (int k = 0; k < count; ++k)
        m_tracks.insert(insertAt + k, block.at(k));
    endMoveRows();
    return true;
}

QList<TrackData> PlaylistModel::snapshotTracks() const {
    return m_tracks; // explicit by-value copy for off-thread serialization
}

const TrackData* PlaylistModel::trackAt(int row) const {
    if (row < 0 || row >= m_tracks.size())
        return nullptr;
    return &m_tracks.at(row);
}

QString PlaylistModel::trackFilePath(int row) const {
    if (row < 0 || row >= m_tracks.size())
        return {};
    return m_tracks.at(row).filePath;
}

QString PlaylistModel::trackVersionTag(int row) const {
    if (row < 0 || row >= m_tracks.size())
        return {};
    const TrackData& t = m_tracks.at(row);
    return QString::number(t.modified.toMSecsSinceEpoch())
           + QLatin1Char('-') + QString::number(t.fileSize);
}

QVariantList PlaylistModel::replayGainRows(const QVariantList& rows) const {
    // One map per valid row. Display strings keep the tag's own precision (so a
    // "-4" tag and a "-6.00 dB" tag render side by side, precision intact); the
    // numbers are for the QML Summary aggregation and are absent (an invalid
    // QVariant, which reads as undefined in QML) when the tag is missing or
    // unparseable. Out-of-range rows are silently skipped, so a stale selection
    // cannot index past the end.
    QVariantList out;
    out.reserve(rows.size());
    for (const QVariant& rv : rows) {
        bool ok = false;
        const int row = rv.toInt(&ok);
        if (!ok || row < 0 || row >= m_tracks.size())
            continue;
        const TrackData& t = m_tracks.at(row);
        const replaygain::Values v = replaygain::read(t.extraTags);

        QString name = renderField(t, ColumnField::Title);
        if (name.isEmpty())
            name = t.fileName;

        QVariantMap m;
        m.insert(QStringLiteral("row"),        row);  // opaque staging key only
        m.insert(QStringLiteral("path"),       t.filePath);  // the write identity
        m.insert(QStringLiteral("name"),       name);
        // Album grouping key for "Scan album gain (multiple albums, by tags)",
        // the conventional grouping pattern: %album artist% | %date% | %album%
        // (album artist falls back to the track artist, and the date falls back
        // to the parsed year when no raw date string was tagged). Joined with a
        // unit separator so tag content containing a pipe cannot collide.
        // Case-sensitive exact match, deliberately. EMPTY when the album tag is
        // empty: the pane then treats the row as its own singleton group.
        QString albumKey;
        if (!t.album.isEmpty()) {
            const QString artistPart = !t.albumArtists.isEmpty()
                    ? t.albumArtists.join(QStringLiteral(", "))
                    : t.artists.join(QStringLiteral(", "));
            QString datePart = t.dateRaw;
            if (datePart.isEmpty() && t.year > 0)
                datePart = QString::number(t.year);
            albumKey = artistPart + QChar(0x1F) + datePart + QChar(0x1F) + t.album;
        }
        m.insert(QStringLiteral("albumKey"),   albumKey);
        m.insert(QStringLiteral("trackGain"),  replaygain::formatGainDisplay(v.trackGainRaw));
        m.insert(QStringLiteral("albumGain"),  replaygain::formatGainDisplay(v.albumGainRaw));
        m.insert(QStringLiteral("trackPeak"),  replaygain::formatPeakDisplay(v.trackPeakRaw));
        m.insert(QStringLiteral("albumPeak"),  replaygain::formatPeakDisplay(v.albumPeakRaw));
        out.append(m);
    }
    return out;
}

QList<int> PlaylistModel::selectedRowList(QItemSelectionModel* sel) const {
    // Range walk + bitmap sweep (see the header): never per-cell. The bitmap
    // doubles as the dedupe and the sort, since the sweep emits ascending.
    QList<int> out;
    if (!sel)
        return out;
    const QItemSelection ranges = sel->selection();
    if (ranges.isEmpty())
        return out;
    const int n = static_cast<int>(m_tracks.size());
    std::vector<bool> mark(static_cast<size_t>(n), false);
    int selected = 0;
    for (const QItemSelectionRange& r : ranges) {
        const int top = std::max(0, r.top());
        const int bottom = std::min(n - 1, r.bottom());
        for (int i = top; i <= bottom; ++i) {
            if (!mark[static_cast<size_t>(i)]) {
                mark[static_cast<size_t>(i)] = true;
                ++selected;
            }
        }
    }
    out.reserve(selected);
    for (int i = 0; i < n; ++i) {
        if (mark[static_cast<size_t>(i)])
            out.push_back(i);
    }
    return out;
}

QVariantList PlaylistModel::trackKeys(const QVariantList& rows) const {
    // One { path, subsong } map per valid row, input order preserved. This is
    // the durable identity the Properties window snapshots on open: paths
    // survive reorders and removals, and the subsong index keeps cue-style
    // siblings sharing one physical file distinct.
    QVariantList out;
    out.reserve(rows.size());
    for (const QVariant& rv : rows) {
        bool ok = false;
        const int row = rv.toInt(&ok);
        if (!ok || row < 0 || row >= m_tracks.size())
            continue;
        const TrackData& t = m_tracks.at(row);
        QVariantMap m;
        m.insert(QStringLiteral("path"),    t.filePath);
        m.insert(QStringLiteral("subsong"), t.subsongIndex);
        out.append(m);
    }
    return out;
}

QVariantList PlaylistModel::rowsForKeys(const QVariantList& keys) const {
    // Index the current tracks by identity, each identity holding its rows in
    // ascending order. A key then CONSUMES the first unclaimed row it matches,
    // so a selection that legitimately contained the same track twice (the
    // playlist holds duplicate entries) maps back to two distinct rows instead
    // of the same one twice. Keys whose identity is gone are dropped silently:
    // the caller's re-pull simply shrinks (its panes keep their copied data).
    QHash<QPair<QString, int>, QList<int>> byKey;
    byKey.reserve(static_cast<int>(m_tracks.size()));
    for (int row = 0; row < m_tracks.size(); ++row) {
        const TrackData& t = m_tracks.at(row);
        byKey[qMakePair(t.filePath, t.subsongIndex)].append(row);
    }

    QVariantList out;
    out.reserve(keys.size());
    for (const QVariant& kv : keys) {
        const QVariantMap m = kv.toMap();
        const auto id = qMakePair(m.value(QStringLiteral("path")).toString(),
                                  m.value(QStringLiteral("subsong")).toInt());
        const auto it = byKey.find(id);
        if (it == byKey.end() || it->isEmpty())
            continue;
        out.append(it->takeFirst());
    }
    return out;
}

namespace {

// A stable identity for a track's album art: rows that share this key are taken
// to share a cover, so a selection counts as "one album" iff every selected row
// yields the same key. Pure field/path comparison, no filesystem access, so the
// uniformity test stays cheap even for a select-all over a large playlist.
//
//  - Tagged (non-empty album): the album tag, as album title + album artist +
//    year, case-folded so trivial case differences do not split an album. This
//    groups a multi-disc album and a same-album set spread across folders under
//    one key, which is what lets a same-album selection show its cover whether
//    the art is an external sidecar or an embedded picture (the picture itself is
//    resolved later, per file, by AlbumArtProvider).
//  - Untagged (empty album): fall back to the row's FOLDER (the directory of the
//    file path), so loose untagged files in different folders are not collapsed
//    into one phantom album; same-folder untagged files still group, matching how
//    an external folder cover is shared.
QString albumArtKey(const TrackData& t) {
    const QString album = t.album.trimmed();
    if (!album.isEmpty()) {
        // The unit-separator join keeps two different field splits from aliasing
        // to one key (e.g. "A" + "BC" never equals "AB" + "C").
        const QChar sep(QChar(0x1F));
        return QStringLiteral("A:") + album.toCaseFolded()
             + sep + joinedValues(t.albumArtists).trimmed().toCaseFolded()
             + sep + QString::number(t.year);
    }
    // Folder fallback: take the directory portion of the absolute path WITHOUT a
    // QFileInfo (no stat), tolerating either separator since a library path can
    // be Windows-style even on a posix dev build.
    const QString& path     = t.filePath;
    const int      slash     = path.lastIndexOf(QLatin1Char('/'));
    const int      backslash = path.lastIndexOf(QLatin1Char('\\'));
    const int      cut       = std::max(slash, backslash);
    const QString  folder    = cut >= 0 ? path.left(cut) : path;
    return QStringLiteral("P:") + folder;
}

} // namespace

QString PlaylistModel::albumArtSourceForSelection(QItemSelectionModel* selection) const {
    if (!selection)
        return {};

    // The selected rows, one index per row (column 0). A valid current index with
    // an EMPTY set is treated as a single-row selection, so a click that did not
    // register as a range selection still shows that row's art; a null current
    // index with an empty set is "nothing selected" and clears the pane.
    QModelIndexList rowIdx = selection->selectedRows(0);
    if (rowIdx.isEmpty()) {
        const QModelIndex cur = selection->currentIndex();
        if (!cur.isValid())
            return {};
        rowIdx.append(cur.sibling(cur.row(), 0));
    }

    // Decide uniformity and pick a representative in one pass. The representative
    // is the current/anchor row when it is part of the selection and present,
    // else the first present selected row. "Present" gates on the row's
    // availability so a missing file never drives the art (it would only resolve
    // to the placeholder anyway).
    const QModelIndex curIdx = selection->currentIndex();
    const int currentRow     = curIdx.isValid() ? curIdx.row() : -1;

    QString sharedKey;
    bool    haveKey        = false;
    int     representative = -1;
    int     firstPresent   = -1;

    for (const QModelIndex& idx : rowIdx) {
        const int row = idx.row();
        if (row < 0 || row >= m_tracks.size())
            continue;
        const TrackData& t = m_tracks.at(row);

        const QString key = albumArtKey(t);
        if (!haveKey) {
            sharedKey = key;
            haveKey   = true;
        } else if (key != sharedKey) {
            // Two albums in the selection: clear the pane.
            return {};
        }

        if (t.available) {
            if (firstPresent < 0)
                firstPresent = row;
            if (row == currentRow)
                representative = row;
        }
    }

    if (!haveKey)
        return {};                  // selection held only out-of-range rows
    if (representative < 0)
        representative = firstPresent;
    if (representative < 0)
        return {};                  // uniform album, but no present file to show

    // Resolve exactly as a single selection does: the provider tries the sidecar
    // cover first, then the embedded picture, keyed by the file path; the "?v="
    // staleness tag (mtime+size) makes a re-read cover produce a new URL so it
    // reloads.
    return artUrlForRow(representative);
}

QString PlaylistModel::artUrlForRow(int row) const {
    if (row < 0 || row >= m_tracks.size())
        return {};
    const QString path = trackFilePath(row);
    if (path.isEmpty())
        return {};
    // Percent-encoded the same way QML's encodeURIComponent did; the provider
    // decodes with QUrl::fromPercentEncoding. The "?v=" tag is the audio
    // file's mtime+size PLUS the art epoch: the former catches a changed
    // track file, the latter (bumped per reloader refresh) catches a changed
    // SIDECAR cover, which the audio file's stats cannot see.
    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(path));
    return QStringLiteral("image://rawformart/") + encoded
         + QStringLiteral("?v=") + trackVersionTag(row)
         + QStringLiteral("-e") + QString::number(m_artEpoch);
}

QString PlaylistModel::albumArtSourceForRow(int row) const {
    if (row < 0 || row >= m_tracks.size())
        return {};
    if (!m_tracks.at(row).available)
        return {}; // a missing file only resolves to the placeholder anyway
    return artUrlForRow(row);
}

void PlaylistModel::selectRowRange(QItemSelectionModel* selection,
                                   int lo, int hi, bool clearFirst) {
    // Guard the wiring, not just the arguments: selecting THIS model's indexes
    // on a selection model bound to a DIFFERENT model (a stale QML binding
    // mid tab-switch) would corrupt that model's selection, so it is refused
    // outright rather than "best effort".
    if (!selection || selection->model() != this)
        return;

    const int rows = static_cast<int>(m_tracks.size());
    const int cols = columnCount();
    if (rows <= 0 || cols <= 0)
        return;
    if (lo > hi)
        std::swap(lo, hi);
    lo = std::max(lo, 0);
    hi = std::min(hi, rows - 1);
    if (lo > hi)
        return; // fully out of range: a no-op, never an implicit clear

    // ONE full-width range, applied with ONE select(). Spanning every column
    // explicitly (instead of the Rows flag on a column-0 range) keeps the
    // STORED selection in the shape per-row selects produce, so per-cell
    // `selected` reads in the view and selectedIndexes() consumers (delete,
    // drag-block detection) are undisturbed, while the selection model holds a
    // single range and emits a single selectionChanged. That single emission
    // is the whole point: the metadata pane re-aggregation and the album-art
    // scan run once per GESTURE, not once per row (a per-row loop is
    // quadratic; observed at minutes for 5000 rows).
    const QItemSelection sel(index(lo, 0), index(hi, cols - 1));
    selection->select(sel, clearFirst ? QItemSelectionModel::ClearAndSelect
                                      : QItemSelectionModel::Select);
}

void PlaylistModel::beginAdditiveRangeDrag(QItemSelectionModel* selection) {
    // Same wrong-model refusal as selectRowRange: snapshotting a foreign
    // model's selection would seed the session with indexes select() below
    // silently drops, so the baseline would lie.
    if (!selection || selection->model() != this)
        return;
    m_additiveDragBase   = selection->selection();
    m_additiveDragActive = true;
}

void PlaylistModel::updateAdditiveRangeDrag(QItemSelectionModel* selection,
                                            int lo, int hi) {
    if (!selection || selection->model() != this)
        return;
    // No open session: refuse. Applying against an empty baseline would be a
    // ClearAndSelect down to just the range, destroying the selection this
    // gesture exists to preserve.
    if (!m_additiveDragActive)
        return;

    const int rows = static_cast<int>(m_tracks.size());
    const int cols = columnCount();
    if (rows <= 0 || cols <= 0)
        return;
    if (lo > hi)
        std::swap(lo, hi);
    lo = std::max(lo, 0);
    hi = std::min(hi, rows - 1);
    if (lo > hi)
        return; // fully out of range: a no-op, same contract as selectRowRange

    // Baseline UNION range, applied as ONE ClearAndSelect. merge() with
    // Select is the union operator: it splits/dedupes overlapping ranges, so
    // a drag sweeping across baseline rows never stores duplicate rows (which
    // would double-count in selectedRows() consumers, e.g. the metadata
    // pane's aggregate fields). One select() means one selectionChanged per
    // ROW CHANGE of the drag, the same batching discipline as selectRowRange.
    QItemSelection out = m_additiveDragBase;
    out.merge(QItemSelection(index(lo, 0), index(hi, cols - 1)),
              QItemSelectionModel::Select);
    selection->select(out, QItemSelectionModel::ClearAndSelect);
}

void PlaylistModel::endAdditiveRangeDrag() {
    // Drop the snapshot, not just the gate: its ranges hold persistent
    // indexes the model would otherwise keep updating on every row change
    // for as long as the (invisible) baseline lingered.
    m_additiveDragBase   = QItemSelection();
    m_additiveDragActive = false;
}

bool PlaylistModel::showColumn(const QString& fieldId) {
    // Resolve the id with the same rules applyColumnLayout uses (custom ids
    // must have a live registry definition; native ids must exist in the
    // schema catalog), refusing duplicates so a double-fired menu action can't
    // show a column twice.
    VisualColumn vc;
    if (isCustomFieldId(fieldId)) {
        const QString id = customIdFromFieldId(fieldId);
        if (id.isEmpty() || !m_customColumns || !m_customColumns->byId(id))
            return false;
        for (const VisualColumn& c : m_columns)
            if (c.isCustom && c.customId == id)
                return false; // already shown
        vc = VisualColumn{ true, ColumnField{}, id };
    } else {
        const std::optional<ColumnField> f = fieldFromString(fieldId);
        if (!f)
            return false;
        bool inCatalog = false;
        for (const ColumnSchema::Entry& e : m_schema.entries) {
            if (e.field == *f) {
                inCatalog = true;
                break;
            }
        }
        if (!inCatalog)
            return false;
        for (const VisualColumn& c : m_columns)
            if (!c.isCustom && c.field == *f)
                return false; // already shown
        vc = VisualColumn{ false, *f, {} };
    }

    // Append as the last visual column. A granular INSERT, never a reset (see
    // the header doc): the selection model remaps and the playback cursor
    // survives. The user reorders from there if they want it elsewhere.
    const int at = static_cast<int>(m_columns.size());
    beginInsertColumns(QModelIndex(), at, at);
    m_columns.append(vc);
    endInsertColumns();
    emit columnLayoutChanged(); // alignment bindings re-read (invokable-based)
    return true;
}

bool PlaylistModel::hideColumn(const QString& fieldId) {
    // Never remove the last column: a blank header has no affordance to get
    // columns back (the menu hangs off a header cell).
    if (m_columns.size() <= 1)
        return false;

    int at = -1;
    for (int c = 0; c < m_columns.size(); ++c) {
        if (columnFieldId(c) == fieldId) {
            at = c;
            break;
        }
    }
    if (at < 0)
        return false; // not shown

    // Granular REMOVE, never a reset (see showColumn). The bound selection
    // model shrinks its own ranges across begin/endRemoveColumns; the playback
    // cursor's column-0 anchor is re-pinned by AudioController when this
    // removes visual column 0.
    beginRemoveColumns(QModelIndex(), at, at);
    m_columns.removeAt(at);
    endRemoveColumns();
    // Refresh the shifted header titles: every section at/after the removed
    // index slid left, but the reused header delegates keep their injected
    // `display` until a dataChanged (see moveVisualColumn). Removing the LAST
    // column shifts nothing, hence the guard.
    if (at < m_columns.size())
        emit headerDataChanged(Qt::Horizontal, at,
                               static_cast<int>(m_columns.size()) - 1);
    emit columnLayoutChanged();
    return true;
}

bool PlaylistModel::isColumnShown(const QString& fieldId) const {
    for (int c = 0; c < m_columns.size(); ++c)
        if (columnFieldId(c) == fieldId)
            return true;
    return false;
}

void PlaylistModel::reselectFullWidth(QItemSelectionModel* selection) {
    if (!selection || selection->model() != this)
        return; // same wrong-model refusal as selectRowRange
    const int cols = columnCount();
    if (cols <= 0)
        return;

    const QItemSelection cur = selection->selection();
    if (cur.isEmpty())
        return;

    // Collect the selected ROW spans and merge overlapping/adjacent ones. The
    // stored ranges are normally disjoint (QItemSelectionModel maintains that
    // through merge), but this helper must not rely on it: hand-built overlaps
    // passed straight to select() would be stored as-is, and duplicate rows
    // then double-count in selectedRows() consumers (the metadata pane's
    // aggregate fields, e.g. Total size).
    QList<std::pair<int, int>> spans;
    spans.reserve(static_cast<int>(cur.size()));
    for (const QItemSelectionRange& r : cur) {
        if (r.isValid())
            spans.append({ r.top(), r.bottom() });
    }
    if (spans.isEmpty())
        return;
    std::ranges::sort(spans);

    QItemSelection full;
    int top = spans.first().first;
    int bot = spans.first().second;
    for (qsizetype i = 1; i < spans.size(); ++i) {
        const auto& s = spans.at(i);
        if (s.first <= bot + 1) { // overlapping or adjacent: extend the span
            bot = std::max(bot, s.second);
            continue;
        }
        full.append(QItemSelectionRange(index(top, 0), index(bot, cols - 1)));
        top = s.first;
        bot = s.second;
    }
    full.append(QItemSelectionRange(index(top, 0), index(bot, cols - 1)));

    // ONE replace, one selectionChanged; current index and anchor untouched.
    selection->select(full, QItemSelectionModel::ClearAndSelect);
}

QString PlaylistModel::renderField(const TrackData& t, ColumnField field) {
    // Thin forwarder. The per-field rendering lives in PatternEvaluator
    // (renderFieldValue), a Quick-free unit, so the custom-column evaluator
    // reuses the exact same rendering and it stays unit-testable. This static
    // entry point is kept for the existing callers (data(), replayGainRows()).
    return renderFieldValue(t, field);
}

} // namespace rawform
