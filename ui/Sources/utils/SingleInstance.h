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

// SingleInstance.h
//
// One rawform per configuration directory. The first process to take the lock
// file in that directory is the holder: it listens on a local socket and
// receives the files later launches were asked to open. A later launch, the
// newcomer, hands its files to the holder over that socket and exits.
//
// Two locations, on purpose. The lock lives with the configuration it
// protects. The socket cannot: a socket path is limited to about a hundred
// bytes, so it lives in the short per-user runtime directory, under a name
// derived from the lock directory. Two configurations therefore never share a
// socket, and every instance on one configuration finds the same one.
//
// Who owns a lock is decided by the lock file itself, never by a process id.
// QLockFile keeps an advisory lock on the file while it holds it, and removing
// the file requires that lock; a process id means nothing to a process in
// another PID namespace (two Flatpak sandboxes), where the newcomer can carry
// the very number a dead holder left behind.
//
// Wire format, UTF-8 lines:
//     rawform-open 1
//     file:///percent/encoded/url        (zero or more)
//     <empty line>
// The holder answers "ok" and a newline, then closes. A wrong header or an
// oversized batch is closed without an answer.
//
// No GUI dependency: Qt Core, plus Qt Network for the local socket.

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <chrono>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QLockFile;

namespace rawform {

/// The directory the hand-off socket lives in: $XDG_RUNTIME_DIR/rawform when
/// that variable names an absolute path, on every platform, and inside a
/// Flatpak sandbox $XDG_RUNTIME_DIR/app/$FLATPAK_ID, the one runtime directory
/// every instance of the app shares. Without the variable: Qt's runtime
/// location on Linux, the per-user temporary directory on macOS (Qt's runtime
/// location there is under ~/Library, which rawform does not write to). Empty
/// when no private runtime directory can be had; the holder then runs without
/// a socket.
[[nodiscard]] QString defaultSocketDir();

/// The files named on a command line, as file URLs. @p args is the argument
/// list without the program name. An entry starting with '-' is an option and
/// is skipped; a file: URI becomes its local path; a relative path resolves
/// against @p workingDir; a path that does not exist is dropped with one
/// warning.
[[nodiscard]] QList<QUrl> fileUrlsFromArguments(const QStringList& args,
                                                const QString& workingDir);

class SingleInstance : public QObject {
    Q_OBJECT

public:
    enum class Role {
        Holder,    ///< This process is the instance; go on starting.
        Forwarded, ///< A running instance took the files; exit with success.
        Failed,    ///< A running instance holds the lock and does not answer.
    };

    /// How long a newcomer waits. The connect window is how long it keeps
    /// trying the lock and the socket before it concludes nobody is
    /// listening; the ack wait is how long a connected holder may take to
    /// answer, which has to cover a holder that is still starting.
    struct Timeouts {
        std::chrono::milliseconds connectWindow{2000};
        std::chrono::milliseconds ackWait{5000};
    };

    /// @p lockDir holds the lock file, @p socketDir the socket. Neither needs
    /// to exist yet. An empty @p socketDir means no socket.
    SingleInstance(const QString& lockDir, const QString& socketDir,
                   QObject* parent = nullptr);
    ~SingleInstance() override;

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    void setTimeouts(const Timeouts& timeouts);

    /// Take the lock or hand @p files to whoever holds it. Blocks for at most
    /// the connect window plus the ack wait, without running an event loop,
    /// so it can be called before the application's loop starts.
    ///
    /// As the holder the socket is listening when this returns, and @p files
    /// is not used: the caller opens its own files. Batches from newcomers
    /// are read and answered from the event loop of the calling thread.
    ///
    /// A lock that cannot be taken for a reason other than another instance
    /// (a read-only or unusable configuration directory) also returns Holder,
    /// with one warning and no socket: the application starts unguarded
    /// rather than not at all.
    [[nodiscard]] Role start(const QList<QUrl>& files);

    /// The socket this guard listens on or connects to; empty without a
    /// socket directory. Derived from the lock directory's canonical path, so
    /// it is final only once that directory exists.
    [[nodiscard]] QString socketPath() const;

    [[nodiscard]] QString lockPath() const;

signals:
    /// A newcomer's batch, possibly empty (a launch with no file still asks
    /// for the window).
    void filesReceived(const QList<QUrl>& files);

private:
    enum class Attempt {
        Acked,       ///< The holder answered.
        NoListener,  ///< Refused, absent, or closed without an answer.
        Mute,        ///< Connected, and no answer within the ack wait.
    };

    [[nodiscard]] Role becomeHolder();
    [[nodiscard]] Attempt forward(const QList<QUrl>& files) const;
    void ensureDirectories() const;
    void acceptPending();
    void readBatch(QLocalSocket* socket);
    void dropConnection(QLocalSocket* socket);

    QString  m_lockDir;
    QString  m_socketDir;
    QString  m_socketPath;
    Timeouts m_timeouts;

    // Declared in this order so the server goes first: its close removes the
    // socket file, and that must happen while the lock still keeps the next
    // holder (and its fresh socket of the same name) out.
    std::unique_ptr<QLockFile>        m_lock;
    QHash<QLocalSocket*, QByteArray>  m_pending;
    std::unique_ptr<QLocalServer>     m_server;
};

} // namespace rawform
