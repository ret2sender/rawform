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

#include "metadata/PropertiesMetadataModel.h"

#include <QChar>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm> // std::ranges::sort, std::ranges::any_of
#include <functional> // std::ranges::greater

namespace rawform {

namespace {

// The fixed, always-shown editable schema, in foobar's order. Each entry is a
// {display label, canonical tag key} pair. Composer / Performer / Comment are not
// promoted to TrackData fields, so they read from extraTags by these keys.
struct SchemaField {
    const char* label;
    const char* key;
};
const SchemaField kSchema[] = {
    { "Artist Name",  "ARTIST" },
    { "Track Title",  "TITLE" },
    { "Album Title",  "ALBUM" },
    { "Date",         "DATE" },
    { "Genre",        "GENRE" },
    { "Composer",     "COMPOSER" },
    { "Performer",    "PERFORMER" },
    { "Album Artist", "ALBUMARTIST" },
    { "Track Number", "TRACKNUMBER" },
    { "Total Tracks", "TOTALTRACKS" },
    { "Disc Number",  "DISCNUMBER" },
    { "Total Discs",  "TOTALDISCS" },
    { "Comment",      "COMMENT" },
};

// The guillemet marker foobar prefixes onto a value that disagrees across the
// selection. Built from QChars so this source file stays pure ASCII (the only
// place the glyphs exist is at runtime). Reads "<<multiple values>> " with proper
// guillemets and a trailing space before the per-track list.
QString multipleMarker() {
    static const QString kMarker =
        QChar(0x00AB) + QStringLiteral("multiple values") + QChar(0x00BB)
        + QLatin1Char(' ');
    return kMarker;
}

// --- display cap -------------------------------------------------------
// Hard ceiling on any VALUE-CELL display string this model produces. The cell is
// one elided line; no window width can show more than a few hundred characters.
// The scenario this prevents: joining EVERY per-track value of a 15k-track
// selection into one string (megabytes per row for common tags, tens of
// megabytes when a lyrics- or log-sized tag disagrees), which QQuickText then
// shapes IN FULL to elide it, an OOM. The cap is display only: m_original /
// m_staged, editText, the Edit Value dialog seeds, and collectEdits all read the
// full per-track lists. 2048 chars comfortably overfills any realistic cell
// while keeping the worst-case layout cost trivial.
constexpr qsizetype kDisplayCapChars = 2048;

// U+2026 HORIZONTAL ELLIPSIS, built from a QChar so the source stays pure ASCII
// (same convention as the guillemet marker above).
QChar displayEllipsis() {
    return QChar(0x2026);
}

// Cap a single display value. Used for the uniform branch of collapseList (a
// selection can agree on a huge value, e.g. a shared album-wide log tag; agreeing
// does not make it renderable) and for any other one-value display surface.
QString capDisplay(const QString& s) {
    if (s.size() <= kDisplayCapChars)
        return s;
    QString out = s.left(kDisplayCapChars);
    out.append(displayEllipsis());
    return out;
}

// --- per-field value policy --------------------------------------------
// List-type keys hold an ordered set of distinct values (multiple Vorbis fields);
// every other key is scalar (exactly one value, written verbatim and never split,
// so a comment or a date that happens to contain a semicolon stays intact). The
// schema's text fields and the number pairs are scalar; ARTIST / ALBUMARTIST /
// GENRE / COMPOSER / PERFORMER are lists; any custom "<KEY>" row defaults to list,
// matching foobar.
bool isListKey(const QString& key) {
    static const QSet<QString> kList = {
        QStringLiteral("ARTIST"), QStringLiteral("ALBUMARTIST"),
        QStringLiteral("GENRE"), QStringLiteral("COMPOSER"),
        QStringLiteral("PERFORMER"),
    };
    static const QSet<QString> kScalar = {
        QStringLiteral("TITLE"), QStringLiteral("ALBUM"), QStringLiteral("DATE"),
        QStringLiteral("TRACKNUMBER"), QStringLiteral("TOTALTRACKS"),
        QStringLiteral("DISCNUMBER"), QStringLiteral("TOTALDISCS"),
        QStringLiteral("COMMENT"),
    };
    if (kList.contains(key))
        return true;
    if (kScalar.contains(key))
        return false;
    return true; // custom keys default to list
}

// Split a list-type value into its distinct values. The separator is a semicolon
// with optional surrounding whitespace, so "a;b", "a ; b", and "a; b" all yield
// {a, b}; each value is trimmed and empties are dropped.
QStringList splitList(const QString& raw) {
    static const QRegularExpression kSep(QStringLiteral("\\s*;\\s*"));
    QStringList out;
    const QStringList parts = raw.split(kSep, Qt::SkipEmptyParts);
    for (const QString& p : parts) {
        const QString t = p.trimmed();
        if (!t.isEmpty())
            out.push_back(t);
    }
    return out;
}

// Conform a typed value to its canonical display form per field policy: a list
// key collapses to "; "-joined distinct values; a scalar key is just trimmed (its
// internal semicolons are preserved as one value).
QString normalizeForDisplay(const QString& key, const QString& raw) {
    if (isListKey(key))
        return splitList(raw).join(QStringLiteral("; "));
    return raw.trimmed();
}

// Build the explicit value list to write for a key from its (already normalized)
// per-track display value. Empty display -> empty list (a removal). A list key
// splits back into distinct values; a scalar key is exactly one value.
QStringList valueListFor(const QString& key, const QString& displayValue) {
    if (displayValue.isEmpty())
        return {};
    if (isListKey(key))
        return splitList(displayValue);
    return QStringList{ displayValue };
}

// Capitalize the first letter of every whitespace-delimited word and lower-case
// the rest, so "cigARETTES aFTER sex" becomes "Cigarettes After Sex". The word
// boundary is whitespace only, so "o'brien" -> "O'brien" and "hip-hop" ->
// "Hip-hop". Applied to the joined display of a list field, so "folk; blues"
// becomes "Folk; Blues" (the "; " re-splits on normalization).
QString capitalizeValue(const QString& s) {
    QString out = s;
    bool startOfWord = true;
    for (int i = 0; i < out.size(); ++i) {
        const QChar c = out.at(i);
        if (c.isSpace()) {
            startOfWord = true;
        } else {
            out[i] = startOfWord ? c.toUpper() : c.toLower();
            startOfWord = false;
        }
    }
    return out;
}

// Underscores to spaces, then trim and collapse internal whitespace runs to a
// single space: "  foo___bar  baz " becomes "foo bar baz".
QString cleanValue(const QString& s) {
    QString out = s;
    out.replace(QLatin1Char('_'), QLatin1Char(' '));
    return out.simplified();
}

} // namespace

PropertiesMetadataModel::PropertiesMetadataModel(QObject* parent)
    : QAbstractTableModel(parent) {
    rebuild(); // empty selection: schema rows with blank values, then "+ add new"
}

bool PropertiesMetadataModel::isReservedKey(const QString& key) {
    // Keys that must not surface as "<custom>" rows: the schema's own keys and
    // their read-aliases (shown by the fixed rows), plus technical keys owned by
    // the reader or other tabs. ENCODER / ENCODING / CUESHEET are consumed before
    // extraTags is filled, but are listed defensively. YEAR and ORIGINALDATE are
    // deliberately NOT reserved: DATE alone owns the schema's Date field
    // (TrackReader falls back to neither), so both read as ordinary "<YEAR>" /
    // "<ORIGINALDATE>" custom rows, visible and removable; reserving them here
    // would hide exactly the on-disk keys that could resurrect a removed Date.
    static const QSet<QString> kReserved = {
        QStringLiteral("ARTIST"),      QStringLiteral("TITLE"),
        QStringLiteral("ALBUM"),       QStringLiteral("DATE"),
        QStringLiteral("GENRE"),       QStringLiteral("COMPOSER"),
        QStringLiteral("PERFORMER"),   QStringLiteral("ALBUMARTIST"),
        QStringLiteral("TRACKNUMBER"), QStringLiteral("TRACKTOTAL"),
        QStringLiteral("TOTALTRACKS"), QStringLiteral("DISCNUMBER"),
        QStringLiteral("DISCTOTAL"),   QStringLiteral("TOTALDISCS"),
        QStringLiteral("COMMENT"),     QStringLiteral("ENCODER"),
        QStringLiteral("ENCODING"),    QStringLiteral("CUESHEET"),
        QStringLiteral("REPLAYGAIN_TRACK_GAIN"),
        QStringLiteral("REPLAYGAIN_TRACK_PEAK"),
        QStringLiteral("REPLAYGAIN_ALBUM_GAIN"),
        QStringLiteral("REPLAYGAIN_ALBUM_PEAK"),
    };
    return kReserved.contains(key);
}

QString PropertiesMetadataModel::perTrackValue(const TrackData& t, const QString& key) {
    // Promoted fields read straight off TrackData; the raw track/disc text is
    // preferred so a tagged "05" stays "05" here too. Everything else comes from
    // extraTags. joinedValues collapses a within-track multi-value with "; ".
    if (key == QStringLiteral("ARTIST"))      return joinedValues(t.artists);
    if (key == QStringLiteral("TITLE"))       return t.title;
    if (key == QStringLiteral("ALBUM"))       return t.album;
    if (key == QStringLiteral("DATE"))
        return !t.dateRaw.isEmpty() ? t.dateRaw
             : (t.year > 0 ? QString::number(t.year) : QString{});
    if (key == QStringLiteral("GENRE"))       return joinedValues(t.genres);
    if (key == QStringLiteral("ALBUMARTIST")) return joinedValues(t.albumArtists);
    if (key == QStringLiteral("TRACKNUMBER"))
        return !t.trackNoRaw.isEmpty() ? t.trackNoRaw
             : (t.trackNo > 0 ? QString::number(t.trackNo) : QString{});
    if (key == QStringLiteral("TOTALTRACKS"))
        return t.trackTotal > 0 ? QString::number(t.trackTotal) : QString{};
    if (key == QStringLiteral("DISCNUMBER"))
        return !t.discNoRaw.isEmpty() ? t.discNoRaw
             : (t.discNo > 0 ? QString::number(t.discNo) : QString{});
    if (key == QStringLiteral("TOTALDISCS"))
        return t.discTotal > 0 ? QString::number(t.discTotal) : QString{};
    // COMPOSER, PERFORMER, COMMENT, and all custom keys.
    return joinedValues(t.extraTags.value(key));
}

QString PropertiesMetadataModel::collapseList(const QStringList& perTrack, bool& multipleOut) {
    multipleOut = false;
    if (perTrack.isEmpty())
        return {};

    // Disagreement scan first (cheap: shared-data compares short-circuit on the
    // first mismatch), so the uniform path never allocates anything but the cap.
    const QString& first = perTrack.first();
    bool differs = false;
    for (const QString& v : perTrack) {
        if (v != first) {
            differs = true;
            break;
        }
    }
    if (!differs)
        return capDisplay(first); // single track, or all equal; capped

    // Disagreeing selection: the guillemet marker plus the per-track values in
    // selection order, EXCEPT that the join is built incrementally and stops at
    // kDisplayCapChars (see the display-cap block above for the megastring this
    // prevents). The full per-track truth is never needed here because this
    // string only feeds a one-line elided cell (per-track inspection lives in
    // the Edit Value dialog, which reads effectivePerTrack, not this).
    // QStringView::left keeps even a single oversized value (a lyrics-sized
    // tag) from being appended in full before truncation.
    multipleOut = true;
    static const QString kSep = QStringLiteral("; ");
    QString out = multipleMarker();
    out.reserve(kDisplayCapChars + 2);
    for (qsizetype i = 0; i < perTrack.size(); ++i) {
        if (out.size() >= kDisplayCapChars) {
            out.append(displayEllipsis()); // more entries remain past the cap
            return out;
        }
        if (i > 0)
            out += kSep;
        const QString& v = perTrack.at(i);
        const qsizetype room = kDisplayCapChars - out.size();
        if (v.size() > room) {
            out += QStringView{ v }.left(room);
            out.append(displayEllipsis());
            return out;
        }
        out += v;
    }
    return out; // the whole join fit under the cap
}

QString PropertiesMetadataModel::aggregateValue(const QString& key, bool& multipleOut) const {
    QStringList perTrack;
    perTrack.reserve(static_cast<qsizetype>(m_selection.size()));
    for (const TrackData& t : m_selection)
        perTrack.push_back(perTrackValue(t, key));
    return collapseList(perTrack, multipleOut);
}

QStringList PropertiesMetadataModel::effectivePerTrack(const QString& key) const {
    // The staged edit wins while present; otherwise the on-disk original. Both are
    // length == selection size (rebuild fills originals for every row key).
    return m_staged.contains(key) ? m_staged.value(key) : m_original.value(key);
}

int PropertiesMetadataModel::rowForKey(const QString& key) const {
    for (int r = 0; r < m_rows.size(); ++r) {
        const Row& row = m_rows.at(r);
        if ((row.kind == Kind::Field || row.kind == Kind::Custom) && row.key == key)
            return r;
    }
    return -1;
}

void PropertiesMetadataModel::refreshRowForKey(const QString& key) {
    const int r = rowForKey(key);
    if (r < 0)
        return;
    Row& row = m_rows[r];
    bool multiple = false;
    row.value = collapseList(effectivePerTrack(key), multiple);
    row.multiple = multiple;
    row.dirty = m_staged.contains(key);
    emit dataChanged(index(r, 0), index(r, static_cast<int>(Column::Value)),
                     { DisplayRole, ValueRole, IsMultipleRole, IsDirtyRole });
}

void PropertiesMetadataModel::rebuild() {
    m_rows.clear();
    m_original.clear();
    m_staged.clear();

    // Register one field/custom row: snapshot its on-disk per-track values into
    // m_original, then collapse to the display value + multiple flag. (At rebuild
    // time staging is empty, so effective == original.)
    const auto addFieldRow = [this](Kind kind, const QString& label, const QString& key) {
        QStringList perTrack;
        perTrack.reserve(static_cast<qsizetype>(m_selection.size()));
        for (const TrackData& t : m_selection)
            perTrack.push_back(perTrackValue(t, key));
        m_original.insert(key, perTrack);
        bool multiple = false;
        const QString value = collapseList(perTrack, multiple);
        m_rows.push_back(Row{ kind, label, key, value, multiple, false });
    };

    // The single "Metadata" section header.
    m_rows.push_back(Row{ Kind::Section, QStringLiteral("Metadata"), {}, {}, false, false });

    // The fixed schema, always present even when empty.
    for (const SchemaField& f : kSchema)
        addFieldRow(Kind::Field, QString::fromLatin1(f.label), QString::fromLatin1(f.key));

    // Custom rows: every non-reserved tag key present anywhere in the selection,
    // alphabetized for a stable order, rendered "<KEY>".
    QSet<QString> customKeys;
    for (const TrackData& t : m_selection) {
        for (auto it = t.extraTags.constBegin(); it != t.extraTags.constEnd(); ++it) {
            if (!isReservedKey(it.key()))
                customKeys.insert(it.key());
        }
    }
    QStringList sortedCustom(customKeys.constBegin(), customKeys.constEnd());
    std::ranges::sort(sortedCustom);
    for (const QString& key : sortedCustom)
        addFieldRow(Kind::Custom, QLatin1Char('<') + key + QLatin1Char('>'), key);

    // Trailing affordance.
    m_rows.push_back(Row{ Kind::Add, QStringLiteral("+ add new"), {}, {}, false, false });
}

void PropertiesMetadataModel::setSelection(PlaylistModel* playlist, const QList<int>& rows) {
    const bool wasDirty = !m_staged.isEmpty();
    beginResetModel();
    m_selection.clear();
    if (playlist) {
        for (int row : rows) {
            if (const TrackData* t = playlist->trackAt(row))
                m_selection.push_back(*t);
        }
    }
    rebuild(); // clears staging + originals, repopulates both
    endResetModel();
    if (wasDirty) // a fresh selection is never dirty
        emit dirtyChanged();
}

int PropertiesMetadataModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
}

int PropertiesMetadataModel::columnCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(Column::Count);
}

QVariant PropertiesMetadataModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const int row = index.row();
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row& r = m_rows.at(row);

    switch (role) {
    case Qt::DisplayRole:
        switch (static_cast<Column>(index.column())) {
        case Column::Name:
            return r.name;
        case Column::Value:
            return (r.kind == Kind::Field || r.kind == Kind::Custom) ? r.value : QString{};
        case Column::Count:
            break;
        }
        return {};
    case ValueRole:
        return (r.kind == Kind::Field || r.kind == Kind::Custom) ? r.value : QString{};
    case IsSectionRole:  return r.kind == Kind::Section;
    case IsAddRole:      return r.kind == Kind::Add;
    case IsCustomRole:   return r.kind == Kind::Custom;
    case IsMultipleRole: return r.multiple;
    case IsDirtyRole:    return r.dirty;
    default:
        return {};
    }
}

QVariant PropertiesMetadataModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal)
        return {};
    switch (static_cast<Column>(section)) {
    case Column::Name:  return QStringLiteral("Name");
    case Column::Value: return QStringLiteral("Value");
    case Column::Count: break;
    }
    return {};
}

QHash<int, QByteArray> PropertiesMetadataModel::roleNames() const {
    return {
        { DisplayRole,   QByteArrayLiteral("display") },
        { ValueRole,     QByteArrayLiteral("value") },
        { IsSectionRole, QByteArrayLiteral("isSection") },
        { IsAddRole,     QByteArrayLiteral("isAdd") },
        { IsCustomRole,  QByteArrayLiteral("isCustom") },
        { IsMultipleRole, QByteArrayLiteral("isMultiple") },
        { IsDirtyRole,   QByteArrayLiteral("isDirty") },
    };
}

bool PropertiesMetadataModel::isSectionRow(int row) const {
    return row >= 0 && row < m_rows.size() && m_rows.at(row).kind == Kind::Section;
}

bool PropertiesMetadataModel::isAddRow(int row) const {
    return row >= 0 && row < m_rows.size() && m_rows.at(row).kind == Kind::Add;
}

int PropertiesMetadataModel::fieldIndexInSection(int row) const {
    if (row < 0 || row >= m_rows.size())
        return 0;
    const Kind k = m_rows.at(row).kind;
    if (k == Kind::Section || k == Kind::Add)
        return 0;
    // Count the field/custom rows between the section header and this row.
    int idx = 0;
    for (int r = row - 1; r >= 0; --r) {
        if (m_rows.at(r).kind == Kind::Section)
            break;
        ++idx;
    }
    return idx;
}

QString PropertiesMetadataModel::valueAt(int row) const {
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row& r = m_rows.at(row);
    return (r.kind == Kind::Field || r.kind == Kind::Custom) ? r.value : QString{};
}

QString PropertiesMetadataModel::editText(int row) const {
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row& r = m_rows.at(row);
    if (r.kind != Kind::Field && r.kind != Kind::Custom)
        return {};
    const QStringList eff = effectivePerTrack(r.key);
    if (eff.size() <= 1)
        return eff.value(0); // single track or empty selection
    // Uniform -> the shared value; differing -> the per-track list joined "; "
    // (the inverse of the "multiple values" marker, ready to edit positionally).
    const QString& first = eff.first();
    for (const QString& v : eff) {
        if (v != first)
            return eff.join(QStringLiteral("; "));
    }
    return first;
}

QString PropertiesMetadataModel::editValidatorPattern(int row) const {
    static const QString kAny = QStringLiteral("^.*$");
    if (row < 0 || row >= m_rows.size())
        return kAny;
    const Row& r = m_rows.at(row);
    if (r.kind != Kind::Field && r.kind != Kind::Custom)
        return kAny;
    const QString& key = r.key;

    // Inline editing is single-track, so no inter-track separator is ever typed:
    // numeric fields are digits only, Date is digits and dash, and list fields
    // (which carry the "; " multi-value separator) are unrestricted.
    const bool numeric =
        key == QStringLiteral("TRACKNUMBER") || key == QStringLiteral("TOTALTRACKS") ||
        key == QStringLiteral("DISCNUMBER")  || key == QStringLiteral("TOTALDISCS");
    if (numeric)
        return QStringLiteral("^[0-9]*$");
    if (key == QStringLiteral("DATE"))
        return QStringLiteral("^[0-9-]*$"); // YYYY, YYYY-MM, YYYY-MM-DD
    return kAny;
}

void PropertiesMetadataModel::applyInlineEdit(int row, const QString& text) {
    if (row < 0 || row >= m_rows.size())
        return;
    const Row& r = m_rows.at(row);
    if (r.kind != Kind::Field && r.kind != Kind::Custom)
        return;
    const QString key = r.key;
    // Inline editing is single-track only; a multi-selection edits through the Edit
    // Value dialog. The pane gates this, so n != 1 should not reach us; no-op
    // defensively if it does.
    if (m_selection.size() != 1)
        return;

    // Conform the typed text to its canonical display form per field policy: list
    // keys collapse "a;b" / "a ; b" -> "a; b" (distinct values); scalar keys are
    // just trimmed and keep any internal semicolons as one value.
    stageList(key, QStringList{ normalizeForDisplay(key, text) });
}

void PropertiesMetadataModel::stageList(const QString& key, const QStringList& newList) {
    // Drop the stage if it equals the on-disk value (so dirty() stays exact and we
    // never write a no-op); otherwise record it. Refresh the row either way.
    bool changedStaging = false;
    if (newList == m_original.value(key)) {
        if (m_staged.remove(key) > 0)
            changedStaging = true;
    } else {
        m_staged.insert(key, newList);
        changedStaging = true;
    }
    if (changedStaging) {
        refreshRowForKey(key);
        emit dirtyChanged();
    }
}

QString PropertiesMetadataModel::fieldLabel(int row) const {
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row& r = m_rows.at(row);
    return (r.kind == Kind::Field || r.kind == Kind::Custom) ? r.name : QString{};
}

QString PropertiesMetadataModel::fieldKey(int row) const {
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row& r = m_rows.at(row);
    return (r.kind == Kind::Field || r.kind == Kind::Custom) ? r.key : QString{};
}

QString PropertiesMetadataModel::sharedEditValue(int row) const {
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row& r = m_rows.at(row);
    if (r.kind != Kind::Field && r.kind != Kind::Custom)
        return {};
    const QStringList eff = effectivePerTrack(r.key);
    if (eff.isEmpty())
        return {};
    const QString& first = eff.first();
    for (const QString& v : eff) {
        if (v != first)
            return {}; // disagreeing selection: Single tab starts blank
    }
    return first; // uniform
}

bool PropertiesMetadataModel::fieldUniform(int row) const {
    if (row < 0 || row >= m_rows.size())
        return true;
    const Row& r = m_rows.at(row);
    if (r.kind != Kind::Field && r.kind != Kind::Custom)
        return true;
    const QStringList eff = effectivePerTrack(r.key);
    if (eff.size() <= 1)
        return true;
    const QString& first = eff.first();
    for (const QString& v : eff) {
        if (v != first)
            return false;
    }
    return true;
}

QStringList PropertiesMetadataModel::perTrackEditValues(int row) const {
    if (row < 0 || row >= m_rows.size())
        return {};
    const Row& r = m_rows.at(row);
    if (r.kind != Kind::Field && r.kind != Kind::Custom)
        return {};
    return effectivePerTrack(r.key);
}

QStringList PropertiesMetadataModel::trackLabels() const {
    QStringList out;
    out.reserve(static_cast<qsizetype>(m_selection.size()));
    for (const TrackData& t : m_selection) {
        if (!t.title.isEmpty())
            out.push_back(t.title);
        else
            out.push_back(QFileInfo(t.filePath).fileName());
    }
    return out;
}

void PropertiesMetadataModel::stageFieldValues(const QString& key, const QStringList& perTrackValues) {
    const int n = static_cast<int>(m_selection.size());
    if (perTrackValues.size() != n) // contract: exactly one value per selected track
        return;
    if (rowForKey(key) < 0) // only known field/custom rows are stageable in 2b-i
        return;
    QStringList newList;
    newList.reserve(perTrackValues.size());
    for (const QString& raw : perTrackValues)
        newList.push_back(normalizeForDisplay(key, raw));
    stageList(key, newList);
}

QStringList PropertiesMetadataModel::blankPerTrack() const {
    return QStringList(static_cast<qsizetype>(m_selection.size()));
}

QString PropertiesMetadataModel::addCustomFieldValues(const QString& rawKey,
                                                      const QStringList& perTrackValues) {
    // Normalize the key to the on-disk convention (trimmed, uppercased), then run
    // the four rejections in order. Each returns a message for the dialog's inline
    // error; OK is never disabled, so the dialog simply stays open and shows it.
    const QString key = rawKey.trimmed().toUpper();
    if (key.isEmpty())
        return QStringLiteral("Enter a field name.");
    if (rowForKey(key) >= 0)
        return key + QStringLiteral(" already exists; edit it directly.");
    if (isReservedKey(key))
        return key + QStringLiteral(" is a managed field and cannot be added.");

    const int n = static_cast<int>(m_selection.size());
    if (perTrackValues.size() != n)
        return {}; // contract violation; the dialog always passes one value per track

    // Conform each value to the field policy (custom keys are list fields) and
    // require at least one non-empty, so we never insert a blank field.
    QStringList normalized;
    normalized.reserve(perTrackValues.size());
    bool anyNonEmpty = false;
    for (const QString& raw : perTrackValues) {
        const QString v = normalizeForDisplay(key, raw);
        if (!v.isEmpty())
            anyNonEmpty = true;
        normalized.push_back(v);
    }
    if (!anyNonEmpty)
        return QStringLiteral("Enter a value.");

    // Insert a Custom row just before the trailing "+ add new" row. The on-disk
    // baseline is empty (the field does not exist yet), so staging the normalized
    // values marks it dirty and collectEdits emits a write. If the user reverts or
    // the window reloads before applying, rebuild() drops this row (it is not in
    // any track's extraTags yet); once applied it is read back from disk.
    const int insertAt = static_cast<int>(m_rows.size()) - 1; // "+ add new" is last
    bool multiple = false;
    const QString display = collapseList(normalized, multiple);
    beginInsertRows(QModelIndex{}, insertAt, insertAt);
    m_rows.insert(insertAt, Row{ Kind::Custom,
                                 QLatin1Char('<') + key + QLatin1Char('>'),
                                 key, display, multiple, true });
    endInsertRows();

    m_original.insert(key, QStringList(static_cast<qsizetype>(n))); // empty baseline
    m_staged.insert(key, normalized);
    emit dirtyChanged();
    return {};
}

void PropertiesMetadataModel::removeAllFields() {
    // The Tools > Remove tags staging half: every editable row, exactly as
    // a select-all + Remove would collect them. Descending order and the per-row
    // stage-empty / session-drop policy come from removeFields.
    QList<int> all;
    for (int r = 0; r < m_rows.size(); ++r) {
        const Kind k = m_rows.at(r).kind;
        if (k == Kind::Field || k == Kind::Custom)
            all.push_back(r);
    }
    removeFields(all);
}

void PropertiesMetadataModel::removeFields(const QList<int>& rows) {
    // Process high-index first: an outright row drop shifts the rows below it, so
    // taking the highest indices first keeps every not-yet-processed index valid.
    QList<int> sorted = rows;
    std::ranges::sort(sorted, std::ranges::greater{});
    for (int row : sorted)
        removeFieldAt(row);
}

void PropertiesMetadataModel::removeFieldAt(int row) {
    if (row < 0 || row >= m_rows.size())
        return;
    const Row& r = m_rows.at(row);
    if (r.kind != Kind::Field && r.kind != Kind::Custom)
        return;
    const QString key = r.key;

    if (r.kind == Kind::Custom) {
        const QStringList& orig = m_original.value(key);
        const bool onDisk = std::ranges::any_of(orig, [](const QString& s) { return !s.isEmpty(); });
        if (!onDisk) {
            // Added this session and never written: drop the row entirely rather
            // than leave an empty custom row lingering.
            beginRemoveRows(QModelIndex{}, row, row);
            m_rows.removeAt(row);
            endRemoveRows();
            m_original.remove(key);
            m_staged.remove(key);
            emit dirtyChanged();
            return;
        }
    }

    // Schema field, or an on-disk custom field: stage empty across the selection so
    // collectEdits writes a removal on Apply (coupling and aliases handled there).
    stageList(key, QStringList(static_cast<qsizetype>(m_selection.size())));
}

// =============================================================================
// Clipboard: Copy / Cut / Paste in a per-track block text format.
//
// One block per selected track, in selection order; each block is a run of
// "KEY=value" lines (one per selected field, in row order); blocks are separated
// by a blank line. List fields keep their "; "-joined form on a single line and
// re-split through normalizeForDisplay on paste.
//
// Values are ESCAPED so one field is always exactly one line. Tag values,
// COMMENT and LYRICS above all, routinely contain embedded newlines; emitted
// raw, a continuation line has no '=' and would be dropped on parse (truncating
// the value), and an embedded blank line would split the track block itself,
// misaligning blocks against the selection. Both silently corrupt data on the
// advertised Cut / Apply / Paste / Apply workflow, so serializeFields encodes
// backslash, LF, and CR as the two-character sequences \\ \n \r, and
// parseFieldBlocks reverses them in a single left-to-right scan (so \\n decodes
// to a literal backslash-n, never a newline). rawform-originated clipboards
// therefore round-trip losslessly.
//
// Accepted tradeoff: a FOREIGN clipboard whose value contains a
// literal backslash sequence (a hand-typed "C:\new\folder", say) will have its
// \n decoded into a newline on paste. Our own serialize is immune (it emits the
// doubled backslash), and multi-line restore correctness on the Cut path
// outweighs that rare edge; detecting "our" clipboards heuristically would be
// fragile. The one other known lossiness: a value containing a literal "; " in
// a LIST field, which the list editing already treats as a separator
// everywhere else.
// =============================================================================

namespace {

// Escape one value for the block format: backslash, LF, and CR become the
// two-character sequences \\ \n \r, so the emitted value never spans lines and
// never manufactures a block separator. Everything else passes through, which
// keeps the common case byte-identical and human-readable.
QString escapeClipboardValue(const QString& raw) {
    QString out;
    out.reserve(raw.size());
    for (const QChar c : raw) {
        if (c == QLatin1Char('\\'))
            out += QStringLiteral("\\\\");
        else if (c == QLatin1Char('\n'))
            out += QStringLiteral("\\n");
        else if (c == QLatin1Char('\r'))
            out += QStringLiteral("\\r");
        else
            out += c;
    }
    return out;
}

// Reverse escapeClipboardValue in one left-to-right scan: a consumed \\ emits a
// backslash and SKIPS the pair, so a serialized backslash-n (\\n on the wire)
// decodes back to backslash-n, never to a newline. An unrecognized or trailing
// lone backslash passes through verbatim, so a foreign clipboard that never
// heard of this escaping degrades gently rather than eating characters.
QString unescapeClipboardValue(const QString& escaped) {
    QString out;
    out.reserve(escaped.size());
    for (qsizetype i = 0; i < escaped.size(); ++i) {
        const QChar c = escaped.at(i);
        if (c == QLatin1Char('\\') && i + 1 < escaped.size()) {
            const QChar next = escaped.at(i + 1);
            if (next == QLatin1Char('\\')) { out += QLatin1Char('\\'); ++i; continue; }
            if (next == QLatin1Char('n'))  { out += QLatin1Char('\n');  ++i; continue; }
            if (next == QLatin1Char('r'))  { out += QLatin1Char('\r');  ++i; continue; }
        }
        out += c;
    }
    return out;
}

} // namespace

QString PropertiesMetadataModel::serializeFields(const QList<int>& rows) const {
    // Collect the target keys from the selected field/custom rows, ascending by row
    // so line order within each block is stable and matches the on-screen order.
    QList<int> ordered = rows;
    std::ranges::sort(ordered);

    QStringList keys;
    keys.reserve(ordered.size());
    for (const int row : ordered) {
        if (row < 0 || row >= m_rows.size())
            continue;
        const Row& r = m_rows.at(row);
        if (r.kind != Kind::Field && r.kind != Kind::Custom)
            continue; // section/add rows carry no value
        keys.push_back(r.key);
    }
    if (keys.isEmpty())
        return {};

    // Precompute each key's effective per-track values once (effectivePerTrack walks
    // the row list), then index by track inside the per-track block loop below.
    QList<QStringList> effs;
    effs.reserve(keys.size());
    for (const QString& key : keys)
        effs.push_back(effectivePerTrack(key));

    const int n = static_cast<int>(m_selection.size());
    QStringList blocks;
    blocks.reserve(n);
    for (int t = 0; t < n; ++t) {
        QStringList lines;
        lines.reserve(keys.size());
        for (int k = 0; k < keys.size(); ++k) {
            const QStringList& eff = effs.at(k);
            const QString value = (t < eff.size()) ? eff.at(t) : QString{};
            // Escaped so a multi-line value stays one line and cannot forge a
            // block separator; parseFieldBlocks undoes it. See the section
            // comment above for the full rationale.
            lines.push_back(keys.at(k) + QLatin1Char('=')
                            + escapeClipboardValue(value));
        }
        blocks.push_back(lines.join(QLatin1Char('\n')));
    }
    return blocks.join(QStringLiteral("\n\n"));
}

QList<QHash<QString, QString>>
PropertiesMetadataModel::parseFieldBlocks(const QString& text) const {
    // Normalize line endings so a CRLF clipboard parses the same as native LF.
    QString norm = text;
    norm.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    norm.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    // Blocks are separated by a blank line (which may carry stray spaces or tabs).
    static const QRegularExpression blankLine(QStringLiteral("\\n[ \\t]*\\n"));
    const QStringList rawBlocks = norm.split(blankLine, Qt::SkipEmptyParts);

    QList<QHash<QString, QString>> blocks;
    blocks.reserve(rawBlocks.size());
    for (const QString& rawBlock : rawBlocks) {
        QHash<QString, QString> fields;
        const QStringList lines = rawBlock.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& line : lines) {
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq < 0)
                continue; // not a "KEY=value" line
            const QString key = line.left(eq).trimmed().toUpper();
            if (key.isEmpty())
                continue;
            // Everything after the first '=' is the value, run back through the
            // escape decoding (restoring any embedded newlines serializeFields
            // flattened); list/scalar normalization happens downstream in
            // stageFieldValues / addCustomFieldValues.
            fields.insert(key, unescapeClipboardValue(line.mid(eq + 1)));
        }
        if (!fields.isEmpty())
            blocks.push_back(fields);
    }
    return blocks;
}

bool PropertiesMetadataModel::canPasteFields(const QString& text) const {
    return !parseFieldBlocks(text).isEmpty();
}

QString PropertiesMetadataModel::pasteFields(const QString& text) {
    QList<QHash<QString, QString>> blocks = parseFieldBlocks(text);
    const int n = static_cast<int>(blocks.size());
    if (n == 0)
        return QStringLiteral("Nothing to paste.");

    const int m = static_cast<int>(m_selection.size());
    if (n != m && n != 1)
        return QStringLiteral("Clipboard has %1 tracks, selection has %2.").arg(n).arg(m);

    // Alias mapping: other taggers, and our own on-disk writer, spell the
    // totals TRACKTOTAL / DISCTOTAL. Both are reserved keys here, so without the
    // remap a paste carrying them would be silently dropped by the add path even
    // though the schema edits exactly those values. The schema spelling wins when
    // a block carries both.
    static const struct { const char* from; const char* to; } kPasteAliases[] = {
        {"TRACKTOTAL", "TOTALTRACKS"},
        {"DISCTOTAL",  "TOTALDISCS"},
    };
    for (QHash<QString, QString>& block : blocks) {
        for (const auto& a : kPasteAliases) {
            const QString from = QLatin1String(a.from);
            if (!block.contains(from))
                continue;
            const QString to = QLatin1String(a.to);
            if (!block.contains(to))
                block.insert(to, block.value(from));
            block.remove(from);
        }
    }

    // Union of keys across all blocks, collected deterministically (QHash iteration
    // order is unspecified, so we do not rely on it for the staging order).
    QStringList keys;
    for (const QHash<QString, QString>& block : blocks) {
        for (auto it = block.cbegin(); it != block.cend(); ++it) {
            if (!keys.contains(it.key()))
                keys.push_back(it.key());
        }
    }

    // Stage each key across the current track selection: broadcast a single source
    // block to all tracks, else map block t to track t. Absence preserves:
    // for a known key, a track whose block does not CONTAIN the key keeps its
    // current effective value, so only an explicit "KEY=" empty line removes.
    // (contains(), deliberately not an empty-value test, is what separates the
    // two.) rawform's own serialize emits a uniform key set, so round trips
    // never exercise it; the case it decides is a hand-crafted or foreign
    // clipboard with heterogeneous blocks, where clearing fields the source
    // simply never mentioned would be a surprise. A fully-preserving paste
    // stages values equal to the on-disk originals, which stageList drops, so
    // no dirty noise appears either. An existing field is staged (overwritten);
    // an unknown key goes
    // through the add path, which skips reserved and all-empty new keys itself,
    // so its return is intentionally ignored here (an unknown key also has no
    // current values, so absence-preservation cannot apply to it). Note the
    // broadcast case never exercises the preserve branch: with one block, every
    // unioned key is by construction contained in it.
    for (const QString& key : keys) {
        const bool known = rowForKey(key) >= 0;
        const QStringList current = known ? effectivePerTrack(key) : QStringList{};
        QStringList perTrack;
        perTrack.reserve(m);
        for (int t = 0; t < m; ++t) {
            const QHash<QString, QString>& src = (n == 1) ? blocks.at(0) : blocks.at(t);
            if (known && !src.contains(key))
                perTrack.push_back(t < current.size() ? current.at(t) : QString{});
            else
                perTrack.push_back(src.value(key));
        }
        if (known)
            stageFieldValues(key, perTrack);
        else
            addCustomFieldValues(key, perTrack);
    }
    return {};
}

// =============================================================================
// Transforms: value transforms (Capitalize, Clean up), Crop, and Auto
// track number. All stage through the normal edit machinery, so they are shown
// with a dirty marker, reverted by Cancel, and written on Apply.
// =============================================================================

void PropertiesMetadataModel::transformSelectedValues(const QList<int>& rows,
                                                      QString (*fn)(const QString&)) {
    for (const int row : rows) {
        if (row < 0 || row >= m_rows.size())
            continue;
        const Row& r = m_rows.at(row);
        if (r.kind != Kind::Field && r.kind != Kind::Custom)
            continue; // section / add rows carry no value to transform
        const QString key = r.key;
        const QStringList eff = effectivePerTrack(key);
        QStringList out;
        out.reserve(eff.size());
        for (const QString& v : eff)
            out.push_back(fn(v));
        // stageFieldValues normalizes and drops the stage if the result equals the
        // on-disk original (an idempotent transform on clean data leaves no edit).
        stageFieldValues(key, out);
    }
}

void PropertiesMetadataModel::capitalizeFields(const QList<int>& rows) {
    transformSelectedValues(rows, &capitalizeValue);
}

void PropertiesMetadataModel::cleanWhitespaceFields(const QList<int>& rows) {
    transformSelectedValues(rows, &cleanValue);
}

void PropertiesMetadataModel::cropToFields(const QList<int>& keepRows) {
    // Remove every editable row not in the keep set. Build the complement against
    // the current rows, then hand it to removeFields (which sorts descending and
    // applies the per-row stage-empty / drop policy). Row indices may shift when a
    // session-only custom row drops, so the caller should not reuse keepRows after.
    const QSet<int> keep(keepRows.cbegin(), keepRows.cend());
    QList<int> toRemove;
    for (int r = 0; r < m_rows.size(); ++r) {
        const Row& row = m_rows.at(r);
        if (row.kind != Kind::Field && row.kind != Kind::Custom)
            continue;
        if (!keep.contains(r))
            toRemove.push_back(r);
    }
    removeFields(toRemove);
}

void PropertiesMetadataModel::autoNumberTracks() {
    const int n = static_cast<int>(m_selection.size());
    if (n == 0)
        return;
    // Number 1..N in selection order (which is playlist order) and set the total to
    // N, both unpadded. Both are schema rows, so stageFieldValues stages them; on
    // Apply collectEdits writes TRACKNUMBER and TRACKTOTAL.
    QStringList numbers;
    QStringList totals;
    numbers.reserve(n);
    totals.reserve(n);
    const QString total = QString::number(n);
    for (int i = 0; i < n; ++i) {
        numbers.push_back(QString::number(i + 1));
        totals.push_back(total);
    }
    stageFieldValues(QStringLiteral("TRACKNUMBER"), numbers);
    stageFieldValues(QStringLiteral("TOTALTRACKS"), totals);
}

QVariantList PropertiesMetadataModel::collectEdits() const {
    QVariantList edits;
    const int n = static_cast<int>(m_selection.size());

    const auto eff = [this](const QString& k, int i) {
        return effectivePerTrack(k).value(i);
    };
    const auto orig = [this](const QString& k, int i) {
        return m_original.value(k).value(i);
    };
    const auto push = [&edits](const QString& path, const QString& key,
                               const QStringList& values) {
        QVariantMap m;
        m.insert(QStringLiteral("path"), path);
        m.insert(QStringLiteral("key"), key);
        m.insert(QStringLiteral("values"), values); // explicit list; empty = remove
        edits.push_back(m);
    };
    // Resolve a key's effective per-track display value into the explicit value
    // list to write, applying the list/scalar policy. Scalar keys (numbers, date,
    // the coupled pair) become one value or, when empty, a removal.
    const auto pushKey = [this, &push](const QString& path, const QString& key, int i) {
        push(path, key, valueListFor(key, effectivePerTrack(key).value(i)));
    };
    // As pushKey, but written under a different tag name than the value is read from:
    // used for the totals, whose internal/schema key is TOTALTRACKS / TOTALDISCS but
    // whose on-disk spelling is TRACKTOTAL / DISCTOTAL (the foobar Vorbis convention).
    const auto pushAs = [this, &push](const QString& path, const QString& writeKey,
                                      const QString& valueKey, int i) {
        push(path, writeKey, valueListFor(valueKey, effectivePerTrack(valueKey).value(i)));
    };

    // Coupled-pair keys are handled explicitly; the generic sweep below skips them so
    // they are never double-emitted. The totals carry two spellings: the schema key
    // (kTrackTot / kDiscTot) is what the model stages under and reads back, while the
    // on-disk spelling written is TRACKTOTAL / DISCTOTAL, with the schema spelling
    // cleared so the two cannot coexist.
    static const QString kTrackNo   = QStringLiteral("TRACKNUMBER");
    static const QString kTrackTot  = QStringLiteral("TOTALTRACKS"); // schema key; cleared on write
    static const QString kTrackTotW = QStringLiteral("TRACKTOTAL");  // spelling written to disk
    static const QString kDiscNo    = QStringLiteral("DISCNUMBER");
    static const QString kDiscTot   = QStringLiteral("TOTALDISCS");  // schema key; cleared on write
    static const QString kDiscTotW  = QStringLiteral("DISCTOTAL");   // spelling written to disk

    const bool trackPair = m_staged.contains(kTrackNo) || m_staged.contains(kTrackTot);
    const bool discPair  = m_staged.contains(kDiscNo)  || m_staged.contains(kDiscTot);

    for (int i = 0; i < n; ++i) {
        const QString path = m_selection.at(i).filePath;
        if (path.isEmpty())
            continue;

        // Track Number / Total Tracks: write the whole pair for any track whose
        // number or total changed, using current values where unedited. The total is
        // written under TRACKTOTAL and the schema TOTALTRACKS spelling is cleared, so
        // the two cannot coexist. An empty total writes as a removal (pushAs yields an
        // empty list), never as a manufactured blank tag.
        if (trackPair &&
            (eff(kTrackNo, i) != orig(kTrackNo, i) || eff(kTrackTot, i) != orig(kTrackTot, i))) {
            pushKey(path, kTrackNo, i);
            pushAs(path, kTrackTotW, kTrackTot, i); // write TRACKTOTAL from the schema value
            push(path, kTrackTot, QStringList{});   // clear the TOTALTRACKS spelling
        }
        if (discPair &&
            (eff(kDiscNo, i) != orig(kDiscNo, i) || eff(kDiscTot, i) != orig(kDiscTot, i))) {
            pushKey(path, kDiscNo, i);
            pushAs(path, kDiscTotW, kDiscTot, i);   // write DISCTOTAL from the schema value
            push(path, kDiscTot, QStringList{});    // clear the TOTALDISCS spelling
        }

        // Every other staged key (including DATE, an ordinary scalar with no
        // YEAR coupling): emit the changed tracks plainly.
        for (auto it = m_staged.constBegin(); it != m_staged.constEnd(); ++it) {
            const QString& key = it.key();
            if (key == kTrackNo || key == kTrackTot || key == kDiscNo ||
                key == kDiscTot)
                continue; // handled above
            if (eff(key, i) != orig(key, i))
                pushKey(path, key, i);
        }
    }
    return edits;
}

void PropertiesMetadataModel::adoptEdits(const QStringList& failedPaths) {
    if (m_staged.isEmpty())
        return;

    const QSet<QString> failed(failedPaths.cbegin(), failedPaths.cend());
    const int n = static_cast<int>(m_selection.size());

    // Per staged key, fold the staged per-track values into m_original: the
    // written value becomes the baseline for every track whose write landed;
    // a failed track keeps its original. m_selection is intentionally left
    // untouched: it only feeds rebuild(), which runs solely from setSelection
    // (where it is replaced wholesale anyway); everything displayed after this
    // point derives from m_original via effectivePerTrack.
    const QStringList keys = m_staged.keys();
    for (const QString& key : keys) {
        const QStringList staged = m_staged.value(key);
        QStringList folded = m_original.value(key);
        if (folded.size() != n)
            folded = QStringList(n, QString()); // defensive; both lists are built per selection
        for (int i = 0; i < n && i < staged.size(); ++i) {
            if (!failed.contains(m_selection.at(i).filePath))
                folded[i] = staged.at(i);
        }
        m_original.insert(key, folded);
    }

    m_staged.clear();
    for (const QString& key : keys)
        refreshRowForKey(key); // effective == new original everywhere: dirty clears
    emit dirtyChanged();
}

} // namespace rawform
