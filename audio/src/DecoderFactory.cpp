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

// DecoderFactory.cpp
//
// The format dispatcher. It is the only file that knows about every concrete
// decoder at once; everything above it programs against IDecoderFactory and
// never sees a codec header.
//
// Dispatch policy, in full:
//
//   1. Sniff the leading bytes (primary signal). "fLaC" is FLAC; "ID3" or an
//      MPEG frame sync (0xFF Ex) is MP3; "RIFF"/"FORM"/".snd"/"caff" is a
//      libsndfile container; "OggS" is an Ogg container; the AC-3, DTS, ADTS,
//      and ASF sync words are FFmpeg-only elementary streams or containers.
//      Anything else is unclassified.
//
//   2. The extension is a HINT, not a verdict: it reorders the candidate list so
//      the likely decoder is tried first, but it never decides alone.
//
//   3. A try-open cascade is the fallback: candidates are attempted in order and
//      the first that opens wins. This makes a wrong sniff self-correcting, for
//      instance an ID3-tagged FLAC (which sniffs as MP3) still opens because
//      FlacDecoder follows Mpg123Decoder in the list.
//
//   4. The frozen-decoder rule is structural: when the file is FLAC or MP3,
//      libsndfile is removed from the candidate list entirely, so its own
//      optional FLAC/MP3 support can never shadow libFLAC or libmpg123. "FLAC or
//      MP3" is decided by content sniff always, and by a .flac/.mp3 extension
//      UNLESS the content unmistakably says a sndfile container (the one
//      mislabel case: a real WAV named ".mp3" should still play through sndfile,
//      because that is not libsndfile decoding an MP3).
//
//   5. When nothing opens, return nullptr and report the most-likely decoder's
//      own error, so the user sees a relevant reason rather than the last
//      decoder's incidental one.
//
//   6. (Optional build.) FFmpeg is the terminal fallback, appended STRICTLY
//      LAST after all sniff/extension reordering, so the native reference
//      decoders are never shadowed and the common path never pays its open cost.
//      It is reached only when every native candidate has returned nullptr,
//      which is the long tail (AAC/M4A, AC3, DTS, Opus, ALAC, WMA, and so on).
//      It is never moved to front by the extension hint and never removed; it
//      simply catches the fall-through. The sniffed FFmpeg-only kinds (AC-3,
//      DTS, ADTS, ASF) get an EMPTY native list so FFmpeg is reached directly;
//      the unsniffed long tail (an M4A "ftyp" box, for instance) pays a few
//      failed native opens first, which is accepted. The whole FFmpeg branch
//      compiles out when RAWFORM_HAVE_FFMPEG is 0.
//
// An env-gated diagnostic (RAWFORM_DECODER_LOG set to anything) prints which
// decoder won for each opened path. It is silent by default and is the
// debug-log view of what SourceInfo.decoder (DecoderKind) records permanently:
// the deciding decoder's identity, most usefully a confirmation that FFmpeg
// never shadowed a native reference decoder (which the codec read-out alone
// cannot prove for FLAC/MP3, since FfmpegDecoder maps those families back onto
// the existing tags).

#include "DecoderFactory.h"

#include "decoders/FlacDecoder.h"
#include "decoders/Mpg123Decoder.h"
#include "decoders/SndfileDecoder.h"
#if RAWFORM_HAVE_FFMPEG
#include "decoders/FfmpegDecoder.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace rawform::audio {

namespace {

// The decoders the cascade can try. Kept local to this file; the rest of the
// engine never names a concrete decoder. Ffmpeg is present only in an FFmpeg
// build and is always the terminal candidate (appended last below).
enum class Dec {
    Flac,
    Mp3,
    Sndfile,
#if RAWFORM_HAVE_FFMPEG
    Ffmpeg,
#endif
};

// What the leading bytes look like. Distinct from Dec because one sniff can map
// to several candidate decoders: "Sndfile" means "a container libsndfile reads",
// and "Ogg" is split out because an Ogg stream routes only to libsndfile or
// FFmpeg, never to the raw-FLAC or MP3 decoders (which key off their own magic).
enum class Sniff { Flac, Mp3, Ogg, Ac3, Dts, Wma, Adts, Sndfile, Unknown };

// Reads up to 16 leading bytes and classifies them. A file that cannot be opened
// for reading sniffs as Unknown; the cascade then fails with each decoder's real
// open error, which is the right place for the diagnostic.
Sniff sniffBytes(const std::string& path) {
    unsigned char b[16] = {0};
    std::size_t   n     = 0;
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return Sniff::Unknown;
        }
        f.read(reinterpret_cast<char*>(b), sizeof(b));
        n = static_cast<std::size_t>(f.gcount());
    }

    auto starts = [&](const char* sig, std::size_t len) {
        if (n < len) {
            return false;
        }
        for (std::size_t i = 0; i < len; ++i) {
            if (b[i] != static_cast<unsigned char>(sig[i])) {
                return false;
            }
        }
        return true;
    };

    // Exact match on the first four bytes, for the binary sync words below.
    // Spelled as raw byte values rather than a string literal because the DTS
    // syncs contain embedded NULs and high bytes that are awkward (and easy to
    // get wrong) as escaped string constants.
    auto sync4 = [&](unsigned s0, unsigned s1, unsigned s2, unsigned s3) {
        return n >= 4 && b[0] == s0 && b[1] == s1 && b[2] == s2 && b[3] == s3;
    };

    if (starts("fLaC", 4)) {
        return Sniff::Flac;
    }
    if (starts("ID3", 3)) {
        return Sniff::Mp3;  // ID3v2-tagged; almost always MP3, FLAC fallback covers the rest
    }
    // AC3 / E-AC3: the frame sync word 0x0B77, which a raw .ac3 leads with.
    if (n >= 2 && b[0] == 0x0B && b[1] == 0x77) {
        return Sniff::Ac3;
    }
    // DTS: the core sync word in its four byte orders (16- and 14-bit, big- and
    // little-endian), plus the DTS-HD extension-substream sync. Checked BEFORE the
    // MPEG-frame-sync test below so the 14-bit little-endian core (which begins
    // 0xFF 0x1F) can never be mistaken for an MPEG frame. Routing it here is also
    // what keeps a raw .dts away from Mpg123Decoder, whose permissive sync scan
    // otherwise false-positives on DTS payload bytes and opens the file as bogus
    // "MP3" (the same hazard the Ogg case guards against).
    if (sync4(0x7F, 0xFE, 0x80, 0x01) ||  // core, 16-bit big-endian
        sync4(0x1F, 0xFF, 0xE8, 0x00) ||  // core, 14-bit big-endian
        sync4(0xFE, 0x7F, 0x01, 0x80) ||  // core, 16-bit little-endian
        sync4(0xFF, 0x1F, 0x00, 0xE8) ||  // core, 14-bit little-endian
        sync4(0x64, 0x58, 0x20, 0x25)) {  // DTS-HD extension substream (EXSS)
        return Sniff::Dts;
    }
    // WMA / ASF: the 16-byte ASF_Header_Object GUID that opens every ASF file.
    // Routed to FFmpeg only, for the same reason as AC3/DTS: ASF is a structured
    // binary container the native decoders cannot read, and letting Mpg123Decoder
    // scan its payload risks the same MPEG frame-sync false positive.
    static const unsigned char kAsfGuid[16] = {
        0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
        0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
    if (n >= 16) {
        bool asf = true;
        for (int i = 0; i < 16; ++i) {
            if (b[i] != kAsfGuid[i]) {
                asf = false;
                break;
            }
        }
        if (asf) {
            return Sniff::Wma;
        }
    }
    // ADTS AAC: the 0xFFF syncword with the layer field 00, a reserved (invalid)
    // value in real MPEG audio, so this test and the MPEG frame-sync test below
    // are disjoint. Checked BEFORE the MPEG test because an ADTS header
    // also matches its looser (b[1] & 0xE0) == 0xE0 pattern and would otherwise
    // sniff as Mp3, handing a raw .aac to Mpg123Decoder, whose permissive resync
    // scan over AAC payload is the same false-positive hazard the DTS and Ogg
    // cases guard against.
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) {
        return Sniff::Adts;
    }
    if (n >= 2 && b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) {
        return Sniff::Mp3;  // MPEG frame sync
    }
    if (starts("OggS", 4)) {
        return Sniff::Ogg;  // Ogg container (Vorbis/Opus/FLAC); never MP3 or raw FLAC
    }
    if (starts("RIFF", 4) || starts("FORM", 4) || starts(".snd", 4) ||
        starts("caff", 4)) {
        return Sniff::Sndfile;
    }
    return Sniff::Unknown;
}

// Lowercased file extension without the dot, or empty when there is none. Only
// the basename's last dot counts, so a directory like "my.music/song" with a
// dotless file name yields no extension.
std::string lowerExtension(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot   = path.find_last_of('.');
    if (dot == std::string::npos ||
        (slash != std::string::npos && dot < slash)) {
        return {};
    }
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

// Stable move-to-front: brings `d` to the head of `order` if present, preserving
// the relative order of everything else. Used to let the extension hint bias the
// cascade without discarding the sniff-derived ordering.
void moveToFront(std::vector<Dec>& order, Dec d) {
    const auto it = std::ranges::find(order, d);
    if (it != order.end()) {
        std::rotate(order.begin(), it, it + 1);
    }
}

// Human name of the concrete decoder behind a cascade slot, for the diagnostic.
// The switch is exhaustive over Dec in both builds (the Ffmpeg arm shares the
// enum's guard), so it stays -Wswitch clean.
const char* decoderName(Dec d) noexcept {
    switch (d) {
        case Dec::Flac:    return "FlacDecoder";
        case Dec::Mp3:     return "Mpg123Decoder";
        case Dec::Sndfile: return "SndfileDecoder";
#if RAWFORM_HAVE_FFMPEG
        case Dec::Ffmpeg:  return "FfmpegDecoder";
#endif
    }
    return "?";
}

// Optional routing diagnostic. Silent unless RAWFORM_DECODER_LOG is set in the
// environment, in which case it prints which decoder opened each path. This is
// the direct, unambiguous confirmation that a given file routed to the decoder
// it should have, in particular that a native format was NOT caught by the
// FFmpeg fallback. It runs only on the success path, only on a manual open, and
// never on the real-time thread, so the getenv probe per open is free.
void logDecoderChoice(const std::string& path, Dec d) {
    if (std::getenv("RAWFORM_DECODER_LOG") != nullptr) {
        std::fprintf(stderr, "[decoder] %s via %s\n", path.c_str(),
                     decoderName(d));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
std::unique_ptr<IDecoder> DecoderFactory::open(const std::string& path,
                                               std::string*       error) {
    const Sniff       sniff = sniffBytes(path);
    const std::string ext   = lowerExtension(path);

    const bool extFlac = (ext == "flac");
    const bool extMp3  = (ext == "mp3" || ext == "mp2");

    // Base ordering from the sniff. Sndfile leads when the content is one of its
    // containers or is unclassified (it reads the widest range), otherwise the
    // sniffed codec leads.
    std::vector<Dec> order;
    switch (sniff) {
        case Sniff::Flac:    order = {Dec::Flac, Dec::Mp3, Dec::Sndfile}; break;
        case Sniff::Mp3:     order = {Dec::Mp3, Dec::Flac, Dec::Sndfile}; break;
        // Ogg container. The only valid native candidate is libsndfile (it reads
        // Ogg/FLAC; the Vorbis/Opus it declines on an FFmpeg build fall through
        // to FFmpeg). The raw-FLAC and MP3 decoders are deliberately excluded:
        // an Ogg stream carries neither magic, and offering it to Mpg123Decoder
        // risks a frame-sync false positive (Opus packets can contain 0xFF 0xEx
        // bytes that libmpg123 mistakes for an MPEG frame, opening the file as
        // bogus "MP3"). So: Sndfile, then the terminal FFmpeg appended below,
        // and nothing else.
        case Sniff::Ogg:     order = {Dec::Sndfile}; break;
        // Raw AC3 / DTS / ADTS-AAC elementary stream, or an ASF / WMA container.
        // No native decoder reads any of these, and offering them to
        // Mpg123Decoder triggers a frame-sync false positive (it opens as bogus
        // "MP3" and the cascade never reaches FFmpeg), the same hazard the Ogg
        // case guards against. So there are NO native candidates: the cascade
        // is empty here and only the terminal FFmpeg appended below runs. On a
        // native-only build it stays empty and the open fails cleanly, which is
        // correct: the engine cannot decode these without FFmpeg, and a clean
        // failure beats a wrongful "MP3" open playing garbage.
        case Sniff::Ac3:
        case Sniff::Dts:
        case Sniff::Wma:
        case Sniff::Adts:    order = {}; break;
        case Sniff::Sndfile:
        case Sniff::Unknown: order = {Dec::Sndfile, Dec::Flac, Dec::Mp3}; break;
    }

    // Extension hint reorders only; it never removes a candidate on its own.
    if (extFlac) {
        moveToFront(order, Dec::Flac);
    } else if (extMp3) {
        moveToFront(order, Dec::Mp3);
    } else if (!ext.empty()) {
        moveToFront(order, Dec::Sndfile);  // wav/aiff/au/ogg/caf/... -> sndfile first
    }

    // Frozen-decoder rule. Exclude sndfile when the file is FLAC or MP3 by
    // content, or by a .flac/.mp3 extension that the content does not contradict.
    // An Ogg sniff is a second contradiction that keeps sndfile IN: the rule
    // exists to keep libsndfile away from raw FLAC/MP3 BITSTREAMS, where its own
    // optional support would shadow the reference decoders, and an Ogg container
    // is neither. Ogg-contained FLAC is precisely libsndfile's job in this
    // cascade (see the Sniff::Ogg routing above), and such files are named
    // ".flac" often enough in the wild that erasing sndfile here would hand them
    // to the FFmpeg fallback, or, on a native-only build, leave them unplayable,
    // contradicting the Ogg case's own routing intent.
    const bool contentFlacOrMp3 = (sniff == Sniff::Flac || sniff == Sniff::Mp3);
    const bool extFlacOrMp3 = (extFlac || extMp3);
    const bool excludeSndfile =
        contentFlacOrMp3 ||
        (extFlacOrMp3 && sniff != Sniff::Sndfile && sniff != Sniff::Ogg);
    if (excludeSndfile) {
        std::erase(order, Dec::Sndfile);
    }

#if RAWFORM_HAVE_FFMPEG
    // Terminal fallback. Appended after every sniff/extension reorder and after
    // the frozen-decoder erase, so it is unconditionally last: the native
    // reference decoders are always tried first, and FFmpeg only catches what
    // they all decline. Never hinted to the front, never removed.
    order.push_back(Dec::Ffmpeg);
#endif

    // Try each candidate in order; the first that opens wins. Remember the first
    // (most likely) failure so a total miss reports a relevant reason.
    std::string primaryErr;
    for (std::size_t i = 0; i < order.size(); ++i) {
        std::string               e;
        std::unique_ptr<IDecoder> dec;
        switch (order[i]) {
            case Dec::Flac:    dec = FlacDecoder::open(path, &e);    break;
            case Dec::Mp3:     dec = Mpg123Decoder::open(path, &e);  break;
            case Dec::Sndfile: dec = SndfileDecoder::open(path, &e); break;
#if RAWFORM_HAVE_FFMPEG
            case Dec::Ffmpeg:  dec = FfmpegDecoder::open(path, &e);  break;
#endif
        }
        if (dec) {
            logDecoderChoice(path, order[i]);
            return dec;
        }
        if (i == 0) {
            primaryErr = std::move(e);
        }
    }

    if (error) {
        *error = "no decoder could open '" + path + "'";
        if (!primaryErr.empty()) {
            *error += ": " + primaryErr;
        }
    }
    return nullptr;
}

}  // namespace rawform::audio
