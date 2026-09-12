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

// MetadataEditor.cpp
//
// The write worker. See the header for the contract. Mirrors ReplayGainEditor's
// structure: build per-file jobs on the GUI thread, run writeOne across the
// global pool, collect verdicts back on the GUI thread in onFinished.

#include "media/MetadataEditor.h"

#include "media/TagWriteLock.h"      // cross-window same-file write lease
#include "settings/SettingsStore.h"  // tagging snapshot for the MP3 save policy

#include <QFile>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QtConcurrent>

#include <utility> // std::move

#include <taglib/commentsframe.h>      // COMM frames for the encoding pass
#include <taglib/fileref.h>
#include <taglib/id3v2.h>              // ID3v2::Version (v3 / v4)
#include <taglib/id3v2tag.h>           // frame list for the encoding pass
#include <taglib/mpegfile.h>           // typed MP3 save + strip + tag presence
#include <taglib/textidentificationframe.h>  // T*** frames (TXXX included)
#include <taglib/tfile.h>              // also File::StripTags / File::DuplicateTags
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>

namespace rawform {
namespace {

// One key/value instruction for a file. An empty value list means "remove".
struct Mutation {
    QString     key;
    QStringList values;
};

// One file's write job: every mutation that targets this path, in arrival order,
// plus the tagging settings frozen at apply() time. The snapshot rides in
// the job, so a Settings change mid-write cannot affect an in-flight pass and
// the worker never touches the SettingsStore QObject. stripAll marks a
// Tools > Remove tags job: the write starts from a bare file instead of the
// captured tag view (see writeOne).
struct Job {
    QString                        path;
    QList<Mutation>                mutations;
    SettingsStore::TaggingSnapshot tagging;
    bool                           stripAll = false;
};

// Convert an explicit value list into the TagLib value list to store. The list is
// authoritative (no splitting); empty entries are dropped so a stray blank does
// not write an empty field. Each surviving entry becomes one distinct value (for
// FLAC/Vorbis, one comment field per value).
TagLib::StringList toTagValues(const QStringList& values) {
    TagLib::StringList out;
    for (const QString& v : values) {
        if (v.isEmpty())
            continue;
        out.append(TagLib::String(v.toUtf8().constData(), TagLib::String::UTF8));
    }
    return out;
}

// Apply one mutation to the live PropertyMap: remove on an empty value list, else
// replace the whole value list for that key (set semantics, not append).
void applyMutation(TagLib::PropertyMap& props, const Mutation& m) {
    const TagLib::String key(m.key.toUtf8().constData(), TagLib::String::UTF8);
    const TagLib::StringList values = toTagValues(m.values);
    if (values.isEmpty()) {
        props.erase(key);
        return;
    }
    props.replace(key, values);
}

// The snapshot's encoding int as TagLib's String::Type, mapped locally in the
// worker. Exhaustive for -Wswitch; the trailing return covers a
// hypothetically out-of-range int with the documented default.
TagLib::String::Type textEncodingFor(int enc) {
    switch (static_cast<SettingsStore::Id3v2Encoding>(enc)) {
        case SettingsStore::EncLatin1: return TagLib::String::Latin1;
        case SettingsStore::EncUtf16:  return TagLib::String::UTF16;
        case SettingsStore::EncUtf8:   return TagLib::String::UTF8;
    }
    return TagLib::String::UTF16;
}

// Write one file. Off the GUI thread. Never throws; failure is r.ok == false.
//
// MP3 (detected by TYPE, a dynamic_cast on the FileRef's file, never by
// extension) takes the settings-honoring path; everything else takes the
// format-blind FileRef::save(). The MP3 sequence is order-sensitive:
//
//   1. Read properties() FIRST. On a file whose only tag block is the one
//      about to be stripped (an ID3v1-only or APEv2-only MP3), properties()
//      is served from that block; capturing it before the strip is what
//      migrates the unedited fields into the ID3v2 block written below.
//      Stripping first would silently discard every field the user did not
//      touch in this edit.
//   2. Strip per settings, guarded on presence so a file with nothing to
//      remove is not rewritten twice (TagLib's strip() is an immediate file
//      operation, separate from save()). A failed strip fails the job; the
//      save would fail the same way and half-applying the policy is worse.
//   3. setProperties(). For MPEG this updates ID3v2 (creating it if needed)
//      and best-effort syncs an EXISTING ID3v1; it never touches APEv2, which
//      is exactly the pane's stated Preserve semantics. A just-stripped ID3v1
//      is gone by now, so it cannot be resurrected here.
//   4. The text-encoding pass. TagLib offers no create-time encoding
//      knob for programmatic edits: frames built by setProperties are
//      hardcoded UTF-8 internally, and the frame factory's
//      setDefaultTextEncoding only re-encodes frames the factory creates
//      while PARSING a tag, so the naive global approach silently does
//      nothing for edits (observed: every 2.3 write rendered UTF-16, the
//      downgrade of that hardcoded UTF-8, regardless of the setting). The
//      working mechanism is per frame: stamp setTextEncoding on every
//      text-bearing frame in the tag. Every frame rather than only the
//      edited ones, because save() rewrites the whole ID3v2 block anyway, so
//      this makes the block's encoding uniform and deterministic, which is
//      what "Text encoding: X" plainly promises. UTF-8 under a 2.3 save
//      still degrades safely: TagLib's render-time check converts it to
//      UTF-16, matching the pane hint.
//   5. The typed save. The mask always carries ID3v2; ID3v1 joins it for
//      Write (Duplicate creates a missing block from the v2 values) and, for
//      Preserve, only when the block already exists on disk: the
//      "never add one" promise is enforced by the mask itself rather than
//      trusted to DuplicateTags, whose masked-but-absent behavior varies by
//      TagLib revision. StripNone always: what should be stripped already
//      was, explicitly, in step 2, so removal is a deliberate act rather than
//      a side effect of the save mask.
//
// A Remove-tags job (j.stripAll) reshapes the sequence: step 0 strips ALL
// MP3 tag blocks structurally, step 1 captures an EMPTY view instead of the
// merged one (nothing survives by definition), step 2 is skipped (subsumed),
// step 2b clears a non-MPEG file's unsupported and complex properties, and a
// strip-only MP3 job returns after step 3's mutations when nothing remains to
// write (setProperties on an empty map would recreate an empty ID3v2 block).
MetadataEditor::WriteResult writeOne(const Job& j) {
    MetadataEditor::WriteResult r;
    r.path = j.path;

    // Serialize against any OTHER window's write pass targeting this same file
    // (per-pass dedupe already rules out a collision within our own pass). Held
    // across open -> save, covering the whole MP3 strip/save choreography too,
    // so the read-modify-write is atomic with respect to other tag writers; see
    // TagWriteLock.
    const TagWriteLock lease(j.path);

    const QByteArray nativePath = QFile::encodeName(j.path);
    TagLib::FileRef ref(nativePath.constData(), /*readAudioProperties=*/false);
    TagLib::File* file = ref.file();
    if (!file)
        return r;

    auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(file);

    // Step 0: the Remove-tags strip. For MP3 this is STRUCTURAL:
    // strip(AllTags) removes the ID3v1, ID3v2, and APEv2 blocks from the file
    // immediately (TagLib's strip is its own file operation, separate from
    // save), deliberately overriding the Preserve settings; "removes known tag
    // types" is the feature's contract. A failed strip fails the job. Note the
    // strip persists even if a later step fails, so a strip-plus-edits job that
    // dies mid-save leaves a stripped file without the new values; acceptable,
    // since retrying the job converges.
    if (j.stripAll && mpeg != nullptr) {
        if (!mpeg->strip(TagLib::MPEG::File::AllTags))
            return r;
    }

    // Step 1: capture the merged view before anything is removed. A strip job
    // starts from an EMPTY view instead: nothing on disk survives, so
    // nothing is migrated; only the job's own mutations can put values back.
    TagLib::PropertyMap props =
            j.stripAll ? TagLib::PropertyMap{} : file->properties();

    // Step 2: MP3-only explicit strips per the tagging settings. Skipped for a
    // strip job, whose AllTags strip above already covers both blocks.
    if (mpeg != nullptr && !j.stripAll) {
        if (j.tagging.id3v1Mode == SettingsStore::Id3v1Strip &&
            mpeg->hasID3v1Tag()) {
            if (!mpeg->strip(TagLib::MPEG::File::ID3v1))
                return r;
        }
        if (j.tagging.apeMode == SettingsStore::ApeStrip && mpeg->hasAPETag()) {
            if (!mpeg->strip(TagLib::MPEG::File::APE))
                return r;
        }
    }

    // Step 2b (non-MPEG strip only): TagLib exposes no structural strip
    // for the other containers, so the removal is by content. setProperties
    // below replaces the whole SUPPORTED map (everything not in the new map is
    // erased), which leaves two categories to clear by hand: unsupported /
    // binary entries (removeUnsupportedProperties over the map's own
    // unsupportedData listing) and complex properties, i.e. embedded pictures
    // (every advertised key set to an empty list). The emptied tag STRUCTURE
    // itself is outside this strip's reach and can survive where the format
    // requires or TagLib keeps it, e.g. FLAC's Vorbis comment block with only
    // its vendor string.
    if (j.stripAll && mpeg == nullptr) {
        file->removeUnsupportedProperties(file->properties().unsupportedData());
        const TagLib::StringList complexKeys = file->complexPropertyKeys();
        for (const TagLib::String& key : complexKeys)
            file->setComplexProperties(key, {});
    }

    // Step 3: apply the edits onto the captured view and push it back. A strip
    // job whose mutations left nothing to write is DONE here for MP3: the
    // AllTags strip already persisted, and setProperties on an empty map would
    // needlessly recreate an empty ID3v2 block, the exact structure Remove
    // tags exists to remove.
    for (const Mutation& m : j.mutations)
        applyMutation(props, m);
    if (j.stripAll && mpeg != nullptr && props.isEmpty()) {
        r.ok = true;
        return r;
    }
    file->setProperties(props);

    // Step 4: MP3-only per-frame text-encoding stamp (see the sequence
    // comment above for why this replaces the frame-factory global). The tag
    // exists by now (setProperties created it if needed); the null guard is
    // belt and suspenders. TextIdentificationFrame covers every T*** frame,
    // TXXX included through its UserTextIdentificationFrame subclass;
    // CommentsFrame is COMM. Other frame types carry no text encoding byte.
    if (mpeg != nullptr) {
        if (TagLib::ID3v2::Tag* v2 = mpeg->ID3v2Tag()) {
            const TagLib::String::Type enc =
                    textEncodingFor(j.tagging.id3v2Encoding);
            for (TagLib::ID3v2::Frame* frame : v2->frameList()) {
                if (auto* t = dynamic_cast<
                            TagLib::ID3v2::TextIdentificationFrame*>(frame)) {
                    t->setTextEncoding(enc);
                } else if (auto* c = dynamic_cast<
                                   TagLib::ID3v2::CommentsFrame*>(frame)) {
                    c->setTextEncoding(enc);
                }
            }
        }
    }

    // Step 5: save. Typed for MP3, format-blind for everything else.
    if (mpeg != nullptr) {
        int tags = TagLib::MPEG::File::ID3v2;
        TagLib::File::DuplicateTags dup =
                TagLib::File::DuplicateTags::DoNotDuplicate;
        switch (static_cast<SettingsStore::Id3v1Mode>(j.tagging.id3v1Mode)) {
            case SettingsStore::Id3v1Write:
                tags |= TagLib::MPEG::File::ID3v1;
                dup = TagLib::File::DuplicateTags::Duplicate;
                break;
            case SettingsStore::Id3v1Preserve:
                // Membership in the mask is gated on the block actually
                // existing on disk, so "never add one" holds STRUCTURALLY: a
                // file without ID3v1 cannot gain it here no matter how this
                // TagLib revision interprets DuplicateTags for masked-but-
                // absent tags (observed: it creates one). Presence is read
                // from the freshly opened file, after the strip step, so it
                // reflects disk truth for this save.
                if (mpeg->hasID3v1Tag())
                    tags |= TagLib::MPEG::File::ID3v1;
                break;
            case SettingsStore::Id3v1Strip:
                break;  // stripped above; the mask stays ID3v2-only
        }

        const TagLib::ID3v2::Version version =
                (j.tagging.id3v2Version == SettingsStore::Id3v2_4)
                        ? TagLib::ID3v2::v4
                        : TagLib::ID3v2::v3;

        r.ok = mpeg->save(tags, TagLib::File::StripTags::StripNone, version, dup);
    } else {
        r.ok = ref.save();
    }
    return r;
}

}  // namespace

MetadataEditor::MetadataEditor(QObject* parent) : QObject(parent) {
    QObject::connect(&m_watcher, &QFutureWatcher<WriteResult>::finished,
                     this, &MetadataEditor::onFinished);
}

MetadataEditor::~MetadataEditor() {
    // Let an in-flight pass settle so worker tasks do not outlive this object.
    if (m_watcher.isRunning())
        m_watcher.waitForFinished();
}

void MetadataEditor::apply(const QVariantList& edits, const QStringList& stripAllPaths) {
    if (m_busy)
        return;
    if (edits.isEmpty() && stripAllPaths.isEmpty()) {
        emit applied(0, {});  // nothing to do; keep the caller's flow uniform
        return;
    }

    // Group mutations by path so each file is opened and saved exactly once.
    // Insertion order within a path is preserved (QList), so a later edit for the
    // same key naturally wins at apply time (replace overwrites).
    QHash<QString, Job> byPath;
    byPath.reserve(edits.size());
    for (const QVariant& ev : edits) {
        const QVariantMap m = ev.toMap();
        const QString path = m.value(QStringLiteral("path")).toString();
        const QString key  = m.value(QStringLiteral("key")).toString();
        if (path.isEmpty() || key.isEmpty())
            continue;
        Job& j = byPath[path];
        j.path = path;
        j.mutations.push_back(Mutation{ key, m.value(QStringLiteral("values")).toStringList() });
    }

    // Mark (and if needed create) a job for every Remove-tags path. A
    // strip path with no mutations still gets a full job, so a file whose
    // fields were already blank in the model is stripped all the same; a strip
    // path that also carries mutations strips first, then writes them (see
    // writeOne's step order).
    for (const QString& path : stripAllPaths) {
        if (path.isEmpty())
            continue;
        Job& j = byPath[path];
        j.path = path;
        j.stripAll = true;
    }

    QList<Job> jobs = byPath.values();
    if (jobs.isEmpty()) {
        emit applied(0, {});
        return;
    }

    // Freeze the tagging settings NOW, on the GUI thread, and stamp the
    // same snapshot into every job. A null store (editor running standalone)
    // falls back to the default-constructed snapshot, which equals the documented
    // factory defaults by construction. The text encoding rides in the
    // snapshot too and is applied per frame inside writeOne; no process-global
    // TagLib state is involved.
    {
        const SettingsStore* store = SettingsStore::instance();
        const SettingsStore::TaggingSnapshot snap =
                (store != nullptr) ? store->taggingSnapshot()
                                   : SettingsStore::TaggingSnapshot{};
        for (Job& j : jobs)
            j.tagging = snap;
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

void MetadataEditor::onFinished() {
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

void MetadataEditor::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

}  // namespace rawform
