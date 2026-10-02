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

// PlaylistSearch.h
//
// The live half of Edit > Find: one search session over one playlist. The
// Find dialog owns an instance, binds `model` to the active tab and
// `customColumns` to the registry, and pushes its two text boxes in; this
// class turns them into a match set and keeps it current as the playlist
// changes underneath.
//
// Two cadences, on purpose. The Filter text is PARSED synchronously on every
// change (parseFilter is O(entries)), so the dialog's underline diagnostics
// describe the text on screen and never lag it. Everything O(rows), the
// per-row haystacks (PlaylistSearchFilter::buildHaystack over the search set)
// and the match pass, runs on a 120 ms coalescing timer, so a burst of
// keystrokes costs one rebuild. A query edit alone re-runs only the match
// pass over the cached haystacks; a filter, column, registry or row change
// rebuilds the haystacks first.
//
// Idle discipline. The dialog exists from launch and is bound to the active
// tab, so without a gate every scan batch would cost a haystack rebuild over
// a playlist nobody is searching. Two rules keep the idle cost at zero:
// `enabled` (the dialog binds it to its visibility) suspends the timer, drops
// the haystack cache and leaves the dirty flags standing until the next show;
// and while enabled with a blank String, a timeout refreshes the tag keys
// (the Filter underline needs them on screen) but leaves the haystacks dirty,
// since nothing would be matched against them. The first typed term rebuilds
// them, one window later.
//
// The search set (R1 default): the Filter's valid entries, or, when the Filter
// is empty, the model's VISIBLE columns in visual order, native fields as
// themselves and custom columns as their patterns. A Filter with entries but
// none valid yields an empty set, and an empty set means "no search in
// force" (`active` false), never "nothing matches": a half-typed filter must
// not empty the playlist.
//
// The tab's PlaylistFilterProxy reads accepts(row) to decide visibility; the
// dialog only steps the current row through the rows that remain.

#pragma once

#include "columns/CustomColumnRegistry.h"
#include "playlist/PlaylistModel.h"
#include "search/PlaylistSearchFilter.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QQmlEngine> // QML_ELEMENT
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

namespace rawform {

class PlaylistSearch : public QObject {
    Q_OBJECT
    QML_ELEMENT

    /// The playlist searched (the active tab's model). Null is valid: no
    /// matches, diagnostics still computed against an empty tag-key set.
    Q_PROPERTY(PlaylistModel* model READ model WRITE setModel NOTIFY modelChanged)
    /// The custom-column registry, for bare-name resolution and the visible
    /// custom columns' patterns. Null is valid (no custom columns).
    Q_PROPERTY(CustomColumnRegistry* customColumns READ customColumns
                   WRITE setCustomColumns NOTIFY customColumnsChanged)
    /// False suspends the O(rows) work: signals only mark what they
    /// invalidate, the haystack cache is released, and `active` reads false.
    /// True resumes with one coalesced pass over whatever went stale. The
    /// dialog binds it to its visibility.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    /// The String box.
    Q_PROPERTY(QString queryText READ queryText WRITE setQueryText
                   NOTIFY queryTextChanged)
    /// The Filter box (see PlaylistSearchFilter.h for the grammar).
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText
                   NOTIFY filterTextChanged)
    /// One map per non-empty Filter entry, in order: { start, length, valid,
    /// text }, spans into filterText. Updated synchronously with filterText.
    Q_PROPERTY(QVariantList entryDiagnostics READ entryDiagnostics
                   NOTIFY diagnosticsChanged)
    /// True when at least one Filter entry is invalid (the danger tint).
    Q_PROPERTY(bool filterHasInvalid READ filterHasInvalid NOTIFY diagnosticsChanged)
    /// True while a search is in force: the query has at least one term AND
    /// the search set is non-empty AND the match set describes the current
    /// model (a rebind invalidates it until the next match pass). False
    /// means the playlist is untouched.
    Q_PROPERTY(bool active READ active NOTIFY matchesChanged)
    /// The number of matching rows (0 when not active).
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchesChanged)

public:
    explicit PlaylistSearch(QObject* parent = nullptr);
    ~PlaylistSearch() override = default;

    [[nodiscard]] PlaylistModel*         model() const { return m_model.data(); }
    [[nodiscard]] CustomColumnRegistry*  customColumns() const {
        return m_registry.data();
    }
    [[nodiscard]] bool                   enabled() const { return m_enabled; }
    [[nodiscard]] QString                queryText() const { return m_queryText; }
    [[nodiscard]] QString                filterText() const { return m_filterText; }
    [[nodiscard]] QVariantList           entryDiagnostics() const;
    [[nodiscard]] bool                   filterHasInvalid() const;
    [[nodiscard]] bool                   active() const;
    [[nodiscard]] int                    matchCount() const;

    void setModel(PlaylistModel* model);
    void setCustomColumns(CustomColumnRegistry* registry);
    void setEnabled(bool enabled);
    void setQueryText(const QString& text);
    void setFilterText(const QString& text);

    /// True when SOURCE row @p row matches the search in force. False for
    /// every row when not active, and for an out-of-range row. The filter
    /// proxy's filterAcceptsRow.
    [[nodiscard]] bool accepts(int row) const;

signals:
    void modelChanged();
    void customColumnsChanged();
    void enabledChanged();
    void queryTextChanged();
    void filterTextChanged();
    void diagnosticsChanged();
    void matchesChanged();

private:
    /// Reparse the Filter against the current context and reselect the
    /// search set. Synchronous (cheap); schedules the O(rows) rebuild.
    void reparse();

    /// Arm the coalescing timer for a haystack rebuild + match pass.
    void scheduleRebuild();

    /// Arm the coalescing timer for a match pass only (query edits).
    void scheduleRematch();

    /// The one place the timer is started: a no-op while disabled, so every
    /// invalidation above it only leaves its flag behind.
    void armTimer();

    /// The timer's slot: tag keys, parse, haystacks (if there are terms to
    /// match), then rematch, each only when flagged.
    void onCoalesceTimeout();

    /// One folded haystack per row over m_searchSet.
    void rebuildHaystacks();

    /// The match pass over the cached haystacks; emits matchesChanged.
    void rematch();

    /// Recompute the upper-cased tag keys present in any track (the pattern
    /// validation context). One pass over the model.
    void refreshExtraTagKeys();

    /// The R1 default search set: the model's visible columns, in visual
    /// order, custom columns as their patterns.
    [[nodiscard]] QList<search::Searchable> visibleColumnSet() const;

    /// Drop the previous model's connections and wire the new one.
    void rebindModel(PlaylistModel* model);

    QPointer<PlaylistModel>        m_model;
    QPointer<CustomColumnRegistry> m_registry;
    QList<QMetaObject::Connection> m_modelConnections;
    QList<QMetaObject::Connection> m_registryConnections;

    bool        m_enabled = true;
    QString     m_queryText;
    QString     m_filterText;
    QStringList m_terms;         ///< queryTerms(m_queryText)

    search::ParsedFilter          m_parsed;
    QSet<QString>                 m_extraTagKeys;
    QList<search::Searchable>     m_searchSet;
    QList<QString>                m_haystacks;  ///< one per model row, folded
    QList<int>                    m_matches;    ///< ascending rows
    /// False from a model rebind until the next match pass: the rows in
    /// m_matches belong to the previous model until then.
    bool                          m_matchesValid = false;

    /// The coalescing timer and what its next timeout must redo, in
    /// dependency order (see onCoalesceTimeout).
    QTimer m_coalesce;
    bool   m_tagKeysDirty   = true;
    bool   m_reparseDirty   = true;
    bool   m_haystacksDirty = true;
};

} // namespace rawform
