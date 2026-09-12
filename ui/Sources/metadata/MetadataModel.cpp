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

#include "metadata/MetadataModel.h"

#include "utils/Formats.h"
#include "playlist/PlaylistModel.h"

#include <QLocale>
#include <QSet>

#include <utility> // std::move

namespace rawform {

MetadataModel::MetadataModel(QObject* parent)
    : QAbstractTableModel(parent) {
    buildTemplate();
    rebuildValueCache();    // empty selection: every field aggregates to ""
    m_rows = visibleRows(); // so conditional / single-only rows are hidden until a selection
}

void MetadataModel::buildTemplate() {
    // Built once into m_template. Order matches foobar2000's pane. The field()
    // helper derives the SINGLE-ONLY flag from isSingleOnlyField(id), so the
    // "hidden in a multi-selection" set lives in exactly one place; only the
    // CONDITIONAL flag and the multi-selection label are passed per field.
    const auto section = [this](const QString& title) {
        m_template.push_back(Row{ true, title, FieldId::None, false, false, QString{} });
    };
    const auto field = [this](const QString& name, FieldId id,
                              bool conditional = false,
                              const QString& multiName = QString{}) {
        m_template.push_back(Row{ false, name, id, conditional,
                                  isSingleOnlyField(id), multiName });
    };

    // The Metadata section is omitted entirely in Details mode (the Properties
    // Details tab shows only Location + General; its sibling Metadata tab owns the
    // editable tag fields).
    if (!m_detailsMode) {
        section(QStringLiteral("Metadata"));
        field(QStringLiteral("Artist Name"),  FieldId::Artist);
        field(QStringLiteral("Track Title"),  FieldId::Title);
        field(QStringLiteral("Album Title"),  FieldId::Album);
        field(QStringLiteral("Date"),         FieldId::Year);
        field(QStringLiteral("Genre"),        FieldId::Genre);
        field(QStringLiteral("Album Artist"), FieldId::AlbumArtist);
        field(QStringLiteral("Track Number"), FieldId::TrackNo);
        field(QStringLiteral("Total Tracks"), FieldId::TrackTotal);
        field(QStringLiteral("Disc Number"),  FieldId::DiscNo);
        field(QStringLiteral("Total Discs"),  FieldId::DiscTotal);
    }

    section(QStringLiteral("Location"));
    // The two path fields pluralize their label in a multi-selection; they are
    // also the only JOIN fields that use ", " instead of "; " (see joinSeparator).
    field(QStringLiteral("File name"),     FieldId::FileName, /*cond=*/false,
          QStringLiteral("File names"));
    field(QStringLiteral("Folder name"),   FieldId::FolderName, /*cond=*/false,
          QStringLiteral("Folder names"));
    field(QStringLiteral("File path"),     FieldId::FilePath);     // single-only
    field(QStringLiteral("Subsong index"), FieldId::SubsongIndex); // single-only
    field(QStringLiteral("File size"),     FieldId::FileSize, /*cond=*/false,
          QStringLiteral("Total size"));
    field(QStringLiteral("Last modified"), FieldId::Modified);
    field(QStringLiteral("Created"),       FieldId::Created);      // single-only

    section(QStringLiteral("General"));
    // Items Selected is redundant in Details mode (the Properties window title
    // already reads "Properties - N items"), so it is omitted there.
    if (!m_detailsMode)
        field(QStringLiteral("Items Selected"), FieldId::ItemsSelected);
    field(QStringLiteral("Duration"),          FieldId::Length); // numeric aggregate (sum)
    field(QStringLiteral("Sample rate"),       FieldId::SampleRate);
    field(QStringLiteral("Channels"),          FieldId::Channels);
    field(QStringLiteral("Bits per sample"),   FieldId::BitsPerSample, /*conditional=*/true);
    field(QStringLiteral("Bitrate"),           FieldId::Bitrate, /*cond=*/false,
          QStringLiteral("Avg. bitrate")); // numeric aggregate (duration-weighted mean)
    field(QStringLiteral("Codec"),             FieldId::Codec);
    field(QStringLiteral("Codec Profile"),     FieldId::CodecProfile, /*conditional=*/true);
    field(QStringLiteral("Encoding"),          FieldId::Encoding);
    field(QStringLiteral("Tool"),              FieldId::Tool);
    field(QStringLiteral("Tag Type"),          FieldId::TagType, /*conditional=*/true);
    field(QStringLiteral("Embedded cuesheet"), FieldId::EmbeddedCuesheet);
    field(QStringLiteral("Audio MD5"),         FieldId::AudioMd5, /*conditional=*/true);
}

QList<MetadataModel::Row> MetadataModel::visibleRows() const {
    // Sections and always-on fields are always present. A single-only field is
    // dropped the moment the selection holds more than one track. A conditional
    // field is dropped when it resolves to an empty value. (Order matters:
    // single-only is checked first so a multi-selection never even consults the
    // value of, say, Audio MD5.)
    //
    // Sections never end up empty here: every section keeps at least one
    // always-on field even in a multi-selection (Metadata keeps Artist Name etc.,
    // Location keeps File names / Folder name / Total size / Last modified,
    // General keeps Items Selected etc.), so no empty-header pruning is needed.
    const bool multi = m_selection.size() > 1;
    QList<Row> out;
    out.reserve(m_template.size());
    for (const Row& r : m_template) {
        if (!r.isSection) {
            if (r.singleOnly && multi)
                continue;
            if (r.conditional && m_valueCache.value(r.field).isEmpty())
                continue;
        }
        out.push_back(r);
    }
    return out;
}

bool MetadataModel::sameShape(const QList<Row>& a, const QList<Row>& b) {
    if (a.size() != b.size())
        return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a.at(i).isSection != b.at(i).isSection || a.at(i).field != b.at(i).field)
            return false;
    }
    return true;
}

int MetadataModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
}

int MetadataModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(Column::Count);
}

QVariant MetadataModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const int row = index.row();
    if (row < 0 || row >= m_rows.size())
        return {};

    const Row& r = m_rows.at(row);

    switch (role) {
    case Qt::DisplayRole:
        switch (static_cast<Column>(index.column())) {
        case Column::Name: {
            if (r.isSection)
                return r.name;
            // The two path fields pluralize on the number of DISTINCT values,
            // not the selection size: two files in a single folder keep the
            // singular "Folder name", two folders flip it to "Folder names".
            if (r.field == FieldId::FileName || r.field == FieldId::FolderName)
                return m_distinctCount.value(r.field) > 1 ? r.multiName : r.name;
            // The remaining relabels (Total size, Avg. bitrate) switch purely on
            // the selection size: a multi-selection is a sum / average whether or
            // not the per-track values happen to differ.
            if (!r.multiName.isEmpty() && m_selection.size() > 1)
                return r.multiName;
            return r.name;
        }
        case Column::Value:
            if (r.isSection)
                return QString{};
            return m_valueCache.value(r.field); // aggregated once per selection
        case Column::Count:
            break;
        }
        return {};
    case IsSectionRole:
        return r.isSection;
    default:
        return {};
    }
}

QVariant MetadataModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return {};
    switch (static_cast<Column>(section)) {
    case Column::Name:  return QStringLiteral("Name");
    case Column::Value: return QStringLiteral("Value");
    case Column::Count: break;
    }
    return {};
}

QHash<int, QByteArray> MetadataModel::roleNames() const {
    return {
        { DisplayRole,   QByteArrayLiteral("display") },
        { IsSectionRole, QByteArrayLiteral("isSection") },
    };
}

bool MetadataModel::isSingleOnlyField(FieldId id) {
    switch (id) {
    case FieldId::FilePath:
    case FieldId::SubsongIndex:
    case FieldId::Created:
    case FieldId::AudioMd5:
        return true;
    default:
        return false;
    }
}

QString MetadataModel::joinSeparator(FieldId id) {
    switch (id) {
    case FieldId::FileName:
    case FieldId::FolderName:
        return QStringLiteral(", "); // foobar joins the path fields with a comma
    default:
        return QStringLiteral("; "); // everything else uses a semicolon
    }
}

MetadataModel::Agg MetadataModel::aggStrategy(FieldId id) {
    switch (id) {
    case FieldId::ItemsSelected: return Agg::Count;
    case FieldId::Length:        return Agg::SumDuration;
    case FieldId::FileSize:      return Agg::SumBytes;
    case FieldId::Bitrate:       return Agg::AvgBitrate;
    case FieldId::Modified:      return Agg::MaxModified;
    case FieldId::Tool:          return Agg::WeightedShare;
    default:                     return Agg::Join;
    }
}

QString MetadataModel::formatDurationSamples(qint64 totalMs, qint64 totalSamples) {
    if (totalMs <= 0)
        return {};
    const qint64 h  = totalMs / 3600000;
    const qint64 m  = (totalMs / 60000) % 60;
    const qint64 s  = (totalMs / 1000) % 60;
    const qint64 ms = totalMs % 1000;
    // H:MM:SS.mmm once an hour is reached, else M:SS.mmm (leading unit unpadded,
    // matching foobar's "5:00.720" and "43:05.960").
    QString clock = h > 0
        ? QStringLiteral("%1:%2:%3").arg(h)
              .arg(m, 2, 10, QLatin1Char('0'))
              .arg(s, 2, 10, QLatin1Char('0'))
        : QStringLiteral("%1:%2").arg(m)
              .arg(s, 2, 10, QLatin1Char('0'));
    clock += QStringLiteral(".%1").arg(ms, 3, 10, QLatin1Char('0'));
    // Group the sample count the same way the byte counts are grouped (locale
    // thousands separator), so "13 261 752" lines up with the File size format.
    const QString samples = QLocale().toString(totalSamples);
    return QStringLiteral("%1 (%2 samples)").arg(clock, samples);
}

QString MetadataModel::formatSize(qint64 bytes) {
    if (bytes <= 0)
        return {};
    // The compact half is the shared formatter (same string as the playlist's
    // File size column); this pane appends the exact byte count.
    return QStringLiteral("%1 (%2 bytes)")
        .arg(formats::fileSizeText(bytes), QLocale().toString(bytes));
}

QString MetadataModel::formatBitrate(int kbps) {
    return kbps > 0 ? QStringLiteral("%1 kbps").arg(kbps) : QString{};
}

QString MetadataModel::fieldValue(const TrackData& t, FieldId id) {
    const auto intOrEmpty = [](int v) {
        return v > 0 ? QString::number(v) : QString{};
    };
    const auto dateOrEmpty = [](const QDateTime& dt) {
        return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString{};
    };

    switch (id) {
    case FieldId::Artist:       return joinedValues(t.artists);
    case FieldId::Title:        return t.title;
    case FieldId::Album:        return t.album;
    case FieldId::Year:         return !t.dateRaw.isEmpty() ? t.dateRaw : intOrEmpty(t.year);
    case FieldId::Genre:        return joinedValues(t.genres);
    case FieldId::AlbumArtist:  return joinedValues(t.albumArtists);
    // Track / disc numbers prefer the tag's RAW text, exactly as Year prefers
    // dateRaw: a "05" stays "05" (the parsed int still drives sort and the
    // playlist column), and a non-numeric designator such as "A1" displays as
    // tagged instead of vanishing as int 0. Empty raw falls back to the int.
    case FieldId::TrackNo:      return !t.trackNoRaw.isEmpty() ? t.trackNoRaw : intOrEmpty(t.trackNo);
    case FieldId::TrackTotal:   return intOrEmpty(t.trackTotal);
    case FieldId::DiscNo:       return !t.discNoRaw.isEmpty() ? t.discNoRaw : intOrEmpty(t.discNo);
    case FieldId::DiscTotal:    return intOrEmpty(t.discTotal);
    case FieldId::FileName:     return t.fileName;
    case FieldId::FolderName:   return t.folderName;
    case FieldId::FilePath:     return t.filePath;
    case FieldId::SubsongIndex: return QString::number(t.subsongIndex);
    case FieldId::Modified:     return dateOrEmpty(t.modified);
    case FieldId::Created:      return dateOrEmpty(t.created);
    case FieldId::FileSize:     return formatSize(t.fileSize);
    case FieldId::ItemsSelected:
        // Synthetic: it has no per-track value. The count is supplied by
        // aggregatedValue()'s Agg::Count branch. Returning empty here keeps the
        // exhaustive switch honest without inventing a per-track meaning.
        return {};
    case FieldId::Length: {
        if (t.durationMs <= 0)
            return {};
        const qint64 samples =
            static_cast<qint64>(t.durationMs) * t.sampleRateHz / 1000;
        return formatDurationSamples(t.durationMs, samples);
    }
    case FieldId::SampleRate:
        return t.sampleRateHz > 0
            ? QStringLiteral("%1 Hz").arg(t.sampleRateHz) : QString{};
    case FieldId::Channels:
        // Subtlety kept (NOT foobar's bare integer): Mono / Stereo for the two
        // common cases, then the bare channel count for anything higher.
        switch (t.channels) {
        case 0:  return {};
        case 1:  return QStringLiteral("Mono");
        case 2:  return QStringLiteral("Stereo");
        default: return QString::number(t.channels);
        }
    case FieldId::Bitrate:
        return formatBitrate(t.bitrateKbps);
    case FieldId::Codec:
        return t.codec; // TrackReader's content-derived label; empty stays empty
    case FieldId::Encoding: {
        // Derived from the codec label, so there is no stored field and no cache
        // surface. Lowercase ("lossless" / "lossy") to match foobar. The label
        // is content-derived (TrackReader tells ALAC from AAC inside an m4a by
        // the MP4 properties), so the set below decides by codec, never by
        // container.
        if (t.codec.isEmpty())
            return {};
        static const QSet<QString> kLossless = {
            QStringLiteral("FLAC"), QStringLiteral("WAV"), QStringLiteral("AIFF"),
            QStringLiteral("WavPack"), QStringLiteral("Monkey's Audio"),
            QStringLiteral("TTA"), QStringLiteral("ALAC"), QStringLiteral("PCM")
        };
        return kLossless.contains(t.codec)
            ? QStringLiteral("lossless") : QStringLiteral("lossy");
    }
    case FieldId::Tool:
        return t.tool; // empty stays empty (always-on row, shows blank)
    case FieldId::EmbeddedCuesheet:
        // Lowercase yes / no, to match foobar. An empty selection is handled
        // upstream by aggregatedValue returning empty, so the row blanks there.
        return t.hasEmbeddedCuesheet ? QStringLiteral("yes")
                                     : QStringLiteral("no");
    case FieldId::BitsPerSample:
        // Subtlety kept: the "bit" suffix stays (foobar shows a bare integer).
        // Conditional: 0 means lossy / not applicable, so the row hides.
        return t.bitsPerSample > 0 ? QStringLiteral("%1 bit").arg(t.bitsPerSample)
                                   : QString{};
    case FieldId::CodecProfile:
        return t.codecProfile; // conditional; empty (most files) hides the row
    case FieldId::TagType:
        return t.tagType;      // conditional; empty hides the row
    case FieldId::AudioMd5:
        // Uppercase hex, to match foobar. Conditional + single-only: empty
        // (non-FLAC / unset) hides it, and any multi-selection hides it too.
        return t.audioMd5.toUpper();
    case FieldId::None:
        break;
    }
    return {};
}

QString MetadataModel::aggDuration() const {
    qint64 totalMs = 0;
    qint64 totalSamples = 0;
    for (const TrackData& t : m_selection) {
        if (t.durationMs <= 0)
            continue;
        totalMs += t.durationMs;
        // Per-track sample count, summed: handles a selection that spans several
        // sample rates correctly (each track's samples = ms * its own rate).
        totalSamples += static_cast<qint64>(t.durationMs) * t.sampleRateHz / 1000;
    }
    return formatDurationSamples(totalMs, totalSamples);
}

QString MetadataModel::aggTotalSize() const {
    qint64 total = 0;
    for (const TrackData& t : m_selection)
        if (t.fileSize > 0)
            total += t.fileSize;
    return formatSize(total);
}

QString MetadataModel::aggAvgBitrate() const {
    // Duration-weighted mean, which equals total audio bits / total duration:
    // a 30s 320kbps track and a 5min 128kbps track average toward 128, not 224.
    // Falls back to a plain mean only if no track reports a duration.
    qint64 weighted = 0; // sum(bitrate_i * durationMs_i)
    qint64 totalMs  = 0;
    int simpleSum = 0;
    int simpleCnt = 0;
    for (const TrackData& t : m_selection) {
        if (t.bitrateKbps <= 0)
            continue;
        simpleSum += t.bitrateKbps;
        ++simpleCnt;
        if (t.durationMs > 0) {
            weighted += static_cast<qint64>(t.bitrateKbps) * t.durationMs;
            totalMs  += t.durationMs;
        }
    }
    int avg = 0;
    if (totalMs > 0)
        avg = static_cast<int>((weighted + totalMs / 2) / totalMs); // rounded
    else if (simpleCnt > 0)
        avg = (simpleSum + simpleCnt / 2) / simpleCnt;              // rounded
    return formatBitrate(avg);
}

QString MetadataModel::aggMaxModified() const {
    // Most recent modified timestamp across the selection. Foobar keeps "Last
    // modified" in a multi-selection (unlike "Created", which it drops), so the
    // single most meaningful value is the latest touch.
    QDateTime best;
    for (const TrackData& t : m_selection) {
        if (t.modified.isValid() && (!best.isValid() || t.modified > best))
            best = t.modified;
    }
    return best.isValid()
        ? best.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QString{};
}

QString MetadataModel::aggWeightedShare(FieldId id) const {
    // Group the selection by per-track value, accumulating a WEIGHT per group,
    // then render "value (share%)" for each. Reproduces foobar's Tool field in a
    // multi-selection, e.g. "...1.2.1... (51.4%); ...1.1.4... (48.6%)".
    //
    // Weight is the track duration: foobar's percentages are non-round (51.4 on
    // two tracks is not a track count), and duration is the same weighting it
    // uses for Avg. bitrate. If NO track reports a duration we fall back to a
    // weight of 1 each, so equal-length or duration-less selections degrade to
    // plain track proportions (the tidy 25 / 25 / 50 case).
    bool anyDuration = false;
    for (const TrackData& t : m_selection) {
        if (t.durationMs > 0) {
            anyDuration = true;
            break;
        }
    }

    // First-seen order is preserved (groups[]), with an index map for the lookup.
    // Empty values group under "(unknown)"; we also track whether ANY real value
    // exists, so an all-empty field returns blank (so always-on rows stay empty
    // and conditional rows still hide) rather than a lone "(unknown) (100%)".
    static const QString kUnknown = QStringLiteral("(unknown)");
    struct Group { QString value; qint64 weight = 0; };
    QList<Group> groups;
    QHash<QString, qsizetype> indexOf;
    qint64 total = 0;
    bool anyPresent = false;

    for (const TrackData& t : m_selection) {
        const QString v = fieldValue(t, id);
        if (!v.isEmpty())
            anyPresent = true;
        const QString key = v.isEmpty() ? kUnknown : v;
        // A duration-less track contributes nothing to the shares when other
        // tracks do carry durations; in the no-durations-at-all case every track
        // weighs 1 (handled by the anyDuration branch).
        const qint64 w = anyDuration ? (t.durationMs > 0 ? t.durationMs : 0) : 1;
        total += w;

        const auto it = indexOf.constFind(key);
        if (it != indexOf.constEnd())
            groups[it.value()].weight += w;
        else {
            indexOf.insert(key, groups.size());
            groups.push_back(Group{ key, w });
        }
    }

    if (!anyPresent)
        return {};

    // A single distinct value renders bare: a uniform multi-selection (or a
    // single track) shows just the tool, never a redundant "(100%)".
    if (groups.size() == 1)
        return groups.first().value;

    // Two or more distinct values: each gets its duration-weighted share, to one
    // decimal (foobar's precision; rounding can leave the sum at 99.9, as it does
    // in foobar). The distinct-value cap mirrors joinDeduped; a truncated list's
    // shares are necessarily partial, but 20+ distinct encoders in one selection
    // is pathological.
    constexpr int kMaxDistinctValues = 20;
    const QString sep = QStringLiteral("; ");
    QStringList out;
    for (const Group& g : groups) {
        if (out.size() == kMaxDistinctValues)
            return out.join(sep) + sep + QStringLiteral("…");
        const double share = total > 0
            ? static_cast<double>(g.weight) * 100.0 / static_cast<double>(total)
            : 0.0;
        out.push_back(QStringLiteral("%1 (%2%)").arg(g.value, QString::number(share, 'f', 1)));
    }
    return out.join(sep);
}

QString MetadataModel::joinDeduped(FieldId id, const QString& sep) const {
    // Precondition: at least two tracks selected (single selection is handled in
    // aggregatedValue, which returns the per-track value untouched).
    //
    // First decide whether this is a MIX: collect the per-track values, noting
    // whether any track carries the tag at all. If NONE do, the field is blank
    // for the whole selection (so always-on rows show empty and conditional rows
    // hide); we do NOT manufacture a lone "(unknown)". If SOME do, the tracks
    // that lack it contribute the literal "(unknown)" placeholder.
    QStringList raw;
    raw.reserve(static_cast<qsizetype>(m_selection.size()));
    bool anyPresent = false;
    for (const TrackData& t : m_selection) {
        const QString v = fieldValue(t, id);
        if (!v.isEmpty())
            anyPresent = true;
        raw.push_back(v);
    }
    if (!anyPresent)
        return {};

    // De-dupe preserving first-seen order, substituting "(unknown)" for the gaps,
    // and cap the distinct count so a large mixed selection cannot build an
    // unreadable, slow-to-shape megastring. Exactly kMaxDistinctValues with no
    // further distinct value shows in full (no ellipsis); one more triggers the
    // truncation marker.
    constexpr int kMaxDistinctValues = 20;
    static const QString kUnknown = QStringLiteral("(unknown)");
    QStringList ordered;
    QSet<QString> seen;
    for (const QString& v : raw) {
        const QString disp = v.isEmpty() ? kUnknown : v;
        if (seen.contains(disp))
            continue;
        if (ordered.size() == kMaxDistinctValues)
            return ordered.join(sep) + sep + QStringLiteral("…");
        seen.insert(disp);
        ordered.push_back(disp);
    }
    return ordered.join(sep);
}

QString MetadataModel::aggregatedValue(FieldId id) const {
    if (m_selection.isEmpty())
        return {};

    // Numeric aggregates produce a single value for any selection size (and so
    // also serve the single-selection case directly: a sum / average / max over
    // one track is that track's value).
    switch (aggStrategy(id)) {
    case Agg::Count:         return QString::number(m_selection.size());
    case Agg::SumDuration:   return aggDuration();
    case Agg::SumBytes:      return aggTotalSize();
    case Agg::AvgBitrate:    return aggAvgBitrate();
    case Agg::MaxModified:   return aggMaxModified();
    case Agg::WeightedShare: return aggWeightedShare(id);
    case Agg::Join:          break; // handled below
    }

    // Single-only fields never contribute a value in a multi-selection; short-
    // circuit so we never build (e.g.) a join of 50 distinct Audio MD5 hashes
    // that the row would then hide anyway.
    if (isSingleOnlyField(id) && m_selection.size() > 1)
        return {};

    // JOIN: one track stays untouched (a long path keeps its full text and stays
    // horizontally scrollable); two or more de-dupe + join with the field's
    // separator, filling gaps with "(unknown)" only when the selection is mixed.
    if (m_selection.size() == 1)
        return fieldValue(m_selection.first(), id);
    return joinDeduped(id, joinSeparator(id));
}

void MetadataModel::rebuildValueCache() {
    m_valueCache.clear();
    for (const Row& r : m_template) {
        if (r.isSection || m_valueCache.contains(r.field))
            continue;
        m_valueCache.insert(r.field, aggregatedValue(r.field));
    }

    // Distinct-value counts for the two path fields only, used solely to decide
    // their Name label ("File name" vs "File names", "Folder name" vs "Folder
    // names"). Counting distinct values, not selection size, is what keeps two
    // files in one folder on the singular "Folder name".
    m_distinctCount.clear();
    for (const FieldId id : { FieldId::FileName, FieldId::FolderName }) {
        QSet<QString> distinct;
        for (const TrackData& t : m_selection) {
            const QString v = fieldValue(t, id);
            if (!v.isEmpty())
                distinct.insert(v);
        }
        m_distinctCount.insert(id, static_cast<int>(distinct.size()));
    }
}

void MetadataModel::setSelection(PlaylistModel* playlist, const QList<int>& rows) {
    m_selection.clear();
    if (playlist) {
        for (int row : rows) {
            if (const TrackData* t = playlist->trackAt(row))
                m_selection.push_back(*t);
        }
    }
    rebuildValueCache(); // aggregate once per field; reads below hit the cache

    // Rebuild the visible rows for the new selection, then choose the cheapest
    // correct notification:
    //  - Same shape (no conditional / single-only row appeared or disappeared):
    //    only the cell CONTENTS changed. Swap the rows and emit one dataChanged
    //    over BOTH columns: the Value text changed, and so can a Name label
    //    (Total size, File names, ...), so the Name column must be re-read too.
    //    Delegates are reused, so rapid playlist navigation stays flicker-free.
    //  - Shape changed: a structural reset is required.
    QList<Row> next = visibleRows();
    if (sameShape(next, m_rows)) {
        m_rows = std::move(next);
        if (!m_rows.isEmpty()) {
            const QModelIndex top = index(0, static_cast<int>(Column::Name));
            const QModelIndex bottom = index(static_cast<int>(m_rows.size()) - 1,
                                             static_cast<int>(Column::Value));
            emit dataChanged(top, bottom, { Qt::DisplayRole });
        }
    } else {
        beginResetModel();
        m_rows = std::move(next);
        endResetModel();
    }
}

void MetadataModel::clear() {
    setSelection(nullptr, {});
}

void MetadataModel::setDetailsMode(bool on) {
    if (m_detailsMode == on)
        return;
    m_detailsMode = on;
    // The template (which sections/fields exist at all) depends on the mode, so a
    // change rebuilds it from scratch under a full reset. In practice QML sets this
    // once right after construction, before any selection, so the reset is free.
    beginResetModel();
    m_template.clear();
    buildTemplate();
    rebuildValueCache();
    m_rows = visibleRows();
    endResetModel();
    emit detailsModeChanged();
}

bool MetadataModel::isSectionRow(int row) const {
    if (row < 0 || row >= m_rows.size())
        return false;
    return m_rows.at(row).isSection;
}

int MetadataModel::fieldIndexInSection(int row) const {
    if (row < 0 || row >= m_rows.size() || m_rows.at(row).isSection)
        return 0;
    // Walk back to the section header; count field rows in between.
    int idx = 0;
    for (int r = row - 1; r >= 0; --r) {
        if (m_rows.at(r).isSection)
            break;
        ++idx;
    }
    return idx;
}

QString MetadataModel::valueAt(int row) const {
    // Same source the Value column renders from (see data(), Column::Value), but
    // reachable from QML by plain row index. Section rows carry no value, so they
    // measure as empty (zero width) and never widen the Value column.
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row& r = m_rows.at(row);
    if (r.isSection)
        return {};
    return m_valueCache.value(r.field); // aggregated once per selection
}

} // namespace rawform
