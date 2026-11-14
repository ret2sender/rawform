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

// SingleInstanceTest.cpp
//
// The single-instance guard over a real lock file and a real local socket in
// a QTemporaryDir: who becomes the holder, what a newcomer hands over, which
// lock files are taken over and which are left alone, and what happens when
// the configuration directory is missing or refuses the lock.
//
// One process plays every part. A holder answers from the event loop of the
// main thread, as it does in the application, and a newcomer blocks in
// start(), so each newcomer runs on a second thread while the main thread
// spins the loop. A lock held by "another process" is a QLockFile the test
// holds itself: Qt keeps a separate advisory lock per open file, so it
// excludes a guard in the same process exactly as it would across processes.
//
// The socket cases need a real AF_UNIX socket and fail, on purpose, where
// the system refuses to create one: a skipped hand-off would report a
// protocol that never ran as passing.
//
// Same harness shape as the other ui tests (plain main, counted checks,
// non-zero exit on failure).

#include "utils/SingleInstance.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtGlobal> // qInstallMessageHandler

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

using rawform::SingleInstance;
using Role = rawform::SingleInstance::Role;
using namespace std::chrono_literals;

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

// Warnings are counted wherever they are raised, a newcomer's thread
// included, and still reach the log.
std::atomic<int> g_warnings{0};
QtMessageHandler g_previousHandler = nullptr;

void countWarnings(QtMsgType type, const QMessageLogContext& context,
                   const QString& message) {
    if (type == QtWarningMsg) {
        g_warnings.fetch_add(1);
    }
    if (g_previousHandler != nullptr) {
        g_previousHandler(type, context, message);
    }
}

/// Waits short enough that a case which must run a wait to its end stays
/// quick, long enough that an immediate outcome is told apart from one.
constexpr SingleInstance::Timeouts kQuick{.connectWindow = 400ms, .ackWait = 400ms};

void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename Predicate>
bool spinUntil(Predicate done, int maxMs) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > maxMs) {
            return false;
        }
        spin(5);
    }
    return true;
}

/// A temporary directory with a short name: the socket path has to fit a
/// socket address, and the default template spends 28 bytes on the name of
/// this binary under a temporary root that is already long on macOS.
struct Scratch {
    QTemporaryDir dir{QDir::tempPath() + QStringLiteral("/rwf-XXXXXX")};

    [[nodiscard]] QString path(const QString& name) const {
        return dir.path() + QLatin1Char('/') + name;
    }
    [[nodiscard]] QString lock() const { return path(QStringLiteral("config")); }
    [[nodiscard]] QString run() const { return path(QStringLiteral("run")); }
};

QString lockFileIn(const QString& lockDir) {
    return lockDir + QStringLiteral("/instance.lock");
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/// What a holder was handed.
struct Received {
    int         batches = 0;
    QList<QUrl> last;
};

void watch(SingleInstance& holder, Received& received) {
    QObject::connect(&holder, &SingleInstance::filesReceived, &holder,
                     [&received](const QList<QUrl>& files) {
                         ++received.batches;
                         received.last = files;
                     });
}

struct Outcome {
    Role   role = Role::Failed;
    qint64 elapsedMs = 0;
};

/// A launch on a second thread, the main thread's loop running meanwhile.
/// The guard lives and dies on that thread, so a lock it took is released
/// when this returns. A QThread, not a bare thread: Qt's sockets want the
/// event dispatcher a QThread brings, even where no loop runs.
Outcome launch(const QString& lockDir, const QString& socketDir,
               const QList<QUrl>& files, const SingleInstance::Timeouts& timeouts) {
    std::atomic<Role>   role{Role::Failed};
    std::atomic<qint64> elapsedMs{0};
    std::atomic<bool>   done{false};

    const std::unique_ptr<QThread> worker(QThread::create([&] {
        QElapsedTimer timer;
        timer.start();
        {
            SingleInstance guard(lockDir, socketDir);
            guard.setTimeouts(timeouts);
            role.store(guard.start(files));
            elapsedMs.store(timer.elapsed());
        }
        done.store(true);
    }));
    worker->start();
    while (!done.load()) {
        spin(5);
    }
    worker->wait();
    return {.role = role.load(), .elapsedMs = elapsedMs.load()};
}

/// The bytes Qt writes into a lock file for this process, taken from a real
/// lock so the test knows nothing of the format beyond its first line.
QByteArray genuineLockBytes(const QString& lockPath) {
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0ms);
    if (!lock.tryLock(0ms)) {
        return {};
    }
    QByteArray bytes;
    QFile      file(lockPath);
    if (file.open(QIODevice::ReadOnly)) {
        bytes = file.readAll();
    }
    lock.unlock();
    return bytes;
}

// The first guard on a configuration is the holder, with its lock file and
// its socket in place for as long as it lives and gone afterwards.
void testFirstIsHolder() {
    const Scratch scratch;
    QString       lockPath;
    QString       socketPath;
    {
        SingleInstance holder(scratch.lock(), scratch.run());
        CHECK(holder.start({}) == Role::Holder);
        lockPath = holder.lockPath();
        socketPath = holder.socketPath();
        CHECK(lockPath == lockFileIn(scratch.lock()));
        CHECK(socketPath.startsWith(scratch.run() + QStringLiteral("/instance-")));
        CHECK(QFileInfo::exists(lockPath));
        CHECK(QFileInfo::exists(socketPath));
    }
    CHECK(!QFileInfo::exists(lockPath));
    CHECK(!QFileInfo::exists(socketPath));
}

// A second launch hands its files over and the holder receives exactly them,
// a space and a newline in a name included; a launch with no file still
// arrives, as an empty batch.
void testForward() {
    const Scratch  scratch;
    SingleInstance holder(scratch.lock(), scratch.run());
    Received       received;
    watch(holder, received);
    CHECK(holder.start({}) == Role::Holder);

    const QList<QUrl> files{
        QUrl::fromLocalFile(scratch.path(QStringLiteral("plain.flac"))),
        QUrl::fromLocalFile(scratch.path(QStringLiteral("with space.flac"))),
        QUrl::fromLocalFile(scratch.path(QStringLiteral("line\nbreak.flac"))),
    };
    Outcome outcome = launch(scratch.lock(), scratch.run(), files, {});
    CHECK(outcome.role == Role::Forwarded);
    CHECK(received.batches == 1);
    CHECK(received.last == files);
    CHECK(received.last.size() == 3
          && received.last.at(2).toLocalFile()
                 == scratch.path(QStringLiteral("line\nbreak.flac")));

    outcome = launch(scratch.lock(), scratch.run(), {}, {});
    CHECK(outcome.role == Role::Forwarded);
    CHECK(received.batches == 2);
    CHECK(received.last.isEmpty());
}

/// Connect a bare client, send @p bytes, and report whether the holder closed
/// the connection without a word.
bool closedUnanswered(const QString& socketPath, const QByteArray& bytes) {
    QLocalSocket client;
    client.connectToServer(socketPath);
    if (!spinUntil([&] { return client.state() == QLocalSocket::ConnectedState; },
                   2000)) {
        return false;
    }
    client.write(bytes);
    const bool closed = spinUntil(
        [&] { return client.state() == QLocalSocket::UnconnectedState; }, 5000);
    return closed && client.readAll().isEmpty();
}

// A peer that does not speak the protocol, or sends more than a batch may
// hold, is closed without an answer and without a signal; the holder goes on
// serving.
void testRefusedBatches() {
    const Scratch  scratch;
    SingleInstance holder(scratch.lock(), scratch.run());
    Received       received;
    watch(holder, received);
    CHECK(holder.start({}) == Role::Holder);

    CHECK(closedUnanswered(holder.socketPath(),
                           QByteArrayLiteral("rawform-shut 1\nfile:///x.flac\n\n")));
    CHECK(received.batches == 0);

    const QByteArray oversized =
        QByteArrayLiteral("rawform-open 1\n") + QByteArray(8 * 1024 * 1024 + 4096, 'x');
    CHECK(closedUnanswered(holder.socketPath(), oversized));
    CHECK(received.batches == 0);

    const Outcome outcome = launch(scratch.lock(), scratch.run(), {}, {});
    CHECK(outcome.role == Role::Forwarded);
    CHECK(received.batches == 1);
}

// A lock file nobody holds is taken over. One that names a dead process goes
// at once, inside the lock attempt. One that names a live process of the
// same name, which is what a crashed holder leaves to a launch in another
// PID namespace, goes once the socket has stayed silent for the whole window.
void testAbandonedLocks() {
    const Scratch scratch;
    CHECK(QDir().mkpath(scratch.lock()));
    const QString    lockPath = lockFileIn(scratch.lock());
    const QByteArray genuine = genuineLockBytes(lockPath);
    CHECK(!genuine.isEmpty());
    CHECK(!QFileInfo::exists(lockPath));

    // No system hands out a process id this large.
    const QByteArray dead =
        QByteArrayLiteral("2000000000") + genuine.mid(genuine.indexOf('\n'));
    CHECK(writeFile(lockPath, dead));
    Outcome outcome = launch(scratch.lock(), scratch.run(), {}, kQuick);
    CHECK(outcome.role == Role::Holder);
    CHECK(outcome.elapsedMs < kQuick.connectWindow.count());

    CHECK(writeFile(lockPath, genuine));
    outcome = launch(scratch.lock(), scratch.run(), {}, kQuick);
    CHECK(outcome.role == Role::Holder);
    CHECK(outcome.elapsedMs >= kQuick.connectWindow.count());
}

// A live holder keeps its lock whatever the file's age: a lock held for an
// hour still receives the next launch's files.
void testOldLockOfLiveHolder() {
    const Scratch  scratch;
    SingleInstance holder(scratch.lock(), scratch.run());
    Received       received;
    watch(holder, received);
    CHECK(holder.start({}) == Role::Holder);

    const QDateTime anHourAgo = QDateTime::currentDateTimeUtc().addSecs(-3600);
    {
        QFile lockFile(holder.lockPath());
        CHECK(lockFile.open(QIODevice::ReadOnly)
              && lockFile.setFileTime(anHourAgo, QFileDevice::FileModificationTime));
    }
    CHECK(QFileInfo(holder.lockPath()).lastModified() <= anHourAgo.addSecs(60));

    const QList<QUrl> files{QUrl::fromLocalFile(scratch.path(QStringLiteral("a.flac")))};
    const Outcome outcome = launch(scratch.lock(), scratch.run(), files, {});
    CHECK(outcome.role == Role::Forwarded);
    CHECK(received.batches == 1);
    CHECK(received.last == files);
    CHECK(QFileInfo::exists(holder.lockPath()));
}

// A live process that holds the lock and does not answer is never displaced:
// the launch fails with one message and the lock stays where it is. Mute two
// ways: nothing listens, or something accepts the connection and says nothing.
void testMuteHolder() {
    const Scratch scratch;
    CHECK(QDir().mkpath(scratch.lock()));
    CHECK(QDir().mkpath(scratch.run()));
    const QString lockPath = lockFileIn(scratch.lock());

    QLockFile held(lockPath);
    held.setStaleLockTime(0ms);
    CHECK(held.tryLock(0ms));

    int     warningsBefore = g_warnings.load();
    Outcome outcome = launch(scratch.lock(), scratch.run(), {}, kQuick);
    CHECK(outcome.role == Role::Failed);
    CHECK(outcome.elapsedMs >= kQuick.connectWindow.count());
    CHECK(g_warnings.load() == warningsBefore + 1);
    CHECK(held.isLocked());
    CHECK(QFileInfo::exists(lockPath));

    QLocalServer silent;
    CHECK(silent.listen(SingleInstance(scratch.lock(), scratch.run()).socketPath()));
    warningsBefore = g_warnings.load();
    outcome = launch(scratch.lock(), scratch.run(), {}, kQuick);
    CHECK(outcome.role == Role::Failed);
    CHECK(outcome.elapsedMs >= kQuick.ackWait.count());
    CHECK(g_warnings.load() == warningsBefore + 1);
    CHECK(silent.hasPendingConnections());
    CHECK(held.isLocked());
    CHECK(QFileInfo::exists(lockPath));
}

// A first launch finds neither directory: both are created, the socket's one
// private to the user.
void testCreatesItsDirectories() {
    const Scratch scratch;
    const QString lockDir = scratch.path(QStringLiteral("fresh/config"));
    const QString socketDir = scratch.path(QStringLiteral("fresh/run"));
    CHECK(!QFileInfo::exists(lockDir));

    SingleInstance holder(lockDir, socketDir);
    CHECK(holder.start({}) == Role::Holder);
    CHECK(QFileInfo(lockDir).isDir());
    CHECK(QFileInfo(socketDir).isDir());
    CHECK(QFileInfo::exists(holder.lockPath()));
    CHECK(QFileInfo::exists(holder.socketPath()));

    const QFileDevice::Permissions shared =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
        | QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
    CHECK(!(QFileInfo(socketDir).permissions() & shared));
}

enum class Refusal { BlockedPath, ReadOnlyFolder };

/// Make the lock impossible to take in @p lockDir. False when it still can
/// be: a read-only folder does not stop a user the permissions do not bind.
bool refuse(Refusal refusal, const QString& lockDir) {
    switch (refusal) {
    case Refusal::BlockedPath:
        // A regular file where the directory should be refuses every user.
        return writeFile(lockDir, QByteArray()) && QFileInfo(lockDir).isFile();
    case Refusal::ReadOnlyFolder: {
        if (!QDir().mkpath(lockDir)
            || !QFile::setPermissions(lockDir,
                                      QFileDevice::ReadOwner | QFileDevice::ExeOwner)) {
            return false;
        }
        QFile probe(lockDir + QStringLiteral("/probe"));
        if (probe.open(QIODevice::WriteOnly)) {
            probe.close();
            probe.remove();
            return false;
        }
        return true;
    }
    }
    return false;
}

// A configuration directory that cannot hold the lock does not keep the
// application from starting: the guard reports itself the holder, says so
// once, and leaves no lock and no socket behind.
void testRefusedLock(Refusal refusal) {
    const Scratch scratch;
    const QString lockDir = scratch.lock();
    if (!refuse(refusal, lockDir)) {
        std::puts("SingleInstanceTest: a read-only folder does not bind this user; "
                  "that form is not asserted");
    } else {
        const int      warningsBefore = g_warnings.load();
        SingleInstance guard(lockDir, scratch.run());
        CHECK(guard.start({}) == Role::Holder);
        CHECK(g_warnings.load() == warningsBefore + 1);
        CHECK(!QFileInfo::exists(guard.lockPath()));
        CHECK(QDir(scratch.run())
                  .entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)
                  .isEmpty());
    }
    if (refusal == Refusal::ReadOnlyFolder) {
        QFile::setPermissions(lockDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                           | QFileDevice::ExeOwner);
    }
}

// Two configurations sharing one runtime directory are two instances: each
// has its own socket, the second does not remove the first one's, and a
// launch reaches the holder of its own configuration only.
void testSocketPerConfiguration() {
    const Scratch scratch;
    const QString lockA = scratch.path(QStringLiteral("a"));
    const QString lockB = scratch.path(QStringLiteral("b"));

    SingleInstance holderA(lockA, scratch.run());
    SingleInstance holderB(lockB, scratch.run());
    Received       receivedA;
    Received       receivedB;
    watch(holderA, receivedA);
    watch(holderB, receivedB);
    CHECK(holderA.start({}) == Role::Holder);
    CHECK(holderB.start({}) == Role::Holder);
    CHECK(holderA.socketPath() != holderB.socketPath());
    CHECK(QFileInfo::exists(holderA.socketPath()));
    CHECK(QFileInfo::exists(holderB.socketPath()));

    const QList<QUrl> forA{QUrl::fromLocalFile(scratch.path(QStringLiteral("a.flac")))};
    const QList<QUrl> forB{QUrl::fromLocalFile(scratch.path(QStringLiteral("b.flac")))};

    Outcome outcome = launch(lockA, scratch.run(), forA, {});
    CHECK(outcome.role == Role::Forwarded);
    CHECK(receivedA.batches == 1);
    CHECK(receivedA.last == forA);
    CHECK(receivedB.batches == 0);

    outcome = launch(lockB, scratch.run(), forB, {});
    CHECK(outcome.role == Role::Forwarded);
    CHECK(receivedB.batches == 1);
    CHECK(receivedB.last == forB);
    CHECK(receivedA.batches == 1);
}

// The command line as a launcher or a person writes it: options are not
// files, a file: URI and a relative path both resolve, and a path that does
// not exist is dropped with one warning.
void testArguments() {
    const Scratch scratch;
    const QString base = scratch.dir.path();
    CHECK(QDir().mkpath(scratch.path(QStringLiteral("sub"))));
    const QString spaced = scratch.path(QStringLiteral("with space.flac"));
    const QString relative = scratch.path(QStringLiteral("rel.flac"));
    const QString nested = scratch.path(QStringLiteral("sub/deep.flac"));
    const QString broken = scratch.path(QStringLiteral("line\nbreak.flac"));
    for (const QString& path : {spaced, relative, nested, broken}) {
        CHECK(writeFile(path, QByteArrayLiteral("x")));
    }

    const QStringList args{
        QStringLiteral("-platform"),
        QStringLiteral("--"),
        QString::fromLatin1(QUrl::fromLocalFile(spaced).toEncoded()),
        QStringLiteral("rel.flac"),
        QStringLiteral("./sub/../sub/deep.flac"),
        broken,
        QStringLiteral("missing.flac"),
        QString(),
    };
    const QList<QUrl> expected{
        QUrl::fromLocalFile(spaced),
        QUrl::fromLocalFile(relative),
        QUrl::fromLocalFile(nested),
        QUrl::fromLocalFile(broken),
    };

    const int         warningsBefore = g_warnings.load();
    const QList<QUrl> urls = rawform::fileUrlsFromArguments(args, base);
    CHECK(urls == expected);
    CHECK(g_warnings.load() == warningsBefore + 1);
}

} // namespace

int main(int argc, char** argv) {
    const QCoreApplication app(argc, argv);
    g_previousHandler = qInstallMessageHandler(countWarnings);

    testFirstIsHolder();
    testForward();
    testRefusedBatches();
    testAbandonedLocks();
    testOldLockOfLiveHolder();
    testMuteHolder();
    testCreatesItsDirectories();
    testRefusedLock(Refusal::BlockedPath);
    testRefusedLock(Refusal::ReadOnlyFolder);
    testSocketPerConfiguration();
    testArguments();

    if (g_failures == 0) {
        std::puts("SingleInstanceTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "SingleInstanceTest: %d check(s) failed\n", g_failures);
    return 1;
}
