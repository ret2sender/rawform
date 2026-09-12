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

#include "media/TrackData.h"
#include "playlist/PlaylistModel.h" // complete type: setSelection() is a
                                    // Q_INVOKABLE taking PlaylistModel*, which
                                    // moc must see fully defined to build the
                                    // meta-type QML marshals into. (The sibling
                                    // models include it for the same reason.)

#include <QAbstractTableModel>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QQmlEngine>
#include <QString>

#include <cstdint> // std::int64_t-style widths via qint64

namespace rawform {

/**
 * @brief Two-column (Name / Value) model backing the metadata pane.
 *
 * A flat Name/Value table broken into sections ("Metadata", "Location",
 * "General") with header rows. The full set of possible rows is a static
 * template; the VISIBLE rows are rebuilt from it on each selection, because some
 * rows are conditional. Two reasons a row can vanish:
 *
 *  - CONDITIONAL: the field has no value for the current selection (e.g. a FLAC
 *    has no "Codec Profile" worth showing). Hidden whenever its value is empty.
 *  - SINGLE-ONLY: the field is meaningless for more than one track, so it is
 *    hidden the moment the selection holds two or more rows. These mirror
 *    foobar2000: "File path", "Subsong index", "Created" and "Audio MD5" all
 *    disappear in a multi-selection.
 *
 * To keep navigation smooth, setSelection compares the new visible shape to the
 * current one: an unchanged shape emits dataChanged() (delegate reuse, no
 * flicker), and only a changed shape (a conditional or single-only row appearing
 * or disappearing) triggers a begin/endResetModel.
 *
 * Multi-selection presentation also matches foobar2000 field by field:
 *
 *  - JOIN fields (most of Metadata, plus Codec / Tool / etc.): each track's
 *    value is collected, de-duplicated (first-seen order) and joined. The
 *    separator is "; " by default (so a multi-value Genre and a cross-track join
 *    read the same way foobar shows them), with the two path fields ("File name"
 *    and "Folder name") joining with ", " instead. When the selection is a MIX
 *    (some tracks carry the tag, some do not), the missing tracks contribute the
 *    literal "(unknown)"; when EVERY track lacks the tag the value is left blank,
 *    so always-on rows show empty and conditional rows still hide.
 *  - NUMERIC AGGREGATES (single value, never a join):
 *      Items Selected = the selection count,
 *      Duration       = summed length, foobar's "M:SS.mmm (N samples)" format,
 *      Total size     = summed bytes,
 *      Avg. bitrate   = duration-weighted mean,
 *      Last modified  = the most recent timestamp.
 *  - RELABELED fields: a few Name labels change in a multi-selection:
 *      "File name"   -> "File names"   (when >1 distinct file),
 *      "Folder name" -> "Folder names" (when >1 distinct folder),
 *      "File size"   -> "Total size"   (whenever >1 track is selected),
 *      "Bitrate"     -> "Avg. bitrate" (whenever >1 track is selected).
 *    The two path fields pluralize on the number of DISTINCT values, not the
 *    selection size, so two files in one folder keep the singular "Folder name".
 *
 * TrackData stays in C++; main.cpp hands this model the selected tracks and QML
 * only ever reads display strings.
 */
class MetadataModel : public QAbstractTableModel {
    Q_OBJECT
    QML_ELEMENT

    /// Details mode: when true, the model drops the "Metadata" section entirely
    /// and the "Items Selected" row, leaving only Location + General. This is what
    /// backs the Properties window's Details tab (the window title already carries
    /// the selection count, so "Items Selected" would be redundant there). The
    /// dock metadata pane leaves this false and shows the full set. Set once from
    /// QML at construction; toggling rebuilds the template under a model reset.
    Q_PROPERTY(bool detailsMode READ detailsMode WRITE setDetailsMode NOTIFY detailsModeChanged)

public:
    enum class Column : int {
        Name = 0, ///< "Name": the field label (or section title).
        Value,    ///< "Value": the aggregated field value (empty for sections).
        Count
    };
    Q_ENUM(Column)

    enum Roles {
        DisplayRole = Qt::DisplayRole,
        IsSectionRole = Qt::UserRole + 1, ///< bool: true if this row is a section header.
    };

    explicit MetadataModel(QObject* parent = nullptr);
    ~MetadataModel() override = default;

    // --- QAbstractTableModel interface -----------------------------------
    [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    [[nodiscard]] QVariant headerData(int section, Qt::Orientation orientation,
                                      int role = Qt::DisplayRole) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] bool detailsMode() const { return m_detailsMode; }
    void setDetailsMode(bool on);

    /// Sets the selection to the given rows, then rebuilds the visible rows and
    /// their values (a reset only if the visible shape changed). Q_INVOKABLE so
    /// the Properties window's Details tab (pure QML) can feed this directly with
    /// its snapshot; the dock pane still calls it from C++ in main.cpp.
    Q_INVOKABLE void setSelection(rawform::PlaylistModel* playlist, const QList<int>& rows);

    /// Clear the selection (drops to the no-selection visible set: always-on
    /// rows with blank values; conditional and single-only rows hidden).
    void clear();

    /// True if @p row is a section header (vs a field). Invokable from QML so
    /// the view's rowHeightProvider can size section rows (26px) differently
    /// from field rows (19px). Takes a plain int (no meta-type concerns).
    Q_INVOKABLE [[nodiscard]] bool isSectionRow(int row) const;

    /// 0-based index of a field row WITHIN its section (the first field after a
    /// section header is 0, the next 1, ...). Lets the view restart its zebra
    /// striping at each section. Returns 0 for section rows / out of range.
    Q_INVOKABLE [[nodiscard]] int fieldIndexInSection(int row) const;

    /// Value-column (column 1) display string for @p row. The metadata pane is a
    /// 2-column table model, and QML can only read column 0 through a delegate,
    /// so the view has no other way to reach the Value text it needs to MEASURE
    /// (to size the Value column for horizontal scrolling). This mirrors
    /// data(index(row, 1), DisplayRole) without QML needing a QModelIndex.
    /// Returns an empty string for section rows or an out-of-range row. The view
    /// re-measures off the model's dataChanged() / modelReset(), so no extra
    /// signal is introduced here.
    Q_INVOKABLE [[nodiscard]] QString valueAt(int row) const;

signals:
    void detailsModeChanged();

private:
    /// Identifies which TrackData field a value row extracts. Section rows use
    /// FieldId::None. ItemsSelected is synthetic (it has no per-track value; its
    /// value is the selection count), so fieldValue() returns empty for it and
    /// aggregatedValue() supplies the count.
    enum class FieldId {
        None = 0,
        Artist, Title, Album, Year, Genre, AlbumArtist,
        TrackNo, TrackTotal, DiscNo, DiscTotal,
        FileName, FolderName, FilePath, SubsongIndex,
        FileSize, Modified, Created,
        /// General section (audio + encoding properties, after Location).
        /// ItemsSelected is the count of the current selection. Length (label
        /// "Duration"), Bitrate ("Avg. bitrate" in multi), FileSize ("Total size"
        /// in multi) and Modified ("Last modified") are NUMERIC AGGREGATES: they
        /// sum / average / max across the selection rather than de-dupe + join.
        /// SampleRate, Channels, Codec and Encoding read straight off TrackData;
        /// Encoding is derived from the codec (no stored field). Tool and
        /// EmbeddedCuesheet are promoted tag-derived extras. BitsPerSample,
        /// CodecProfile, TagType and AudioMd5 are CONDITIONAL (hidden when empty);
        /// AudioMd5 is additionally SINGLE-ONLY (hidden in any multi-selection).
        ItemsSelected, Length, SampleRate, Channels, Bitrate, Codec, Encoding,
        Tool, EmbeddedCuesheet, BitsPerSample, CodecProfile, TagType, AudioMd5
    };

    /// How a field collapses a multi-selection into one Value string.
    enum class Agg {
        Join,         ///< de-dupe + join (with "(unknown)" for missing-in-a-mix)
        Count,        ///< the selection size ("Items Selected")
        SumDuration,  ///< summed length, "M:SS.mmm (N samples)"
        SumBytes,     ///< summed file sizes, "X MB (N bytes)"
        AvgBitrate,   ///< duration-weighted mean bitrate, "X kbps"
        MaxModified,  ///< the most recent modified timestamp
        WeightedShare ///< per-value share of the selection, "value (P%); ..."
    };

    /// One template row: a section header or a named field.
    ///  - conditional: dropped from the visible set when its value is empty.
    ///  - singleOnly:  dropped whenever the selection holds more than one track.
    ///  - multiName:   alternate Name label used in a multi-selection (empty
    ///                 means the single-selection @ref name is used throughout).
    struct Row {
        bool    isSection   = false;
        QString name;            ///< section title, or field label (STATIC)
        FieldId field       = FieldId::None;
        bool    conditional = false;
        bool    singleOnly  = false;
        QString multiName;       ///< alternate label for >1-track selections
    };

    /// Build the static row TEMPLATE (every possible section + field). Called
    /// once in the constructor; m_template never changes afterward.
    void buildTemplate();

    /// The visible subset of m_template for the current selection: sections and
    /// always-on fields always; conditional fields only when they have a value;
    /// single-only fields only when exactly one track is selected.
    [[nodiscard]] QList<Row> visibleRows() const;

    /// True if two row lists have the same shape (same sequence of sections and
    /// fields), so only their Value (or relabeled Name) contents could differ.
    /// Lets setSelection take the cheap dataChanged path instead of a reset.
    [[nodiscard]] static bool sameShape(const QList<Row>& a, const QList<Row>& b);

    /// Which aggregation a field uses across a multi-selection.
    [[nodiscard]] static Agg aggStrategy(FieldId id);

    /// Separator for a JOIN field: "; " by default, ", " for the two path
    /// fields (File name / Folder name), matching foobar2000.
    [[nodiscard]] static QString joinSeparator(FieldId id);

    /// True for fields hidden in any multi-selection (File path, Subsong index,
    /// Created, Audio MD5). The template's singleOnly flag is derived from this,
    /// so there is one source of truth.
    [[nodiscard]] static bool isSingleOnlyField(FieldId id);

    /// Extract one track's value for a field, as a display string. (Numeric
    /// aggregate fields format their single-track value here too, for any direct
    /// caller; aggregatedValue() computes the across-selection version itself.)
    [[nodiscard]] static QString fieldValue(const TrackData& t, FieldId id);

    // --- Shared value formatters (used by both single and aggregate paths) ---
    [[nodiscard]] static QString formatDurationSamples(qint64 totalMs, qint64 totalSamples);
    [[nodiscard]] static QString formatSize(qint64 bytes);
    [[nodiscard]] static QString formatBitrate(int kbps);

    // --- Across-selection numeric aggregates (read m_selection directly) -----
    [[nodiscard]] QString aggDuration() const;    ///< summed length + samples
    [[nodiscard]] QString aggTotalSize() const;   ///< summed bytes
    [[nodiscard]] QString aggAvgBitrate() const;  ///< duration-weighted mean
    [[nodiscard]] QString aggMaxModified() const; ///< most recent timestamp

    /// Group the selection by a field's per-track value and render each distinct
    /// value with its share of the selection, e.g. "libFLAC ... (51.4%); libFLAC
    /// ... (48.6%)". Shares are DURATION-weighted (matching how Avg. bitrate is
    /// weighted, and reproducing foobar's non-round percentages); when no track
    /// reports a duration every track weights equally, so equal-length or
    /// duration-less selections fall back to plain proportions. A single distinct
    /// value (or a single-track selection) renders bare, with no "(100%)". Empty
    /// per-track values group under "(unknown)" only when the selection is mixed;
    /// an all-empty field returns blank. Used for Tool (see aggStrategy).
    [[nodiscard]] QString aggWeightedShare(FieldId id) const;

    /// De-dupe + join a JOIN field across a multi-selection (size >= 2). Missing
    /// values become "(unknown)" only when the selection is a MIX; an all-missing
    /// field returns empty so conditional rows still hide. The distinct values
    /// are capped (a huge selection cannot build an unreadable, slow-to-shape
    /// megastring); past the cap a trailing ellipsis marks the truncation.
    [[nodiscard]] QString joinDeduped(FieldId id, const QString& sep) const;

    /// Aggregate a field across the current selection. Single selection returns
    /// the per-track value untouched (so a long path stays full + scrollable);
    /// an empty selection returns empty; otherwise it dispatches on aggStrategy.
    [[nodiscard]] QString aggregatedValue(FieldId id) const;

    /// Recompute m_valueCache (one aggregatedValue per distinct field) and
    /// m_distinctCount (for the two path fields' pluralization) for the current
    /// selection, so the expensive work runs once per selection, not per cell.
    void rebuildValueCache();

    QList<Row>       m_template;   ///< full static set (never changes after ctor)
    QList<Row>       m_rows;       ///< currently visible subset (varies per sel.)
    QList<TrackData> m_selection;  ///< current selected tracks (values source)
    QHash<FieldId, QString> m_valueCache;    ///< aggregated value per field
    QHash<FieldId, int>     m_distinctCount;  ///< distinct values, path fields only
    bool m_detailsMode = false;    ///< true = Location + General only (no Metadata, no Items Selected)
};

} // namespace rawform
