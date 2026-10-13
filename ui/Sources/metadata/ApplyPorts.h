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

// ApplyPorts.h
//
// The three collaborators the Properties apply sequencer
// (PropertiesApplyController) drives, reduced to what it needs of them: a
// metadata writer, a ReplayGain writer and a path-directed reloader. Each is
// an abstract QObject carrying one command and the one completion signal the
// sequencer waits on; MetadataEditor, ReplayGainEditor and MetadataReloader
// derive from them and are what the window instantiates.
//
// Why a base class and not the concrete types: the sequencer's unit test
// drives the phases by emitting these signals on stand-ins that touch no
// file and no audio engine. The concrete editors pull in TagLib and the
// playback controller, and the reloader the track reader, which the test
// must not link. The completion signals are declared HERE and only here: a
// signal redeclared on a subclass would be a second, unrelated signal.
//
// Anonymous in QML: the window never names a port, it names the concrete
// type; qmllint needs the base registered to resolve the inherited signal.

#pragma once

#include <QObject>
#include <QQmlEngine> // QML_ANONYMOUS
#include <QStringList>
#include <QVariantList>

namespace rawform {

/// Writes a batch of metadata edits and tag strips, then reports.
class MetadataWritePort : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

public:
    using QObject::QObject;

    /// @p edits: { path, key, values } maps; @p stripAllPaths: files whose
    /// known tag types are removed wholesale first. Always ends in applied().
    virtual void apply(const QVariantList& edits, const QStringList& stripAllPaths) = 0;

signals:
    /// GUI thread, when the write pass finishes. @p okCount files were written;
    /// @p failedPaths could not be (read-only, unsupported format, gone).
    void applied(int okCount, const QStringList& failedPaths);
};

/// Writes a batch of ReplayGain edits, then reports.
class ReplayGainWritePort : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

public:
    using QObject::QObject;

    /// @p edits: { path, trackGain?, albumGain?, trackPeak?, albumPeak? } maps.
    /// Always ends in applied().
    virtual void apply(const QVariantList& edits) = 0;

signals:
    /// GUI thread, when the write pass finishes. @p okCount files were written;
    /// @p failedPaths could not be (read-only, unsupported format, gone).
    void applied(int okCount, const QStringList& failedPaths);
};

/// Re-reads the rows holding given files, one token per request.
class ReloadPort : public QObject {
    Q_OBJECT
    QML_ANONYMOUS

public:
    using QObject::QObject;

    /// Force a re-read of every row currently holding one of @p paths. Returns
    /// a token; pathsReloaded(token) fires when exactly this pass is done,
    /// never before the call returns. A port destroyed with passes pending
    /// flushes their tokens from its destructor, so a waiter never hangs.
    virtual int reloadPaths(const QStringList& paths) = 0;

signals:
    /// The pass identified by @p token has finished, whether or not anything
    /// changed.
    void pathsReloaded(int token);
};

}  // namespace rawform
