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

// PlaylistSearch.cpp
//
// See the header for the contract and the two cadences. Dirty flags plus one
// single-shot timer are the whole scheduling story: every model or registry
// signal marks what it invalidates and arms the timer; the timeout does the
// expensive work once, in dependency order (tag keys -> parse -> haystacks ->
// match). Only a Filter edit parses synchronously, for the underline.

#include "search/PlaylistSearch.h"

#include "columns/ColumnSchema.h" // fieldFromString, isCustomFieldId, customIdFromFieldId

#include <QAbstractItemModel>
#include <QVariantMap>

#include <algorithm>
#include <optional>

namespace rawform {

using search::Searchable;

namespace {

/// The coalescing window for the O(rows) work (R6).
constexpr int kCoalesceMs = 120;

} // namespace

PlaylistSearch::PlaylistSearch(QObject* parent) : QObject(parent) {
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(kCoalesceMs);
    m_coalesce.callOnTimeout(this, &PlaylistSearch::onCoalesceTimeout);
}

// ---------------------------------------------------------------------------
// Property reads
// ---------------------------------------------------------------------------

QVariantList PlaylistSearch::entryDiagnostics() const {
    QVariantList out;
    out.reserve(static_cast<int>(m_parsed.entries.size()));
    for (const search::FilterEntry& e : m_parsed.entries) {
        QVariantMap m;
        m.insert(QStringLiteral("start"), static_cast<int>(e.start));
        m.insert(QStringLiteral("length"), static_cast<int>(e.length));
        m.insert(QStringLiteral("valid"), e.valid);
        m.insert(QStringLiteral("text"), e.text);
        out.append(m);
    }
    return out;
}

bool PlaylistSearch::filterHasInvalid() const {
    return m_parsed.hasInvalid();
}

bool PlaylistSearch::active() const {
    return m_model && m_matchesValid && !m_terms.isEmpty() && !m_searchSet.isEmpty();
}

int PlaylistSearch::matchCount() const {
    return active() ? static_cast<int>(m_matches.size()) : 0;
}

// ---------------------------------------------------------------------------
// Property writes
// ---------------------------------------------------------------------------

void PlaylistSearch::setModel(PlaylistModel* model) {
    if (model == m_model.data()) {
        return;
    }
    rebindModel(model);
    // The match set describes the OLD model until the next pass; until then
    // the search is not in force (active() false), so the proxy passes every
    // row of the new playlist rather than hiding by stale row numbers.
    m_matchesValid = false;
    emit modelChanged();
    emit matchesChanged();
    // A new playlist: new tag keys, possibly a new visible-column set, and
    // every haystack. Parse now so the diagnostics track the new context at
    // once; the rows follow on the timer.
    refreshExtraTagKeys();
    reparse();
}

void PlaylistSearch::setCustomColumns(CustomColumnRegistry* registry) {
    if (registry == m_registry.data()) {
        return;
    }
    for (const QMetaObject::Connection& c : m_registryConnections) {
        QObject::disconnect(c);
    }
    m_registryConnections.clear();
    m_registry = registry;
    if (registry) {
        // A definition edit can change a bare-name resolution or a shown
        // custom column's pattern, either of which changes the search set.
        m_registryConnections << connect(registry, &CustomColumnRegistry::columnsChanged,
                                         this, &PlaylistSearch::reparse);
        m_registryConnections << connect(registry, &CustomColumnRegistry::columnRemoved,
                                         this, &PlaylistSearch::reparse);
    }
    emit customColumnsChanged();
    reparse();
}

void PlaylistSearch::setQueryText(const QString& text) {
    if (text == m_queryText) {
        return;
    }
    m_queryText = text;
    m_terms     = search::queryTerms(text);
    emit queryTextChanged();
    scheduleRematch();
}

void PlaylistSearch::setFilterText(const QString& text) {
    if (text == m_filterText) {
        return;
    }
    m_filterText = text;
    emit filterTextChanged();
    reparse();
}

// ---------------------------------------------------------------------------
// Match access
// ---------------------------------------------------------------------------

bool PlaylistSearch::accepts(int row) const {
    if (!active() || row < 0) {
        return false;
    }
    return std::ranges::binary_search(m_matches, row);
}

// ---------------------------------------------------------------------------
// Rebuild pipeline
// ---------------------------------------------------------------------------

void PlaylistSearch::rebindModel(PlaylistModel* model) {
    for (const QMetaObject::Connection& c : m_modelConnections) {
        QObject::disconnect(c);
    }
    m_modelConnections.clear();
    m_model = model;
    if (!model) {
        return;
    }
    // Rows: any structural or content change can add, drop or move matches
    // and can introduce a tag key the validation has not seen. Columns: the
    // visible set is the default search set. All coalesced through the timer;
    // the tag-key refresh rides the same timeout so a scan's batch inserts
    // cost one pass per window, not one per batch.
    const auto rowsChanged = [this]() {
        m_tagKeysDirty   = true;
        m_reparseDirty   = true;
        m_haystacksDirty = true;
        m_coalesce.start();
    };
    const auto columnsChanged = [this]() {
        m_reparseDirty   = true;
        m_haystacksDirty = true;
        m_coalesce.start();
    };
    using M = QAbstractItemModel;
    m_modelConnections << connect(model, &M::rowsInserted, this, rowsChanged);
    m_modelConnections << connect(model, &M::rowsRemoved, this, rowsChanged);
    m_modelConnections << connect(model, &M::rowsMoved, this, rowsChanged);
    m_modelConnections << connect(model, &M::modelReset, this, rowsChanged);
    m_modelConnections << connect(model, &M::dataChanged, this, rowsChanged);
    m_modelConnections << connect(model, &M::columnsInserted, this, columnsChanged);
    m_modelConnections << connect(model, &M::columnsRemoved, this, columnsChanged);
    m_modelConnections << connect(model, &M::columnsMoved, this, columnsChanged);
    m_modelConnections << connect(model, &PlaylistModel::columnLayoutChanged, this,
                                  columnsChanged);
}

void PlaylistSearch::refreshExtraTagKeys() {
    m_extraTagKeys.clear();
    m_tagKeysDirty = false;
    if (!m_model) {
        return;
    }
    const int n = m_model->rowCount();
    for (int r = 0; r < n; ++r) {
        if (const TrackData* t = m_model->trackAt(r)) {
            const auto& tags = t->extraTags;
            for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
                m_extraTagKeys.insert(it.key().toUpper());
            }
        }
    }
}

QList<Searchable> PlaylistSearch::visibleColumnSet() const {
    QList<Searchable> set;
    if (!m_model) {
        return set;
    }
    const QStringList order = m_model->currentColumnOrder();
    for (const QString& fieldId : order) {
        if (isCustomFieldId(fieldId)) {
            if (!m_registry) {
                continue;
            }
            if (const CustomColumn* c = m_registry->byId(customIdFromFieldId(fieldId))) {
                set.append(Searchable::forPattern(c->pattern));
            }
        } else if (const std::optional<ColumnField> f = fieldFromString(fieldId)) {
            set.append(Searchable::forField(*f));
        }
    }
    return set;
}

void PlaylistSearch::reparse() {
    m_reparseDirty = false;
    search::FilterContext ctx;
    if (m_registry) {
        ctx.customColumns = m_registry->all();
    }
    ctx.extraTagKeys = m_extraTagKeys;
    m_parsed = search::parseFilter(m_filterText, ctx);
    // R1 default: an EMPTY Filter searches what the user sees. A Filter with
    // entries but no valid one is an empty set (no search in force), not the
    // default; falling back silently would hide the mistake.
    m_searchSet = m_filterText.trimmed().isEmpty() ? visibleColumnSet()
                                                   : m_parsed.searchables();
    emit diagnosticsChanged();
    scheduleRebuild();
}

void PlaylistSearch::scheduleRebuild() {
    m_haystacksDirty = true;
    m_coalesce.start();
}

void PlaylistSearch::scheduleRematch() {
    m_coalesce.start();
}

void PlaylistSearch::onCoalesceTimeout() {
    if (m_tagKeysDirty) {
        refreshExtraTagKeys();
        // A key may have appeared or vanished; a pattern entry's validity can
        // flip with it, so the parse is redone against the fresh set.
        m_reparseDirty = true;
    }
    if (m_reparseDirty) {
        // reparse() re-arms the timer; the flags it sets are consumed right
        // below in this same pass, so the re-arm is stopped afterwards.
        reparse();
    }
    if (m_haystacksDirty) {
        rebuildHaystacks();
    }
    rematch();
    m_coalesce.stop();
}

void PlaylistSearch::rebuildHaystacks() {
    m_haystacksDirty = false;
    m_haystacks.clear();
    if (!m_model || m_searchSet.isEmpty()) {
        return;
    }
    const int n = m_model->rowCount();
    m_haystacks.reserve(n);
    for (int r = 0; r < n; ++r) {
        const TrackData* t = m_model->trackAt(r);
        m_haystacks.append(t ? search::buildHaystack(*t, m_searchSet) : QString());
    }
}

void PlaylistSearch::rematch() {
    m_matches.clear();
    m_matchesValid = true;
    if (active()) {
        const qsizetype n = std::min<qsizetype>(m_haystacks.size(), m_model->rowCount());
        for (qsizetype r = 0; r < n; ++r) {
            if (search::haystackMatches(m_haystacks.at(r), m_terms)) {
                m_matches.append(static_cast<int>(r));
            }
        }
    }
    emit matchesChanged();
}

} // namespace rawform
