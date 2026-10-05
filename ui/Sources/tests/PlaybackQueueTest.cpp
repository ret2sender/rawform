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

// PlaybackQueueTest.cpp
//
// Standalone unit test for PlaybackQueue: the origin sidecar must stay
// aligned with the queue model's rows through every structural path, and the
// controller-facing queries must answer over the pair.
//
// Same harness shape as the other tests (plain main, counted checks, non-zero
// exit on failure). Qt-Quick-free and filesystem-free, but it does link the
// real PlaylistModel (the queue IS a PlaylistModel), which brings its own
// spine: the column schema, the pattern evaluator, the custom-column registry
// (yaml-cpp, no file is read), and Qt6::Qml for the QML_ELEMENT headers.
// No QCoreApplication: direct connections and persistent indexes work without
// one.
//
// What is covered:
//  1. ENQUEUE. Rows arrive in any order with duplicates and an out-of-range
//     entry; the queue holds them once each in ascending playlist order, each
//     with the right origin model and row.
//  2. ENQUEUE NEXT. Goes to the front, ahead of everything queued before.
//  3. ORIGIN SELF-HEALING. An insert above an origin row shifts the origin's
//     index; deleting the origin row invalidates it while the model pointer
//     survives; destroying the source model nulls the pointer.
//  4. REORDER. A moveTracks on the queue model permutes the sidecar exactly
//     like the rows (downward and upward moves).
//  5. CULL. takeFront drops the first n entries and their origins; clamped.
//  6. FOREIGN INSERTS. An insertTracks that did not come through enqueue (a
//     scanner batch into the queue tab) gets null origins and does not
//     disturb its neighbors.
//  7. QUERIES. firstIndexOfPath finds the lowest match and -1 for none;
//     paths() lists every entry in order.
//  8. RESET. clear() empties both halves; a setTracks reset re-sizes the
//     sidecar to all-null origins.
//
// Build via the CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_queue_test
//     ctest --test-dir build            # or run ./build/rawform_queue_test

#include "playback/PlaybackQueue.h"

#include "columns/ColumnSchema.h"
#include "media/TrackData.h"
#include "playlist/PlaylistModel.h"

#include <QList>
#include <QString>
#include <QVariantList>

#include <cstdio>
#include <memory>

using namespace rawform;

namespace {

int g_checks   = 0;
int g_failures = 0;

void check(bool ok, const char* label) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

/// A track whose path encodes its playlist and row, so an origin can be
/// verified from the queue row's path alone.
TrackData track(const char* list, int row) {
    TrackData t;
    t.filePath = QStringLiteral("/music/%1/%2.flac").arg(QLatin1String(list)).arg(row);
    t.fileName = QStringLiteral("%1.flac").arg(row);
    t.title    = QStringLiteral("%1 %2").arg(QLatin1String(list)).arg(row);
    return t;
}

/// A fresh model with @p n tracks named after @p list. Schema and registry
/// as main.cpp sets them (code-built schema, no custom columns).
std::unique_ptr<PlaylistModel> makeModel(const char* list, int n) {
    auto m = std::make_unique<PlaylistModel>();
    m->setSchema(ColumnSchema::load());
    m->setCustomColumns(nullptr);
    QList<TrackData> tracks;
    for (int i = 0; i < n; ++i) {
        tracks.push_back(track(list, i));
    }
    m->setTracks(std::move(tracks));
    return m;
}

QString pathOf(const char* list, int row) {
    return track(list, row).filePath;
}

bool originIs(const PlaybackQueue::Origin& o, const PlaylistModel* model, int row) {
    return o.model.data() == model && o.index.isValid() && o.index.row() == row;
}

// ---------------------------------------------------------------------------

void testEnqueueOrderAndOrigins() {
    auto a     = makeModel("a", 10);
    auto queue = makeModel("q", 0);
    PlaybackQueue pq(queue.get());

    const int added = pq.enqueue(a.get(), { 7, 2, 2, 42, 0 });
    check(added == 3, "enqueue: duplicates and out-of-range dropped (3 added)");
    check(pq.count() == 3, "enqueue: count 3");
    check(pq.pathAt(0) == pathOf("a", 0), "enqueue: row 0 is a:0 (ascending order)");
    check(pq.pathAt(1) == pathOf("a", 2), "enqueue: row 1 is a:2");
    check(pq.pathAt(2) == pathOf("a", 7), "enqueue: row 2 is a:7");
    check(originIs(pq.originAt(0), a.get(), 0), "enqueue: origin 0 -> a row 0");
    check(originIs(pq.originAt(1), a.get(), 2), "enqueue: origin 1 -> a row 2");
    check(originIs(pq.originAt(2), a.get(), 7), "enqueue: origin 2 -> a row 7");
    check(pq.originAt(3).model.isNull() && !pq.originAt(3).index.isValid(),
          "enqueue: out-of-range origin is null");
    check(pq.enqueue(nullptr, { 0 }) == 0, "enqueue: null source adds nothing");
    check(pq.enqueue(a.get(), {}) == 0, "enqueue: empty rows add nothing");
    check(pq.trackAt(1) != nullptr && pq.trackAt(1)->title == QStringLiteral("a 2"),
          "trackAt: the copied TrackData");
}

void testEnqueueNext() {
    auto a     = makeModel("a", 5);
    auto b     = makeModel("b", 5);
    auto queue = makeModel("q", 0);
    PlaybackQueue pq(queue.get());

    pq.enqueue(a.get(), { 0, 1 });
    const int added = pq.enqueueNext(b.get(), { 4, 3 });
    check(added == 2, "enqueueNext: 2 added");
    check(pq.count() == 4, "enqueueNext: count 4");
    check(pq.pathAt(0) == pathOf("b", 3), "enqueueNext: front is b:3 (playlist order)");
    check(pq.pathAt(1) == pathOf("b", 4), "enqueueNext: then b:4");
    check(pq.pathAt(2) == pathOf("a", 0), "enqueueNext: the earlier entries follow");
    check(pq.pathAt(3) == pathOf("a", 1), "enqueueNext: in their order");
    check(originIs(pq.originAt(0), b.get(), 3), "enqueueNext: origin 0 -> b row 3");
    check(originIs(pq.originAt(2), a.get(), 0), "enqueueNext: origin 2 -> a row 0");
}

void testOriginSelfHealing() {
    auto a     = makeModel("a", 5);
    auto queue = makeModel("q", 0);
    PlaybackQueue pq(queue.get());
    pq.enqueue(a.get(), { 1, 3 });

    // An insert above both origins shifts them down by the insert size.
    a->insertTracks(0, { track("a", 90), track("a", 91) });
    check(originIs(pq.originAt(0), a.get(), 3),
          "self-heal: origin follows an insert above (1 -> 3)");
    check(originIs(pq.originAt(1), a.get(), 5),
          "self-heal: origin follows an insert above (3 -> 5)");

    // Deleting the first origin's row invalidates it; the other shifts up.
    a->removeTracks({ 3 });
    check(pq.originAt(0).model.data() == a.get() && !pq.originAt(0).index.isValid(),
          "self-heal: a deleted origin row invalidates the index, keeps the model");
    check(originIs(pq.originAt(1), a.get(), 4),
          "self-heal: the other origin shifts up (5 -> 4)");

    // The queue rows themselves are untouched by edits to the source.
    check(pq.count() == 2 && pq.pathAt(0) == pathOf("a", 1),
          "self-heal: queue rows untouched");

    // Destroying the source nulls the pointer (QPointer), the row stays queued.
    a.reset();
    check(pq.originAt(1).model.isNull(),
          "self-heal: a destroyed source nulls the origin model");
    check(pq.count() == 2, "self-heal: entries survive their source's destruction");
}

void testReorderPermutesSidecar() {
    auto a     = makeModel("a", 5);
    auto b     = makeModel("b", 5);
    auto queue = makeModel("q", 0);
    PlaybackQueue pq(queue.get());
    pq.enqueue(a.get(), { 0, 1, 2 });
    pq.enqueue(b.get(), { 0 });
    // Queue: a0 a1 a2 b0

    // Downward move: lift a0 (row 0) to the end (boundary 4).
    check(queue->moveTracks(0, 1, 4), "reorder: downward move accepted");
    check(pq.pathAt(3) == pathOf("a", 0), "reorder: a0 now last");
    check(originIs(pq.originAt(3), a.get(), 0),
          "reorder: origin moved with the row (down)");
    check(originIs(pq.originAt(0), a.get(), 1), "reorder: a1 origin now first");
    check(originIs(pq.originAt(2), b.get(), 0), "reorder: b0 origin at index 2");

    // Upward move of a two-row block: lift (b0, a0) at rows 2..3 to the front.
    check(queue->moveTracks(2, 2, 0), "reorder: upward block move accepted");
    check(pq.pathAt(0) == pathOf("b", 0) && pq.pathAt(1) == pathOf("a", 0),
          "reorder: block now leads (b0, a0)");
    check(originIs(pq.originAt(0), b.get(), 0), "reorder: origin b0 first (up)");
    check(originIs(pq.originAt(1), a.get(), 0), "reorder: origin a0 second (up)");
    check(originIs(pq.originAt(2), a.get(), 1), "reorder: origin a1 third (up)");
    check(originIs(pq.originAt(3), a.get(), 2), "reorder: origin a2 last (up)");
    check(pq.count() == 4, "reorder: count unchanged");
}

void testTakeFront() {
    auto a     = makeModel("a", 5);
    auto queue = makeModel("q", 0);
    PlaybackQueue pq(queue.get());
    pq.enqueue(a.get(), { 0, 1, 2, 3 });

    pq.takeFront(2);
    check(pq.count() == 2, "takeFront: two removed");
    check(pq.pathAt(0) == pathOf("a", 2), "takeFront: a2 now first");
    check(originIs(pq.originAt(0), a.get(), 2), "takeFront: origin a2 first");
    check(originIs(pq.originAt(1), a.get(), 3), "takeFront: origin a3 second");

    pq.takeFront(0);
    check(pq.count() == 2, "takeFront: zero is a no-op");
    pq.takeFront(99);
    check(pq.count() == 0, "takeFront: clamped to the count");
    check(pq.originAt(0).model.isNull(), "takeFront: sidecar empty too");
}

void testForeignInsertGetsNullOrigins() {
    auto a     = makeModel("a", 5);
    auto queue = makeModel("q", 0);
    PlaybackQueue pq(queue.get());
    pq.enqueue(a.get(), { 0, 1 });

    // A scanner-style batch landing between the two entries.
    queue->insertTracks(1, { track("x", 0), track("x", 1) });
    check(pq.count() == 4, "foreign insert: count 4");
    check(originIs(pq.originAt(0), a.get(), 0), "foreign insert: neighbor before intact");
    check(pq.originAt(1).model.isNull() && !pq.originAt(1).index.isValid(),
          "foreign insert: null origin (x0)");
    check(pq.originAt(2).model.isNull(), "foreign insert: null origin (x1)");
    check(originIs(pq.originAt(3), a.get(), 1), "foreign insert: neighbor after intact");
    check(pq.pathAt(3) == pathOf("a", 1), "foreign insert: rows and sidecar agree");
}

void testQueries() {
    auto a     = makeModel("a", 5);
    auto queue = makeModel("q", 0);
    PlaybackQueue pq(queue.get());
    pq.enqueue(a.get(), { 2, 3 });
    pq.enqueue(a.get(), { 2 }); // a2 twice

    check(pq.firstIndexOfPath(pathOf("a", 2)) == 0, "firstIndexOfPath: lowest match");
    check(pq.firstIndexOfPath(pathOf("a", 3)) == 1, "firstIndexOfPath: second entry");
    check(pq.firstIndexOfPath(pathOf("a", 4)) == -1, "firstIndexOfPath: absent -> -1");
    check(pq.firstIndexOfPath(QString()) == -1, "firstIndexOfPath: empty -> -1");

    const std::vector<std::string> p = pq.paths();
    check(p.size() == 3, "paths: three entries");
    check(p.size() == 3 && p[0] == pathOf("a", 2).toStdString()
              && p[1] == pathOf("a", 3).toStdString()
              && p[2] == pathOf("a", 2).toStdString(),
          "paths: queue order, duplicates kept");
    check(pq.pathAt(7).isEmpty(), "pathAt: out of range is empty");
    check(pq.trackAt(7) == nullptr, "trackAt: out of range is null");
}

void testClearAndReset() {
    auto a     = makeModel("a", 5);
    auto queue = makeModel("q", 0);
    PlaybackQueue pq(queue.get());
    pq.enqueue(a.get(), { 0, 1, 2 });

    pq.clear();
    check(pq.count() == 0, "clear: empty");
    check(queue->rowCount() == 0, "clear: the model is empty too");
    check(pq.paths().empty(), "clear: no paths");
    pq.clear();
    check(pq.count() == 0, "clear: idempotent");

    // A reset that LEAVES rows (a load-time setTracks) re-sizes to null origins.
    pq.enqueue(a.get(), { 0 });
    queue->setTracks({ track("y", 0), track("y", 1) });
    check(pq.count() == 2, "reset: count follows the model");
    check(pq.originAt(0).model.isNull() && pq.originAt(1).model.isNull(),
          "reset: origins degrade to null");
    check(pq.firstIndexOfPath(pathOf("y", 1)) == 1, "reset: rows still queryable");
}

} // namespace

int main() {
    testEnqueueOrderAndOrigins();
    testEnqueueNext();
    testOriginSelfHealing();
    testReorderPermutesSidecar();
    testTakeFront();
    testForeignInsertGetsNullOrigins();
    testQueries();
    testClearAndReset();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
