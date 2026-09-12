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

#include "media/TrackScanner.h"

#include "media/TrackReader.h"

#include <QChar>
#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringConverter>
#include <QTextStream>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

namespace rawform {
namespace {

// --- Format gating -------------------------------------------------------

/// Audio extensions we add to a playlist: everything TagLib reads, plus ac3/dts,
/// which TagLib cannot parse but the engine reads through its FFmpeg metadata
/// probe (TrackReader's reader cascade) and decodes through its FFmpeg
/// fallback, so they import and play on an FFmpeg-enabled build. Lower-case, no
/// dot.
bool isAudioSuffix(const QString& suffixLower) {
    static const QSet<QString> kAudio = {
        QStringLiteral("flac"), QStringLiteral("mp3"),  QStringLiteral("wav"),
        QStringLiteral("ogg"),  QStringLiteral("oga"),  QStringLiteral("opus"),
        QStringLiteral("m4a"),  QStringLiteral("mp4"),  QStringLiteral("aac"),
        QStringLiteral("wv"),   QStringLiteral("ape"),  QStringLiteral("wma"),
        QStringLiteral("aiff"), QStringLiteral("aif"),  QStringLiteral("mpc"),
        QStringLiteral("tta"),  QStringLiteral("ac3"),  QStringLiteral("dts"),
    };
    return kAudio.contains(suffixLower);
}

bool isM3uSuffix(const QString& suffixLower) {
    return suffixLower == QLatin1String("m3u") || suffixLower == QLatin1String("m3u8");
}

// --- M3U expansion -------------------------------------------------------

/// Expand one .m3u/.m3u8 to the EXISTING audio files it references, in order.
/// Relative paths resolve against the playlist's own directory. We do NOT trust
/// #EXTINF (titles/durations there are routinely stale); the m3u governs only
/// WHICH files and in WHAT order; each goes through readTrack afterwards like
/// any other. A referenced .m3u is NOT recursed into (one level only; avoids
/// cycles).
QStringList expandM3u(const QString& m3uPath) {
    QStringList out;
    QFile f(m3uPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return out;

    const QDir base = QFileInfo(m3uPath).absoluteDir();
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8); // covers .m3u8 and most .m3u in practice

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (line.startsWith(QLatin1String("file://")))
            line = QUrl(line).toLocalFile();

        const QString abs = QDir::isAbsolutePath(line)
                                ? line
                                : base.absoluteFilePath(line);
        const QFileInfo fi(abs);
        if (fi.exists() && fi.isFile() && isAudioSuffix(fi.suffix().toLower()))
            out << fi.absoluteFilePath();
    }
    return out;
}

// --- Phase 1: enumeration (runs off-thread) ------------------------------

/// Expand the raw inputs into an ordered, deduped list of audio file paths.
///   - a folder       -> recursive walk, audio natural-sorted, then any .m3u in
///                        it expanded and MERGED (deduped), never loses tracks;
///   - an .m3u/.m3u8   -> its referenced audio, in m3u order;
///   - an audio file   -> itself;
///   - anything else   -> ignored.
/// Dedup is by canonical path across the WHOLE request (overlapping drops),
/// preserving first-seen order. Duplicates across SEPARATE requests are allowed
/// (foobar lets you queue the same track twice).
QStringList enumerateFiles(const QStringList& inputs) {
    QStringList ordered;
    QSet<QString> seen;

    QCollator coll;
    coll.setNumericMode(true);
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    const auto natLess = [&coll](const QString& a, const QString& b) {
        return coll.compare(a, b) < 0;
    };

    const auto addFile = [&](const QString& path) {
        const QFileInfo fi(path);
        QString key = fi.canonicalFilePath();
        if (key.isEmpty())
            key = fi.absoluteFilePath();
        if (seen.contains(key))
            return;
        seen.insert(key);
        ordered << fi.absoluteFilePath();
    };

    for (const QString& input : inputs) {
        const QFileInfo fi(input);

        if (fi.isDir()) {
            QStringList audio;
            QStringList m3us;
            QDirIterator it(fi.absoluteFilePath(),
                            QDir::Files | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString p = it.next();
                const QString suf = QFileInfo(p).suffix().toLower();
                if (isAudioSuffix(suf))
                    audio << QFileInfo(p).absoluteFilePath();
                else if (isM3uSuffix(suf))
                    m3us << QFileInfo(p).absoluteFilePath();
            }
            // Walk defines the set (natural order) ...
            std::ranges::sort(audio, natLess);
            for (const QString& a : std::as_const(audio))
                addFile(a);
            // ... then any m3u in the folder is merged in (its referenced files
            // not already added), so the m3u's ordering shows only where the
            // walk would not have covered it. Merge never loses tracks.
            std::ranges::sort(m3us, natLess);
            for (const QString& m : std::as_const(m3us))
                for (const QString& a : expandM3u(m))
                    addFile(a);
        } else if (isM3uSuffix(fi.suffix().toLower())) {
            for (const QString& a : expandM3u(fi.absoluteFilePath()))
                addFile(a);
        } else if (fi.exists() && fi.isFile()
                   && isAudioSuffix(fi.suffix().toLower())) {
            addFile(fi.absoluteFilePath());
        }
    }
    return ordered;
}

} // namespace

TrackScanner::TrackScanner(QObject* parent)
    : QObject(parent) {
    connect(&m_enumWatcher, &QFutureWatcher<QStringList>::finished,
            this, &TrackScanner::onEnumerated);
    connect(&m_scanWatcher, &QFutureWatcher<TrackData>::resultsReadyAt,
            this, &TrackScanner::onResultsReady);
    connect(&m_scanWatcher, &QFutureWatcher<TrackData>::finished,
            this, &TrackScanner::onScanFinished);

    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(33); // ~1 frame: coalesce bursts into one insertion
    connect(&m_flushTimer, &QTimer::timeout, this, &TrackScanner::flush);
}

void TrackScanner::scan(const QList<QUrl>& urls, int at) {
    QStringList inputs;
    inputs.reserve(urls.size());
    for (const QUrl& u : urls) {
        const QString p = u.isLocalFile() ? u.toLocalFile() : u.toString();
        if (!p.isEmpty())
            inputs << p;
    }
    if (inputs.isEmpty())
        return;

    if (m_scanning) {
        // Serialize: run after the active request finishes. Its target row is
        // resolved against the live rowCount at start time, so it can't land
        // out of range even if the active scan grew the list meanwhile.
        m_queue.push_back({ std::move(inputs), at });
        return;
    }
    startRequest(std::move(inputs), at);
}

void TrackScanner::startRequest(QStringList inputs, int at) {
    m_requestedAt = at;
    m_added = 0;
    m_skipped = 0;
    m_nextEmit = 0;
    m_total = 0; // No total until enumeration completes
    m_buffer.clear();
    setScanning(true);

    // Resolve the insertion point in the receiver (it knows the model); we just
    // echo the request through.
    emit scanStarted(at);

    m_enumWatcher.setFuture(QtConcurrent::run(enumerateFiles, std::move(inputs)));
}

void TrackScanner::onEnumerated() {
    QStringList files = m_enumWatcher.result();
    if (files.isEmpty()) {
        onScanFinished(); // nothing to scan; close the request out cleanly
        return;
    }
    m_nextEmit = 0;
    // The total is the enumerated FILE count; the one (0, total) emission
    // lets the consumer arm a zero-width bar the moment the size is known.
    m_total = static_cast<int>(files.size());
    emit scanProgress(0, m_total);
    // Move the sequence into the future so it owns the data for the whole
    // computation (an lvalue would have to outlive every worker).
    m_scanWatcher.setFuture(QtConcurrent::mapped(std::move(files), readTrack));
}

void TrackScanner::onResultsReady(int /*begin*/, int /*end*/) {
    // Drain strictly in input order, never blocking on a result that is not
    // ready yet, so batches stay contiguous and ordered even though workers
    // finish out of order. Invalid (unreadable) files are counted, not emitted.
    const QFuture<TrackData> future = m_scanWatcher.future();
    const int before = m_nextEmit;
    while (future.isResultReadyAt(m_nextEmit)) {
        const TrackData t = future.resultAt(m_nextEmit);
        ++m_nextEmit;
        if (t.valid)
            m_buffer.push_back(t);
        else
            ++m_skipped;
    }
    // The drain cursor IS the progress (files processed, valid or not);
    // emitted only when it advanced, so a burst of not-yet-ready wakeups
    // never spams identical values at the consumer.
    if (m_nextEmit != before)
        emit scanProgress(m_nextEmit, m_total);
    if (!m_buffer.isEmpty() && !m_flushTimer.isActive())
        m_flushTimer.start();
}

void TrackScanner::flush() {
    if (m_buffer.isEmpty())
        return;
    m_added += static_cast<int>(m_buffer.size());
    emit tracksReady(m_buffer);
    m_buffer.clear();
}

void TrackScanner::onScanFinished() {
    m_flushTimer.stop();
    flush(); // emit the tail
    // Finalize the bar before the finish notification, so a consumer
    // that clears its state on scanFinished has seen a complete bar first.
    emit scanProgress(m_total, m_total);
    emit scanFinished(m_added, m_skipped);

    if (!m_queue.isEmpty()) {
        const PendingRequest next = m_queue.takeFirst();
        startRequest(next.inputs, next.at); // stays scanning across the handoff
        return;
    }
    setScanning(false);
}

void TrackScanner::setScanning(bool on) {
    if (m_scanning == on)
        return;
    m_scanning = on;
    emit scanningChanged();
}

} // namespace rawform
