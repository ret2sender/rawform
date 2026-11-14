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

// SingleInstance.cpp
//
// Implementation of the single-instance guard declared in SingleInstance.h.

#include "utils/SingleInstance.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QOperatingSystemVersion>
#include <QPointer>
#include <QStandardPaths>
#include <QThread>
#include <QtGlobal> // qEnvironmentVariable, qWarning

#include <algorithm>

#include <sys/un.h> // sockaddr_un, for the length a socket path may have

using namespace std::chrono_literals;

namespace rawform {

namespace {

constexpr QByteArrayView kHeader("rawform-open 1");
constexpr QByteArrayView kAck("ok\n");

/// A batch larger than this is refused unread: nothing a launcher passes on
/// a command line comes close, and the holder must not buffer without bound
/// for whatever connects to its socket.
constexpr qsizetype kMaxBatchBytes = 1024 * 1024;

/// The pause between two attempts at the lock and the socket.
constexpr std::chrono::milliseconds kRetryStep = 100ms;

/// One socket per configuration: the name carries a digest of the lock
/// directory, so a second configuration sharing the runtime directory neither
/// answers for the first nor removes its socket.
QString deriveSocketPath(const QString& lockDir, const QString& socketDir) {
    if (socketDir.isEmpty()) {
        return {};
    }
    QString identity = QDir(lockDir).canonicalPath();
    if (identity.isEmpty()) {
        identity = QDir(lockDir).absolutePath();
    }
    const QByteArray digest =
        QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha1).toHex();
    return socketDir + QLatin1String("/instance-") + QLatin1String(digest.first(8))
           + QLatin1String(".sock");
}

/// The bytes a socket path may occupy, terminator excluded.
qsizetype socketPathLimit() {
    return static_cast<qsizetype>(sizeof(sockaddr_un::sun_path)) - 1;
}

qsizetype socketPathBytes(const QString& path) {
    return QFile::encodeName(path).size();
}

const char* describe(QLockFile::LockError error) {
    switch (error) {
    case QLockFile::NoError:
        return "no error";
    case QLockFile::LockFailedError:
        return "held by another process";
    case QLockFile::PermissionError:
        return "permission denied";
    case QLockFile::UnknownError:
        return "the directory cannot hold it";
    }
    return "";
}

} // namespace

QString defaultSocketDir() {
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtime.startsWith(QLatin1Char('/'))) {
        const QString flatpakId = qEnvironmentVariable("FLATPAK_ID");
        if (!flatpakId.isEmpty()) {
            return runtime + QLatin1String("/app/") + flatpakId;
        }
        return runtime + QLatin1String("/rawform");
    }
    if (QOperatingSystemVersion::currentType() == QOperatingSystemVersion::MacOS) {
        return QDir::tempPath() + QLatin1String("/rawform");
    }
    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (fallback.isEmpty()) {
        return {};
    }
    return fallback + QLatin1String("/rawform");
}

QList<QUrl> fileUrlsFromArguments(const QStringList& args, const QString& workingDir) {
    QList<QUrl> urls;
    const QDir base(workingDir);
    for (const QString& arg : args) {
        if (arg.isEmpty() || arg.startsWith(QLatin1Char('-'))) {
            continue;
        }
        const QString named = arg.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)
                                  ? QUrl(arg).toLocalFile()
                                  : arg;
        const QString path = QDir::cleanPath(base.absoluteFilePath(named));
        if (named.isEmpty() || !QFileInfo::exists(path)) {
            qWarning("rawform: no such file: %s", qUtf8Printable(arg));
            continue;
        }
        urls.append(QUrl::fromLocalFile(path));
    }
    return urls;
}

SingleInstance::SingleInstance(const QString& lockDir, const QString& socketDir,
                               QObject* parent)
    : QObject(parent)
    , m_lockDir(QDir::cleanPath(lockDir))
    , m_socketDir(socketDir.isEmpty() ? QString() : QDir::cleanPath(socketDir)) {
}

SingleInstance::~SingleInstance() {
    if (m_server) {
        // Open connections are the server's children and close with it; what
        // they signal on the way out has no guard left to reach.
        const QList<QLocalSocket*> sockets = m_server->findChildren<QLocalSocket*>();
        for (QLocalSocket* socket : sockets) {
            socket->disconnect(this);
        }
        m_server.reset();
    }
}

void SingleInstance::setTimeouts(const Timeouts& timeouts) {
    m_timeouts = timeouts;
}

QString SingleInstance::socketPath() const {
    return m_socketPath.isEmpty() ? deriveSocketPath(m_lockDir, m_socketDir)
                                  : m_socketPath;
}

QString SingleInstance::lockPath() const {
    return m_lockDir + QLatin1String("/instance.lock");
}

SingleInstance::Role SingleInstance::start(const QList<QUrl>& files) {
    ensureDirectories();
    m_socketPath = deriveSocketPath(m_lockDir, m_socketDir);

    m_lock = std::make_unique<QLockFile>(lockPath());
    // The lock is held for as long as the application runs, so its age says
    // nothing. Staleness is decided below, by whether the file can be removed.
    m_lock->setStaleLockTime(0ms);

    // The lock and the socket are tried together for the whole window. A
    // holder that is quitting still owns both for a moment and answers
    // neither: going back to the lock is what lets this launch take its
    // place instead of giving up on it.
    const QDeadlineTimer window(m_timeouts.connectWindow);
    bool retry = true;
    while (retry) {
        if (m_lock->tryLock(0ms)) {
            return becomeHolder();
        }
        if (m_lock->error() != QLockFile::LockFailedError) {
            qWarning("rawform: cannot take the instance lock %s (%s); starting without "
                     "the single-instance guard",
                     qUtf8Printable(lockPath()), describe(m_lock->error()));
            return Role::Holder;
        }
        switch (forward(files)) {
        case Attempt::Acked:
            return Role::Forwarded;
        case Attempt::Mute:
            retry = false;
            break;
        case Attempt::NoListener:
            retry = !window.hasExpired();
            break;
        }
        if (retry) {
            QThread::sleep(std::min<std::chrono::nanoseconds>(
                kRetryStep, window.remainingTimeAsDuration()));
        }
    }

    // Nobody answered. Removing the lock file succeeds only when no live
    // process holds it, whatever process id it names, so a live holder is
    // never displaced and a dead one never blocks the launch.
    if (m_lock->removeStaleLockFile() && m_lock->tryLock(0ms)) {
        return becomeHolder();
    }

    qint64  pid = 0;
    QString host;
    QString program;
    if (m_lock->getLockInfo(&pid, &host, &program) && pid > 0) {
        qWarning("rawform is already running (pid %lld) and does not answer",
                 static_cast<long long>(pid));
    } else {
        qWarning("rawform is already running and does not answer");
    }
    return Role::Failed;
}

void SingleInstance::ensureDirectories() const {
    QDir().mkpath(m_lockDir);
    if (m_socketDir.isEmpty() || QFileInfo::exists(m_socketDir)) {
        return;
    }
    // The leaf is created private; what leads to it is the system's.
    QDir().mkpath(QFileInfo(m_socketDir).absolutePath());
    QDir().mkdir(m_socketDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                  | QFileDevice::ExeOwner);
}

SingleInstance::Role SingleInstance::becomeHolder() {
    if (m_socketPath.isEmpty()) {
        qWarning("rawform: no runtime directory for the hand-off socket; later launches "
                 "cannot reach this window");
        return Role::Holder;
    }
    if (socketPathBytes(m_socketPath) > socketPathLimit()) {
        qWarning("rawform: the hand-off socket path is %lld bytes, over the %lld a socket "
                 "path may have: %s; later launches cannot reach this window",
                 static_cast<long long>(socketPathBytes(m_socketPath)),
                 static_cast<long long>(socketPathLimit()), qUtf8Printable(m_socketPath));
        return Role::Holder;
    }

    // Holding the lock is what makes a socket file of this name a leftover.
    QLocalServer::removeServer(m_socketPath);
    m_server = std::make_unique<QLocalServer>();
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server->listen(m_socketPath)) {
        qWarning("rawform: cannot listen on %s (%s); later launches cannot reach this "
                 "window",
                 qUtf8Printable(m_socketPath), qUtf8Printable(m_server->errorString()));
        m_server.reset();
        return Role::Holder;
    }
    connect(m_server.get(), &QLocalServer::newConnection, this,
            &SingleInstance::acceptPending);
    return Role::Holder;
}

SingleInstance::Attempt SingleInstance::forward(const QList<QUrl>& files) const {
    if (m_socketPath.isEmpty() || socketPathBytes(m_socketPath) > socketPathLimit()) {
        return Attempt::NoListener;
    }
    QLocalSocket socket;
    socket.connectToServer(m_socketPath);
    if (!socket.waitForConnected(static_cast<int>(kRetryStep.count()))) {
        return Attempt::NoListener;
    }

    QByteArray batch;
    batch += kHeader;
    batch += '\n';
    for (const QUrl& url : files) {
        // Fully encoded, so a newline in a file name travels as %0A and
        // cannot end the line.
        batch += url.toEncoded();
        batch += '\n';
    }
    batch += '\n';
    socket.write(batch);
    socket.flush();

    // A holder that is still starting has the connection queued and reads it
    // once its event loop runs; the wait below also pushes out whatever the
    // socket buffer did not take at once.
    const QDeadlineTimer deadline(m_timeouts.ackWait);
    QByteArray reply;
    for (;;) {
        reply += socket.readAll();
        if (reply.startsWith(kAck)) {
            return Attempt::Acked;
        }
        if (socket.state() != QLocalSocket::ConnectedState) {
            return Attempt::NoListener;
        }
        if (deadline.hasExpired()) {
            return Attempt::Mute;
        }
        socket.waitForReadyRead(std::max(1, static_cast<int>(deadline.remainingTime())));
    }
}

void SingleInstance::acceptPending() {
    while (QLocalSocket* socket = m_server->nextPendingConnection()) {
        m_pending.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this,
                [this, socket] { readBatch(socket); });
        connect(socket, &QLocalSocket::disconnected, this,
                [this, socket] { dropConnection(socket); });
        readBatch(socket);
        if (socket->state() == QLocalSocket::UnconnectedState) {
            dropConnection(socket);
        }
    }
}

void SingleInstance::readBatch(QLocalSocket* socket) {
    const auto entry = m_pending.find(socket);
    if (entry == m_pending.end()) {
        return; // answered or refused already
    }
    QByteArray& buffer = entry.value();
    buffer += socket->readAll();

    // Refuse as early as the bytes allow: a peer that is not a rawform
    // newcomer gets no answer and no buffer.
    const qsizetype headerEnd = buffer.indexOf('\n');
    const QByteArrayView seen = headerEnd >= 0 ? QByteArrayView(buffer).first(headerEnd)
                                               : QByteArrayView(buffer);
    const bool headerOk = headerEnd >= 0 ? seen == kHeader : kHeader.startsWith(seen);
    if (!headerOk || buffer.size() > kMaxBatchBytes) {
        m_pending.erase(entry);
        socket->abort();
        dropConnection(socket);
        return;
    }

    const qsizetype end = buffer.indexOf("\n\n");
    if (end < 0) {
        return; // more to come
    }

    QList<QUrl> urls;
    if (end > headerEnd) {
        const QList<QByteArray> lines =
            buffer.sliced(headerEnd + 1, end - headerEnd - 1).split('\n');
        for (const QByteArray& line : lines) {
            const QUrl url = QUrl::fromEncoded(line, QUrl::StrictMode);
            if (url.isValid() && url.isLocalFile()) {
                urls.append(url);
            }
        }
    }

    // Forgotten before the signal: a receiver that runs an event loop must
    // not find this batch pending a second time.
    m_pending.erase(entry);
    socket->write(kAck.data(), kAck.size());
    socket->flush();

    const QPointer<QLocalSocket> open(socket);
    emit filesReceived(urls);
    if (open) {
        open->disconnectFromServer();
    }
}

void SingleInstance::dropConnection(QLocalSocket* socket) {
    m_pending.remove(socket);
    socket->deleteLater();
}

} // namespace rawform
