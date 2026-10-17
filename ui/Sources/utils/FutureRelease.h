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

// FutureRelease.h
//
// Drop a finished future's result store from a QFutureWatcher. A watcher keeps
// its future, and the future keeps every result the pool produced, until the
// next setFuture; for the workers that map a whole playlist (the reloader's
// verdicts, the scanner's TrackData) that is megabytes resident for as long as
// the app idles after a pass. Header-only, for the pattern's few call sites.

#pragma once

#include <QCoreApplication>
#include <QEvent>
#include <QFuture>
#include <QFutureWatcher>

namespace rawform {

/// Replace @p watcher's future with a default-constructed one, releasing the
/// result store, without the watcher ever reporting on the empty future.
///
/// The second half is the part that needs care. A default QFuture is already
/// in the Started | Canceled | Finished state, and connecting a watcher to a
/// future replays that state to it: started(), canceled() and finished() for a
/// future that has nothing in it. The replay travels as posted events, so it
/// is delivered on a LATER event-loop turn, after this call has returned and
/// whatever handler called it has finished; every call site is itself a
/// finished-signal handler (the natural moment to let go of the results), and
/// the replayed finished() would run that handler again against an empty
/// store (the scenario this prevents: the scanner's enumeration handler
/// reading result() of a future with no result, a crash on every drop of
/// files into a playlist). Removing the watcher's pending call-out events
/// right after the assignment discards the replay before it is delivered;
/// nothing else of the watcher's is pending at that point, because the
/// assignment itself removed the old future's events. Qt does the same removal
/// when it disconnects a watcher from a future. A watcher that never had a
/// future, or was already released, is a no-op here.
///
/// Call after the last resultAt() / result() read: the store is gone when this
/// returns.
template <typename T>
void releaseFuture(QFutureWatcher<T>& watcher) {
    watcher.setFuture(QFuture<T>());
    QCoreApplication::removePostedEvents(&watcher, QEvent::FutureCallOut);
}

} // namespace rawform
