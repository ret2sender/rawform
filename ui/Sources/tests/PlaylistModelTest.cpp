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

// PlaylistModelTest.cpp
//
// Standalone unit test for PlaylistModel's row mutations as the view sees
// them: through the tab's PlaylistFilterProxy with a QItemSelectionModel bound
// to it, with and without a PlaylistSearch filter in force.
//
// Same harness shape as the other tests (plain main, counted checks, non-zero
// exit on failure). Qt-Quick-free and filesystem-free; it links the model's
// spine (schema, pattern evaluator, custom-column registry with yaml-cpp, no
// file is read), the proxy and the search, and Qt6::Qml for the QML_ELEMENT
// headers. A QCoreApplication exists for the search's coalescing timer.
//
// What is covered:
//  1. PER-RUN REMOVAL (few runs). Rows leave through one rowsRemoved per
//     contiguous run; the surviving order, the surviving selection, the
//     selection model's own current-row relocation, the bracket signals and
//     rowAfterRemoval's mapping.
//  2. COMPACTION (many runs). The same removal above kCompactRemovalRuns runs
//     as one layoutAboutToBeChanged / layoutChanged pair: same surviving
//     order and selection, persistent indexes moved or invalidated, a current
//     index on a removed row invalidated (the tab hook's relocation rule is
//     checked as arithmetic on rowAfterRemoval), no rowsRemoved.
//  3. COMPACTION UNDER A FILTER. With a search hiding half the rows, a
//     many-run removal of visible rows leaves the proxy showing exactly the
//     surviving matches at once (the match set is carried across the layout
//     change), and the viewport anchor mapping answers in proxy rows.
//  4. REMOVE UNAVAILABLE. Shares the paths and the bracket.
//  5. BLOCK MOVE. moveTracks down and up: one rowsMoved, the rotated order,
//     the selection carried with the block.
//  6. SCALE. 15k rows: every other row removed through the proxy with the rows
//     selected and with the selection cleared first (the Delete key's order),
//     and a 5000-row block moved to the middle. The numbers are printed; the
//     only assertion is a loose tripwire (one second) that the old per-run and
//     per-element paths exceed by an order of magnitude at this size.
//
// Build via the CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_model_test
//     ctest --test-dir build -L ui       # or run ./build/rawform_model_test

#include "playlist/PlaylistModel.h"

#include "columns/ColumnSchema.h"
#include "media/TrackData.h"
#include "playlist/PlaylistFilterProxy.h"
#include "search/PlaylistSearch.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QList>
#include <QPersistentModelIndex>
#include <QString>
#include <QTimer>
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

/// A track whose path and title encode its ORIGINAL row, so the order after
/// any mutation can be read back from the paths alone. Even rows are titled
/// "even", odd rows "odd": the filter test searches for one of the two.
TrackData track(int row) {
    TrackData t;
    t.filePath = QStringLiteral("/music/%1.flac").arg(row);
    t.fileName = QStringLiteral("%1.flac").arg(row);
    t.title    = QStringLiteral("%1 %2").arg(row % 2 == 0 ? QStringLiteral("even")
                                                           : QStringLiteral("odd"))
                                           .arg(row);
    return t;
}

std::unique_ptr<PlaylistModel> makeModel(int n) {
    auto m = std::make_unique<PlaylistModel>();
    m->setSchema(ColumnSchema::load());
    m->setCustomColumns(nullptr);
    QList<TrackData> tracks;
    tracks.reserve(n);
    for (int i = 0; i < n; ++i) {
        tracks.push_back(track(i));
    }
    m->setTracks(std::move(tracks));
    return m;
}

/// The original row of the track now at @p row (-1 if out of range).
int originalRowAt(const PlaylistModel& m, int row) {
    const TrackData* t = m.trackAt(row);
    if (t == nullptr) {
        return -1;
    }
    return t->fileName.section(QLatin1Char('.'), 0, 0).toInt();
}

QVariantList rowsOf(const QList<int>& rows) {
    QVariantList out;
    out.reserve(rows.size());
    for (const int r : rows) {
        out.append(r);
    }
    return out;
}

/// Spin the event loop for @p ms so the search's coalescing timer fires.
void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

/// The structural signals a removal or move may emit, counted.
struct SignalCounts {
    int rowsRemoved   = 0;
    int rowsMoved     = 0;
    int layoutChanged = 0;
    int bulkStarted   = 0;
    int bulkFinished  = 0;

    explicit SignalCounts(PlaylistModel* m) {
        QObject::connect(m, &QAbstractItemModel::rowsRemoved, m,
                         [this]() { ++rowsRemoved; });
        QObject::connect(m, &QAbstractItemModel::rowsMoved, m, [this]() { ++rowsMoved; });
        QObject::connect(m, &QAbstractItemModel::layoutChanged, m,
                         [this]() { ++layoutChanged; });
        QObject::connect(m, &PlaylistModel::bulkRemovalStarted, m,
                         [this]() { ++bulkStarted; });
        QObject::connect(m, &PlaylistModel::bulkRemovalFinished, m,
                         [this]() { ++bulkFinished; });
    }
};

/// Every selected PROXY row, ascending, through the proxy's range walk.
QList<int> selectedRows(const PlaylistFilterProxy& proxy, QItemSelectionModel* sel) {
    return proxy.selectedRowList(sel);
}

// ---------------------------------------------------------------------------

void testPerRunRemoval() {
    auto model = makeModel(200);
    PlaylistFilterProxy proxy(model.get());
    QItemSelectionModel sel(&proxy);
    SignalCounts counts(model.get());

    // Rows 0-9 and 118-122 selected; the current index on row 4, which goes.
    proxy.selectRowRange(&sel, 0, 9, true);
    proxy.selectRowRange(&sel, 118, 122, false);
    sel.setCurrentIndex(proxy.index(4, 0), QItemSelectionModel::NoUpdate);

    // Three runs: {3,4,5}, {50}, {120,121}; handed over in any order with a
    // duplicate and an out-of-range row, as the view's dedupe allows.
    proxy.removeTracks(rowsOf({ 121, 5, 50, 3, 4, 120, 4, 9999 }));

    check(model->rowCount() == 194, "per-run: 6 rows removed");
    check(counts.rowsRemoved == 3, "per-run: one rowsRemoved per run (3)");
    check(counts.layoutChanged == 0, "per-run: no layoutChanged");
    check(counts.bulkStarted == 1 && counts.bulkFinished == 1, "per-run: bracket once");
    check(originalRowAt(*model, 3) == 6 && originalRowAt(*model, 47) == 51
              && originalRowAt(*model, 116) == 122 && originalRowAt(*model, 193) == 199,
          "per-run: surviving order");

    const QList<int> selected = selectedRows(proxy, &sel);
    check(selected == QList<int>({ 0, 1, 2, 3, 4, 5, 6, 114, 115, 116 }),
          "per-run: the surviving selection remapped");
    check(sel.currentIndex().isValid() && sel.currentIndex().row() == 2,
          "per-run: current relocated by the selection model to the row above the run");

    check(model->rowAfterRemoval(-1) == -1, "rowAfterRemoval: negative stays -1");
    check(model->rowAfterRemoval(0) == 0, "rowAfterRemoval: row above every run unmoved");
    check(model->rowAfterRemoval(4) == 3,
          "rowAfterRemoval: removed row -> the row that took its place");
    check(model->rowAfterRemoval(6) == 3, "rowAfterRemoval: that row itself lands there");
    check(model->rowAfterRemoval(50) == 47 && model->rowAfterRemoval(122) == 116
              && model->rowAfterRemoval(199) == 193,
          "rowAfterRemoval: survivors shifted by the rows removed below them");
    check(model->rowAfterRemoval(200) == 194,
          "rowAfterRemoval: past the end -> the row count");
}

void testCompaction() {
    constexpr int kRows = 200;
    auto model = makeModel(kRows);
    PlaylistFilterProxy proxy(model.get());
    QItemSelectionModel sel(&proxy);
    SignalCounts counts(model.get());

    // Rows 10-29 selected (even and odd alike); current on 20 (even, goes);
    // persistent indexes on 21 (odd, stays) and 40 (even, goes).
    proxy.selectRowRange(&sel, 10, 29, true);
    sel.setCurrentIndex(proxy.index(20, 0), QItemSelectionModel::NoUpdate);
    const QPersistentModelIndex stays(model->index(21, 0));
    const QPersistentModelIndex goes(model->index(40, 0));

    // Every even row: 100 runs, well above the threshold.
    QList<int> evens;
    for (int r = 0; r < kRows; r += 2) {
        evens.append(r);
    }
    check(evens.size() > PlaylistModel::kCompactRemovalRuns,
          "compaction: run count above the threshold");
    proxy.removeTracks(rowsOf(evens));

    check(model->rowCount() == kRows / 2, "compaction: half the rows removed");
    check(counts.rowsRemoved == 0, "compaction: no rowsRemoved");
    check(counts.layoutChanged == 1, "compaction: exactly one layoutChanged");
    check(counts.bulkStarted == 1 && counts.bulkFinished == 1,
          "compaction: bracket once");
    bool ordered = true;
    for (int r = 0; r < model->rowCount(); ++r) {
        ordered = ordered && originalRowAt(*model, r) == 2 * r + 1;
    }
    check(ordered, "compaction: survivors in order, one pass");

    // Odd rows 11..29 were selected: ten rows, now at 5..14.
    check(selectedRows(proxy, &sel) == QList<int>({ 5, 6, 7, 8, 9, 10, 11, 12, 13, 14 }),
          "compaction: the surviving selection remapped through the proxy");
    check(stays.isValid() && stays.row() == 10,
          "compaction: persistent index on a survivor follows it");
    check(!goes.isValid(), "compaction: persistent index on a removed row invalidated");
    check(!sel.currentIndex().isValid(), "compaction: a current index on a removed row "
                                         "is invalid (the tab hook re-places it)");
    // The hook's rule: the nearest surviving row above the removed one, which
    // is the row before the one that took its place.
    check(model->rowAfterRemoval(20) == 10 && model->rowAfterRemoval(19) == 9,
          "compaction: rowAfterRemoval gives the replacement (10) and the survivor above "
          "(9)");

    // A current index on a survivor rides along.
    sel.setCurrentIndex(proxy.index(10, 0), QItemSelectionModel::NoUpdate);
    QList<int> more;
    for (int r = 0; r < model->rowCount(); r += 3) {
        more.append(r); // 34 runs, still the compaction path
    }
    proxy.removeTracks(rowsOf(more));
    check(counts.layoutChanged == 2 && counts.rowsRemoved == 0,
          "compaction: second removal compacts too");
    check(sel.currentIndex().isValid() &&
              sel.currentIndex().row() == model->rowAfterRemoval(10),
          "compaction: a surviving current index moved to its new row");
}

void testCompactionUnderFilter() {
    constexpr int kRows = 200;
    auto model = makeModel(kRows);
    PlaylistFilterProxy proxy(model.get());
    QItemSelectionModel sel(&proxy);
    PlaylistSearch search;
    search.setCustomColumns(nullptr);
    search.setModel(model.get());
    search.setEnabled(true);
    proxy.setSearch(&search);
    search.setQueryText(QStringLiteral("odd"));
    spin(300); // the coalesce window plus the match pass
    check(search.active() && proxy.rowCount() == kRows / 2,
          "filter: the odd rows are the visible set");

    // Every other VISIBLE row: proxy rows 0, 2, 4, ... are source rows 1, 5,
    // 9, ...: fifty runs.
    QList<int> visible;
    for (int p = 0; p < proxy.rowCount(); p += 2) {
        visible.append(p);
    }
    SignalCounts counts(model.get());
    proxy.removeTracks(rowsOf(visible));
    check(counts.layoutChanged == 1 && counts.rowsRemoved == 0,
          "filter: the removal compacted");
    check(model->rowCount() == kRows - 50, "filter: fifty source rows removed");

    // Before any timer fires, the proxy must already show exactly the
    // surviving matches: the match set was carried across the layout change.
    check(proxy.rowCount() == 50,
          "filter: the proxy shows the surviving matches at once");
    bool allOdd = true;
    for (int p = 0; p < proxy.rowCount(); ++p) {
        const int src = proxy.mapRowToSource(p);
        allOdd = allOdd && src >= 0 && originalRowAt(*model, src) % 2 == 1
                 && originalRowAt(*model, src) % 4 == 3;
    }
    check(allOdd, "filter: every visible row is a surviving odd row (3, 7, 11, ...)");

    // The viewport anchor: a parked source row that was removed (old row 5)
    // lands on old row 6 (hidden), so the anchor is the first visible row at
    // or after it, old row 7, now source row 5 and proxy row 1.
    check(proxy.anchorRowAfterRemoval(5) == 1, "filter: anchor maps through the filter");
    check(proxy.anchorRowAfterRemoval(-1) == -1,
          "filter: a never-parked anchor stays -1");

    spin(300); // the rebuild lands; the picture must not change
    check(proxy.rowCount() == 50 && search.matchCount() == 50,
          "filter: the rebuilt match set agrees with the carried one");
}

void testRemoveUnavailable() {
    auto model = makeModel(120);
    PlaylistFilterProxy proxy(model.get());
    SignalCounts counts(model.get());

    // Flag every third row missing: forty runs, the compaction path.
    for (int r = 0; r < 120; r += 3) {
        TrackData t = *model->trackAt(r);
        t.available = false;
        model->refreshTrack(r, std::move(t));
    }
    check(proxy.removeUnavailableTracks() == 40, "unavailable: forty removed");
    check(model->rowCount() == 80 && counts.layoutChanged == 1 && counts.rowsRemoved == 0,
          "unavailable: compacted in one pass");
    check(originalRowAt(*model, 0) == 1 && originalRowAt(*model, 1) == 2
              && originalRowAt(*model, 2) == 4 && originalRowAt(*model, 79) == 119,
          "unavailable: surviving order");
    check(proxy.removeUnavailableTracks() == 0 && counts.bulkStarted == 1,
          "unavailable: nothing left to remove, no bracket");

    // Two missing rows: the per-run path.
    for (const int r : { 5, 40 }) {
        TrackData t = *model->trackAt(r);
        t.available = false;
        model->refreshTrack(r, std::move(t));
    }
    check(proxy.removeUnavailableTracks() == 2 && counts.rowsRemoved == 2
              && counts.layoutChanged == 1,
          "unavailable: two runs go through rowsRemoved");
}

void testBlockMove() {
    auto model = makeModel(100);
    PlaylistFilterProxy proxy(model.get());
    QItemSelectionModel sel(&proxy);
    SignalCounts counts(model.get());

    // Down: rows 10-14 to the gap before row 50 -> they land at 45-49.
    proxy.selectRowRange(&sel, 10, 14, true);
    check(proxy.moveTracks(10, 5, 50), "move down: accepted");
    check(counts.rowsMoved == 1, "move down: one rowsMoved");
    check(originalRowAt(*model, 10) == 15 && originalRowAt(*model, 44) == 49
              && originalRowAt(*model, 45) == 10 && originalRowAt(*model, 49) == 14
              && originalRowAt(*model, 50) == 50,
          "move down: rotated order");
    check(selectedRows(proxy, &sel) == QList<int>({ 45, 46, 47, 48, 49 }),
          "move down: the selection travels with the block");

    // Up: rows 60-64 to the gap before row 20 -> they land at 20-24.
    check(proxy.moveTracks(60, 5, 20), "move up: accepted");
    check(counts.rowsMoved == 2, "move up: one rowsMoved");
    check(originalRowAt(*model, 20) == 60 && originalRowAt(*model, 24) == 64
              && originalRowAt(*model, 25) == 25 && originalRowAt(*model, 64) == 59,
          "move up: rotated order");

    // Refusals unchanged.
    check(!proxy.moveTracks(10, 5, 12), "move: in-place destination refused");
    check(!proxy.moveTracks(98, 5, 0), "move: out-of-range block refused");
    check(!proxy.moveTracks(0, 5, 101), "move: out-of-range destination refused");
    check(counts.rowsMoved == 2, "move: refusals emit nothing");

    // To the very end and to the very top.
    check(proxy.moveTracks(0, 3, 100) && originalRowAt(*model, 99) == 2
              && originalRowAt(*model, 97) == 0,
          "move: a block to the end");
    check(proxy.moveTracks(97, 3, 0) && originalRowAt(*model, 0) == 0
              && originalRowAt(*model, 2) == 2,
          "move: and back to the top");
}

void testScale() {
    constexpr int kRows = 15000;
    auto model = makeModel(kRows);
    PlaylistFilterProxy proxy(model.get());
    QItemSelectionModel sel(&proxy);

    // Every other row selected, as one merged selection in one select().
    const int cols = proxy.columnCount();
    QItemSelection scattered;
    for (int r = 0; r < kRows; r += 2) {
        scattered.append(
            QItemSelectionRange(proxy.index(r, 0), proxy.index(r, cols - 1)));
    }
    sel.select(scattered, QItemSelectionModel::ClearAndSelect);
    sel.setCurrentIndex(proxy.index(0, 0), QItemSelectionModel::NoUpdate);
    const QList<int> rows = selectedRows(proxy, &sel);
    check(rows.size() == kRows / 2, "scale: 7500 rows selected");

    QElapsedTimer clock;
    clock.start();
    proxy.removeTracks(rowsOf(rows));
    const qint64 selectedMs = clock.elapsed();
    check(model->rowCount() == kRows / 2,
          "scale: 7500 scattered rows removed with the selection held");
    std::printf("scale: 7500 scattered rows of 15k removed, selection held: %lld ms\n",
                static_cast<long long>(selectedMs));

    // The Delete key's order: selection cleared, then the removal.
    auto model2 = makeModel(kRows);
    PlaylistFilterProxy proxy2(model2.get());
    QItemSelectionModel sel2(&proxy2);
    QItemSelection scattered2;
    for (int r = 0; r < kRows; r += 2) {
        scattered2.append(
            QItemSelectionRange(proxy2.index(r, 0), proxy2.index(r, cols - 1)));
    }
    sel2.select(scattered2, QItemSelectionModel::ClearAndSelect);
    const QList<int> rows2 = selectedRows(proxy2, &sel2);
    clock.restart();
    sel2.clearSelection();
    proxy2.removeTracks(rowsOf(rows2));
    const qint64 clearedMs = clock.elapsed();
    check(model2->rowCount() == kRows / 2,
          "scale: 7500 scattered rows removed after clearSelection");
    std::printf(
        "scale: the same removal after clearSelection (the Delete path): %lld ms\n",
        static_cast<long long>(clearedMs));

    // A 5000-row block from the top to the middle.
    auto model3 = makeModel(kRows);
    PlaylistFilterProxy proxy3(model3.get());
    QItemSelectionModel sel3(&proxy3);
    clock.restart();
    const bool moved = proxy3.moveTracks(0, 5000, 10000);
    const qint64 moveMs = clock.elapsed();
    check(moved && originalRowAt(*model3, 0) == 5000 &&
              originalRowAt(*model3, 5000) == 0 && originalRowAt(*model3, 9999) == 4999 &&
              originalRowAt(*model3, 10000) == 10000,
          "scale: 5000-row block moved to the middle");
    std::printf("scale: 5000 rows of 15k moved to the middle: %lld ms\n",
                static_cast<long long>(moveMs));

    // Tripwires, an order of magnitude above the compaction and rotate costs
    // on a slow Debug runner and an order of magnitude below the old paths.
    check(selectedMs < 1000,
          "scale: scattered removal under one second (selection held)");
    check(clearedMs < 1000, "scale: scattered removal under one second (Delete path)");
    check(moveMs < 1000, "scale: block move under one second");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    testPerRunRemoval();
    testCompaction();
    testCompactionUnderFilter();
    testRemoveUnavailable();
    testBlockMove();
    testScale();

    std::printf("PlaylistModelTest: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
