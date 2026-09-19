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

// ReplayGainRowsModel.h
//
// The C++ backing model for the ReplayGain Properties pane: per-track rows
// (current + on-open original values plus a selection flag), the staging rules,
// the scan scoping, and the Summary aggregation.
//
// Why C++ rather than a QML ListModel plus pane-side JS: rows arrive from
// PlaylistModel as one implicitly shared QVariantList and are ingested in one
// beginResetModel, so a library-sized selection (15k+ rows) opens without a
// per-track marshal into JS objects, and every aggregation pass is a plain
// vector walk instead of an allocating rowModel.get(i) per row.
//
// Division of labor with the pane: the pane keeps GESTURES (click
// modifiers arrive here pre-read, drag-select resolves pointer to row indices
// in view space, keyboard focus juggling stays where the Items are) and the
// scanNoop message wording. This model owns STATE: rows, selection membership
// and the anchor, staging with the revert-on-equal rule, scan scope
// resolution, the Summary, and dirty.
//
// Staging semantics:
//  - A committed cell that equals its original AT WRITE PRECISION stores the
//    original string verbatim (gains compare as their 2-decimal " dB" form,
//    peaks as %.6f), so re-typing "-8" over "-8 dB" neither dirties nor
//    reformats the cell, and plain string inequality is a correct dirty test
//    everywhere downstream.
//  - Scope: the selected rows, or every row when none are selected; allRows
//    forces every row (the window-scoped Tools menu override, by design).
//  - collectEdits diffs current vs original and reports only changed fields;
//    empty means remove the tag. Each edit carries the file path (the durable
//    write identity captured at open) and the playlist row (the opaque scan /
//    staging round-trip key; the editor ignores it).
//
// Value parsing is strict (replaygain::parseGainDb / parsePeak require the
// whole trimmed token, after stripping a trailing "dB" and mapping comma to
// dot, to parse), so "-4x" is rejected as invalid input rather than truncated
// to -4.

#pragma once

#include "playlist/PlaylistModel.h" // complete type: setSelection() takes a
                                    // PlaylistModel* and calls replayGainRows()
                                    // on it C++-to-C++, keeping PlaylistModel
                                    // the single authority on name rendering
                                    // and the albumKey grouping rule. (Same
                                    // include rationale as the sibling models.)

#include <QAbstractListModel>
#include <QList>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <optional>

namespace rawform {

class ReplayGainRowsModel : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT

    // Summary block, refreshed by one linear pass after every mutation (the
    // live scope is picked only after the selection is counted). Plain
    // notifying properties: the pane's footer rows and the host window's Apply
    // gating bind these.
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY summaryChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY summaryChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)
    Q_PROPERTY(QString trackGainSummary READ trackGainSummary NOTIFY summaryChanged)
    Q_PROPERTY(QString albumGainSummary READ albumGainSummary NOTIFY summaryChanged)
    Q_PROPERTY(QString lowestGain READ lowestGain NOTIFY summaryChanged)
    Q_PROPERTY(QString highestGain READ highestGain NOTIFY summaryChanged)
    Q_PROPERTY(QString totalPeak READ totalPeak NOTIFY summaryChanged)

public:
    explicit ReplayGainRowsModel(QObject* parent = nullptr);

    /// The four editable cells. Values double as the pane's column order.
    /// TrackPeak / AlbumPeak are "peak kind" for parsing and change detection;
    /// the gains are "gain kind".
    enum Field { TrackGain = 0, AlbumGain = 1, TrackPeak = 2, AlbumPeak = 3 };
    Q_ENUM(Field)

    /// Role names, as the pane's delegate binds them: name, sel,
    /// tg / ag / tp / ap (current) and tg0 / ag0 / tp0 / ap0 (on-open, or
    /// adopted, originals; the delegate derives per-cell dirtiness as
    /// current != original).
    enum Roles {
        NameRole = Qt::UserRole + 1,
        SelRole,
        TgRole, AgRole, TpRole, ApRole,
        Tg0Role, Ag0Role, Tp0Role, Ap0Role,
    };

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex{}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    // --- population --------------------------------------------------------

    /// Snapshot the given playlist rows into this model (one model reset).
    /// Ingests through playlist->replayGainRows() so the name rendering
    /// (Title, file name fallback) and the albumKey grouping rule keep their
    /// single authority; the QVariantList handoff is C++-to-C++ and implicitly
    /// shared, so no per-row JS materialization happens anywhere on this path.
    /// Out-of-range rows were already skipped there. Resets selection, anchor,
    /// staging, and the Summary.
    Q_INVOKABLE void setSelection(rawform::PlaylistModel* playlist, const QVariantList& rows);

    /// Empty the model (window teardown).
    Q_INVOKABLE void clear();

    [[nodiscard]] int count() const { return static_cast<int>(m_rows.size()); }

    // --- selection ---------------------------------------------------------
    // The anchor lives here with the membership it anchors. `modifiers` is the
    // raw Qt modifier mask from the pane's mouse / key handlers: Control OR
    // Meta toggles (Qt swaps Ctrl and Cmd on macOS, so either is accepted),
    // Shift ranges from the anchor, plain selects
    // exclusively. Full-span selection changes emit ONE dataChanged over the
    // span rather than one per row.

    Q_INVOKABLE void select(int row, int modifiers);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();

    [[nodiscard]] bool hasSelection() const { return m_selCount > 0; }
    [[nodiscard]] int selectionCount() const { return m_selCount; }

    // --- staging -----------------------------------------------------------

    /// True when @p raw is committable into a cell of @p field's kind: empty
    /// (a clear) always is; otherwise the whole token must parse (strict; see
    /// the file top).
    Q_INVOKABLE [[nodiscard]] bool validInput(const QString& raw, int field) const;

    /// Commit one cell with the revert-on-equal rule; recomputes the Summary.
    Q_INVOKABLE void commitCell(int row, int field, const QString& value);

    /// Apply @p value across the current scope (selected rows, else all rows;
    /// @p allRows forces all). One dataChanged span, one Summary recompute:
    /// the batch shape that keeps a library-sized scope linear.
    Q_INVOKABLE void setScoped(int field, const QString& value, bool allRows);

    /// The window-scoped Tools > Clear: stages empty into all four fields of
    /// every row (written as removals on Apply / OK; Cancel undoes it).
    Q_INVOKABLE void clearAllReplayGain();

    // --- scanning ----------------------------------------------------------

    /// Resolve the scan scope to { row, path, name [, albumKey] } items for the
    /// host's off-thread scan. mode 0 per-track, 1 whole scope as one album,
    /// 2 partitioned into albums by albumKey (empty-key rows are singleton
    /// groups). @p skipExisting applies the pane's rules against the ORIGINALS:
    /// track mode drops rows already carrying a track gain; album mode skips
    /// the whole batch only when EVERY scoped row carries an album gain (a
    /// partial skip would corrupt the album combine); by-tags mode applies that
    /// whole-or-nothing rule per group. An empty return means nothing to scan;
    /// the pane words the scanNoop message (mode-dependent copy stays UI-side).
    Q_INVOKABLE [[nodiscard]] QVariantList scanItems(int mode, bool allRows,
                                                     bool skipExisting) const;

    /// Stage measured values back into the cells, keyed by playlist row. Each
    /// result map carries only what it measured (trackGain always, trackPeak
    /// when non-zero, plus albumGain / albumPeak in album mode). Revert-on-
    /// equal per cell; ONE recompute for the whole batch.
    Q_INVOKABLE void stageScanResults(const QVariantList& results);

    // --- apply / revert ----------------------------------------------------

    /// Diff current vs original; only changed fields are reported (empty means
    /// remove). Same map shape the window already consumes: { row, path,
    /// trackGain?, albumGain?, trackPeak?, albumPeak? }.
    Q_INVOKABLE [[nodiscard]] QVariantList collectEdits() const;

    /// Fold staged values into the originals after a write pass that cannot be
    /// re-pulled (rows or tab gone): the values ARE on disk, so they become the
    /// baseline. Rows whose path is in @p failedPaths did NOT get their write
    /// and revert to their originals instead, mirroring what the re-pull path
    /// gives a failed file.
    Q_INVOKABLE void adoptEdits(const QStringList& failedPaths);

    /// Discard staging: every current value returns to its original, selection
    /// clears. Named revertEdits, NOT revert: QAbstractItemModel declares a
    /// virtual revert() slot (the submit / revert editing protocol), and an
    /// unmarked same-name method is a shadowing error under AppleClang's
    /// -Winconsistent-missing-override, while MARKING it override would hijack
    /// the protocol slot so a generic base-class revert() call could silently
    /// discard staged user edits. A distinct name keeps the two APIs apart
    /// (and pairs with adoptEdits). Reverting to the ORIGINALS rather than to
    /// an open-time snapshot matters after adoptEdits: the originals are then
    /// the adopted baseline, so a revert cannot resurrect stale pre-apply
    /// values (the scenario a snapshot replay would allow).
    Q_INVOKABLE void revertEdits();

signals:
    void countChanged();
    void summaryChanged();
    void dirtyChanged();

private:
    struct Row {
        QString name;      // Title, or file name when empty (rendered upstream)
        QString path;      // durable write identity, captured at open
        QString albumKey;  // by-tags grouping key; empty == singleton group
        int playlistRow = -1; // opaque scan / staging round-trip key
        QString tg, ag, tp, ap;      // current (staged) display strings
        QString tg0, ag0, tp0, ap0;  // originals (on-open, or adopted)
        bool sel = false;
    };

    // Field plumbing: member access and the gain / peak kind split.
    [[nodiscard]] static bool isPeak(int field) { return field >= TrackPeak; }
    [[nodiscard]] static QString& cur(Row& r, int field);
    [[nodiscard]] static const QString& cur(const Row& r, int field);
    [[nodiscard]] static QString& orig(Row& r, int field);
    [[nodiscard]] static const QString& orig(const Row& r, int field);
    [[nodiscard]] static int roleFor(int field);

    /// The revert-on-equal comparator: empty vs empty is unchanged, empty vs
    /// present is changed, both present compare at write precision (gains via
    /// replaygain::formatGainValue, peaks at 6 decimals), unparseable falls
    /// back to raw string inequality. Parsing and formatting are the shared
    /// media/ReplayGainTags.h functions (one source of truth with the editor
    /// and the scan controller); the strict whole-token rule is theirs.
    [[nodiscard]] static bool fieldChanged(const QString& curV, const QString& origV,
                                           bool peak);

    /// Apply one cell under the revert-on-equal rule; true when the stored
    /// string actually changed (the caller batches dataChanged / recompute).
    bool applyCell(int i, int field, const QString& value);

    /// Scope indices: selected rows, else every row; allRows forces all.
    [[nodiscard]] QList<int> scopedIndices(bool allRows) const;

    /// The single-pass Summary + dirty refresh: one walk
    /// gathers the selection count, dirtiness, and the gain / peak populations
    /// for both scopes, then picks the live scope. Emits summaryChanged always
    /// (cheap, and the strings usually did move) and dirtyChanged on flips.
    void recomputeSummary();

    QList<Row> m_rows;
    int m_anchor = -1;

    // Summary cache (refreshed by recomputeSummary).
    int m_selCount = 0;
    bool m_dirty = false;
    QString m_tgSummary;
    QString m_agSummary;
    QString m_lowest = QStringLiteral("n/a");
    QString m_highest = QStringLiteral("n/a");
    QString m_peak = QStringLiteral("n/a");

public:
    [[nodiscard]] bool dirty() const { return m_dirty; }
    [[nodiscard]] QString trackGainSummary() const { return m_tgSummary; }
    [[nodiscard]] QString albumGainSummary() const { return m_agSummary; }
    [[nodiscard]] QString lowestGain() const { return m_lowest; }
    [[nodiscard]] QString highestGain() const { return m_highest; }
    [[nodiscard]] QString totalPeak() const { return m_peak; }
};

} // namespace rawform
