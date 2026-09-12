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

#include <QMutex>
#include <QSet>
#include <QString>
#include <QWaitCondition>

#include <utility>

namespace rawform {

/*
 * TagWriteLock: a process-wide, per-path lease for everything that mutates a
 * media file: the two tag writers (MetadataEditor, ReplayGainEditor) and the
 * renamer (FileRenamer, which holds it on the source path for the whole move).
 *
 * Why it exists. The Properties windows are non-modal and multi-instance, and
 * each owns its own MetadataEditor / ReplayGainEditor. Every editor already
 * dedupes by path WITHIN its own pass, so one pass never writes a file from
 * two pool threads; but two windows can stage the same file and Apply at the
 * same time, and two TagLib read-modify-write saves interleaving on one file
 * is permanent damage (a torn or duplicated tag block). A rename landing
 * between another window's open and save is the same class of damage. This
 * lease serializes exactly that collision and nothing else: distinct paths
 * proceed in parallel (a batch write across many files keeps its full
 * thread-pool width), and only a second holder of the SAME path blocks until
 * the first releases.
 *
 * How. One global mutex guards a set of busy paths plus a wait condition.
 * Construction acquires the lease (waiting while the path is busy), the
 * destructor releases it and wakes waiters: plain RAII, so the writers'
 * early-return failure paths release correctly. wakeAll rather than wakeOne
 * because waiters may be waiting on DIFFERENT paths through the one condition;
 * a woken thread whose path is still busy simply re-waits. Collisions are rare
 * and tag writes are short, so the thundering herd stays theoretical.
 *
 * Scope. Serializes mutators against each other only. Readers (TrackReader,
 * the ReplayGain scan) stay unsynchronized on purpose: they are pervasive,
 * and a read racing a write is self-healing (the post-apply reload re-reads
 * the file), while a write racing a write is not.
 *
 * The path string is the identity, verbatim, matching the editors' own dedupe
 * keys; both come from the same PlaylistModel snapshot, so two spellings of
 * one file would bypass this lease exactly as they would bypass per-pass
 * dedupe (and the scanner does not produce such aliases).
 */
class TagWriteLock {
public:
    explicit TagWriteLock(QString path) : m_path(std::move(path)) {
        QMutexLocker locker(&mutex());
        while (busyPaths().contains(m_path))
            waiters().wait(&mutex());
        busyPaths().insert(m_path);
    }

    ~TagWriteLock() {
        QMutexLocker locker(&mutex());
        busyPaths().remove(m_path);
        waiters().wakeAll();
    }

    TagWriteLock(const TagWriteLock&) = delete;
    TagWriteLock& operator=(const TagWriteLock&) = delete;

private:
    /// Meyers singletons rather than namespace-scope statics: constructed on
    /// first use from whichever pool thread gets there first, with no
    /// cross-translation-unit initialization-order concerns.
    static QMutex& mutex() {
        static QMutex m;
        return m;
    }
    static QWaitCondition& waiters() {
        static QWaitCondition c;
        return c;
    }
    static QSet<QString>& busyPaths() {
        static QSet<QString> s;
        return s;
    }

    QString m_path;
};

} // namespace rawform
