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

// ReplayGainEditor.cpp
//
// The write worker. See the header for the contract. The write mirrors the read
// in TrackReader: open with QFile::encodeName + TagLib::FileRef, take the file's
// PropertyMap, replace or erase the four REPLAYGAIN_* keys, set it back, and save.
// Values are normalized to a canonical form on the way out (gains signed two
// decimals + " dB", peaks fixed six decimals) so the cache and display settle to
// one representation regardless of how the user typed them.

#include "media/ReplayGainEditor.h"

#include "media/ReplayGainTags.h"   // shared parse + canonical gain formatting
#include "media/TagWriteLock.h"     // cross-window same-file write lease

#include <QFile>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtConcurrent>

#include <optional>

#include <taglib/fileref.h>
#include <taglib/tfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>

namespace rawform {
namespace {

// One file's write job. Each optional follows the apply() contract: nullopt
// leaves the tag alone, an empty string removes it, a non-empty string sets it.
struct Job {
    QString path;
    std::optional<QString> trackGain;
    std::optional<QString> albumGain;
    std::optional<QString> trackPeak;
    std::optional<QString> albumPeak;
};

// Normalize a typed gain to canonical "-6.00 dB". Empty stays empty (a removal);
// an unparseable non-empty value yields nullopt (leave the tag untouched), which
// should not happen because the field is validated in the UI before it gets here.
std::optional<QString> normalizeGain(const QString& raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return QString();  // removal marker
    const auto db = replaygain::parseGainDb(s);
    if (!db)
        return std::nullopt;
    return replaygain::formatGainValue(*db);
}

// Normalize a typed peak to fixed six decimals. Empty stays empty (removal);
// unparseable/non-positive yields nullopt (leave untouched).
std::optional<QString> normalizePeak(const QString& raw) {
    const QString s = raw.trimmed();
    if (s.isEmpty())
        return QString();
    const auto p = replaygain::parsePeak(s);
    if (!p)
        return std::nullopt;
    return QString::asprintf("%.6f", *p);
}

// Resolve the per-field instruction from the edit map onto a job. A field is only
// considered when the map carries the key (the UI sends only changed fields).
void mergeField(std::optional<QString>& slot, const QVariantMap& m,
                const QString& key, bool isPeak) {
    if (!m.contains(key))
        return;
    const QString raw = m.value(key).toString();
    const std::optional<QString> norm = isPeak ? normalizePeak(raw) : normalizeGain(raw);
    if (norm)
        slot = norm;  // later edits to the same path win
}

// Apply one optional to the PropertyMap: skip (leave), erase (empty), or replace.
void applyToProps(TagLib::PropertyMap& props, const char* key,
                  const std::optional<QString>& opt) {
    if (!opt)
        return;
    const TagLib::String k(key);
    if (opt->isEmpty()) {
        props.erase(k);
        return;
    }
    props.replace(k, TagLib::StringList(
        TagLib::String(opt->toUtf8().constData(), TagLib::String::UTF8)));
}

// Write one file. Off the GUI thread. Never throws; failure is r.ok == false.
ReplayGainEditor::WriteResult writeOne(const Job& j) {
    ReplayGainEditor::WriteResult r;
    r.path = j.path;

    // Serialize against any OTHER window's write pass targeting this same file
    // (per-pass dedupe already rules out a collision within our own pass). Held
    // across open -> save so the whole read-modify-write is atomic with respect
    // to other tag writers; see TagWriteLock.
    const TagWriteLock lease(j.path);

    const QByteArray enc = QFile::encodeName(j.path);
    TagLib::FileRef ref(enc.constData(), /*readAudioProperties=*/false);
    TagLib::File* file = ref.file();
    if (!file)
        return r;

    TagLib::PropertyMap props = file->properties();
    applyToProps(props, "REPLAYGAIN_TRACK_GAIN", j.trackGain);
    applyToProps(props, "REPLAYGAIN_ALBUM_GAIN", j.albumGain);
    applyToProps(props, "REPLAYGAIN_TRACK_PEAK", j.trackPeak);
    applyToProps(props, "REPLAYGAIN_ALBUM_PEAK", j.albumPeak);
    file->setProperties(props);

    r.ok = ref.save();
    return r;
}

}  // namespace

ReplayGainEditor::ReplayGainEditor(QObject* parent) : QObject(parent) {
    QObject::connect(&m_watcher, &QFutureWatcher<WriteResult>::finished,
                     this, &ReplayGainEditor::onFinished);
}

ReplayGainEditor::~ReplayGainEditor() {
    // Let an in-flight pass settle so worker tasks do not outlive this object.
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
}

void ReplayGainEditor::apply(const QVariantList& edits) {
    if (m_busy)
        return;
    if (edits.isEmpty()) {
        emit applied(0, {});  // nothing to do; keep the caller's flow uniform
        return;
    }

    // Dedupe by path: subsongs share a file, and writing one file from two pool
    // threads would race. Later edits to the same path overwrite earlier fields.
    // The path comes FROM the edit map: it was captured when the Properties
    // window opened, so a playlist that reordered or shrank since then cannot
    // redirect the write to a different file.
    QHash<QString, Job> byPath;
    byPath.reserve(edits.size());
    for (const QVariant& ev : edits) {
        const QVariantMap m = ev.toMap();
        const QString path = m.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            continue;
        Job& j = byPath[path];
        j.path = path;
        mergeField(j.trackGain, m, QStringLiteral("trackGain"), /*isPeak=*/false);
        mergeField(j.albumGain, m, QStringLiteral("albumGain"), /*isPeak=*/false);
        mergeField(j.trackPeak, m, QStringLiteral("trackPeak"), /*isPeak=*/true);
        mergeField(j.albumPeak, m, QStringLiteral("albumPeak"), /*isPeak=*/true);
    }

    QList<Job> jobs = byPath.values();
    if (jobs.isEmpty()) {
        emit applied(0, {});
        return;
    }

    // If playback holds one of these files, stop it before the rewrite
    // (TagLib shifting audio bytes under a live decoder is the hazard; see
    // AudioController::stopIfPlayingAny for the policy and its caveats). The
    // call is synchronous on the GUI thread, so the stop command is posted to
    // the engine before any worker below opens a file.
    if (m_audio) {
        QStringList paths;
        paths.reserve(jobs.size());
        for (const Job& j : jobs)
            paths.push_back(j.path);
        m_audio->stopIfPlayingAny(paths);
    }

    setBusy(true);
    m_watcher.setFuture(QtConcurrent::mapped(std::move(jobs), writeOne));
}

void ReplayGainEditor::onFinished() {
    const QFuture<WriteResult> f = m_watcher.future();
    int okCount = 0;
    QStringList failed;
    for (int i = 0; i < f.resultCount(); ++i) {
        const WriteResult r = f.resultAt(i);
        if (r.ok)
            ++okCount;
        else
            failed << r.path;
    }
    setBusy(false);
    emit applied(okCount, failed);
}

void ReplayGainEditor::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

}  // namespace rawform
