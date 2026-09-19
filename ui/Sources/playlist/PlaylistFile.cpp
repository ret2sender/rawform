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

// PlaylistFile.cpp
//
// Implementation of the .rwfpl reader and writer declared in PlaylistFile.h: the
// FourCC chunk identifiers, the length-prefixed TrackData record encoding, the
// bounded container reads that keep a corrupt or truncated file from being
// trusted, and the chunk writing and parsing loops.

#include "playlist/PlaylistFile.h"

#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QSaveFile>
#include <QtGlobal>

#include <utility> // std::move

namespace rawform {
namespace {

// --- Chunk identifiers (FourCC) --------------------------------------------
// Internal to the format; not part of the public surface.
constexpr quint32 byteOf(char c) {
    return static_cast<unsigned char>(c);
}
constexpr quint32 fourcc(char a, char b, char c, char d) {
    return (byteOf(a) << 24) | (byteOf(b) << 16) | (byteOf(c) << 8) | byteOf(d);
}
constexpr quint32 kChunkMeta   = fourcc('M', 'E', 'T', 'A');
constexpr quint32 kChunkTracks = fourcc('T', 'R', 'K', 'S');
constexpr quint32 kChunkLayout = fourcc('L', 'A', 'Y', 'O');
/// The current (focus) row. Its own chunk, NOT a META append, so a reader
/// without it skips the whole thing via the length prefix.
constexpr quint32 kChunkCurrent = fourcc('C', 'U', 'R', 'R');
/// The scroll position (first visible row). Same own-chunk rationale as
/// CURR.
constexpr quint32 kChunkScroll = fourcc('S', 'C', 'R', 'L');

/// Apply the format's fixed byte order + the given stream version to a stream.
void configure(QDataStream& s, int streamVersion) {
    s.setByteOrder(QDataStream::LittleEndian);
    s.setVersion(streamVersion);
}

// --- TrackData record encoding ---------------------------------------------
// The FROZEN on-disk field order. Adding a field appends at the END only; the
// record length prefix lets older readers ignore trailing additions. NEVER
// reorder or remove an existing field without bumping kFormatVersion.
//
// QStringList and QMap<QString,QStringList> use QDataStream's own container
// operators, which length-prefix their elements for us. Integers are written at
// fixed widths: the int members go through QDataStream's qint32 overload,
// which is exact because Qt defines qint32 as int and asserts sizeof(int) == 4
// (qglobal); fileSize is a qint64 member already. The reader mirrors this with
// explicitly qint32/qint64-typed locals.

void writeTrackFields(QDataStream& s, const TrackData& t) {
    s << t.artists
      << t.albumArtists
      << t.genres
      << t.album
      << t.title
      << t.trackNo
      << t.discNo
      << t.trackTotal
      << t.discTotal
      << t.year
      << t.dateRaw
      << t.durationMs
      << t.bitrateKbps
      << t.sampleRateHz
      << t.channels
      << t.codec
      << t.extraTags
      << t.hasEmbeddedArt
      << t.fileName
      << t.folderName
      << t.filePath
      << t.fileSize
      << t.modified
      << t.created
      << t.subsongIndex
      << t.tool
      << t.hasEmbeddedCuesheet
      << t.bitsPerSample
      << t.codecProfile
      << t.tagType
      << t.audioMd5
      // Appended after the v1 field set (see the append rule above): no
      // kFormatVersion bump needed, since the record length prefix lets a v1
      // reader ignore these trailing bytes and a newer reader detect them.
      << t.trackNoRaw
      << t.discNoRaw;
    // NOT written: valid, available (recomputed on load).
}

// --- Bounded container reads -----------------------------------------------
// QDataStream's own container operator>> reads the element count and calls
// reserve(count) BEFORE reading a single element (readArrayBasedContainer),
// trusting the stream. Qt chunk-limits STRING payload allocation against
// truncated/corrupt input, but not container counts, so one flipped byte in a
// list's length prefix inside an otherwise length-checked record could demand
// a multi-gigabyte reserve (count 0xFFFFFFxx * sizeof(QString)) and abort the
// process; the playlist-file test's bit-flip sweep caught exactly that under
// the sanitizers' hard-failing allocator. These helpers read the same wire
// format the writer's operator<< produced, but validate the count against the
// bytes actually remaining in the record first: a serialized QString costs at
// least its own 4-byte length prefix, so a count above remaining/4 is
// corruption, rejected before any allocation.

bool readBoundedStringList(QDataStream& s, QStringList& out) {
    quint32 n = 0;
    s >> n;
    if (s.status() != QDataStream::Ok)
        return false;
    const QIODevice* dev = s.device();
    const qint64 remaining = dev ? (dev->size() - dev->pos()) : 0;
    if (static_cast<qint64>(n) > remaining / 4) {
        s.setStatus(QDataStream::ReadCorruptData);
        return false;
    }
    out.clear();
    out.reserve(static_cast<qsizetype>(n));
    for (quint32 i = 0; i < n; ++i) {
        QString v;
        s >> v;  // per-element reads are chunk-limited by QDataStream itself
        if (s.status() != QDataStream::Ok)
            return false;
        out.append(std::move(v));
    }
    return true;
}

bool readBoundedTagMap(QDataStream& s, QMap<QString, QStringList>& out) {
    quint32 n = 0;
    s >> n;
    if (s.status() != QDataStream::Ok)
        return false;
    const QIODevice* dev = s.device();
    const qint64 remaining = dev ? (dev->size() - dev->pos()) : 0;
    // A pair costs at least the key's 4-byte prefix plus the value list's
    // 4-byte count.
    if (static_cast<qint64>(n) > remaining / 8) {
        s.setStatus(QDataStream::ReadCorruptData);
        return false;
    }
    out.clear();
    for (quint32 i = 0; i < n; ++i) {
        QString key;
        s >> key;
        if (s.status() != QDataStream::Ok)
            return false;
        QStringList values;
        if (!readBoundedStringList(s, values))
            return false;
        out.insert(key, values); // QMap has no rvalue insert; both are shared copies
    }
    return true;
}

bool readTrackFields(QDataStream& s, TrackData& t) {
    qint32 trackNo = 0, discNo = 0, trackTotal = 0, discTotal = 0, year = 0;
    qint32 durationMs = 0, bitrateKbps = 0, sampleRateHz = 0, channels = 0, subsongIndex = 0;
    qint32 bitsPerSample = 0;
    qint64 fileSize = 0;

    // The four container fields go through the bounded readers above; every
    // other field is a scalar, a QString, or a QDateTime, which QDataStream
    // reads with its own allocation limits. Field ORDER is the frozen on-disk
    // order.
    if (!readBoundedStringList(s, t.artists)
        || !readBoundedStringList(s, t.albumArtists)
        || !readBoundedStringList(s, t.genres))
        return false;

    s >> t.album
      >> t.title
      >> trackNo
      >> discNo
      >> trackTotal
      >> discTotal
      >> year
      >> t.dateRaw
      >> durationMs
      >> bitrateKbps
      >> sampleRateHz
      >> channels
      >> t.codec;
    if (s.status() != QDataStream::Ok)
        return false;

    if (!readBoundedTagMap(s, t.extraTags))
        return false;

    s >> t.hasEmbeddedArt
      >> t.fileName
      >> t.folderName
      >> t.filePath
      >> fileSize
      >> t.modified
      >> t.created
      >> subsongIndex
      >> t.tool
      >> t.hasEmbeddedCuesheet
      >> bitsPerSample
      >> t.codecProfile
      >> t.tagType
      >> t.audioMd5;

    // An under-read (record shorter than v1's field set) is corruption; a record
    // LONGER than we read is fine (a future version's extra trailing fields,
    // intentionally ignored here).
    if (s.status() != QDataStream::Ok)
        return false;

    // Appended-after-v1 fields: raw track/disc number text. The record sub-stream
    // is length-bounded, so atEnd() cleanly tells a v1 record (no raw text; the
    // ints win on display) from a newer one that carries it. A best-effort read:
    // these are the final bytes, so a malformed tail only blanks the raw strings,
    // it never fails an otherwise-valid record.
    if (!s.atEnd())
        s >> t.trackNoRaw >> t.discNoRaw;

    t.trackNo      = trackNo;
    t.discNo       = discNo;
    t.trackTotal   = trackTotal;
    t.discTotal    = discTotal;
    t.year         = year;
    t.durationMs   = durationMs;
    t.bitrateKbps  = bitrateKbps;
    t.sampleRateHz = sampleRateHz;
    t.channels     = channels;
    t.subsongIndex = subsongIndex;
    t.bitsPerSample = bitsPerSample;
    t.fileSize     = fileSize;
    t.valid        = true;  // a cached record is valid by construction
    t.available    = true;  // recomputed against disk by MetadataReloader::validateAll
    return true;
}

/// Encode one track into a self-contained, length-prefix-ready byte buffer.
QByteArray encodeTrack(const TrackData& t) {
    QByteArray buf;
    QDataStream s(&buf, QIODevice::WriteOnly);
    configure(s, kStreamVersion);
    writeTrackFields(s, t);
    return buf;
}

// --- Chunk writing ---------------------------------------------------------

void writeChunk(QDataStream& out, quint32 id, const QByteArray& payload) {
    out << id << static_cast<quint64>(payload.size());
    if (!payload.isEmpty())
        out.writeRawData(payload.constData(), payload.size());
}

QByteArray buildMetaPayload(const PlaylistDocument& doc) {
    QByteArray buf;
    QDataStream s(&buf, QIODevice::WriteOnly);
    configure(s, kStreamVersion);
    // savedAt + the playlist/tab title. (No track count: the TRKS chunk is the
    // single authority on row count, so a copy here would only be redundancy.)
    s << doc.savedAtUtc << doc.title;
    return buf;
}

QByteArray buildTracksPayload(const PlaylistDocument& doc) {
    QByteArray buf;
    QDataStream s(&buf, QIODevice::WriteOnly);
    configure(s, kStreamVersion);
    s << static_cast<quint64>(doc.tracks.size());
    for (const TrackData& t : doc.tracks) {
        const QByteArray rec = encodeTrack(t);
        s << static_cast<quint32>(rec.size());
        if (!rec.isEmpty())
            s.writeRawData(rec.constData(), rec.size());
    }
    return buf;
}

QByteArray buildLayoutPayload(const PlaylistDocument& doc) {
    QByteArray buf;
    QDataStream s(&buf, QIODevice::WriteOnly);
    configure(s, kStreamVersion);
    s << static_cast<quint32>(doc.fieldIds.size());
    for (qsizetype i = 0; i < doc.fieldIds.size(); ++i) {
        const qint32 w = (i < doc.widths.size()) ? doc.widths.at(i) : 0;
        s << doc.fieldIds.at(i) << w;
    }
    return buf;
}

QByteArray buildCurrentPayload(const PlaylistDocument& doc) {
    QByteArray buf;
    QDataStream s(&buf, QIODevice::WriteOnly);
    configure(s, kStreamVersion);
    // Just the row; -1 for none. Written at a fixed width (qint32) like every
    // other integer in the format. New fields append AFTER, never before.
    s << doc.currentRow;
    return buf;
}

QByteArray buildScrollPayload(const PlaylistDocument& doc) {
    // Mirrors buildCurrentPayload byte for byte in structure: one
    // fixed-width row, -1 for never-scrolled, future fields append after.
    QByteArray buf;
    QDataStream s(&buf, QIODevice::WriteOnly);
    configure(s, kStreamVersion);
    s << doc.scrollRow;
    return buf;
}

// --- Chunk parsing ---------------------------------------------------------

bool parseTracks(const QByteArray& payload, int streamVersion, PlaylistDocument& doc) {
    QDataStream s(payload);
    configure(s, streamVersion);

    quint64 count = 0;
    s >> count;
    if (s.status() != QDataStream::Ok)
        return false;
    // Sanity cap: even an empty record costs its 4-byte length prefix, so a
    // count larger than a quarter of the payload is impossible -> corruption.
    if (count > static_cast<quint64>(payload.size()) / 4)
        return false;

    // Bound the reserve independently of the cap above: a corrupt count that
    // slips past it must not force a huge up-front allocation. Anything beyond
    // the bound grows organically; the per-record length checks in the loop
    // reject the corruption long before that matters.
    constexpr quint64 kReserveCap = 65536;
    doc.tracks.reserve(static_cast<qsizetype>(count < kReserveCap ? count : kReserveCap));
    for (quint64 i = 0; i < count; ++i) {
        quint32 recLen = 0;
        s >> recLen;
        if (s.status() != QDataStream::Ok)
            return false;

        const auto pos = static_cast<quint64>(s.device() ? s.device()->pos() : 0);
        const quint64 remaining = static_cast<quint64>(payload.size()) - pos;
        if (recLen > remaining)
            return false; // record claims more bytes than the chunk has

        QByteArray rec(recLen, Qt::Uninitialized);
        if (recLen > 0 && s.readRawData(rec.data(), recLen) != recLen)
            return false;

        QDataStream rs(rec);
        configure(rs, streamVersion);
        TrackData t;
        if (!readTrackFields(rs, t))
            return false;
        doc.tracks.push_back(std::move(t));
    }
    return true;
}

void parseLayout(const QByteArray& payload, int streamVersion, PlaylistDocument& doc) {
    // Best-effort: the layout is reconciled against the live schema on apply, so
    // a short/partial layout chunk degrades to "fewer saved columns" rather than
    // failing the whole load.
    QDataStream s(payload);
    configure(s, streamVersion);

    quint32 cnt = 0;
    s >> cnt;
    if (s.status() != QDataStream::Ok)
        return;

    for (quint32 i = 0; i < cnt; ++i) {
        QString id;
        qint32 w = 0;
        s >> id >> w;
        if (s.status() != QDataStream::Ok)
            break;
        doc.fieldIds << id;
        doc.widths << w;
    }
}

void parseMeta(const QByteArray& payload, int streamVersion, PlaylistDocument& doc) {
    QDataStream s(payload);
    configure(s, streamVersion);
    QDateTime savedAt;
    QString   title;
    s >> savedAt >> title;
    if (s.status() != QDataStream::Ok)
        return; // leave doc's META fields default on a short/garbled chunk
    doc.savedAtUtc = savedAt;
    doc.title      = title;
}

void parseCurrent(const QByteArray& payload, int streamVersion, PlaylistDocument& doc) {
    // Best-effort like the layout/meta chunks: a short/garbled chunk leaves
    // currentRow at -1 (no focus row) rather than failing the load. The value
    // is NOT clamped here; only the applier knows the live row count.
    QDataStream s(payload);
    configure(s, streamVersion);
    qint32 row = -1;
    s >> row;
    if (s.status() != QDataStream::Ok)
        return;
    doc.currentRow = row;
}

void parseScroll(const QByteArray& payload, int streamVersion, PlaylistDocument& doc) {
    // The parseCurrent contract verbatim: best-effort, -1 on a short or
    // garbled chunk, clamped only by the applier.
    QDataStream s(payload);
    configure(s, streamVersion);
    qint32 row = -1;
    s >> row;
    if (s.status() != QDataStream::Ok)
        return;
    doc.scrollRow = row;
}

} // namespace

bool writePlaylist(const QString& path, const PlaylistDocument& doc, QString* error) {
    const auto fail = [&](PlaylistIoError, const QString& msg) {
        if (error)
            *error = msg;
        return false;
    };

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(PlaylistIoError::Open,
                    QStringLiteral("could not open %1 for writing").arg(path));

    QDataStream out(&file);
    configure(out, kStreamVersion);

    // Header: magic, our format version, the stream version used for the body.
    out << kMagic << kFormatVersion << static_cast<quint32>(kStreamVersion);

    writeChunk(out, kChunkMeta,    buildMetaPayload(doc));
    writeChunk(out, kChunkTracks,  buildTracksPayload(doc));
    writeChunk(out, kChunkLayout,  buildLayoutPayload(doc));
    writeChunk(out, kChunkCurrent, buildCurrentPayload(doc));
    writeChunk(out, kChunkScroll,  buildScrollPayload(doc));

    if (out.status() != QDataStream::Ok) {
        file.cancelWriting();
        return fail(PlaylistIoError::Write,
                    QStringLiteral("stream error while writing %1").arg(path));
    }
    if (!file.commit())
        return fail(PlaylistIoError::Write,
                    QStringLiteral("could not commit %1").arg(path));
    return true;
}

PlaylistReadResult readPlaylist(const QString& path) {
    PlaylistReadResult r;
    const auto fail = [&](PlaylistIoError e, const QString& msg) {
        r.error = e;
        r.message = msg;
        return r;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(PlaylistIoError::Open,
                    QStringLiteral("could not open %1").arg(path));
    const QByteArray all = file.readAll();
    file.close();

    QDataStream in(all);
    in.setByteOrder(QDataStream::LittleEndian); // version not set yet: header is POD

    quint32 magic = 0, fmt = 0, sver = 0;
    in >> magic >> fmt >> sver;
    if (in.status() != QDataStream::Ok)
        return fail(PlaylistIoError::Truncated,
                    QStringLiteral("file is too small to be a playlist (truncated header)"));
    if (magic != kMagic)
        return fail(PlaylistIoError::Magic,
                    QStringLiteral("not a rawform playlist (unexpected file signature)"));
    if (fmt > kFormatVersion)
        return fail(PlaylistIoError::UnsupportedVersion,
                    QStringLiteral("playlist format version %1 is newer than this build "
                                   "understands (%2)").arg(fmt).arg(kFormatVersion));
    if (sver < static_cast<quint32>(kMinStreamVersion)
        || sver > static_cast<quint32>(kStreamVersion))
        return fail(PlaylistIoError::UnsupportedVersion,
                    QStringLiteral("unsupported stream encoding version %1").arg(sver));

    const int streamVersion = static_cast<int>(sver); // in range, checked above
    in.setVersion(streamVersion);

    bool sawTracks = false;
    while (!in.atEnd()) {
        quint32 id = 0;
        quint64 len = 0;
        in >> id >> len;
        if (in.status() != QDataStream::Ok)
            return fail(PlaylistIoError::Truncated, QStringLiteral("truncated chunk header"));

        const auto pos = static_cast<quint64>(in.device() ? in.device()->pos() : 0);
        const quint64 remaining = static_cast<quint64>(all.size()) - pos;
        if (len > remaining)
            return fail(PlaylistIoError::Truncated,
                        QStringLiteral("chunk claims %1 bytes but only %2 remain")
                            .arg(len).arg(remaining));

        // len <= remaining <= all.size(), so it fits the qsizetype/qint64 APIs.
        QByteArray payload(static_cast<qsizetype>(len), Qt::Uninitialized);
        if (len > 0 && in.readRawData(payload.data(), static_cast<qint64>(len))
                           != static_cast<qint64>(len))
            return fail(PlaylistIoError::Truncated, QStringLiteral("short read on chunk payload"));

        if (id == kChunkTracks) {
            if (!parseTracks(payload, streamVersion, r.doc))
                return fail(PlaylistIoError::Corrupt, QStringLiteral("malformed track chunk"));
            sawTracks = true;
        } else if (id == kChunkLayout) {
            parseLayout(payload, streamVersion, r.doc);
        } else if (id == kChunkMeta) {
            parseMeta(payload, streamVersion, r.doc);
        } else if (id == kChunkCurrent) {
            parseCurrent(payload, streamVersion, r.doc);
        } else if (id == kChunkScroll) {
            parseScroll(payload, streamVersion, r.doc);
        }
        // else: unknown chunk, already consumed its payload, so just skip it.
    }

    if (!sawTracks)
        return fail(PlaylistIoError::Corrupt, QStringLiteral("playlist has no track chunk"));

    r.error = PlaylistIoError::None;
    r.message.clear();
    return r;
}

} // namespace rawform
