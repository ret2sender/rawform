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

// ReplayGainRowsModel.cpp
//
// Implementation of the ReplayGain rows model: population from PlaylistModel's
// row list in one reset, the per-cell field plumbing and strict value
// validation, row selection, staging with the revert-on-equal rule, scan scope
// resolution and result staging, the collect / adopt / revert apply path, and
// the Summary recomputation.

#include "ReplayGainRowsModel.h"

#include "media/ReplayGainTags.h"

#include <QHash>
#include <QSet>
#include <QVariantMap>

#include <cmath>

namespace rawform {

namespace {

// The guillemet-wrapped "multiple values" marker, built from QChars so the
// source stays pure ASCII (the same convention, and the same rendered marker,
// as the metadata models).
QString multipleValuesMarker() {
    static const QString kMarker =
        QChar(0x00AB) + QStringLiteral("multiple values") + QChar(0x00BB);
    return kMarker;
}

} // namespace

ReplayGainRowsModel::ReplayGainRowsModel(QObject* parent)
    : QAbstractListModel(parent) {}

// --- QAbstractListModel ----------------------------------------------------

int ReplayGainRowsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant ReplayGainRowsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const Row& r = m_rows.at(index.row());
    switch (role) {
    case NameRole: return r.name;
    case SelRole:  return r.sel;
    case TgRole:   return r.tg;
    case AgRole:   return r.ag;
    case TpRole:   return r.tp;
    case ApRole:   return r.ap;
    case Tg0Role:  return r.tg0;
    case Ag0Role:  return r.ag0;
    case Tp0Role:  return r.tp0;
    case Ap0Role:  return r.ap0;
    default:       return {};
    }
}

QHash<int, QByteArray> ReplayGainRowsModel::roleNames() const {
    // The names the pane's delegate binds (model.tg, model.tg0, model.sel);
    // see the header.
    return {
        { NameRole, QByteArrayLiteral("name") },
        { SelRole,  QByteArrayLiteral("sel")  },
        { TgRole,   QByteArrayLiteral("tg")   },
        { AgRole,   QByteArrayLiteral("ag")   },
        { TpRole,   QByteArrayLiteral("tp")   },
        { ApRole,   QByteArrayLiteral("ap")   },
        { Tg0Role,  QByteArrayLiteral("tg0")  },
        { Ag0Role,  QByteArrayLiteral("ag0")  },
        { Tp0Role,  QByteArrayLiteral("tp0")  },
        { Ap0Role,  QByteArrayLiteral("ap0")  },
    };
}

// --- population ------------------------------------------------------------

void ReplayGainRowsModel::setSelection(PlaylistModel* playlist, const QVariantList& rows) {
    beginResetModel();
    m_rows.clear();
    m_anchor = -1;
    if (playlist) {
        // C++-to-C++ ingestion through the single authority (see the header).
        // The maps are implicitly shared views over PlaylistModel's strings;
        // nothing here touches the JS heap, which is why this model is C++.
        const QVariantList src = playlist->replayGainRows(rows);
        m_rows.reserve(src.size());
        for (const QVariant& rv : src) {
            const QVariantMap m = rv.toMap();
            Row r;
            r.name        = m.value(QStringLiteral("name")).toString();
            r.path        = m.value(QStringLiteral("path")).toString();
            r.albumKey    = m.value(QStringLiteral("albumKey")).toString();
            r.playlistRow = m.value(QStringLiteral("row")).toInt();
            r.tg = r.tg0  = m.value(QStringLiteral("trackGain")).toString();
            r.ag = r.ag0  = m.value(QStringLiteral("albumGain")).toString();
            r.tp = r.tp0  = m.value(QStringLiteral("trackPeak")).toString();
            r.ap = r.ap0  = m.value(QStringLiteral("albumPeak")).toString();
            m_rows.push_back(std::move(r));
        }
    }
    endResetModel();
    emit countChanged();
    recomputeSummary();
}

void ReplayGainRowsModel::clear() {
    setSelection(nullptr, {});
}

// --- field plumbing --------------------------------------------------------

QString& ReplayGainRowsModel::cur(Row& r, int field) {
    switch (field) {
    case AlbumGain: return r.ag;
    case TrackPeak: return r.tp;
    case AlbumPeak: return r.ap;
    case TrackGain:
    default:        return r.tg;
    }
}

const QString& ReplayGainRowsModel::cur(const Row& r, int field) {
    return cur(const_cast<Row&>(r), field);
}

QString& ReplayGainRowsModel::orig(Row& r, int field) {
    switch (field) {
    case AlbumGain: return r.ag0;
    case TrackPeak: return r.tp0;
    case AlbumPeak: return r.ap0;
    case TrackGain:
    default:        return r.tg0;
    }
}

const QString& ReplayGainRowsModel::orig(const Row& r, int field) {
    return orig(const_cast<Row&>(r), field);
}

int ReplayGainRowsModel::roleFor(int field) {
    switch (field) {
    case AlbumGain: return AgRole;
    case TrackPeak: return TpRole;
    case AlbumPeak: return ApRole;
    case TrackGain:
    default:        return TgRole;
    }
}

// --- comparison + validation -----------------------------------------------

bool ReplayGainRowsModel::fieldChanged(const QString& curV, const QString& origV,
                                       bool peak) {
    const QString cs = curV.trimmed();
    const QString os = origV.trimmed();
    const bool cEmpty = cs.isEmpty();
    const bool oEmpty = os.isEmpty();
    if (cEmpty && oEmpty)
        return false;
    if (cEmpty != oEmpty)
        return true;
    if (peak) {
        const auto pc = replaygain::parsePeak(cs);
        const auto po = replaygain::parsePeak(os);
        if (!pc || !po)
            return cs != os;
        return QString::number(*pc, 'f', 6) != QString::number(*po, 'f', 6);
    }
    const auto gc = replaygain::parseGainDb(cs);
    const auto go = replaygain::parseGainDb(os);
    if (!gc || !go)
        return cs != os;
    return replaygain::formatGainValue(*gc) != replaygain::formatGainValue(*go);
}

// --- selection -------------------------------------------------------------

void ReplayGainRowsModel::select(int row, int modifiers) {
    const int n = static_cast<int>(m_rows.size());
    if (row < 0 || row >= n)
        return;
    // ControlModifier is the Command key on macOS (Qt swaps Ctrl and Cmd), so
    // Meta is accepted as well.
    const bool ctrl = (modifiers & Qt::ControlModifier) || (modifiers & Qt::MetaModifier);
    const bool shift = (modifiers & Qt::ShiftModifier);
    if (shift && m_anchor >= 0) {
        const int lo = std::min(m_anchor, row);
        const int hi = std::max(m_anchor, row);
        for (int k = 0; k < n; ++k)
            m_rows[k].sel = (k >= lo && k <= hi);
        emit dataChanged(index(0), index(n - 1), { SelRole });
    } else if (ctrl) {
        m_rows[row].sel = !m_rows.at(row).sel;
        m_anchor = row;
        emit dataChanged(index(row), index(row), { SelRole });
    } else {
        for (int j = 0; j < n; ++j)
            m_rows[j].sel = (j == row);
        m_anchor = row;
        emit dataChanged(index(0), index(n - 1), { SelRole });
    }
    recomputeSummary();
}

void ReplayGainRowsModel::selectAll() {
    const int n = static_cast<int>(m_rows.size());
    if (n == 0)
        return;
    for (Row& r : m_rows)
        r.sel = true;
    m_anchor = 0;
    emit dataChanged(index(0), index(n - 1), { SelRole });
    recomputeSummary();
}

void ReplayGainRowsModel::clearSelection() {
    const int n = static_cast<int>(m_rows.size());
    m_anchor = -1;
    if (n == 0)
        return;
    for (Row& r : m_rows)
        r.sel = false;
    emit dataChanged(index(0), index(n - 1), { SelRole });
    recomputeSummary();
}

// --- staging ---------------------------------------------------------------

bool ReplayGainRowsModel::validInput(const QString& raw, int field) const {
    const QString t = raw.trimmed();
    if (t.isEmpty())
        return true; // a clear is always committable
    return isPeak(field) ? replaygain::parsePeak(t).has_value()
                         : replaygain::parseGainDb(t).has_value();
}

bool ReplayGainRowsModel::applyCell(int i, int field, const QString& value) {
    Row& r = m_rows[i];
    const QString& o = orig(r, field);
    // Revert-on-equal: unchanged at write precision stores the ORIGINAL string
    // verbatim, so the cell neither dirties nor reformats and plain string
    // inequality stays the correct dirty test downstream.
    const QString next = fieldChanged(value, o, isPeak(field)) ? value : o;
    QString& c = cur(r, field);
    if (c == next)
        return false;
    c = next;
    return true;
}

void ReplayGainRowsModel::commitCell(int row, int field, const QString& value) {
    if (row < 0 || row >= m_rows.size())
        return;
    if (applyCell(row, field, value)) {
        emit dataChanged(index(row), index(row), { roleFor(field) });
        recomputeSummary();
    }
}

void ReplayGainRowsModel::setScoped(int field, const QString& value, bool allRows) {
    const int n = static_cast<int>(m_rows.size());
    int lo = -1;
    int hi = -1;
    for (int i = 0; i < n; ++i) {
        if (!allRows && m_selCount > 0 && !m_rows.at(i).sel)
            continue;
        if (applyCell(i, field, value)) {
            if (lo < 0)
                lo = i;
            hi = i;
        }
    }
    if (lo >= 0) {
        // One span, one role: over-notifying unchanged mid-span rows is far
        // cheaper than a per-row signal storm, and only visible delegates
        // re-read anyway.
        emit dataChanged(index(lo), index(hi), { roleFor(field) });
        recomputeSummary();
    }
}

void ReplayGainRowsModel::clearAllReplayGain() {
    setScoped(TrackGain, {}, true);
    setScoped(AlbumGain, {}, true);
    setScoped(TrackPeak, {}, true);
    setScoped(AlbumPeak, {}, true);
}

// --- scanning --------------------------------------------------------------

QList<int> ReplayGainRowsModel::scopedIndices(bool allRows) const {
    QList<int> out;
    const int n = static_cast<int>(m_rows.size());
    out.reserve(n);
    const bool useSel = !allRows && m_selCount > 0;
    for (int i = 0; i < n; ++i) {
        if (!useSel || m_rows.at(i).sel)
            out.push_back(i);
    }
    return out;
}

QVariantList ReplayGainRowsModel::scanItems(int mode, bool allRows,
                                            bool skipExisting) const {
    const QList<int> scoped = scopedIndices(allRows);
    QVariantList items;
    if (scoped.isEmpty())
        return items;

    const auto hasGain = [](const QString& s) { return !s.trimmed().isEmpty(); };
    const auto pushItem = [this, &items](int i, const QString& albumKey = {}) {
        const Row& r = m_rows.at(i);
        QVariantMap m;
        m.insert(QStringLiteral("row"),  r.playlistRow);
        m.insert(QStringLiteral("path"), r.path);
        m.insert(QStringLiteral("name"), r.name);
        if (!albumKey.isEmpty())
            m.insert(QStringLiteral("albumKey"), albumKey);
        items.append(m);
    };

    if (mode == 2) {
        // Bucket the scope by album key, first-seen order. Empty-key rows each
        // form their own singleton group, keyed by a sentinel no real tag can
        // produce (a control character prefix).
        QHash<QString, QList<int>> groups;
        QStringList order;
        for (int i : scoped) {
            QString k = m_rows.at(i).albumKey;
            if (k.isEmpty())
                k = QChar(0x01) + QStringLiteral("solo:") + QString::number(i);
            auto it = groups.find(k);
            if (it == groups.end()) {
                groups.insert(k, { i });
                order.push_back(k);
            } else {
                it->push_back(i);
            }
        }
        for (const QString& k : order) {
            const QList<int>& idxs = groups.value(k);
            if (skipExisting) {
                // Whole-or-nothing per GROUP: fully tagged albums drop out,
                // partially tagged ones rescan complete (a partial skip would
                // corrupt the album combine).
                bool allHave = true;
                for (int i : idxs) {
                    if (!hasGain(m_rows.at(i).ag0)) {
                        allHave = false;
                        break;
                    }
                }
                if (allHave)
                    continue;
            }
            for (int i : idxs)
                pushItem(i, k);
        }
    } else if (mode == 1) {
        if (skipExisting) {
            bool allHaveAlbum = true;
            for (int i : scoped) {
                if (!hasGain(m_rows.at(i).ag0)) {
                    allHaveAlbum = false;
                    break;
                }
            }
            if (allHaveAlbum)
                return items; // empty: the pane words the noop
        }
        for (int i : scoped)
            pushItem(i);
    } else {
        for (int i : scoped) {
            if (skipExisting && hasGain(m_rows.at(i).tg0))
                continue;
            pushItem(i);
        }
    }
    return items;
}

void ReplayGainRowsModel::stageScanResults(const QVariantList& results) {
    const int n = static_cast<int>(m_rows.size());
    QHash<int, int> idxByRow;
    idxByRow.reserve(n);
    for (int i = 0; i < n; ++i)
        idxByRow.insert(m_rows.at(i).playlistRow, i);

    int lo = -1;
    int hi = -1;
    const auto apply = [this, &lo, &hi](int i, int field, const QVariant& v) {
        if (!v.isValid())
            return;
        if (applyCell(i, field, v.toString())) {
            if (lo < 0 || i < lo)
                lo = i;
            if (i > hi)
                hi = i;
        }
    };
    for (const QVariant& rv : results) {
        const QVariantMap res = rv.toMap();
        const auto it = idxByRow.constFind(res.value(QStringLiteral("row")).toInt());
        if (it == idxByRow.constEnd())
            continue;
        const int i = it.value();
        apply(i, TrackGain, res.value(QStringLiteral("trackGain")));
        apply(i, TrackPeak, res.value(QStringLiteral("trackPeak")));
        apply(i, AlbumGain, res.value(QStringLiteral("albumGain")));
        apply(i, AlbumPeak, res.value(QStringLiteral("albumPeak")));
    }
    if (lo >= 0) {
        // One span across all four value roles, ONE recompute for the whole
        // batch: a library-wide scan completion stays linear.
        emit dataChanged(index(lo), index(hi), { TgRole, AgRole, TpRole, ApRole });
        recomputeSummary();
    }
}

// --- apply / revert --------------------------------------------------------

QVariantList ReplayGainRowsModel::collectEdits() const {
    QVariantList edits;
    for (const Row& r : m_rows) {
        QVariantMap d;
        bool changed = false;
        if (r.tg != r.tg0) { d.insert(QStringLiteral("trackGain"), r.tg); changed = true; }
        if (r.ag != r.ag0) { d.insert(QStringLiteral("albumGain"), r.ag); changed = true; }
        if (r.tp != r.tp0) { d.insert(QStringLiteral("trackPeak"), r.tp); changed = true; }
        if (r.ap != r.ap0) { d.insert(QStringLiteral("albumPeak"), r.ap); changed = true; }
        if (changed) {
            d.insert(QStringLiteral("row"),  r.playlistRow);
            d.insert(QStringLiteral("path"), r.path);
            edits.append(d);
        }
    }
    return edits;
}

void ReplayGainRowsModel::adoptEdits(const QStringList& failedPaths) {
    const int n = static_cast<int>(m_rows.size());
    if (n == 0)
        return;
    QSet<QString> failed;
    failed.reserve(failedPaths.size());
    for (const QString& p : failedPaths)
        failed.insert(p);
    for (Row& r : m_rows) {
        if (failed.contains(r.path)) {
            // The write did not land: staged values revert to the originals,
            // the same outcome the re-pull path gives a failed file.
            r.tg = r.tg0; r.ag = r.ag0; r.tp = r.tp0; r.ap = r.ap0;
        } else {
            // The values ARE on disk: they become the baseline.
            r.tg0 = r.tg; r.ag0 = r.ag; r.tp0 = r.tp; r.ap0 = r.ap;
        }
    }
    emit dataChanged(index(0), index(n - 1),
                     { TgRole, AgRole, TpRole, ApRole,
                       Tg0Role, Ag0Role, Tp0Role, Ap0Role });
    recomputeSummary(); // everything is clean (or reverted) now
}

void ReplayGainRowsModel::revertEdits() {
    const int n = static_cast<int>(m_rows.size());
    m_anchor = -1;
    if (n == 0)
        return;
    for (Row& r : m_rows) {
        r.tg = r.tg0; r.ag = r.ag0; r.tp = r.tp0; r.ap = r.ap0;
        r.sel = false;
    }
    emit dataChanged(index(0), index(n - 1),
                     { SelRole, TgRole, AgRole, TpRole, ApRole });
    recomputeSummary();
}

// --- summary ---------------------------------------------------------------

void ReplayGainRowsModel::recomputeSummary() {
    // The single pass: gather selection count, dirtiness, and the
    // gain / peak populations for BOTH scopes in one walk, then pick the live
    // scope (decidable only once the selection is counted).
    const int n = static_cast<int>(m_rows.size());
    int selCount = 0;
    bool anyDirty = false;
    QList<double> tgAll, agAll, pkAll;
    QList<double> tgSel, agSel, pkSel;
    tgAll.reserve(n); agAll.reserve(n); pkAll.reserve(n);
    for (const Row& r : m_rows) {
        if (!anyDirty
            && (r.tg != r.tg0 || r.ag != r.ag0 || r.tp != r.tp0 || r.ap != r.ap0))
            anyDirty = true;
        const auto tg = replaygain::parseGainDb(r.tg);
        const auto ag = replaygain::parseGainDb(r.ag);
        auto pk = replaygain::parsePeak(r.tp);
        if (!pk)
            pk = replaygain::parsePeak(r.ap);
        if (tg) tgAll.push_back(*tg);
        if (ag) agAll.push_back(*ag);
        if (pk) pkAll.push_back(*pk);
        if (r.sel) {
            ++selCount;
            if (tg) tgSel.push_back(*tg);
            if (ag) agSel.push_back(*ag);
            if (pk) pkSel.push_back(*pk);
        }
    }
    const bool hasSel = selCount > 0;
    const int scopeCount = hasSel ? selCount : n;
    const QList<double>& tgs = hasSel ? tgSel : tgAll;
    const QList<double>& ags = hasSel ? agSel : agAll;
    const QList<double>& pks = hasSel ? pkSel : pkAll;

    const auto allEqual = [](const QList<double>& a) {
        for (qsizetype i = 1; i < a.size(); ++i) {
            if (std::abs(a.at(i) - a.at(0)) > 1e-9)
                return false;
        }
        return true;
    };
    const auto gainSummary = [&](const QList<double>& vals) -> QString {
        if (scopeCount == 0 || vals.isEmpty())
            return {};
        if (vals.size() == scopeCount && allEqual(vals))
            return replaygain::formatGainValue(vals.first());
        return multipleValuesMarker();
    };
    const auto minOf = [](const QList<double>& a) {
        double m = a.first();
        for (double v : a) m = std::min(m, v);
        return m;
    };
    const auto maxOf = [](const QList<double>& a) {
        double m = a.first();
        for (double v : a) m = std::max(m, v);
        return m;
    };

    m_selCount = selCount;
    m_tgSummary = gainSummary(tgs);
    m_agSummary = gainSummary(ags);
    m_lowest  = tgs.isEmpty() ? QStringLiteral("n/a") : replaygain::formatGainValue(minOf(tgs));
    m_highest = tgs.isEmpty() ? QStringLiteral("n/a") : replaygain::formatGainValue(maxOf(tgs));
    m_peak    = pks.isEmpty() ? QStringLiteral("n/a")
                              : QString::number(maxOf(pks), 'f', 6);
    emit summaryChanged();
    if (m_dirty != anyDirty) {
        m_dirty = anyDirty;
        emit dirtyChanged();
    }
}

} // namespace rawform
