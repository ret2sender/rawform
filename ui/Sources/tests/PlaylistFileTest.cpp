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

// ---------------------------------------------------------------------------
// rawform: standalone unit test for the .rwfpl playlist file format.
//
// Same harness shape as PatternEvaluatorTest / RenameSanitizerTest (plain
// main, counted checks, non-zero exit on failure), but this one DOES touch
// the filesystem on purpose: the contract under test is the on-disk format,
// so every case goes through real writePlaylist/readPlaylist calls inside a
// QTemporaryDir. Qt-Quick-free; links Qt6::Core plus PlaylistFile.cpp only
// (TrackData is header-only).
//
// What is covered:
//  1. ROUND-TRIP. A document exercising every stored field shape (string
//     lists, the extraTags map, unicode, an embedded newline, >4 GiB file
//     size, the appended-after-v1 raw number texts, layout, title, focus and
//     scroll rows, savedAt) writes and reads back equal, with the two
//     recomputed runtime flags (valid/available) forced true. An EMPTY
//     playlist round-trips too.
//  2. WRITER GUARANTEES. Overwriting an existing file replaces it atomically;
//     writing into a nonexistent directory fails with an error message and
//     must not create the file.
//  3. ERROR TAXONOMY. One case per PlaylistIoError: missing file (Open),
//     wrong magic (Magic), higher format version and out-of-range stream
//     version (UnsupportedVersion), truncated header / truncated chunk header
//     / overlong chunk claim (Truncated), a track chunk with a lying record
//     length or an absurd count, and a file with no track chunk at all
//     (Corrupt).
//  4. TOLERANCE. An unknown chunk in the middle of the file is skipped and
//     parsing continues after it; short optional chunks (META/LAYO/CURR/SCRL)
//     degrade to defaults instead of failing the load.
//  5. TRUNCATION SWEEP. Every proper prefix of a valid file must either
//     return an error or (when the cut lands exactly on a chunk boundary
//     past TRKS) parse with the full track count; either way, no crash. Run
//     under ASan/UBSan this doubles as a cheap fuzz of the bounds checks.
//  6. BIT-FLIP SWEEP. Flipping one byte at a fixed stride must never crash
//     the reader; any (error | success) outcome is acceptable, the property
//     under test is memory safety of the bounds checks, which the sanitizers
//     enforce.
//
// Build via the CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_playlist_test
//     ctest --test-dir build            # or run ./build/rawform_playlist_test
// ---------------------------------------------------------------------------

#include "playlist/PlaylistFile.h"
#include "media/TrackData.h"

#include <QByteArray>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cstdio>

using namespace rawform;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

void checkError(const PlaylistReadResult& r, PlaylistIoError want, const char* label) {
    ++g_checks;
    if (r.error != want) {
        ++g_failures;
        std::fprintf(stderr, R"(FAIL %-34s error=%d want=%d msg="%s")" "\n",
                     label, static_cast<int>(r.error), static_cast<int>(want),
                     qUtf8Printable(r.message));
    }
}

/// A track exercising every stored field shape; `n` varies the values so
/// multiple tracks in one document are distinguishable.
TrackData sampleTrack(int n) {
    TrackData t;
    t.artists       = { QStringLiteral("Artist %1").arg(n), QStringLiteral("Fëat. Ünïcode") };
    t.albumArtists  = { QStringLiteral("Album Artist") };
    t.genres        = { QStringLiteral("Genre A"), QStringLiteral("Genre B") };
    t.album         = QStringLiteral("Albüm – %1").arg(n);  // non-ASCII on purpose
    t.title         = QStringLiteral("Title %1\nsecond line").arg(n);  // embedded newline
    t.trackNo       = n;
    t.discNo        = 1;
    t.trackTotal    = 12;
    t.discTotal     = 2;
    t.year          = 1980 + n;
    t.dateRaw       = QStringLiteral("198%1-07-01").arg(n % 10);
    t.durationMs    = 200000 + n;
    t.bitrateKbps   = 900 + n;
    t.sampleRateHz  = 44100;
    t.channels      = 2;
    t.codec         = QStringLiteral("FLAC");
    t.extraTags.insert(QStringLiteral("BARCODE"), { QStringLiteral("123%1").arg(n) });
    t.extraTags.insert(QStringLiteral("COMMENT"),
                       { QStringLiteral("first"), QStringLiteral("second") });
    t.hasEmbeddedArt = (n % 2) == 0;
    t.fileName      = QStringLiteral("%1 - track.flac").arg(n);
    t.folderName    = QStringLiteral("Some Folder");
    t.filePath      = QStringLiteral("/music/some folder/%1 - track.flac").arg(n);
    t.fileSize      = 5'000'000'000LL + n;  // proves the qint64 path (> 4 GiB)
    t.modified      = QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5), QTimeZone::UTC);
    t.created       = QDateTime(QDate(2025, 12, 31), QTime(23, 59, 58), QTimeZone::UTC);
    t.subsongIndex  = n % 3;
    t.tool          = QStringLiteral("reference libFLAC 1.4.3");
    t.hasEmbeddedCuesheet = (n % 2) == 1;
    t.bitsPerSample = 24;
    t.codecProfile  = QStringLiteral("lossless");
    t.tagType       = QStringLiteral("vorbis");
    t.audioMd5      = QStringLiteral("0123456789abcdef0123456789abcdef");
    t.trackNoRaw    = QStringLiteral("%1/12").arg(n);  // appended-after-v1 fields
    t.discNoRaw     = QStringLiteral("1/2");
    return t;
}

PlaylistDocument sampleDocument() {
    PlaylistDocument doc;
    for (int i = 1; i <= 3; ++i)
        doc.tracks.push_back(sampleTrack(i));
    doc.fieldIds   = { QStringLiteral("track_no"), QStringLiteral("artist"),
                       QStringLiteral("title"), QStringLiteral("custom:abc-123") };
    doc.widths     = { 40, 180, 260 };  // deliberately SHORTER than fieldIds:
                                        // the writer pads the tail with 0
    doc.savedAtUtc = QDateTime(QDate(2026, 8, 27), QTime(12, 0, 0), QTimeZone::UTC);
    doc.title      = QStringLiteral("Tëst Playlist ♫");
    doc.currentRow = 2;
    doc.scrollRow  = 1;
    return doc;
}

bool trackEqual(const TrackData& a, const TrackData& b, const char* label) {
    const bool eq =
        a.artists == b.artists && a.albumArtists == b.albumArtists
        && a.genres == b.genres && a.album == b.album && a.title == b.title
        && a.trackNo == b.trackNo && a.discNo == b.discNo
        && a.trackTotal == b.trackTotal && a.discTotal == b.discTotal
        && a.year == b.year && a.dateRaw == b.dateRaw
        && a.durationMs == b.durationMs && a.bitrateKbps == b.bitrateKbps
        && a.sampleRateHz == b.sampleRateHz && a.channels == b.channels
        && a.codec == b.codec && a.extraTags == b.extraTags
        && a.hasEmbeddedArt == b.hasEmbeddedArt && a.fileName == b.fileName
        && a.folderName == b.folderName && a.filePath == b.filePath
        && a.fileSize == b.fileSize && a.modified == b.modified
        && a.created == b.created && a.subsongIndex == b.subsongIndex
        && a.tool == b.tool && a.hasEmbeddedCuesheet == b.hasEmbeddedCuesheet
        && a.bitsPerSample == b.bitsPerSample && a.codecProfile == b.codecProfile
        && a.tagType == b.tagType && a.audioMd5 == b.audioMd5
        && a.trackNoRaw == b.trackNoRaw && a.discNoRaw == b.discNoRaw;
    check(eq, label);
    return eq;
}

/// Little-endian header bytes: magic, format version, stream version. The test
/// writes these by hand so a header the WRITER would refuse to produce can
/// still be fed to the reader.
QByteArray rawHeader(quint32 magic, quint32 fmt, quint32 sver) {
    QByteArray buf;
    QDataStream s(&buf, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s << magic << fmt << sver;
    return buf;
}

/// One raw (id, length, payload) chunk, matching writeChunk's encoding.
QByteArray rawChunk(quint32 id, const QByteArray& payload) {
    QByteArray buf;
    QDataStream s(&buf, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    s << id << static_cast<quint64>(payload.size());
    buf.append(payload);
    return buf;
}

/// MUST match PlaylistFile.cpp's private fourcc byte-for-byte (a is the high
/// byte); a drift here would silently turn every crafted chunk id unknown.
constexpr quint32 byteOf(char c) {
    return static_cast<unsigned char>(c);
}
constexpr quint32 fourcc(char a, char b, char c, char d) {
    return (byteOf(a) << 24) | (byteOf(b) << 16) | (byteOf(c) << 8) | byteOf(d);
}

/// The stream-version bounds at their on-disk width, so the crafted headers
/// below can be built and nudged out of range without a cast at each site.
constexpr quint32 kSverU32    = static_cast<quint32>(kStreamVersion);
constexpr quint32 kMinSverU32 = static_cast<quint32>(kMinStreamVersion);

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(bytes) == bytes.size();
}

QByteArray readBytes(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

} // namespace

int main() {
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "FAIL could not create a temporary directory\n");
        return 1;
    }
    const QString dir = tmp.path();
    const auto at = [&dir](const char* name) { return dir + QLatin1Char('/') + QLatin1String(name); };

    // --- 1. round-trip -------------------------------------------------------
    {
        const PlaylistDocument doc = sampleDocument();
        const QString path = at("roundtrip.rwfpl");
        QString err;
        check(writePlaylist(path, doc, &err), "write_ok");
        check(err.isEmpty(), "write_no_error_msg");

        const PlaylistReadResult r = readPlaylist(path);
        checkError(r, PlaylistIoError::None, "roundtrip_reads");
        check(r.doc.tracks.size() == doc.tracks.size(), "roundtrip_track_count");
        for (int i = 0; i < r.doc.tracks.size() && i < doc.tracks.size(); ++i) {
            trackEqual(r.doc.tracks.at(i), doc.tracks.at(i), "roundtrip_track_fields");
            check(r.doc.tracks.at(i).valid && r.doc.tracks.at(i).available,
                  "roundtrip_runtime_flags_true");
        }
        check(r.doc.fieldIds == doc.fieldIds, "roundtrip_field_ids");
        // The writer pads missing widths with 0, so read-back widths are
        // parallel to fieldIds even though the input list was shorter.
        check(r.doc.widths == QList<int>({ 40, 180, 260, 0 }), "roundtrip_widths_padded");
        check(r.doc.savedAtUtc == doc.savedAtUtc, "roundtrip_saved_at");
        check(r.doc.title == doc.title, "roundtrip_title");
        check(r.doc.currentRow == doc.currentRow, "roundtrip_current_row");
        check(r.doc.scrollRow == doc.scrollRow, "roundtrip_scroll_row");
    }

    // --- empty playlist round-trips -----------------------------------------
    {
        PlaylistDocument doc;
        doc.title = QStringLiteral("Empty");
        const QString path = at("empty.rwfpl");
        check(writePlaylist(path, doc), "empty_write_ok");
        const PlaylistReadResult r = readPlaylist(path);
        checkError(r, PlaylistIoError::None, "empty_reads");
        check(r.doc.tracks.isEmpty(), "empty_zero_tracks");
        check(r.doc.currentRow == -1 && r.doc.scrollRow == -1, "empty_rows_default");
    }

    // --- 2. writer guarantees ------------------------------------------------
    {
        const QString path = at("overwrite.rwfpl");
        PlaylistDocument a; a.tracks.push_back(sampleTrack(1));
        PlaylistDocument b; b.tracks.push_back(sampleTrack(7)); b.tracks.push_back(sampleTrack(8));
        check(writePlaylist(path, a), "overwrite_first_write");
        check(writePlaylist(path, b), "overwrite_second_write");
        const PlaylistReadResult r = readPlaylist(path);
        check(r.ok() && r.doc.tracks.size() == 2, "overwrite_reads_second_doc");

        const QString badPath = dir + QStringLiteral("/no-such-dir/x.rwfpl");
        QString err;
        check(!writePlaylist(badPath, a, &err), "write_missing_dir_fails");
        check(!err.isEmpty(), "write_missing_dir_reports");
        check(!QFile::exists(badPath), "write_missing_dir_creates_nothing");
    }

    // --- 3. error taxonomy ---------------------------------------------------
    checkError(readPlaylist(at("does-not-exist.rwfpl")),
               PlaylistIoError::Open, "err_open");

    {
        const QString path = at("magic.rwfpl");
        writeBytes(path, rawHeader(0xDEADBEEFu, kFormatVersion, kSverU32));
        checkError(readPlaylist(path), PlaylistIoError::Magic, "err_magic");
    }
    {
        const QString path = at("newer-format.rwfpl");
        writeBytes(path, rawHeader(kMagic, kFormatVersion + 1, kSverU32));
        checkError(readPlaylist(path), PlaylistIoError::UnsupportedVersion, "err_format_newer");
    }
    {
        const QString path = at("stream-high.rwfpl");
        writeBytes(path, rawHeader(kMagic, kFormatVersion, kSverU32 + 999));
        checkError(readPlaylist(path), PlaylistIoError::UnsupportedVersion, "err_stream_high");

        const QString low = at("stream-low.rwfpl");
        writeBytes(low, rawHeader(kMagic, kFormatVersion, kMinSverU32 - 1));
        checkError(readPlaylist(low), PlaylistIoError::UnsupportedVersion, "err_stream_low");
    }
    {
        const QString path = at("tiny.rwfpl");
        writeBytes(path, QByteArray("\x31\x57\x46", 3));  // shorter than the header
        checkError(readPlaylist(path), PlaylistIoError::Truncated, "err_truncated_header");
    }
    {
        const QString path = at("chunk-header.rwfpl");
        QByteArray bytes = rawHeader(kMagic, kFormatVersion, kSverU32);
        bytes.append("\x01\x02\x03\x04", 4);  // half a chunk header
        writeBytes(path, bytes);
        checkError(readPlaylist(path), PlaylistIoError::Truncated, "err_truncated_chunk_header");
    }
    {
        // A chunk claiming far more bytes than the file holds: a bare chunk
        // header whose length field lies (no payload follows at all).
        const QString path = at("chunk-overrun.rwfpl");
        QByteArray hdr;
        QDataStream s(&hdr, QIODevice::WriteOnly);
        s.setByteOrder(QDataStream::LittleEndian);
        s << fourcc('T', 'R', 'K', 'S') << (quint64{1} << 30);  // 1 GiB
        QByteArray bytes = rawHeader(kMagic, kFormatVersion, kSverU32);
        bytes += hdr;
        writeBytes(path, bytes);
        checkError(readPlaylist(path), PlaylistIoError::Truncated, "err_chunk_overrun");
    }
    {
        // TRKS payload whose count no payload of this size could hold: the
        // count cap (payload/4) must reject it without a giant allocation.
        const QString path = at("absurd-count.rwfpl");
        QByteArray payload;
        QDataStream ps(&payload, QIODevice::WriteOnly);
        ps.setByteOrder(QDataStream::LittleEndian);
        ps.setVersion(kStreamVersion);
        ps << quint64{0x00FFFFFFFFFFFFFFull};
        QByteArray bytes = rawHeader(kMagic, kFormatVersion, kSverU32);
        bytes += rawChunk(fourcc('T', 'R', 'K', 'S'), payload);
        writeBytes(path, bytes);
        checkError(readPlaylist(path), PlaylistIoError::Corrupt, "err_absurd_count");
    }
    {
        // A record whose length prefix overruns its chunk.
        const QString path = at("lying-record.rwfpl");
        QByteArray payload;
        QDataStream ps(&payload, QIODevice::WriteOnly);
        ps.setByteOrder(QDataStream::LittleEndian);
        ps.setVersion(kStreamVersion);
        ps << quint64{1} << quint32{0xFFFFFF};  // one record, absurd recLen
        QByteArray bytes = rawHeader(kMagic, kFormatVersion, kSverU32);
        bytes += rawChunk(fourcc('T', 'R', 'K', 'S'), payload);
        writeBytes(path, bytes);
        checkError(readPlaylist(path), PlaylistIoError::Corrupt, "err_lying_record");
    }
    {
        // A record whose FIRST FIELD (the artists list) carries an absurd
        // element count while the record itself is tiny. QDataStream's own
        // container read would reserve() for the full count before reading an
        // element; the bounded readers must reject it against the record's
        // remaining bytes instead (the scenario this pins: a flipped count
        // byte turning the bit-flip sweep into an OOM).
        const QString path = at("absurd-list-count.rwfpl");
        QByteArray rec;
        {
            QDataStream rs(&rec, QIODevice::WriteOnly);
            rs.setByteOrder(QDataStream::LittleEndian);
            rs.setVersion(kStreamVersion);
            rs << quint32{0xFFFFFF00u};  // artists count; nothing follows
        }
        QByteArray payload;
        {
            QDataStream ps(&payload, QIODevice::WriteOnly);
            ps.setByteOrder(QDataStream::LittleEndian);
            ps.setVersion(kStreamVersion);
            ps << quint64{1} << static_cast<quint32>(rec.size());
            payload.append(rec);
        }
        QByteArray bytes = rawHeader(kMagic, kFormatVersion, kSverU32);
        bytes += rawChunk(fourcc('T', 'R', 'K', 'S'), payload);
        writeBytes(path, bytes);
        checkError(readPlaylist(path), PlaylistIoError::Corrupt, "err_absurd_list_count");
    }
    {
        // Structurally fine file with no TRKS chunk at all.
        const QString path = at("no-tracks.rwfpl");
        QByteArray bytes = rawHeader(kMagic, kFormatVersion, kSverU32);
        bytes += rawChunk(fourcc('M', 'E', 'T', 'A'), QByteArray());
        writeBytes(path, bytes);
        checkError(readPlaylist(path), PlaylistIoError::Corrupt, "err_no_track_chunk");
    }

    // --- 4. tolerance --------------------------------------------------------
    {
        // Unknown chunk BETWEEN known ones: skipped wholesale, parsing
        // continues, and the CURR chunk after it still lands.
        const PlaylistDocument doc = sampleDocument();
        const QString ref = at("tolerance-ref.rwfpl");
        check(writePlaylist(ref, doc), "tolerance_ref_write");
        const QByteArray refBytes = readBytes(ref);

        // Rebuild by hand: header + TRKS (reuse nothing from refBytes; craft
        // a minimal one-track payload through the public writer would couple
        // the test to chunk order, so instead splice an unknown chunk into
        // the real file right after the 12-byte header).
        QByteArray spliced = refBytes.left(12);
        spliced += rawChunk(fourcc('X', 'T', 'R', 'A'),
                            QByteArray(37, '\xA5'));  // junk payload
        spliced += refBytes.mid(12);
        const QString path = at("tolerance-unknown.rwfpl");
        writeBytes(path, spliced);
        const PlaylistReadResult r = readPlaylist(path);
        checkError(r, PlaylistIoError::None, "tolerance_unknown_chunk_skipped");
        check(r.doc.tracks.size() == doc.tracks.size(), "tolerance_tracks_survive");
        check(r.doc.currentRow == doc.currentRow, "tolerance_later_chunks_survive");
    }
    {
        // Short optional chunks degrade to defaults; the load still succeeds.
        const QString path = at("tolerance-short-optional.rwfpl");
        PlaylistDocument doc;
        doc.tracks.push_back(sampleTrack(1));
        doc.currentRow = 0;  // non-default, so the real CURR chunk is provable
        const QString ref = at("tolerance-short-ref.rwfpl");
        check(writePlaylist(ref, doc), "short_optional_ref_write");
        const QByteArray refBytes = readBytes(ref);
        QByteArray bytes = refBytes.left(12);
        bytes += rawChunk(fourcc('M', 'E', 'T', 'A'), QByteArray(2, 'x'));  // garbled
        bytes += rawChunk(fourcc('C', 'U', 'R', 'R'), QByteArray(1, 'y'));  // short
        // Reuse the real TRKS chunk from the reference file: it is the first
        // chunk the writer emits after META, so locate it by scanning for the
        // id. Simpler and less coupled: append the reference's chunks minus
        // its 12-byte header; the reader takes the LAST occurrence of the
        // optional chunks, so the garbled ones above must NOT win.
        bytes += refBytes.mid(12);
        writeBytes(path, bytes);
        const PlaylistReadResult r = readPlaylist(path);
        checkError(r, PlaylistIoError::None, "short_optional_still_reads");
        check(r.doc.tracks.size() == 1, "short_optional_tracks_ok");
        // The garbled META/CURR chunks wrote nothing (best-effort parse), and
        // the REAL chunks later in the file then populated the fields: the
        // focus row must be the written 0, not -1 (garbled won) and not
        // garbage (garbled misparsed).
        check(r.doc.currentRow == 0, "short_optional_real_curr_wins");
    }

    // --- 5. truncation sweep -------------------------------------------------
    {
        const PlaylistDocument doc = sampleDocument();
        const QString ref = at("sweep-ref.rwfpl");
        check(writePlaylist(ref, doc), "sweep_ref_write");
        const QByteArray full = readBytes(ref);
        check(full.size() > 64, "sweep_ref_nonempty");

        const QString path = at("sweep.rwfpl");
        int okPrefixes = 0;
        for (int cut = 0; cut < full.size(); ++cut) {
            writeBytes(path, full.left(cut));
            const PlaylistReadResult r = readPlaylist(path);
            if (r.ok()) {
                ++okPrefixes;
                // A prefix may only parse when the cut fell exactly on a
                // chunk boundary AFTER the track chunk; the tracks must then
                // be complete. Anything else is a bounds-check failure.
                check(r.doc.tracks.size() == doc.tracks.size(), "sweep_ok_prefix_full_tracks");
            }
        }
        std::printf("truncation sweep: %d prefixes, %d parsed as boundary-complete\n",
                    static_cast<int>(full.size()), okPrefixes);
    }

    // --- 6. bit-flip sweep ---------------------------------------------------
    {
        const PlaylistDocument doc = sampleDocument();
        const QString ref = at("flip-ref.rwfpl");
        check(writePlaylist(ref, doc), "flip_ref_write");
        const QByteArray full = readBytes(ref);
        const QString path = at("flip.rwfpl");
        for (int i = 0; i < full.size(); i += 7) {
            QByteArray mutated = full;
            mutated[i] = static_cast<char>(~mutated.at(i));  // flip every bit
            writeBytes(path, mutated);
            (void)readPlaylist(path);  // any outcome is fine; must not crash
        }
        check(true, "bitflip_sweep_completed");
    }

    std::printf("playlist file: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
