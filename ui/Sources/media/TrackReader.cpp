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

// TrackReader.cpp
//
// Implementation of readTrack, the single tag-read boundary: the TagLib pass
// (generic PropertyMap promotion plus per-format helpers for codec detection,
// bits per sample, tag types, the FLAC vendor string and audio MD5, and the MP3
// and MP4 bitrate-mode sniffers), the engine's FFmpeg metadata fallback for the
// long tail, and the filesystem fields.

#include "media/TrackReader.h"

// Engine read-side FFmpeg metadata fallback: the second reader in readTrack's
// cascade, used when TagLib declines a file (AC3, DTS, and the rest of the long
// tail). The header names no libav* type; the symbol links in transitively
// through the rawform_audio static library, so the UI needs no FFmpeg of its own.
#include "rawform/audio/MetadataProbe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>
#include <utility>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstringlist.h>

// ---------------------------------------------------------------------------
// Format-specific TagLib headers for content-based codec detection, bits per
// sample, and tag-type reporting.
//
// These header paths and the class/method names used below are the TagLib 2.x
// surface, validated by the shipping build. If a future TagLib moves any of
// them, the fix is localized to the corresponding helper in the anonymous
// namespace; each format sits in its own dynamic_cast block on purpose. The
// plain byte readers (sniffOggCodec, flacVendorString, the MP3 and MP4
// sniffers) do not depend on TagLib at all.
// ---------------------------------------------------------------------------
#include <taglib/flacfile.h>
#include <taglib/flacproperties.h>
#include <taglib/mpegfile.h>
#include <taglib/mpegproperties.h>
#include <taglib/mpegheader.h>
#include <taglib/id3v2tag.h>
#include <taglib/id3v2header.h>
#include <taglib/mp4file.h>
#include <taglib/mp4properties.h>
#include <taglib/wavfile.h>
#include <taglib/wavproperties.h>
#include <taglib/aifffile.h>
#include <taglib/aiffproperties.h>
#include <taglib/wavpackfile.h>
#include <taglib/wavpackproperties.h>
#include <taglib/apefile.h>
#include <taglib/apeproperties.h>
#include <taglib/asffile.h>
#include <taglib/mpcfile.h>
#include <taglib/trueaudiofile.h>
#include <taglib/trueaudioproperties.h>
#include <taglib/oggfile.h>
#include <taglib/xiphcomment.h>
#include <taglib/vorbisfile.h>
#include <taglib/opusfile.h>
#include <taglib/oggflacfile.h>
#include <taglib/speexfile.h>

namespace rawform {
namespace {

QString tstr(const TagLib::String& s) {
    return QString::fromUtf8(s.toCString(true)); // true = UTF-8
}

// --- Byte-buffer helpers for the plain-bytes sniffers ----------------------
// Widened to quint32 at the source so the shift-and-or assembly below needs no
// per-byte cast. Callers bounds-check the offset.

quint32 byteAt(const QByteArray& b, int off) {
    return static_cast<unsigned char>(b[off]);
}

/// Big-endian u32 at `off`.
quint32 beU32(const QByteArray& b, int off) {
    return (byteAt(b, off)     << 24) | (byteAt(b, off + 1) << 16)
         | (byteAt(b, off + 2) << 8)  |  byteAt(b, off + 3);
}

/// Little-endian u32 at `off`.
quint32 leU32(const QByteArray& b, int off) {
    return  byteAt(b, off)            | (byteAt(b, off + 1) << 8)
         | (byteAt(b, off + 2) << 16) | (byteAt(b, off + 3) << 24);
}

/// Last-resort codec label from the container extension. Only used when the
/// content-based path below cannot identify the codec from the concrete type.
QString codecForSuffix(const QString& suffixLower) {
    static const QHash<QString, QString> kLabels = {
        { QStringLiteral("flac"), QStringLiteral("FLAC") },
        { QStringLiteral("mp3"),  QStringLiteral("MP3")  },
        { QStringLiteral("wav"),  QStringLiteral("WAV")  },
        { QStringLiteral("ogg"),  QStringLiteral("Vorbis") },
        { QStringLiteral("oga"),  QStringLiteral("Vorbis") },
        { QStringLiteral("opus"), QStringLiteral("Opus") },
        { QStringLiteral("m4a"),  QStringLiteral("AAC")  },
        { QStringLiteral("mp4"),  QStringLiteral("AAC")  },
        { QStringLiteral("aac"),  QStringLiteral("AAC")  },
        { QStringLiteral("wv"),   QStringLiteral("WavPack") },
        { QStringLiteral("ape"),  QStringLiteral("Monkey's Audio") },
        { QStringLiteral("wma"),  QStringLiteral("WMA")  },
        { QStringLiteral("aiff"), QStringLiteral("AIFF") },
        { QStringLiteral("aif"),  QStringLiteral("AIFF") },
        { QStringLiteral("mpc"),  QStringLiteral("Musepack") },
        { QStringLiteral("tta"),  QStringLiteral("TTA")  },
    };
    return kLabels.value(suffixLower, suffixLower.toUpper());
}

/// Identify the codec inside an Ogg container by its bitstream, not its
/// extension. An Ogg file starts with the capture pattern "OggS", and the first
/// page carries a codec identification header whose magic names the codec:
/// Opus -> "OpusHead", Vorbis -> "\x01vorbis", Ogg FLAC -> "\x7fFLAC",
/// Speex -> "Speex". This is the only reliable way to tell Vorbis from Opus when
/// both can carry a .ogg extension. Returns an empty string if the file is not a
/// recognized Ogg stream. Plain bytes, no TagLib dependency.
QString sniffOggCodec(const QString& path) {
    QFile fp(path);
    if (!fp.open(QIODevice::ReadOnly))
        return {};
    const QByteArray head = fp.read(128);
    if (!head.startsWith("OggS"))
        return {};
    // The identification magics are distinctive and live in this first page;
    // a bounded substring scan does not collide across the codecs.
    if (head.contains("OpusHead")) return QStringLiteral("Opus");
    if (head.contains("vorbis"))   return QStringLiteral("Vorbis");
    if (head.contains("FLAC"))     return QStringLiteral("FLAC");
    if (head.contains("Speex"))    return QStringLiteral("Speex");
    return {}; // Ogg container, but an unrecognized mapping
}

/// Open the Ogg file with the class matching the sniffed codec, so the audio
/// properties and tags come from the correct parser (TagLib's FileRef would
/// resolve .ogg to Vorbis by extension and misread an Opus stream).
std::unique_ptr<TagLib::File> openOggFile(const char* fileName, const QString& codec) {
    if (codec == QLatin1String("Opus"))
        return std::make_unique<TagLib::Ogg::Opus::File>(fileName, true);
    if (codec == QLatin1String("Vorbis"))
        return std::make_unique<TagLib::Ogg::Vorbis::File>(fileName, true);
    if (codec == QLatin1String("FLAC"))
        return std::make_unique<TagLib::Ogg::FLAC::File>(fileName, true);
    if (codec == QLatin1String("Speex"))
        return std::make_unique<TagLib::Ogg::Speex::File>(fileName, true);
    return nullptr;
}

/// The Vorbis-comment VENDOR string of a native FLAC file (e.g. "reference
/// libFLAC 1.4.3"). This is what the FLAC "Tool" field shows; it is NOT an
/// ENCODER tag, so the PropertyMap never carries it, and TagLib does not expose
/// it reliably. We read it directly: after the "fLaC" marker, FLAC metadata
/// blocks are [1 byte: last-flag + type][3 bytes BE: length][body]; the
/// VORBIS_COMMENT block (type 4) begins with a 4-byte little-endian vendor
/// length followed by the vendor bytes. Plain bytes, no TagLib dependency.
QString flacVendorString(const QString& path) {
    QFile fp(path);
    if (!fp.open(QIODevice::ReadOnly))
        return {};
    if (fp.read(4) != "fLaC")
        return {};
    while (true) {
        const QByteArray hdr = fp.read(4);
        if (hdr.size() != 4)
            break;
        const quint32 b0 = byteAt(hdr, 0);
        const bool last = (b0 & 0x80) != 0;
        const quint32 type = b0 & 0x7F;
        // The 24-bit BE length is bytes 1..3; the mask drops the flag/type byte.
        const quint32 len = beU32(hdr, 0) & 0x00FFFFFFu;
        if (type == 4) { // VORBIS_COMMENT
            const QByteArray vl = fp.read(4);
            if (vl.size() != 4)
                return {};
            const quint32 vlen = leU32(vl, 0);
            if (vlen == 0 || vlen > (1u << 20)) // sanity cap at 1 MiB
                return {};
            return QString::fromUtf8(fp.read(static_cast<qint64>(vlen)));
        }
        if (last)
            break;
        fp.seek(fp.pos() + static_cast<qint64>(len)); // skip this block's body
    }
    return {};
}

/// FLAC stores an MD5 of the DECODED audio in its STREAMINFO block; the pane shows it as
/// "Audio MD5". TagLib does not expose it, so read it directly. STREAMINFO is mandatorily
/// the first metadata block: after the "fLaC" marker and the 4-byte block header, the
/// 34-byte body's last 16 bytes are the MD5 (it sits at body offset 18, after min/max
/// block size, min/max frame size, and the packed rate/channels/bits/total-samples word).
/// An all-zero signature means the encoder stored none (e.g. --no-md5), reported here as
/// absent. Plain bytes, no TagLib dependency.
QString flacAudioMd5(const QString& path) {
    QFile fp(path);
    if (!fp.open(QIODevice::ReadOnly))
        return {};
    if (fp.read(4) != "fLaC")
        return {};
    const QByteArray hdr = fp.read(4); // [last-flag + type][3-byte BE length]
    if (hdr.size() != 4)
        return {};
    if ((byteAt(hdr, 0) & 0x7F) != 0) // 0 = STREAMINFO
        return {};
    if (!fp.seek(fp.pos() + 18)) // skip to the MD5 within the STREAMINFO body
        return {};
    const QByteArray md5 = fp.read(16);
    if (md5.size() != 16 || md5.count('\0') == 16)
        return {}; // short read, or no signature stored
    return QString::fromLatin1(md5.toHex()); // 32 lowercase hex chars
}

/// Codec label from the concrete TagLib file type (content, not extension).
/// MP4 carries both AAC and ALAC, distinguished by Properties::codec().
QString codecForFile(TagLib::File* file, const QString& suffix) {
    if (auto* mp4 = dynamic_cast<TagLib::MP4::File*>(file)) {
        if (auto* p = dynamic_cast<TagLib::MP4::Properties*>(mp4->audioProperties())) {
            switch (p->codec()) {
            case TagLib::MP4::Properties::ALAC: return QStringLiteral("ALAC");
            case TagLib::MP4::Properties::AAC:  return QStringLiteral("AAC");
            default: break;
            }
        }
        return QStringLiteral("AAC");
    }
    if (dynamic_cast<TagLib::FLAC::File*>(file))       return QStringLiteral("FLAC");
    if (dynamic_cast<TagLib::MPEG::File*>(file))       return QStringLiteral("MP3");
    if (dynamic_cast<TagLib::RIFF::WAV::File*>(file))  return QStringLiteral("WAV");
    if (dynamic_cast<TagLib::RIFF::AIFF::File*>(file)) return QStringLiteral("AIFF");
    if (dynamic_cast<TagLib::WavPack::File*>(file))    return QStringLiteral("WavPack");
    if (dynamic_cast<TagLib::APE::File*>(file))        return QStringLiteral("Monkey's Audio");
    if (dynamic_cast<TagLib::ASF::File*>(file))        return QStringLiteral("WMA");
    if (dynamic_cast<TagLib::MPC::File*>(file))        return QStringLiteral("Musepack");
    if (dynamic_cast<TagLib::TrueAudio::File*>(file))  return QStringLiteral("TTA");
    return codecForSuffix(suffix); // unrecognized type: fall back to extension
}

/// Bits per sample for lossless / PCM formats. Lossy formats have no fixed value
/// and return 0, which the model treats as "hide the row".
int bitsPerSampleOf(TagLib::AudioProperties* ap) {
    if (!ap)
        return 0;
    if (auto* p = dynamic_cast<TagLib::FLAC::Properties*>(ap))       return p->bitsPerSample();
    if (auto* p = dynamic_cast<TagLib::MP4::Properties*>(ap))        return p->bitsPerSample();
    if (auto* p = dynamic_cast<TagLib::RIFF::WAV::Properties*>(ap))  return p->bitsPerSample();
    if (auto* p = dynamic_cast<TagLib::RIFF::AIFF::Properties*>(ap)) return p->bitsPerSample();
    if (auto* p = dynamic_cast<TagLib::WavPack::Properties*>(ap))    return p->bitsPerSample();
    if (auto* p = dynamic_cast<TagLib::APE::Properties*>(ap))        return p->bitsPerSample();
    if (auto* p = dynamic_cast<TagLib::TrueAudio::Properties*>(ap))  return p->bitsPerSample();
    return 0;
}

/// MPEG layer profile base, e.g. "MPEG-1 Layer III". Only synthesized for MPEG;
/// empty for everything else. This is the codec-profile BASE for MP3; the
/// CBR/VBR suffix is appended by codecProfileFor() below.
QString codecProfileOf(TagLib::AudioProperties* ap) {
    auto* p = dynamic_cast<TagLib::MPEG::Properties*>(ap);
    if (!p)
        return {};
    QString version;
    switch (p->version()) {
    case TagLib::MPEG::Header::Version1:   version = QStringLiteral("MPEG-1");   break;
    case TagLib::MPEG::Header::Version2:   version = QStringLiteral("MPEG-2");   break;
    case TagLib::MPEG::Header::Version2_5: version = QStringLiteral("MPEG-2.5"); break;
    default: return {};
    }
    static constexpr const char* kRoman[] = { "", "I", "II", "III" };
    const int layer = p->layer();
    const QString layerStr = (layer >= 1 && layer <= 3)
        ? QString::fromLatin1(kRoman[layer]) : QString::number(layer);
    return QStringLiteral("%1 Layer %2").arg(version, layerStr);
}

// ===========================================================================
// Bitrate mode (CBR / ABR / VBR), the suffix on the codec profile.
// ===========================================================================
//
// Three sources feed the "Codec Profile" pane field:
//
//   1. MP3 is detected from the stream itself (sniffMp3BitrateMode), because an
//      MP3 can be any of CBR / ABR / VBR and only the bytes can say which. The
//      same pass also recovers the LAME encoder string ("LAME3.100") for the
//      Tool field. The mode is appended to the MPEG layer base, e.g.
//      "MPEG-1 Layer III (VBR, V2)".
//
//   2. AAC inside MP4 is detected from the container (aacProfile): the object
//      type (LC / HE-AAC / ...) from the esds box. CBR/VBR has no dependable
//      signal in an AAC container (real-file analysis showed CBR and VBR FFmpeg
//      output to be statistically identical), so we never guess; the lone honest
//      hint, an esds declaring a peak above the average, yields "(VBR)" and
//      nothing else does. See aacProfile for the full rationale.
//
//   3. The inherently variable-rate families (lossless compressors, plus the
//      modern lossy codecs that are effectively always VBR) need no detection:
//      they report a bare "VBR". Formats whose mode is not knowable (WMA, and
//      uncompressed PCM where the label would be noise) report nothing, so their
//      conditional row stays hidden.
//
// Everything here lands in TrackData::codecProfile; the mode needs no stored
// field of its own.

enum class BitrateMode { Unknown, Cbr, Abr, Vbr };

struct Mp3BitrateInfo {
    BitrateMode mode      = BitrateMode::Unknown;
    int         vbrPreset = -1; // LAME -V n in 0..9; -1 when not VBR / not recovered
    QString     encoder;        // LAME-header encoder string, e.g. "LAME3.100"
};

/// MP3 bitrate mode, the LAME -V preset for VBR, and the LAME encoder string.
///
/// HOW: VBR/ABR encoders write a Xing/Info (or Fraunhofer VBRI) header into the
/// FIRST frame, in the dead space after the Layer III side-information block. We
/// skip any ID3v2 tag, find the first valid MPEG frame, locate that header, and
/// classify it. The header's LAME extension carries an authoritative VBR-method
/// nibble plus a 9-byte encoder string; when there is no LAME extension we fall
/// back to the magic ("Xing" => variable, "Info" => constant), and a frame with
/// no header at all is the classic headerless-CBR case. Plain bytes, no TagLib
/// dependency, the same shape as the FLAC/Ogg sniffers above.
///
/// CONFIDENCE: the CBR/ABR/VBR split and the encoder string are reliable. The -V
/// preset recovery (from the Xing VBR-quality field) is best-effort and the
/// exact quality->V mapping is encoder-dependent; it is isolated to the single
/// gated line at the end and only ever emits a value in 0..9.
Mp3BitrateInfo sniffMp3BitrateMode(const QString& path) {
    Mp3BitrateInfo out;
    QFile fp(path);
    if (!fp.open(QIODevice::ReadOnly))
        return out;

    // ----- 1. Skip an ID3v2 tag so the frame search starts at audio. -------
    // Header: "ID3", version(2), flags(1), size(4, syncsafe = 7 bits/byte). A
    // footer (flag bit 0x10) adds another 10 bytes after the tag body.
    qint64 start = 0;
    {
        const QByteArray id3 = fp.read(10);
        if (id3.size() == 10 && id3.startsWith("ID3")) {
            const quint32 size = (byteAt(id3, 6) << 21) | (byteAt(id3, 7) << 14)
                               | (byteAt(id3, 8) << 7)  |  byteAt(id3, 9);
            const bool footer = (byteAt(id3, 5) & 0x10) != 0;
            start = 10 + static_cast<qint64>(size) + (footer ? 10 : 0);
        }
    }

    // ----- 2. Find the first valid MPEG audio frame sync from `start`. -----
    // Bounded so a pathological file cannot make us scan forever; the first
    // frame always lands within the first few KB of audio in practice.
    constexpr qint64 kScanLimit = 512 * 1024;
    if (!fp.seek(start))
        return out;
    const QByteArray buf = fp.read(kScanLimit);
    // int offsets are exact here: the read is capped at kScanLimit, and every
    // offset below is int arithmetic on that bounded buffer.
    const int n = static_cast<int>(buf.size());
    const auto u8 = [&](int idx) { return static_cast<unsigned char>(buf[idx]); };

    int version = -1, layer = -1, channelMode = -1, frameOff = -1;
    for (int i = 0; i + 4 <= n; ++i) {
        if (u8(i) != 0xFF || (u8(i + 1) & 0xE0) != 0xE0)
            continue;
        const int ver    = (u8(i + 1) >> 3) & 0x03; // 0=2.5, 1=reserved, 2=2, 3=1
        const int lay    = (u8(i + 1) >> 1) & 0x03; // 1=III, 2=II, 3=I, 0=reserved
        const int brIdx  = (u8(i + 2) >> 4) & 0x0F;
        const int srIdx  = (u8(i + 2) >> 2) & 0x03;
        const int chMode = (u8(i + 3) >> 6) & 0x03; // 3 = single channel (mono)
        if (ver == 1 || lay == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3)
            continue; // reserved/invalid combination: a false sync, keep scanning
        version = ver; layer = lay; channelMode = chMode; frameOff = i;
        break;
    }
    if (frameOff < 0)
        return out;

    // The Xing/Info/VBRI header only exists in Layer III. A Layer I/II stream
    // (rare, and effectively always constant rate) is reported CBR directly.
    if (layer != 1) {
        out.mode = BitrateMode::Cbr;
        return out;
    }

    // ----- 3. Locate the Xing/Info tag (after the side-info block). --------
    const bool mpeg1   = (version == 3);
    const bool mono    = (channelMode == 3);
    const int  sideLen = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    const int  tagOff  = frameOff + 4 + sideLen;

    // Fraunhofer VBRI sits at a fixed offset from the header instead; its mere
    // presence means VBR (no LAME extension to refine, so we stop here).
    const int vbriOff = frameOff + 4 + 32;
    if (vbriOff + 4 <= n && buf.mid(vbriOff, 4) == "VBRI") {
        out.mode = BitrateMode::Vbr;
        return out;
    }

    if (tagOff + 4 > n)
        return out; // cannot reach the tag slot; leave Unknown (bare profile)
    const QByteArray magic = buf.mid(tagOff, 4);
    const bool isXing = (magic == "Xing");
    const bool isInfo = (magic == "Info");
    if (!isXing && !isInfo) {
        out.mode = BitrateMode::Cbr; // no VBR header at all: headerless CBR
        return out;
    }

    // Magic-based first guess; the LAME method nibble below refines it.
    out.mode = isXing ? BitrateMode::Vbr : BitrateMode::Cbr;

    // ----- 4. Walk the Xing/Info optional fields to reach the LAME tag. ----
    // Layout: magic(4) flags(4), then conditionally frames(4), bytes(4),
    // TOC(100), quality(4), in that order, gated by the flag bits.
    int p = tagOff + 4;
    if (p + 4 > n)
        return out;
    const quint32 flags = beU32(buf, p);
    p += 4;
    int vbrQuality = -1;
    if (flags & 0x1) p += 4;   // frame count
    if (flags & 0x2) p += 4;   // byte count
    if (flags & 0x4) p += 100; // seek TOC
    if (flags & 0x8) {         // VBR quality indicator present
        if (p + 4 <= n)
            vbrQuality = static_cast<int>(beU32(buf, p));
        p += 4;
    }

    // The LAME extension (36 bytes) begins at p: a 9-byte encoder string, then a
    // byte whose low nibble is the VBR method.
    if (p + 10 <= n) {
        // Encoder string: 9 printable bytes ("LAME3.100", "Lavf58.76", ...),
        // null/space padded. We reject the whole field on any non-printable byte
        // so a zero-filled (no-LAME-extension) slot yields nothing.
        QString encStr;
        bool ok = true;
        for (int k = 0; k < 9; ++k) {
            const unsigned char c = u8(p + k);
            if (c == 0x00)
                break;             // end of string
            if (c < 0x20 || c > 0x7E) { ok = false; break; } // not a real string
            encStr.append(QLatin1Char(static_cast<char>(c)));
        }
        encStr = encStr.trimmed();
        if (ok && !encStr.isEmpty() && encStr.at(0).isLetter())
            out.encoder = encStr;

        // VBR method nibble: authoritative over the magic when sane (this is what
        // separates ABR from VBR, since LAME writes "Xing" for both).
        switch (u8(p + 9) & 0x0F) {
        case 1: case 8: out.mode = BitrateMode::Cbr; break; // CBR, CBR 2-pass
        case 2: case 9: out.mode = BitrateMode::Abr; break; // ABR, ABR 2-pass
        case 3: case 4:
        case 5: case 6: out.mode = BitrateMode::Vbr; break; // VBR methods 1..4
        default: break;                                     // 0/unknown: keep guess
        }
    }

    // ----- 5. Best-effort -V preset for true VBR. --------------------------
    // LAME stores roughly (100 - 10*V) in the Xing quality field, so the inverse
    // is V = (100 - quality) / 10. TWEAK POINT: if your -V files read wrong, this
    // is the one line to adjust. Gated to 0..9.
    if (out.mode == BitrateMode::Vbr && vbrQuality >= 0 && vbrQuality <= 100) {
        const int v = (100 - vbrQuality) / 10;
        if (v >= 0 && v <= 9)
            out.vbrPreset = v;
    }
    return out;
}

/// The parenthetical bitrate-mode tag for an MP3 profile, e.g. "CBR", "ABR",
/// "VBR", or "VBR, V2". Empty when the mode could not be determined.
QString mp3ModeTag(const Mp3BitrateInfo& info) {
    switch (info.mode) {
    case BitrateMode::Cbr: return QStringLiteral("CBR");
    case BitrateMode::Abr: return QStringLiteral("ABR");
    case BitrateMode::Vbr:
        return info.vbrPreset >= 0
            ? QStringLiteral("VBR, V%1").arg(info.vbrPreset)
            : QStringLiteral("VBR");
    case BitrateMode::Unknown:
        break;
    }
    return {};
}

// ===========================================================================
// MP4 / AAC container reading: object type and VBR hint from the esds box.
// ===========================================================================
//
// AAC carries no single CBR/VBR flag, so we read the container directly. We pull
// the moov box (walking top-level boxes by seek, so a moov at the file tail is
// handled without reading the audio mdat), then scan WITHIN moov for the two
// leaf boxes we need. Scanning within moov, rather than a strict tree walk,
// sidesteps the QuickTime sound-sample-entry version quirks a walk to esds would
// have to special-case, while staying clear of false matches in the audio data.

/// The moov box payload bytes, located by walking the top-level box list with
/// seeks. Capped so a pathological file cannot trigger a huge read. Empty on any
/// failure.
QByteArray readMoovPayload(QFile& fp) {
    const qint64 fileSize = fp.size();
    qint64 pos = 0;
    while (pos + 8 <= fileSize) {
        if (!fp.seek(pos))
            break;
        const QByteArray hdr = fp.read(8);
        if (hdr.size() < 8)
            break;
        quint64 size = beU32(hdr, 0);
        const QByteArray type = hdr.mid(4, 4);
        qint64 headerLen = 8;
        if (size == 1) { // 64-bit largesize follows the type
            const QByteArray ext = fp.read(8);
            if (ext.size() < 8)
                break;
            size = 0;
            for (int k = 0; k < 8; ++k)
                size = (size << 8) | static_cast<unsigned char>(ext[k]);
            headerLen = 16;
        } else if (size == 0) { // extends to EOF
            size = static_cast<quint64>(fileSize - pos);
        }
        if (size < static_cast<quint64>(headerLen))
            break; // malformed
        if (type == "moov") {
            if (!fp.seek(pos + headerLen))
                break;
            const qint64 payloadLen = static_cast<qint64>(size) - headerLen;
            // We only need the esds, which sits in stsd near the front of moov
            // (before the large sample tables), so a modest cap is plenty and
            // keeps the read clear of the sample tables this probe never uses.
            return fp.read(qMin<qint64>(payloadLen, 256 * 1024));
        }
        const qint64 next = pos + static_cast<qint64>(size);
        if (next <= pos || next > fileSize)
            break; // no progress or out of range
        pos = next;
    }
    return {};
}

/// Offset just past the first occurrence of a 4-char box type in `buf` (i.e. the
/// box payload start), or -1. Scanning within moov is safe: the esds box precedes
/// its data and appears once for the audio track.
int findBoxPayload(const QByteArray& buf, const char* type) {
    // The moov payload is capped by readMoovPayload, so int offsets are exact.
    const int n = static_cast<int>(buf.size());
    for (int i = 0; i + 8 <= n; ++i) {
        if (buf[i] == type[0] && buf[i + 1] == type[1]
            && buf[i + 2] == type[2] && buf[i + 3] == type[3])
            return i + 4; // payload begins right after the 4-char type
    }
    return -1;
}

/// AAC decoder facts from the esds box: the object-type label and the declared
/// max/avg bitrates. Walks the ES_Descriptor -> DecoderConfigDescriptor ->
/// AudioSpecificConfig. All reads are bounds-checked, so a malformed box degrades
/// to a generic "AAC" with zero bitrates, never a crash.
///
/// LIMITATION: HE-AAC signaled implicitly (backward-compatible SBR) reports as
/// "AAC LC" here, since detecting it needs an SBR-extension scan TagLib's decoder
/// would do but we do not. Explicit HE-AAC (object type 5/29) is caught.
struct AacEsds {
    QString objectType = QStringLiteral("AAC");
    quint32 maxBitrate = 0;
    quint32 avgBitrate = 0;
};

AacEsds parseAacEsds(const QByteArray& m, int esdsPayload) {
    AacEsds out;
    const int n = static_cast<int>(m.size()); // moov payload, capped (see above)
    int p = esdsPayload + 4; // skip the esds version/flags
    const auto need = [&](int k) { return p + k <= n; };
    const auto u8   = [&](int o) { return static_cast<unsigned char>(m[o]); };
    const auto skipLen = [&]() {                // expandable descriptor length
        for (int c = 0; c < 4 && need(1); ++c) {
            if ((u8(p++) & 0x80) == 0)
                break;
        }
    };

    if (!need(1) || u8(p) != 0x03) return out;  // ES_Descriptor
    ++p; skipLen();
    if (!need(3)) return out;
    p += 2;                                      // ES_ID
    const unsigned char esFlags = u8(p++);
    if (esFlags & 0x80) p += 2;                  // streamDependenceFlag
    if (esFlags & 0x40) { if (!need(1)) return out; p += 1 + u8(p); } // URL
    if (esFlags & 0x20) p += 2;                  // OCRstreamFlag

    if (!need(1) || u8(p) != 0x04) return out;   // DecoderConfigDescriptor
    ++p; skipLen();
    if (!need(13)) return out;
    const unsigned char oti = u8(p);             // 0x40 = MPEG-4 Audio
    out.maxBitrate = beU32(m, p + 5);            // bytes 5..8 of this descriptor
    out.avgBitrate = beU32(m, p + 9);            // bytes 9..12
    p += 13;                                      // oti(1)+stream(1)+buf(3)+max(4)+avg(4)
    if (oti != 0x40)
        return out;

    if (!need(1) || u8(p) != 0x05) return out;   // AudioSpecificConfig
    ++p; skipLen();
    if (!need(1)) return out;
    int aot = (u8(p) >> 3) & 0x1F;
    if (aot == 31) {                              // escape: 6 more bits + 32
        if (!need(2)) return out;
        aot = 32 + (((u8(p) & 0x07) << 3) | ((u8(p + 1) >> 5) & 0x07));
    }
    switch (aot) {
    case 1:  out.objectType = QStringLiteral("AAC Main");  break;
    case 2:  out.objectType = QStringLiteral("AAC LC");    break;
    case 3:  out.objectType = QStringLiteral("AAC SSR");   break;
    case 4:  out.objectType = QStringLiteral("AAC LTP");   break;
    case 5:  out.objectType = QStringLiteral("HE-AAC");    break; // SBR
    case 29: out.objectType = QStringLiteral("HE-AAC v2"); break; // PS
    default: break;                               // keep generic "AAC"
    }
    return out;
}

/// Full AAC codec-profile string. The object type (LC / HE-AAC / ...) is read
/// reliably from the esds.
///
/// CBR vs VBR, on the other hand, has NO dependable signal in an MP4/AAC
/// container. Analysis of real FFmpeg-encoded CBR and VBR files showed: esds
/// byte-identical save the bitrate number (both with maxBitrate == avgBitrate),
/// an identical encoder atom, and frame-size variance that overlaps and even
/// inverts (the CBR file varied MORE than the VBR one, per-frame and in every
/// windowed measure). So we never infer the mode from frame sizes. The single
/// trustworthy hint is an esds that declares a peak above the average, which only
/// a genuine VBR stream does; we append "(VBR)" then, with a margin so a CBR
/// reservoir's small headroom is not mistaken for it. FFmpeg sets max == avg in
/// BOTH modes, so its files read as the object type alone.
QString aacProfile(const QString& path) {
    QFile fp(path);
    if (!fp.open(QIODevice::ReadOnly))
        return QStringLiteral("AAC");
    const QByteArray moov = readMoovPayload(fp);
    if (moov.isEmpty())
        return QStringLiteral("AAC"); // could not find moov; at least name it

    const int esds = findBoxPayload(moov, "esds");
    if (esds < 0)
        return QStringLiteral("AAC");
    const AacEsds info = parseAacEsds(moov, esds);

    // VBR only when the container declares a peak more than ~10% above the
    // average. Integer form of maxBitrate > avgBitrate * 1.10.
    if (info.avgBitrate > 0 && info.maxBitrate > (info.avgBitrate / 10) * 11)
        return info.objectType + QStringLiteral(" (VBR)");
    return info.objectType;
}

/// The full "Codec Profile" string for the General pane. MP3 gets the MPEG layer
/// base plus the detected CBR/ABR/VBR tag (from the pre-sniffed `mp3`); AAC gets
/// its object type, plus "(VBR)" only when the container declares a peak above
/// average; the inherently variable families get a bare "VBR"; WMA / uncompressed
/// PCM / unknown return empty so their conditional row hides.
QString codecProfileFor(TagLib::AudioProperties* ap, const QString& codec,
                        const QString& path, const Mp3BitrateInfo& mp3) {
    if (codec == QLatin1String("MP3")) {
        const QString base = codecProfileOf(ap); // "MPEG-1 Layer III" or ""
        const QString tag  = mp3ModeTag(mp3);
        if (base.isEmpty())
            return tag.isEmpty() ? QString{} : QStringLiteral("(%1)").arg(tag);
        return tag.isEmpty() ? base : QStringLiteral("%1 (%2)").arg(base, tag);
    }

    if (codec == QLatin1String("AAC"))
        return aacProfile(path);

    // Inherently variable-rate: lossless compressors, and the lossy codecs that
    // are VBR in all practical encodings (Speex and Musepack included).
    static const QSet<QString> kAlwaysVbr = {
        QStringLiteral("FLAC"), QStringLiteral("ALAC"),
        QStringLiteral("WavPack"), QStringLiteral("Monkey's Audio"),
        QStringLiteral("TTA"), QStringLiteral("Vorbis"),
        QStringLiteral("Opus"), QStringLiteral("Speex"),
        QStringLiteral("Musepack"),
    };
    if (kAlwaysVbr.contains(codec))
        return QStringLiteral("VBR");

    // WMA / uncompressed PCM (WAV, AIFF) / anything else: the mode is either not
    // cheaply knowable or pure noise on an uncompressed file, so report nothing
    // and let the conditional row stay hidden.
    return {};
}

/// Which tag container(s) the file carries, as a display label. Conditional: an
/// empty result hides the row. Format-specific, since "what tags exist" is asked
/// through each concrete file type.
QString tagTypeOf(TagLib::File* file) {
    QStringList parts;
    if (auto* mp3 = dynamic_cast<TagLib::MPEG::File*>(file)) {
        if (mp3->hasID3v2Tag() && mp3->ID3v2Tag() && mp3->ID3v2Tag()->header())
            parts << QStringLiteral("ID3v2.%1").arg(mp3->ID3v2Tag()->header()->majorVersion());
        if (mp3->hasID3v1Tag()) parts << QStringLiteral("ID3v1");
        if (mp3->hasAPETag())   parts << QStringLiteral("APE");
    } else if (auto* flac = dynamic_cast<TagLib::FLAC::File*>(file)) {
        if (flac->hasXiphComment()) parts << QStringLiteral("Vorbis Comment");
        if (flac->hasID3v2Tag())    parts << QStringLiteral("ID3v2");
        if (flac->hasID3v1Tag())    parts << QStringLiteral("ID3v1");
    } else if (dynamic_cast<TagLib::Ogg::File*>(file)) {
        parts << QStringLiteral("Vorbis Comment"); // every Ogg codec uses Xiph comments
    } else if (dynamic_cast<TagLib::MP4::File*>(file)) {
        parts << QStringLiteral("MP4");
    } else if (auto* wv = dynamic_cast<TagLib::WavPack::File*>(file)) {
        if (wv->hasAPETag())   parts << QStringLiteral("APE");
        if (wv->hasID3v1Tag()) parts << QStringLiteral("ID3v1");
    } else if (auto* ape = dynamic_cast<TagLib::APE::File*>(file)) {
        if (ape->hasAPETag())   parts << QStringLiteral("APE");
        if (ape->hasID3v1Tag()) parts << QStringLiteral("ID3v1");
    } else if (auto* wav = dynamic_cast<TagLib::RIFF::WAV::File*>(file)) {
        if (wav->hasID3v2Tag()) parts << QStringLiteral("ID3v2");
    }
    return parts.join(QStringLiteral(" + "));
}

// --- Shared tag promotion --------------------------------------------------

/// Promote the canonical-keyed tags in @p props onto @p t's named fields, then
/// sweep whatever is left into extraTags. Shared by the TagLib reader and the
/// FFmpeg probe so the two cannot drift in how they map tags. Operates on a
/// neutral QMap (no TagLib type), keyed by the uppercase canonical vocabulary
/// (ARTIST, ALBUM, TRACKNUMBER, ...); each caller presents its tags under those
/// keys before calling. Reads and erases each promoted key so the trailing sweep
/// sees only un-promoted tags.
void promoteTagMap(QMap<QString, QStringList>& props, TrackData& t) {
    const auto take = [&props](const char* key) -> QStringList {
        const QString k = QString::fromLatin1(key);
        const auto it = props.find(k);
        if (it == props.end())
            return {};
        // Moved out before the erase: a reference into the node would dangle.
        QStringList out = std::move(it.value());
        props.erase(it);
        return out;
    };

    t.artists      = take("ARTIST");
    t.albumArtists = take("ALBUMARTIST");
    t.genres       = take("GENRE");
    if (const QStringList a = take("ALBUM"); !a.isEmpty())
        t.album = a.first();
    if (const QStringList ti = take("TITLE"); !ti.isEmpty())
        t.title = ti.first();

    // Track / disc numbers and totals. TRACKNUMBER may be "5" or "5/12"; totals
    // may also live in their own keys (Vorbis TRACKTOTAL / id3 TOTALTRACKS).
    const auto firstInt = [](const QStringList& v) -> int {
        if (v.isEmpty())
            return 0;
        bool ok = false;
        const int x = v.first().section(QLatin1Char('/'), 0, 0).toInt(&ok);
        return ok ? x : 0;
    };
    const auto slashTotal = [](const QStringList& v) -> int {
        if (v.isEmpty() || !v.first().contains(QLatin1Char('/')))
            return 0;
        bool ok = false;
        const int x = v.first().section(QLatin1Char('/'), 1, 1).toInt(&ok);
        return ok ? x : 0;
    };
    // Raw tagged text of the position number (the part before any "/total"),
    // trimmed: preserves leading zeros ("05") and non-numeric designators ("A1")
    // for full-fidelity display, beside the parsed int that drives sort.
    const auto rawNum = [](const QStringList& v) -> QString {
        if (v.isEmpty())
            return {};
        return v.first().section(QLatin1Char('/'), 0, 0).trimmed();
    };
    const QStringList trk = take("TRACKNUMBER");
    const QStringList dsc = take("DISCNUMBER");
    QStringList trkTot = take("TRACKTOTAL");
    if (trkTot.isEmpty()) trkTot = take("TOTALTRACKS");
    QStringList dscTot = take("DISCTOTAL");
    if (dscTot.isEmpty()) dscTot = take("TOTALDISCS");
    t.trackNo    = firstInt(trk);
    t.discNo     = firstInt(dsc);
    t.trackNoRaw = rawNum(trk);
    t.discNoRaw  = rawNum(dsc);
    t.trackTotal = !trkTot.isEmpty() ? firstInt(trkTot) : slashTotal(trk);
    t.discTotal  = !dscTot.isEmpty() ? firstInt(dscTot) : slashTotal(dsc);

    // Date: keep the raw string AND a parsed 4-digit year. DATE only, no
    // fallbacks (the scenario this prevents: a YEAR or ORIGINALDATE fallback
    // would consume those keys, so a file whose only date carrier was one of
    // them would show a populated Date with no removable row behind it;
    // removing Date would erase only the DATE key and the next read would
    // resurrect the value). Both keys sweep into extraTags below and surface
    // as ordinary <YEAR> / <ORIGINALDATE> custom rows, removable like any
    // foreign field.
    const QStringList date = take("DATE");
    if (!date.isEmpty()) {
        t.dateRaw = date.first();
        static const QRegularExpression yearRe(QStringLiteral("(\\d{4})"));
        const QRegularExpressionMatch m = yearRe.match(t.dateRaw);
        if (m.hasMatch())
            t.year = m.captured(1).toInt();
    }

    // Tool / encoder, the TAG-derived part only. A higher-precedence source (the
    // MP3 LAME header) may have set t.tool already, so ENCODER then ENCODING fill
    // in only when it is still empty. ENCODER is always consumed (it never
    // belongs in extraTags); ENCODING is consumed only when used, so an unused
    // ENCODING still round-trips through extraTags.
    QString tagEncoder;
    if (const QStringList e = take("ENCODER"); !e.isEmpty())
        tagEncoder = e.first();
    if (t.tool.isEmpty() && !tagEncoder.isEmpty())
        t.tool = tagEncoder;
    if (t.tool.isEmpty()) {
        if (const QStringList e = take("ENCODING"); !e.isEmpty())
            t.tool = e.first();
    }

    // Embedded cuesheet PRESENCE only. A CUESHEET tag (Vorbis comment / APE) is
    // consumed so its (large) body does not bloat extraTags. FLAC's BINARY
    // cuesheet block is outside this reader's contract: TagLib does not expose
    // it, so a FLAC carrying only that block reads as "No".
    if (const QStringList cue = take("CUESHEET"); !cue.isEmpty())
        t.hasEmbeddedCuesheet = true;

    // Everything still in props is preserved verbatim for lossless round-trip.
    for (auto it = props.cbegin(); it != props.cend(); ++it)
        t.extraTags.insert(it.key(), it.value());
}

/// Build the neutral tag map promoteTagMap expects from the probe's verbatim
/// libavformat tags. libav uses lower-case keys in its own spelling; map the ones
/// we promote onto the uppercase canonical vocabulary and pass anything else
/// through upper-cased so it lands verbatim in extraTags. First occurrence of a
/// canonical key wins, and the probe lists format-level entries before stream-
/// level ones, so the container value is preferred over a stream duplicate.
/// Splitting a delimited multi-value string is outside this adapter's contract:
/// such a tag is kept as a single value, which the promotion handles as-is.
QMap<QString, QStringList> tagMapFromProbe(
    const std::vector<std::pair<std::string, std::string>>& tags) {
    static const QHash<QString, QString> kCanon = {
        { QStringLiteral("title"),        QStringLiteral("TITLE") },
        { QStringLiteral("artist"),       QStringLiteral("ARTIST") },
        { QStringLiteral("album"),        QStringLiteral("ALBUM") },
        { QStringLiteral("album_artist"), QStringLiteral("ALBUMARTIST") },
        { QStringLiteral("albumartist"),  QStringLiteral("ALBUMARTIST") },
        { QStringLiteral("genre"),        QStringLiteral("GENRE") },
        { QStringLiteral("track"),        QStringLiteral("TRACKNUMBER") },
        { QStringLiteral("tracktotal"),   QStringLiteral("TRACKTOTAL") },
        { QStringLiteral("totaltracks"),  QStringLiteral("TOTALTRACKS") },
        { QStringLiteral("disc"),         QStringLiteral("DISCNUMBER") },
        { QStringLiteral("disctotal"),    QStringLiteral("DISCTOTAL") },
        { QStringLiteral("totaldiscs"),   QStringLiteral("TOTALDISCS") },
        { QStringLiteral("date"),         QStringLiteral("DATE") },
        { QStringLiteral("encoder"),      QStringLiteral("ENCODER") },
        { QStringLiteral("cuesheet"),     QStringLiteral("CUESHEET") },
    };
    QMap<QString, QStringList> out;
    for (const auto& kv : tags) {
        const QString rawKey = QString::fromStdString(kv.first);
        const QString canon  = kCanon.value(rawKey.toLower(), rawKey.toUpper());
        if (out.contains(canon))
            continue; // first occurrence wins (container over stream)
        out.insert(canon, QStringList{ QString::fromStdString(kv.second) });
    }
    return out;
}

/// Adapt an engine ProbedMetadata onto a TrackData: copy the audio properties,
/// take the content-derived codec label over the provisional extension guess, and
/// run the shared promotion on the probe's tags. tagType, codecProfile, audioMd5,
/// and hasEmbeddedArt are intentionally left at their defaults: the probe surfaces
/// no tag-container name or CBR/VBR profile, and embedded-art bytes are unreachable
/// here because AlbumArtProvider fetches them through TagLib, which cannot open
/// this file. The empty fields simply leave their conditional pane rows hidden.
void fillFromProbe(const audio::ProbedMetadata& md, TrackData& t) {
    t.durationMs    = static_cast<int>(md.durationMs);
    t.bitrateKbps   = static_cast<int>(md.bitrateKbps);
    t.sampleRateHz  = static_cast<int>(md.sampleRate);
    t.channels      = static_cast<int>(md.channels);
    t.bitsPerSample = static_cast<int>(md.bitsPerSample);
    if (!md.codecName.empty())
        t.codec = QString::fromStdString(md.codecName);

    QMap<QString, QStringList> tagMap = tagMapFromProbe(md.tags);
    promoteTagMap(tagMap, t);
}

} // namespace

TrackData readTrack(const QString& path) {
    TrackData t;
    const QFileInfo fi(path);
    t.filePath     = fi.absoluteFilePath();
    t.fileName     = fi.fileName();
    t.folderName   = fi.absoluteDir().dirName();
    t.fileSize     = fi.size();
    t.modified     = fi.lastModified();
    t.created      = fi.birthTime(); // may be invalid on some filesystems
    t.subsongIndex = 0;

    const QString suffix = fi.suffix().toLower();
    t.codec = codecForSuffix(suffix); // provisional; refined from content below

    // -----------------------------------------------------------------------
    // Acquire the right TagLib file object. For Ogg containers we sniff the
    // bitstream and open the matching class, because the extension alone cannot
    // tell Vorbis from Opus (both can be .ogg) and FileRef would misread it.
    // Everything else goes through FileRef, then the codec is taken from the
    // concrete type. The rest of the function reads uniformly from `file`.
    // -----------------------------------------------------------------------
    static const QSet<QString> kOggExts = {
        QStringLiteral("ogg"), QStringLiteral("oga"),
        QStringLiteral("opus"), QStringLiteral("spx"),
    };
    const QByteArray enc = QFile::encodeName(path);

    std::unique_ptr<TagLib::File> owned; // owns the Ogg file when we open one
    TagLib::FileRef ref;                 // owns the file for every other format
    TagLib::File* file = nullptr;

    QString oggCodec;
    if (kOggExts.contains(suffix))
        oggCodec = sniffOggCodec(path);

    if (!oggCodec.isEmpty())
        owned = openOggFile(enc.constData(), oggCodec);

    if (owned && owned->isValid()) {
        file = owned.get();
        t.codec = oggCodec;
    } else {
        owned.reset();
        ref = TagLib::FileRef(enc.constData(), /*readAudioProperties=*/true);
        file = ref.file();
        if (file)
            t.codec = codecForFile(file, suffix);
    }

    // --- Metadata reader cascade -------------------------------------------
    // Ordered precedence, the read-side mirror of DecoderFactory: TagLib is the
    // native reference reader and was tried just above; when it cannot parse the
    // file we fall through to the engine's FFmpeg probe, which reads the long tail
    // (AC3, DTS, and anything else libavformat understands). A third reader would
    // slot in as one more step here. The first reader that succeeds fills the row
    // and returns; only "every reader declined" marks the row invalid (the scanner
    // then drops it). This is what separates genuinely-unreadable from merely-not-
    // TagLib-readable, with no extension allow-list to maintain.
    if (!file || !file->isValid()) {
        // On a native-only build kMetadataProbeAvailable is false and the probe is
        // skipped, so a file TagLib declines is dropped; on an FFmpeg build the
        // probe fills real metadata for what it can open.
        if (audio::kMetadataProbeAvailable) {
            std::string perr;
            if (const auto md = audio::probeAudioMetadata(enc.toStdString(), &perr)) {
                fillFromProbe(*md, t);
                t.valid = true;
                return t;
            }
        }
        t.valid = false; // no reader could parse it: genuinely unreadable
        return t;
    }

    TagLib::AudioProperties* ap = file->audioProperties();
    if (ap) {
        t.durationMs   = ap->lengthInMilliseconds();
        t.bitrateKbps  = ap->bitrate();
        t.sampleRateHz = ap->sampleRate();
        t.channels     = ap->channels();
    }
    t.bitsPerSample = bitsPerSampleOf(ap);

    // MP3 is sniffed once here so both the codec profile and the Tool field can
    // use the result (the LAME-header encoder string rides along in mp3.encoder).
    Mp3BitrateInfo mp3;
    if (t.codec == QLatin1String("MP3"))
        mp3 = sniffMp3BitrateMode(path);

    // Codec profile carries the CBR/ABR/VBR designation: detected from the
    // stream for MP3, from the MP4 container for AAC, asserted for the always-
    // variable families, blank (row hidden) where the mode is not knowable.
    t.codecProfile  = codecProfileFor(ap, t.codec, path, mp3);
    t.tagType       = tagTypeOf(file);

    // Promote the tags. The MP3 LAME-header encoder is the highest-precedence
    // source for the Tool field, so set it before the
    // shared promotion, whose ENCODER/ENCODING fills only when Tool is still empty.
    if (t.codec == QLatin1String("MP3") && !mp3.encoder.isEmpty())
        t.tool = mp3.encoder;

    // TagLib's PropertyMap already uses the uppercase canonical keys promoteTagMap
    // expects, so convert it straight into the neutral map and run the SAME
    // promotion the FFmpeg-probe path runs (artists, album, title, the track/disc
    // numbers and totals, the date and parsed year, the tag-derived Tool field,
    // the embedded-cuesheet flag, and the verbatim extraTags sweep). Sharing it is
    // what keeps the two readers from drifting in how they map tags.
    QMap<QString, QStringList> tagMap;
    {
        const TagLib::PropertyMap props = file->properties();
        for (const auto& kv : props) {
            QStringList vals;
            for (const TagLib::String& s : kv.second)
                vals << tstr(s);
            tagMap.insert(tstr(kv.first), vals);
        }
    }
    promoteTagMap(tagMap, t);

    // ----- TagLib-only enrichment (no FFmpeg-probe equivalent) -------------
    // Native FLAC stores its encoder in the Vorbis-comment VENDOR string, not an
    // ENCODER tag, so the PropertyMap never has it. Read it directly. (Ogg Vorbis
    // / Opus already carry an ENCODER tag, handled by the promotion above.)
    if (t.tool.isEmpty() && oggCodec.isEmpty() && t.codec == QLatin1String("FLAC"))
        t.tool = flacVendorString(path);

    // FLAC Audio MD5 (STREAMINFO), native FLAC only. Conditional in the pane, so
    // it hides for non-FLAC and for FLAC files that stored no signature.
    if (oggCodec.isEmpty() && t.codec == QLatin1String("FLAC"))
        t.audioMd5 = flacAudioMd5(path);

    // Embedded art PRESENCE only: keys, not the picture bytes (those are
    // fetched lazily by AlbumArtProvider for the displayed selection).
    const TagLib::StringList artKeys = file->complexPropertyKeys();
    for (const TagLib::String& k : artKeys) {
        if (k == "PICTURE") {
            t.hasEmbeddedArt = true;
            break;
        }
    }

    t.valid = true;
    return t;
}

} // namespace rawform
