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

// ReplayGainScanController.cpp
//
// The driver body. scan() builds a plain-data Batch on the GUI thread from the
// pane's { row, path, name } item maps (deduping subsongs by path; nothing is
// resolved against the live playlist), then launches the whole pass as one
// QtConcurrent::run task. runScan walks the unique files in
// order, feeding each through one ReplayGainScanner so the album combine sees
// every track; in ScanAlbumsByTags mode the batch arrives partitioned into
// group-contiguous albums and the walk resets the scanner at each boundary, so
// every album gets its own combine from a clean loudness state. It reports
// per-track progress and polls the cancel flag between
// files and (through the scanner's callback) between decode chunks. On completion
// the watcher's finished slot emits the measured values as an edit list for the
// pane to stage. Nothing here writes tags or reloads; that stays on the pane's
// Apply / OK path.
//
// Value formatting matches the rest of the app: gains through formatGainValue
// (signed two decimals + " dB") and peaks as fixed six decimals, the same
// canonical forms ReplayGainEditor normalizes to, so a staged scan result and the
// value finally written agree to the character and the pane's dirty diff behaves.
// Unmeasurable (silent or too-short) tracks get a +0.00 dB track gain and no peak,
// so a zero peak is never staged only to be dropped by the writer.

#include "media/ReplayGainScanController.h"

#include "media/ReplayGainTags.h"  // formatGainValue: shared canonical gain form

#include "rawform/audio/ReplayGainScanner.h"  // the zero-Qt measurement engine
#include "DecoderFactory.h"                    // the real format-dispatching factory

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtConcurrent>

#include <string>
#include <utility>

namespace rawform {
namespace {

// Format a measured peak the way ReplayGainEditor does, so staged and written
// peaks are identical. Empty for a non-positive peak (silence), which the caller
// then simply does not stage.
QString formatPeak(double peak) {
    if (peak <= 0.0)
        return {};
    return QString::asprintf("%.6f", peak);
}

}  // namespace

ReplayGainScanController::ReplayGainScanController(QObject* parent)
    : QObject(parent) {
    QObject::connect(&m_watcher, &QFutureWatcher<RunResult>::finished, this,
                     &ReplayGainScanController::onFinished);
}

ReplayGainScanController::~ReplayGainScanController() {
    // Do not let worker tasks outlive this object: ask them to stop, then wait.
    if (m_watcher.isRunning()) {
        m_cancelRequested.store(true);
        m_watcher.waitForFinished();
    }
}

void ReplayGainScanController::scan(const QVariantList& items, int mode) {
    if (m_busy)
        return;
    if (items.isEmpty()) {
        emit finished(QVariantList(), false, QStringList());  // uniform empty flow
        return;
    }

    // GUI-thread batch build. Each item already carries its file path (captured
    // when the Properties window opened, so a playlist mutated since cannot
    // redirect the scan); dedupe by path so a physical file shared by several
    // subsongs is scanned once. rowsFor records every row key that path feeds,
    // so the single measurement fans out to all of them.
    //
    // ScanAlbumsByTags builds the same shape but partitioned: items are first
    // bucketed by albumKey (groups numbered in first-seen order; a defensively
    // empty key forms a singleton group of its own), the path dedupe runs per
    // GROUP (a file whose subsongs land in different albums must feed each
    // album's combine, so it is decoded once per group), and the buckets are
    // then flattened group-contiguous so the worker can treat a groupOf step
    // as an album boundary.
    Batch batch;
    batch.mode = mode;
    if (mode == ScanAlbumsByTags) {
        struct Group {
            QVector<ScanItem>     items;
            QVector<QVector<int>> rowsFor;
            QHash<QString, int>   pathIndex;
        };
        QVector<Group>      groups;
        QHash<QString, int> groupIndex;
        int                 soloCounter = 0;
        for (const QVariant& iv : items) {
            const QVariantMap m    = iv.toMap();
            const int         row  = m.value(QStringLiteral("row"), -1).toInt();
            const QString     path = m.value(QStringLiteral("path")).toString();
            if (path.isEmpty())
                continue;  // pathless: nothing to scan

            QString key = m.value(QStringLiteral("albumKey")).toString();
            if (key.isEmpty())  // the pane sends sentinels; belt and suspenders
                key = QStringLiteral("\x1Fsolo:%1").arg(soloCounter++);

            int        g;
            const auto git = groupIndex.find(key);
            if (git == groupIndex.end()) {
                g = static_cast<int>(groups.size());
                groupIndex.insert(key, g);
                groups.push_back(Group{});
            } else {
                g = git.value();
            }

            Group&     grp = groups[g];
            const auto pit = grp.pathIndex.find(path);
            if (pit == grp.pathIndex.end()) {
                ScanItem item;
                item.path = path;
                item.name = m.value(QStringLiteral("name")).toString();
                grp.pathIndex.insert(path, static_cast<int>(grp.items.size()));
                grp.items.push_back(std::move(item));
                grp.rowsFor.push_back(QVector<int>{row});
            } else {
                grp.rowsFor[pit.value()].push_back(row);
            }
        }
        for (int g = 0; g < groups.size(); ++g) {
            for (int i = 0; i < groups[g].items.size(); ++i) {
                batch.items.push_back(std::move(groups[g].items[i]));
                batch.rowsFor.push_back(std::move(groups[g].rowsFor[i]));
                batch.groupOf.push_back(g);
            }
        }
    } else {
        QHash<QString, int> pathIndex;
        pathIndex.reserve(items.size());
        for (const QVariant& iv : items) {
            const QVariantMap m    = iv.toMap();
            const int         row  = m.value(QStringLiteral("row"), -1).toInt();
            const QString     path = m.value(QStringLiteral("path")).toString();
            if (path.isEmpty())
                continue;  // pathless: nothing to scan

            const auto it = pathIndex.find(path);
            if (it == pathIndex.end()) {
                ScanItem item;
                item.path = path;
                item.name = m.value(QStringLiteral("name")).toString();
                pathIndex.insert(path, static_cast<int>(batch.items.size()));
                batch.items.push_back(std::move(item));
                batch.rowsFor.push_back(QVector<int>{row});
            } else {
                batch.rowsFor[it.value()].push_back(row);
            }
        }
    }
    if (batch.items.isEmpty()) {
        emit finished(QVariantList(), false, QStringList());
        return;
    }

    // Prime the progress surface for the first track, clear cancel, go.
    m_cancelRequested.store(false);
    m_totalTracks   = static_cast<int>(batch.items.size());
    m_doneTracks    = 0;
    m_trackFraction = 0.0;
    m_currentName   = batch.items.first().name;
    emit progressChanged();

    setBusy(true);
    m_watcher.setFuture(
        QtConcurrent::run(&ReplayGainScanController::runScan, this, batch));
}

void ReplayGainScanController::cancel() {
    m_cancelRequested.store(true);
}

ReplayGainScanController::RunResult ReplayGainScanController::runScan(Batch batch) {
    RunResult out;

    rawform::audio::DecoderFactory    factory;
    rawform::audio::ReplayGainScanner scanner(factory);

    const bool byTags    = (batch.mode == ScanAlbumsByTags);
    const bool albumMode = (batch.mode == ScanAlbum) || byTags;
    const int  total     = static_cast<int>(batch.items.size());

    // Per-unique-file measurement, kept so results can be fanned out to rows and
    // album fields appended after the album combine. measured == false marks a
    // file that failed to open (its rows get no result at all). group is the
    // file's album-group index (always 0 outside ByTags, where the whole batch
    // is one group or no combine happens at all).
    struct Measured {
        int    itemIndex  = -1;
        int    group      = 0;
        bool   measured   = false;
        bool   measurable = false;
        double gainDb     = 0.0;
        double peak       = 0.0;
    };
    QVector<Measured> measured;
    measured.reserve(total);

    // One combined album value per group, indexed by group number. ScanAlbum is
    // simply the one-group case; ScanTrack never pushes or reads an entry.
    QVector<QString> groupGain;
    QVector<QString> groupPeak;
    QVector<bool>    groupHave;

    // Close out the album combine of everything fed to the scanner since the
    // last reset: called at each ByTags group boundary and once after the loop.
    // A silence-only or all-failed-open group combines to nothing (albumResult
    // returns false) and its rows simply get no album fields, exactly the
    // single-album behavior for the same inputs.
    const auto finalizeGroup = [&]() {
        rawform::audio::ReplayGainScanner::AlbumResult album;
        const bool have = scanner.albumResult(&album);
        groupGain.push_back(have ? replaygain::formatGainValue(album.gainDb)
                                 : QString());
        groupPeak.push_back(have ? formatPeak(album.peak) : QString());
        groupHave.push_back(have);
    };

    for (int i = 0; i < total; ++i) {
        if (m_cancelRequested.load()) {
            out.canceled = true;
            return out;  // album needs every track, so a cancel discards the batch
        }

        // ByTags album boundary (items are flattened group-contiguous): close
        // out the finished group, then reset so the next group's combine
        // starts from a clean loudness state.
        if (byTags && i > 0 && batch.groupOf[i] != batch.groupOf[i - 1]) {
            finalizeGroup();
            scanner.reset();
        }

        const ScanItem& item = batch.items[i];
        postProgress(i, total, item.name, 0.0);  // start of this track

        int        lastPct = -1;
        const auto onProgress = [this, i, total, &item, &lastPct](double frac) {
            if (m_cancelRequested.load())
                return false;
            const int pct = static_cast<int>(frac * 100.0);
            if (pct != lastPct) {  // throttle to whole-percent steps
                lastPct = pct;
                postProgress(i, total, item.name, static_cast<qreal>(frac));
            }
            return true;
        };

        rawform::audio::ReplayGainScanner::TrackResult tr;
        std::string                                    err;
        const auto st =
            scanner.addTrack(item.path.toStdString(), onProgress, &tr, &err);

        const int group = byTags ? batch.groupOf[i] : 0;

        if (st == rawform::audio::ReplayGainScanner::Status::Canceled) {
            out.canceled = true;
            return out;
        }
        if (st == rawform::audio::ReplayGainScanner::Status::OpenFailed) {
            out.failedPaths << item.path;
            measured.push_back(
                Measured{i, group, /*measured=*/false, false, 0.0, 0.0});
            continue;  // keep going; each album combines whatever opened
        }
        measured.push_back(
            Measured{i, group, /*measured=*/true, tr.measurable, tr.gainDb, tr.peak});
    }

    // The last (ByTags) or only (ScanAlbum) group's combine, over the
    // measurable tracks (silence and failures excluded by the scanner / by
    // measured == false).
    if (albumMode)
        finalizeGroup();

    // Fan each file's measurement out to every row that shares its path, formatted
    // into the editor's edit-map shape; album fields come from the file's group.
    for (const Measured& m : measured) {
        if (!m.measured)
            continue;  // failed open: leave its rows unstaged

        const QString trackGainStr =
            replaygain::formatGainValue(m.measurable ? m.gainDb : 0.0);
        const QString trackPeakStr = m.measurable ? formatPeak(m.peak) : QString();
        const bool    haveAlbum =
            albumMode && m.group < groupHave.size() && groupHave[m.group];

        for (const int row : batch.rowsFor[m.itemIndex]) {
            QVariantMap rm;
            rm.insert(QStringLiteral("row"), row);
            rm.insert(QStringLiteral("trackGain"), trackGainStr);
            if (!trackPeakStr.isEmpty())
                rm.insert(QStringLiteral("trackPeak"), trackPeakStr);
            if (haveAlbum) {
                rm.insert(QStringLiteral("albumGain"), groupGain[m.group]);
                if (!groupPeak[m.group].isEmpty())
                    rm.insert(QStringLiteral("albumPeak"), groupPeak[m.group]);
            }
            out.results.push_back(rm);
        }
    }

    return out;
}

void ReplayGainScanController::postProgress(int index, int total,
                                            const QString& name, qreal frac) {
    // Marshal onto the GUI thread; QString is copied into the closure so the
    // worker's item reference is not read off-thread.
    QMetaObject::invokeMethod(
        this,
        [this, index, total, name, frac]() {
            m_doneTracks    = index;
            m_totalTracks   = total;
            m_currentName   = name;
            m_trackFraction = frac;
            emit progressChanged();
        },
        Qt::QueuedConnection);
}

void ReplayGainScanController::onFinished() {
    const RunResult r = m_watcher.result();
    setBusy(false);
    emit finished(r.results, r.canceled, r.failedPaths);
}

void ReplayGainScanController::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

}  // namespace rawform
