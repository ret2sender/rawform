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

// NamedPatternStoreTest.cpp
//
// The preset store over real files in a QTemporaryDir, with its writes refused
// two ways: a read-only config folder, and a regular file where the folder
// should be. The first is what a person meets; the second refuses every user,
// root included, so the checks still run in a container where permissions do
// not bind. A refused change must leave the store as it was, report the failure
// on every attempt, and reach the file once the folder accepts writes again.
// Same harness shape as the other ui tests (plain main, counted checks,
// non-zero exit on failure). No QCoreApplication: the connections are direct.

#include "presets/NamedPatternStore.h"
#include "rename/RenamePatternStore.h"
#include "search/SearchPresetStore.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal> // qputenv

#include <cstdio>

using rawform::NamedPatternStore;
using rawform::RenamePatternStore;
using rawform::SearchPresetStore;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,         \
                         __FILE__, __LINE__);                                  \
        }                                                                      \
    } while (0)

/// What the store emitted, as a dialog would see it.
struct Emitted {
    int failed = 0;
    int changed = 0;
    QString lastMessage;
};

void watch(NamedPatternStore& store, Emitted& emitted) {
    QObject::connect(&store, &NamedPatternStore::persistFailed, &store,
                     [&emitted](const QString& message) {
                         ++emitted.failed;
                         emitted.lastMessage = message;
                     });
    QObject::connect(&store, &NamedPatternStore::patternsChanged, &store,
                     [&emitted] { ++emitted.changed; });
}

/// The preset names in dropdown order.
QStringList names(const NamedPatternStore& store) {
    QStringList out;
    for (const QVariant& v : store.catalog()) {
        out.append(v.toMap().value(QStringLiteral("name")).toString());
    }
    return out;
}

enum class Refusal { ReadOnlyFolder, BlockedPath };

const char* label(Refusal refusal) {
    switch (refusal) {
    case Refusal::ReadOnlyFolder:
        return "read-only folder";
    case Refusal::BlockedPath:
        return "blocked path";
    }
    return "";
}

QString asidePath(const QString& configDir) {
    return configDir + QStringLiteral(".aside");
}

/// Make writes under @p configDir fail. False when they still succeed: a
/// read-only folder does not stop a user the permissions do not bind.
bool refuse(Refusal refusal, const QString& configDir) {
    switch (refusal) {
    case Refusal::ReadOnlyFolder: {
        if (!QDir().mkpath(configDir)
            || !QFile::setPermissions(configDir,
                                      QFileDevice::ReadOwner | QFileDevice::ExeOwner)) {
            return false;
        }
        QFile probe(configDir + QStringLiteral("/probe"));
        if (probe.open(QIODevice::WriteOnly)) {
            probe.close();
            probe.remove();
            return false;
        }
        return true;
    }
    case Refusal::BlockedPath: {
        // The folder steps aside rather than going away, so what was on disk
        // before the refusal is still there after it.
        if (QFileInfo::exists(configDir)
            && !QDir().rename(configDir, asidePath(configDir))) {
            return false;
        }
        QFile blocker(configDir);
        if (!blocker.open(QIODevice::WriteOnly)) {
            return false;
        }
        blocker.close();
        return QFileInfo(configDir).isFile();
    }
    }
    return false;
}

/// Undo refuse(), whether or not it took.
void allow(Refusal refusal, const QString& configDir) {
    switch (refusal) {
    case Refusal::ReadOnlyFolder:
        QFile::setPermissions(configDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                             | QFileDevice::ExeOwner);
        return;
    case Refusal::BlockedPath:
        if (QFileInfo(configDir).isFile()) {
            QFile::remove(configDir);
        }
        if (QFileInfo::exists(asidePath(configDir))) {
            QDir().rename(asidePath(configDir), configDir);
        }
        return;
    }
}

/// Point userConfigDir() at @p base; every store built after this reads and
/// writes under base/rawform.
QString useConfigHome(const QString& base) {
    qputenv("XDG_CONFIG_HOME", QFile::encodeName(base));
    return base + QStringLiteral("/rawform");
}

// The finding's case and its neighbors: while writes are refused, a save, the
// same save again, a remove and a last-used change each report the failure
// and leave the store as it was; the identical preset already on disk is no
// write and no failure. Once writes are accepted, the save that failed writes,
// and a fresh store (a newly opened dialog) reads what the first one shows.
void testRefusedChanges(Refusal refusal) {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    if (!tmp.isValid()) {
        return;
    }
    const QString configDir = useConfigHome(tmp.path());

    Emitted emitted;
    RenamePatternStore store;
    watch(store, emitted);

    CHECK(store.save(QStringLiteral("keep"), QStringLiteral("%title%")));
    CHECK(emitted.failed == 0);
    CHECK(names(store) == QStringList{ QStringLiteral("keep") });

    if (!refuse(refusal, configDir)) {
        allow(refusal, configDir);
        if (refusal == Refusal::ReadOnlyFolder) {
            std::printf("SKIP: %s (permissions not enforced for this user)\n",
                        label(refusal));
        } else {
            std::fprintf(stderr, "could not set up the %s\n", label(refusal));
            CHECK(false);
        }
        return;
    }
    emitted.changed = 0;

    CHECK(!store.save(QStringLiteral("sweep"), QStringLiteral("%artist%")));
    CHECK(emitted.failed == 1);
    CHECK(emitted.changed == 0);
    CHECK(names(store) == QStringList{ QStringLiteral("keep") });

    CHECK(!store.save(QStringLiteral("sweep"), QStringLiteral("%artist%")));
    CHECK(emitted.failed == 2);
    CHECK(names(store) == QStringList{ QStringLiteral("keep") });

    CHECK(!store.remove(QStringLiteral("keep")));
    CHECK(emitted.failed == 3);
    CHECK(emitted.changed == 0);
    CHECK(names(store) == QStringList{ QStringLiteral("keep") });

    CHECK(!store.setLastUsed(QStringLiteral("sweep")));
    CHECK(emitted.failed == 4);
    CHECK(store.lastUsed().isEmpty());

    CHECK(store.save(QStringLiteral("keep"), QStringLiteral("%title%")));
    CHECK(emitted.failed == 4);

    allow(refusal, configDir);
    {
        const RenamePatternStore fresh;
        CHECK(names(fresh) == QStringList{ QStringLiteral("keep") });
        CHECK(fresh.lastUsed().isEmpty());
    }

    CHECK(store.save(QStringLiteral("sweep"), QStringLiteral("%artist%")));
    CHECK(store.setLastUsed(QStringLiteral("sweep")));
    CHECK(emitted.failed == 4);
    CHECK(emitted.changed == 1);
    {
        const RenamePatternStore fresh;
        CHECK(names(fresh)
              == (QStringList{ QStringLiteral("keep"), QStringLiteral("sweep") }));
        CHECK(fresh.patternFor(QStringLiteral("sweep")) == QStringLiteral("%artist%"));
        CHECK(fresh.lastUsed() == QStringLiteral("sweep"));
    }
}

// The Find binding reports a refused write under its own noun and file.
void testSearchStoreNamesItsFile() {
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    if (!tmp.isValid()) {
        return;
    }
    const QString configDir = useConfigHome(tmp.path());
    CHECK(refuse(Refusal::BlockedPath, configDir));

    Emitted emitted;
    SearchPresetStore store;
    watch(store, emitted);

    CHECK(!store.save(QStringLiteral("sweep"), QStringLiteral("tone")));
    CHECK(emitted.failed == 1);
    CHECK(emitted.lastMessage.contains(QStringLiteral("Find presets")));
    CHECK(emitted.lastMessage.contains(QStringLiteral("search_filters.yaml")));
    CHECK(names(store).isEmpty());

    allow(Refusal::BlockedPath, configDir);
}

}  // namespace

int main() {
    testRefusedChanges(Refusal::ReadOnlyFolder);
    testRefusedChanges(Refusal::BlockedPath);
    testSearchStoreNamesItsFile();

    if (g_failures == 0) {
        std::puts("NamedPatternStoreTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "NamedPatternStoreTest: %d check(s) failed\n", g_failures);
    return 1;
}
