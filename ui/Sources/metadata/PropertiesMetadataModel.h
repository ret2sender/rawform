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

// PropertiesMetadataModel.h
//
// Backs the Properties window's editable Metadata tab.
//
// Deliberately SEPARATE from MetadataModel. The dock's MetadataModel is a
// read-only, section-grouped technical view that de-dupes multi-selections for a
// compact summary. The Properties Metadata tab is an editor with a different
// contract:
//
//  - A FIXED tag schema shown in full even when empty: Artist Name, Track Title,
//    Album Title, Date, Genre, Composer, Performer, Album Artist, Track Number,
//    Total Tracks, Disc Number, Total Discs, Comment. Each row maps a friendly
//    label to a canonical tag KEY (ARTIST, TITLE, ... COMMENT), which is what
//    the write path (MetadataEditor) targets.
//  - CUSTOM rows for any other (non-technical) tag present across the selection,
//    rendered as "<KEY>" (e.g. "<RELEASE_YEAR>"). The ReplayGain keys are filtered
//    out (they live on the ReplayGain tab), as are the schema's own keys.
//  - A trailing "+ add new" affordance row.
//  - Multi-selection values are NOT de-duped. When the selected tracks agree, the
//    shared value shows plainly; when they differ, the value is prefixed with the
//    guillemet marker and lists the per-track values joined with "; ", in
//    SELECTION ORDER, TRUNCATED at a hard display cap: the cell is one
//    elided line, so the join is built only up to kDisplayCapChars and never
//    materializes a library-sized megastring (the scenario this prevents: an
//    OOM on a 15k-track selection). Display only; the full per-track values
//    live in m_original / m_staged and feed editing through effectivePerTrack,
//    and multi-track edits route to the Edit Value dialog, which reads the
//    per-track lists directly.
//
// The display half is READ-ONLY: it renders the schema, the multi-value marker,
// and the custom rows. Editing gestures (in-place,
// the Single / Individual dialog, add-field, the context-menu actions) live in
// the pane above it and stage through this model; the off-thread writer is
// MetadataEditor. The role surface (the row-kind and multiple roles, plus the
// isSectionRow / isAddRow probes) serves both halves.
//
// Fed from QML via setSelection(PlaylistModel*, rows), the same way the Details
// tab feeds its MetadataModel. TrackData stays in C++; QML reads display strings.

#pragma once

#include "media/TrackData.h"
#include "playlist/PlaylistModel.h" // complete type: setSelection() is a
                                    // Q_INVOKABLE taking PlaylistModel*, which
                                    // moc must see fully defined to build the
                                    // QML-marshaled meta-type. (Same reason
                                    // MetadataModel / ReplayGainEditor include it.)

#include <QAbstractTableModel>
#include <QHash>
#include <QList>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace rawform {

class PropertiesMetadataModel : public QAbstractTableModel {
    Q_OBJECT
    QML_ELEMENT
    // True when any field has a staged edit differing from its on-disk value.
    // Drives the window's Apply button and the commit sequencer.
    Q_PROPERTY(bool dirty READ dirty NOTIFY dirtyChanged)

public:
    enum class Column : int {
        Name = 0, ///< field label, or section title, or the "+ add new" affordance
        Value,    ///< the (possibly multi-value-marked) tag value; empty otherwise
        Count
    };
    Q_ENUM(Column)

    enum Roles {
        DisplayRole = Qt::DisplayRole,
        ValueRole = Qt::UserRole + 1,     ///< column-1 value as a role (ListView reads col 0)
        IsSectionRole,                    ///< bool: the single "Metadata" header row
        IsAddRole,                        ///< bool: the trailing "+ add new" row
        IsCustomRole,                     ///< bool: a "<KEY>" custom-tag row
        IsMultipleRole,                   ///< bool: value carries the multi-value marker
        IsDirtyRole                       ///< bool: row has a staged (uncommitted) edit
    };

    explicit PropertiesMetadataModel(QObject* parent = nullptr);
    ~PropertiesMetadataModel() override = default;

    // --- QAbstractTableModel interface -------------------------------------
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Snapshot the given playlist rows as the editing selection and rebuild the
    /// visible rows + values. Q_INVOKABLE so the Properties window (pure QML) can
    /// feed it on open.
    Q_INVOKABLE void setSelection(rawform::PlaylistModel* playlist, const QList<int>& rows);

    // Row-kind probes for the pane's delegate (plain ints, no meta-type concerns).
    Q_INVOKABLE [[nodiscard]] bool isSectionRow(int row) const;
    Q_INVOKABLE [[nodiscard]] bool isAddRow(int row) const;

    /// 0-based index of a field row within the (single) Metadata section, so the
    /// pane can zebra-stripe. Section / add rows return 0.
    Q_INVOKABLE [[nodiscard]] int fieldIndexInSection(int row) const;

    /// Value-column display string for @p row (empty for section / add rows). Lets
    /// the pane measure value widths without constructing a column-1 QModelIndex,
    /// mirroring MetadataModel::valueAt.
    Q_INVOKABLE [[nodiscard]] QString valueAt(int row) const;

    // --- editing (inline; Edit Value dialog) -------------------------------
    [[nodiscard]] bool dirty() const { return !m_staged.isEmpty(); }

    /// Number of tracks in the current selection. The pane gates editing on this: a
    /// single track edits inline; any multi-selection opens the Edit Value dialog.
    Q_INVOKABLE [[nodiscard]] int selectionCount() const { return static_cast<int>(m_selection.size()); }

    /// Raw, editable text to pre-fill an inline editor on @p row (single-track):
    /// the track's value (list fields joined "; "). Empty for non-editable rows.
    Q_INVOKABLE [[nodiscard]] QString editText(int row) const;

    /// Regex pattern (for a RegularExpressionValidator) constraining input on @p row
    /// by field policy: Track/Total/Disc numbers accept digits only, Date accepts
    /// digits and dash, list fields are unrestricted (they carry the "; "
    /// separator). Used by both the inline editor and the dialog cells. Always
    /// returns a usable anchored pattern (never empty).
    Q_INVOKABLE [[nodiscard]] QString editValidatorPattern(int row) const;

    /// Commit a single-track inline edit on @p row: the text is conformed to the
    /// field policy (list keys collapse "a;b" -> "a; b"; scalar keys are trimmed)
    /// and staged. Every single-track edit is accepted; there is no rejection
    /// path. No-op if more than one track is selected.
    Q_INVOKABLE void applyInlineEdit(int row, const QString& text);

    // --- Edit Value dialog -------------------------------------------------

    /// Display label of @p row's field ("Track Title", "Genre", "<CUSTOM>"), for
    /// the dialog title. Empty for non-editable rows.
    Q_INVOKABLE [[nodiscard]] QString fieldLabel(int row) const;

    /// Tag key of @p row's field ("TITLE", "GENRE", ...), for the dialog's Field
    /// name box and as the stage target. Empty for non-editable rows.
    Q_INVOKABLE [[nodiscard]] QString fieldKey(int row) const;

    /// Pre-fill for the dialog's Single Value tab: the shared value when the whole
    /// selection agrees, else empty (Single broadcasts, so a disagreeing field
    /// starts blank rather than showing a misleading joined blob).
    Q_INVOKABLE [[nodiscard]] QString sharedEditValue(int row) const;

    /// True when @p row's field has the same effective value across the whole
    /// selection (including all-empty): the dialog opens on Single Value when true,
    /// Individual Values when false. A distinct flag rather than reusing
    /// sharedEditValue's blank, since an all-empty field is uniform yet blank.
    /// Non-editable rows return true.
    Q_INVOKABLE [[nodiscard]] bool fieldUniform(int row) const;

    /// Effective per-track display values for @p row (staged if present, else
    /// on-disk), one per selected track in order: the Individual Values tab's
    /// initial cells. List fields are "; "-joined per track.
    Q_INVOKABLE [[nodiscard]] QStringList perTrackEditValues(int row) const;

    /// Per-track row labels for the Individual Values tab's middle column: each
    /// track's Title, falling back to its file name when the title is empty. One
    /// per selected track, in order.
    Q_INVOKABLE [[nodiscard]] QStringList trackLabels() const;

    /// Stage @p perTrackValues (one display string per selected track, in order)
    /// for @p key, the dialog's commit path. Each value is conformed to the field
    /// policy; an empty value stages a removal for that track. Length must equal
    /// the selection size; mismatched calls are ignored. Drops the stage if the
    /// result equals the on-disk value, so dirty() stays exact.
    Q_INVOKABLE void stageFieldValues(const QString& key, const QStringList& perTrackValues);

    /// Add a custom field across the whole selection: the Add-mode dialog's commit
    /// path. @p rawKey is normalized (trimmed, uppercased); @p perTrackValues is one
    /// display string per selected track. Returns "" on success, else a short reason
    /// the field was not added, for the dialog's inline error: an empty key, a
    /// reserved/managed key (a schema field, an alias, or another tab's technical
    /// key), a key that already exists as a row, or values that are all empty
    /// (nothing to write). On success a Custom row is inserted just before the
    /// "+ add new" row with an empty on-disk baseline and the values are staged, so
    /// collectEdits writes it like any custom field. A revert / reload rebuild drops
    /// the row if it was never applied; once written it is read back from disk.
    Q_INVOKABLE QString addCustomFieldValues(const QString& rawKey, const QStringList& perTrackValues);

    /// One empty string per selected track: the Add-mode dialog's blank value seed.
    Q_INVOKABLE [[nodiscard]] QStringList blankPerTrack() const;

    /// Build the flat { path, key, values } edit list for MetadataEditor from the
    /// staged changes. Applies the coupled-pair semantics: a staged Track Number
    /// OR Total Tracks writes both the number and the total for every affected track
    /// (edited value where staged, current value otherwise); the total is written
    /// under the TRACKTOTAL spelling and the TOTALTRACKS spelling is cleared. Disc
    /// behaves identically with DISCNUMBER, DISCTOTAL written, TOTALDISCS cleared.
    /// Date is an ordinary scalar (no YEAR coupling; a YEAR tag is its own field).
    /// Only tracks whose effective value actually changed are emitted.
    Q_INVOKABLE [[nodiscard]] QVariantList collectEdits() const;

    /// Fold the staged edits INTO the on-disk baseline and clear the staging.
    /// Called by the Properties window after a write pass landed but the
    /// playlist can no longer be re-pulled (the
    /// window's tracks, or the whole owning tab, are gone): the values ARE on
    /// disk now, so they become the new baseline, the dirty markers clear, and
    /// a later Apply does not rewrite them. Tracks whose path is in
    /// @p failedPaths did NOT get their write; their staged values revert to
    /// the original instead, the same outcome the re-pull path gives a failed
    /// file (its unchanged tags are re-read from disk).
    Q_INVOKABLE void adoptEdits(const QStringList& failedPaths);

    /// Remove the given field/custom rows (the pane's Remove action / Del key). Each
    /// schema field or on-disk custom field is staged empty, so collectEdits writes
    /// a removal on Apply with the Track/Total, Disc, and Date coupling intact; a
    /// custom row that was only added this session (empty on-disk baseline, never
    /// written) is dropped from the model outright. Rows are processed high-index
    /// first so an outright drop never invalidates a not-yet-processed index.
    /// (removeFields, not removeRows, to avoid QAbstractItemModel's own virtual.)
    Q_INVOKABLE void removeFields(const QList<int>& rows);

    /// Remove EVERY editable field row, the staging half of Tools > Remove
    /// tags: equivalent to select-all + removeFields, so schema and on-disk
    /// custom fields stage empty (written as removals on Apply) and session-only
    /// custom rows drop outright. The structural tag strip itself is the write
    /// pass's job (MetadataEditor stripAllPaths); this only blanks the model.
    Q_INVOKABLE void removeAllFields();

    // --- clipboard ---------------------------------------------------------

    /// Serialize the given field/custom rows to the clipboard text format: one block
    /// per selected track (in selection order), each block a run of "KEY=value"
    /// lines (one per selected field, in row order), blocks separated by a blank
    /// line. An empty field emits "KEY="; list fields keep the "; " form on one line.
    /// This is what Copy / Cut put on the clipboard.
    Q_INVOKABLE [[nodiscard]] QString serializeFields(const QList<int>& rows) const;

    /// True if @p text parses to at least one block with at least one valid
    /// "KEY=value" line; drives the Paste action's enabled state.
    Q_INVOKABLE [[nodiscard]] bool canPasteFields(const QString& text) const;

    /// Paste serialized fields across the whole track selection. The clipboard's N
    /// blocks distribute by count: N == selection size positionally, N == 1 broadcast
    /// to all, otherwise nothing is staged and a "Clipboard has N tracks, selection
    /// has M" message is returned. Per key, an existing field is staged (overwritten)
    /// and an unknown key creates a custom field, reusing the same validation as the
    /// add flow (reserved keys and all-empty new keys are skipped). Returns "" on
    /// success, else the count-mismatch message.
    Q_INVOKABLE QString pasteFields(const QString& text);

    // --- transforms --------------------------------------------------------

    /// Capitalize each selected field's per-track values: upper-case the first
    /// letter of every whitespace-delimited word and lower-case the rest, staged.
    /// The word boundary is whitespace only, so "o'brien" becomes "O'brien". List
    /// fields transform on their joined display and re-normalize.
    Q_INVOKABLE void capitalizeFields(const QList<int>& rows);

    /// Clean whitespace in each selected field's per-track values: underscores to
    /// spaces, then trim and collapse internal whitespace runs to a single space.
    /// Staged.
    Q_INVOKABLE void cleanWhitespaceFields(const QList<int>& rows);

    /// Remove every editable field EXCEPT @p keepRows: the complement is passed to
    /// removeFields (schema and on-disk custom stage empty; a session-only custom
    /// row drops). Staged and revertable, so no confirmation is needed.
    Q_INVOKABLE void cropToFields(const QList<int>& keepRows);

    /// Number the selection 1..N in selection (playlist) order and set Total Tracks
    /// to N, both unpadded, staged. On Apply the total writes as TRACKTOTAL per
    /// collectEdits. Independent of the field-row selection.
    Q_INVOKABLE void autoNumberTracks();

signals:
    void dirtyChanged();

private:
    enum class Kind { Section, Field, Custom, Add };

    /// One visible row. For Field/Custom, `key` is the canonical tag key the
    /// value is read from and written to; `name` is the display label
    /// ("Album Artist", or "<RELEASE_YEAR>" for custom). `multiple` marks a
    /// value that disagrees across the selection (drives the guillemet marker and
    /// the IsMultipleRole). Section/Add rows leave key/value empty.
    struct Row {
        Kind    kind = Kind::Field;
        QString name;
        QString key;
        QString value;
        bool    multiple = false;
        bool    dirty = false; ///< has a staged edit (effective value != on-disk)
    };

    /// Rebuild m_rows from the current selection: the Metadata header, the fixed
    /// schema, the custom rows, then "+ add new".
    void rebuild();

    /// One track's value for a tag key, as a display string. Promoted TrackData
    /// fields are read directly; everything else (Composer / Performer / Comment
    /// and custom keys) comes from extraTags. Within-track multi-values join with
    /// "; " (TrackData::joinedValues).
    [[nodiscard]] static QString perTrackValue(const TrackData& t, const QString& key);

    /// Collapse a key across the selection into a display value plus a "multiple"
    /// flag: all-equal yields the shared value (flag false); any disagreement
    /// yields the guillemet-marked, per-track, selection-ordered list (flag true).
    [[nodiscard]] QString aggregateValue(const QString& key, bool& multipleOut) const;

    /// True for keys that must never appear as custom rows: the schema's own keys
    /// (shown by the fixed rows) and the technical keys owned by other tabs
    /// (ReplayGain). ENCODER / CUESHEET are already consumed by the reader and so
    /// never reach extraTags, but are listed for clarity / robustness.
    [[nodiscard]] static bool isReservedKey(const QString& key);

    /// Per-track value list for @p key, EFFECTIVE: the staged edit if one exists,
    /// else the on-disk original. Always length == selection size.
    [[nodiscard]] QStringList effectivePerTrack(const QString& key) const;

    /// Aggregate a per-track list to a display string + "multiple" flag, the same
    /// way aggregateValue does but on an already-computed list (so staged and
    /// original both collapse identically). The returned string is DISPLAY ONLY
    /// and hard-capped at kDisplayCapChars: built incrementally, it stops
    /// at the cap with an ellipsis instead of joining every per-track value, so
    /// a library-sized selection can never build a megastring here. All editing
    /// surfaces read the full lists via effectivePerTrack, never this.
    [[nodiscard]] static QString collapseList(const QStringList& perTrack, bool& multipleOut);

    /// Recompute a single field/custom row's value/multiple/dirty from its
    /// effective per-track values and emit dataChanged for it. Used after staging.
    void refreshRowForKey(const QString& key);

    /// Row index whose key == @p key, or -1. Linear (rows are few).
    [[nodiscard]] int rowForKey(const QString& key) const;

    /// Stage an already-normalized per-track list for @p key: drop the stage (and
    /// any prior one) when it equals the on-disk original, else record it; refresh
    /// the row and emit dirtyChanged on any change. The shared tail of both
    /// applyInlineEdit and stageFieldValues.
    void stageList(const QString& key, const QStringList& newList);

    /// Remove a single field/custom row: stage empty (schema or on-disk custom) or
    /// drop the row outright (a custom field added this session, never written).
    /// The per-row worker behind removeFields.
    void removeFieldAt(int row);

    /// Parse the clipboard text format into one key->value map per block (track), in
    /// block order. CRLF is normalized; blocks split on blank lines; within a block
    /// each line splits on its first '=' into an upper-cased key and a raw value.
    /// Lines without '=' or with an empty key are skipped; empty blocks are dropped.
    /// Shared by canPasteFields and pasteFields.
    [[nodiscard]] QList<QHash<QString, QString>> parseFieldBlocks(const QString& text) const;

    /// Apply a per-value transform to every field/custom row in @p rows and stage
    /// the result across the selection. The shared body of capitalizeFields and
    /// cleanWhitespaceFields; @p fn maps one display value to its transformed form.
    void transformSelectedValues(const QList<int>& rows, QString (*fn)(const QString&));

    QList<Row>       m_rows;      ///< visible rows for the current selection
    QList<TrackData> m_selection; ///< snapshot of the selected tracks

    /// On-disk per-track values, one list (length == selection) per row key. The
    /// baseline staged edits diff against. Rebuilt on every setSelection.
    QHash<QString, QStringList> m_original;

    /// Staged per-track edits, keyed by tag key; a key is present only while its
    /// value differs from m_original (an edit equal to the original is dropped),
    /// so dirty() is simply "any staged key". One entry per selected track.
    QHash<QString, QStringList> m_staged;
};

} // namespace rawform
