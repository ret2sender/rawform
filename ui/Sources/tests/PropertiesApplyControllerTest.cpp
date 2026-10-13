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

// PropertiesApplyControllerTest.cpp
//
// Standalone unit test for PropertiesApplyController, the Properties
// window's apply and reload sequencer. The three collaborators it drives
// arrive through the ApplyPorts bases, so this test stands in for them with
// recorders that touch no file and complete a phase only when the test says
// so (by emitting applied() / pathsReloaded()); the three pane models and
// the PlaylistModel are the real ones. Nothing is written, nothing is read.
//
// Same harness shape as the other tests (plain main, counted checks,
// non-zero exit on failure). Qt-Quick-free and filesystem-free; links the
// PlaylistModel spine like the queue test. No QCoreApplication: direct
// connections work without one.
//
// What is covered:
//  1. META ONLY. A staged field value writes through the metadata port with
//     no strip, skips the RG port, reloads exactly the touched path, re-pulls
//     the models (staging cleared), flushes the playlist and finishes.
//  2. RG ONLY. A staged gain skips the metadata port and writes through the
//     RG port.
//  3. BOTH. Metadata first, then RG, then one reload of the union of paths.
//  4. NOTHING STAGED. OK finishes at once with a close request, Apply does
//     nothing; neither port is touched and busy never rises.
//  5. STRIP. Remove tags raises stripStaged and a warning status; the pass
//     carries the selection's paths as stripAllPaths even with no field
//     edit; the flag clears when the pass finishes.
//  6. TOTAL FAILURE. No file written: no reload, the staging survives, the
//     status counts the failure, and an OK does not close.
//  7. PARTIAL FAILURE. One of two files failed: the reload still runs, the
//     status counts one, OK does not close.
//  8. RELOADER DEAD. Before the reload phase: the pass finishes without a
//     reload. Mid-pass: the port's destructor flush completes the pass.
//     Reload info with no reloader: the warning status, nothing else.
//  9. MODEL DEAD AT FINISH. The playlist is destroyed while the reload
//     runs: the models adopt the staged values (dirty clears) instead of
//     re-pulling, and the pass still finishes.
// 10. RELOAD INFO. The pass announces itself (reloadStarting), discards a
//     staged strip, runs under busy with its own token, ignores a foreign
//     token, and re-pulls on completion.
//
// Build via the CMake switch:
//     cmake -B build -DRAWFORM_BUILD_TESTS=ON
//     cmake --build build --target rawform_apply_test
//     ctest --test-dir build -L ui

#include "metadata/PropertiesApplyController.h"

#include "columns/ColumnSchema.h"
#include "media/TrackData.h"
#include "metadata/ApplyPorts.h"
#include "metadata/MetadataModel.h"
#include "metadata/PropertiesMetadataModel.h"
#include "metadata/ReplayGainRowsModel.h"
#include "playlist/PlaylistModel.h"

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
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
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

// --- the stand-ins --------------------------------------------------------

/// Records every apply; completes only through finish().
class MetaRecorder final : public MetadataWritePort {
public:
    int          applies = 0;
    QVariantList lastEdits;
    QStringList  lastStrip;

    void apply(const QVariantList& edits, const QStringList& stripAllPaths) override {
        ++applies;
        lastEdits = edits;
        lastStrip = stripAllPaths;
    }
    void finish(int okCount, const QStringList& failed) { emit applied(okCount, failed); }
};

class RgRecorder final : public ReplayGainWritePort {
public:
    int          applies = 0;
    QVariantList lastEdits;

    void apply(const QVariantList& edits) override {
        ++applies;
        lastEdits = edits;
    }
    void finish(int okCount, const QStringList& failed) { emit applied(okCount, failed); }
};

/// Hands out tokens from a counter and, like the real reloader, flushes the
/// pending ones from its destructor.
class ReloadRecorder final : public ReloadPort {
public:
    int         requests = 0;
    QStringList lastPaths;
    QList<int>  pending;

    ~ReloadRecorder() override {
        for (const int token : pending)
            emit pathsReloaded(token);
    }
    int reloadPaths(const QStringList& paths) override {
        ++requests;
        lastPaths = paths;
        pending.push_back(++m_counter);
        return m_counter;
    }
    void finish(int token) {
        pending.removeOne(token);
        emit pathsReloaded(token);
    }

private:
    int m_counter = 100;
};

// --- fixtures --------------------------------------------------------------

TrackData track(int row) {
    TrackData t;
    t.filePath = QStringLiteral("/music/%1.flac").arg(row);
    t.fileName = QStringLiteral("%1.flac").arg(row);
    t.title    = QStringLiteral("Track %1").arg(row);
    t.artists  = { QStringLiteral("Artist") };
    return t;
}

std::unique_ptr<PlaylistModel> makeModel(int n) {
    auto m = std::make_unique<PlaylistModel>();
    m->setSchema(ColumnSchema::load());
    m->setCustomColumns(nullptr);
    QList<TrackData> tracks;
    for (int i = 0; i < n; ++i)
        tracks.push_back(track(i));
    m->setTracks(std::move(tracks));
    return m;
}

/// Everything one scenario needs, wired like the window wires it.
struct World {
    std::unique_ptr<PlaylistModel> playlist;
    PropertiesMetadataModel        metaModel;
    ReplayGainRowsModel            rgModel;
    MetadataModel                  detailsModel;
    MetaRecorder                   meta;
    RgRecorder                     rg;
    std::unique_ptr<ReloadRecorder> reloader = std::make_unique<ReloadRecorder>();

    QList<bool> finishes;      // finished(closeRequested), in order
    int         reloadStarts = 0;
    int         flushes      = 0;

    PropertiesApplyController ctl;  // last, so it dies first

    explicit World(int tracks) : playlist(makeModel(tracks)) {
        QObject::connect(&ctl, &PropertiesApplyController::finished, &ctl,
                         [this](bool close) { finishes.push_back(close); });
        QObject::connect(&ctl, &PropertiesApplyController::reloadStarting, &ctl,
                         [this] { ++reloadStarts; });
        QObject::connect(&ctl, &PropertiesApplyController::filesWritten, &ctl,
                         [this] { ++flushes; });
        ctl.setModels(&metaModel, &rgModel, &detailsModel);
        ctl.setEditors(&meta, &rg);
        ctl.setReloader(reloader.get());
        QVariantList rows;
        for (int i = 0; i < tracks; ++i)
            rows.push_back(i);
        ctl.setPlaylist(playlist.get(), rows);
    }

    /// Stage a value on the first editable field of every track.
    void stageField(const QString& value) {
        for (int row = 0; row < metaModel.rowCount(); ++row) {
            if (metaModel.isSectionRow(row) || metaModel.isAddRow(row))
                continue;
            QStringList per;
            for (int i = 0; i < metaModel.selectionCount(); ++i)
                per.push_back(value);
            metaModel.stageFieldValues(metaModel.fieldKey(row), per);
            return;
        }
    }

    /// Stage a track gain on row 0.
    void stageGain() {
        rgModel.commitCell(0, ReplayGainRowsModel::TrackGain, QStringLiteral("-6.00 dB"));
    }

    [[nodiscard]] QString path(int row) const { return track(row).filePath; }
};

QSet<QString> asSet(const QStringList& l) { return QSet<QString>(l.cbegin(), l.cend()); }

// --- scenarios -------------------------------------------------------------

void testMetaOnly() {
    World w(1);
    w.stageField(QStringLiteral("New Artist"));
    check(w.metaModel.dirty(), "meta-only: the field is staged");
    w.ctl.apply(false);
    check(w.ctl.busy(), "meta-only: busy while the write runs");
    check(w.ctl.busyText() == QStringLiteral("Applying…"), "meta-only: busy text");
    check(w.meta.applies == 1 && w.meta.lastStrip.isEmpty(),
          "meta-only: one write, no strip");
    check(w.rg.applies == 0, "meta-only: RG port untouched before the meta phase ends");
    w.meta.finish(1, {});
    check(w.rg.applies == 0, "meta-only: RG phase skipped (nothing staged)");
    check(w.reloader->requests == 1 && w.reloader->lastPaths == QStringList{ w.path(0) },
          "meta-only: exactly the touched path reloads");
    check(w.ctl.busy() && w.finishes.isEmpty(),
          "meta-only: still busy until the reload ends");
    w.reloader->finish(101);
    check(!w.ctl.busy(), "meta-only: idle after the reload");
    check(!w.metaModel.dirty(), "meta-only: the re-pull cleared the staging");
    check(w.flushes == 1, "meta-only: the playlist flush was requested once");
    check(w.finishes == QList<bool>{ false },
          "meta-only: finished without a close request");
    check(w.ctl.statusText().isEmpty(), "meta-only: no status on success");
    check(w.ctl.rowCount() == 1, "meta-only: row count intact");
}

void testRgOnly() {
    World w(1);
    w.stageGain();
    check(w.rgModel.dirty(), "rg-only: the gain is staged");
    w.ctl.apply(true);
    check(w.meta.applies == 0, "rg-only: metadata port skipped");
    check(w.rg.applies == 1 && w.rg.lastEdits.size() == 1, "rg-only: one RG write");
    w.rg.finish(1, {});
    check(w.reloader->requests == 1, "rg-only: reload after the RG phase");
    w.reloader->finish(101);
    check(!w.rgModel.dirty(), "rg-only: RG staging cleared by the re-pull");
    check(w.finishes == QList<bool>{ true }, "rg-only: OK closes on success");
}

void testBoth() {
    World w(2);
    w.stageField(QStringLiteral("Both"));
    w.stageGain();
    w.ctl.apply(false);
    check(w.meta.applies == 1 && w.rg.applies == 0, "both: metadata first");
    w.meta.finish(2, {});
    check(w.rg.applies == 1, "both: then ReplayGain");
    check(w.reloader->requests == 0, "both: no reload before the RG phase ends");
    w.rg.finish(1, {});
    check(w.reloader->requests == 1, "both: one reload");
    check(asSet(w.reloader->lastPaths) == asSet({ w.path(0), w.path(1) }),
          "both: the union of touched paths");
    w.reloader->finish(101);
    check(!w.ctl.busy() && !w.metaModel.dirty() && !w.rgModel.dirty(),
          "both: clean finish");
}

void testNothingStaged() {
    World w(1);
    w.ctl.apply(true);
    check(!w.ctl.busy(), "nothing: OK with nothing staged never goes busy");
    check(w.finishes == QList<bool>{ true }, "nothing: OK finishes with a close request");
    check(w.meta.applies == 0 && w.rg.applies == 0 && w.reloader->requests == 0,
          "nothing: no port touched");
    w.ctl.apply(false);
    check(w.finishes.size() == 1, "nothing: Apply with nothing staged does nothing");
}

void testStrip() {
    World w(2);
    w.ctl.stageStripAll();
    check(w.ctl.stripStaged(), "strip: staged");
    check(w.ctl.statusLevel() == QStringLiteral("warning")
              && !w.ctl.statusText().isEmpty(),
          "strip: a warning status confirms the intent");
    w.ctl.apply(false);
    check(w.meta.applies == 1, "strip: the metadata port runs even with no field edit");
    check(asSet(w.meta.lastStrip) == asSet({ w.path(0), w.path(1) }),
          "strip: every selection path is stripped");
    check(w.ctl.stripStaged(), "strip: the flag holds while the pass runs");
    w.meta.finish(2, {});
    check(w.reloader->requests == 1
              && asSet(w.reloader->lastPaths) == asSet({ w.path(0), w.path(1) }),
          "strip: stripped paths are touched paths");
    w.reloader->finish(101);
    check(!w.ctl.stripStaged(), "strip: the flag clears with the pass");
    check(w.ctl.statusText().isEmpty(), "strip: the staging warning cleared on apply");
}

void testTotalFailure() {
    World w(1);
    w.stageField(QStringLiteral("Fails"));
    w.ctl.apply(true);
    w.meta.finish(0, { w.path(0) });
    check(w.reloader->requests == 0, "total failure: no reload when nothing landed");
    check(!w.ctl.busy(), "total failure: the pass finished");
    check(w.metaModel.dirty(), "total failure: the staging survives for a retry");
    check(w.flushes == 0, "total failure: no playlist flush");
    check(w.ctl.statusText() == QStringLiteral("1 file(s) could not be written.")
              && w.ctl.statusLevel() == QStringLiteral("error"),
          "total failure: the status counts it");
    check(w.finishes == QList<bool>{ false }, "total failure: OK does not close");
}

void testPartialFailure() {
    World w(2);
    w.stageField(QStringLiteral("Partial"));
    w.ctl.apply(true);
    w.meta.finish(1, { w.path(1) });
    check(w.reloader->requests == 1, "partial: the reload runs for what landed");
    check(asSet(w.reloader->lastPaths) == asSet({ w.path(0), w.path(1) }),
          "partial: the reload covers every touched path");
    w.reloader->finish(101);
    check(w.ctl.statusText() == QStringLiteral("1 file(s) could not be written."),
          "partial: one failure reported");
    check(w.finishes == QList<bool>{ false }, "partial: OK stays open");
    check(!w.ctl.busy(), "partial: idle");
}

void testReloaderDead() {
    {
        World w(1);
        w.stageField(QStringLiteral("Gone"));
        w.ctl.apply(false);
        w.reloader.reset(); // the owning tab closed during the write
        w.meta.finish(1, {});
        check(!w.ctl.busy(), "reloader dead before reload: the pass finishes");
        check(!w.metaModel.dirty(), "reloader dead before reload: the models re-pull");
        check(w.finishes == QList<bool>{ false },
              "reloader dead before reload: finished");
    }
    {
        World w(1);
        w.stageField(QStringLiteral("Mid"));
        w.ctl.apply(false);
        w.meta.finish(1, {});
        check(w.ctl.busy() && w.reloader->pending.size() == 1,
              "reloader dead mid-pass: waiting");
        w.reloader.reset(); // the destructor flush completes the token
        check(!w.ctl.busy(), "reloader dead mid-pass: the flush finishes the pass");
        check(w.finishes == QList<bool>{ false }, "reloader dead mid-pass: finished");
    }
    {
        World w(1);
        w.reloader.reset();
        w.ctl.reloadInfo();
        check(!w.ctl.busy(), "reload info without a reloader: not busy");
        check(w.ctl.statusText()
                      == QStringLiteral("Owning playlist is gone; nothing to reload.")
                  && w.ctl.statusLevel() == QStringLiteral("warning"),
              "reload info without a reloader: the warning status");
        check(w.reloadStarts == 0, "reload info without a reloader: never announced");
    }
}

void testModelDeadAtFinish() {
    World w(1);
    w.stageField(QStringLiteral("Orphan"));
    w.stageGain();
    w.ctl.apply(false);
    w.meta.finish(1, {});
    w.rg.finish(1, {});
    check(w.ctl.busy(), "model dead: waiting on the reload");
    w.playlist.reset(); // the owning tab closed while the reload ran
    w.reloader->finish(101);
    check(!w.ctl.busy(), "model dead: the pass finishes");
    check(!w.metaModel.dirty() && !w.rgModel.dirty(),
          "model dead: the models adopted the edits");
    check(w.flushes == 1, "model dead: the flush still runs (the files were written)");
    check(w.ctl.statusText().isEmpty(), "model dead: no status");
    check(w.ctl.rowCount() == 1, "model dead: the open-time rows stand");
}

void testReloadInfo() {
    World w(2);
    w.stageField(QStringLiteral("Discarded"));
    w.ctl.stageStripAll();
    w.ctl.reloadInfo();
    check(w.reloadStarts == 1, "reload info: announced once");
    check(!w.ctl.stripStaged(), "reload info: the staged strip is discarded");
    check(w.ctl.busy() && w.ctl.busyText() == QStringLiteral("Reloading…"),
          "reload info: busy with the reload text");
    check(w.ctl.statusText().isEmpty(), "reload info: the strip warning cleared");
    check(w.reloader->requests == 1
              && asSet(w.reloader->lastPaths) == asSet({ w.path(0), w.path(1) }),
          "reload info: every selection path");
    w.reloader->pending.push_back(999);
    w.reloader->finish(999);
    check(w.ctl.busy(), "reload info: a foreign token does not finish the pass");
    w.ctl.reloadInfo();
    check(w.reloader->requests == 1, "reload info: no second pass while busy");
    w.ctl.apply(false);
    check(w.meta.applies == 0, "reload info: apply is held off while busy");
    w.reloader->finish(101);
    check(!w.ctl.busy(), "reload info: idle after its own token");
    check(!w.metaModel.dirty(), "reload info: the re-pull discarded the staged field");
    check(w.finishes.isEmpty(), "reload info: not an apply, no finished()");
}

}  // namespace

int main() {
    testMetaOnly();
    testRgOnly();
    testBoth();
    testNothingStaged();
    testStrip();
    testTotalFailure();
    testPartialFailure();
    testReloaderDead();
    testModelDeadAtFinish();
    testReloadInfo();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
