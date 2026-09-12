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

// SndfileDecoder.cpp
//
// libsndfile implementation of the IDecoder contract. Everything that touches
// <sndfile.h> is confined to this translation unit: the SNDFILE handle and the
// SF_INFO it was opened with live in Impl, which the header forward-declares
// only. That is the whole point of the pimpl, it lets the static library link
// SndFile::sndfile privately while the header stays free of the dependency.

#include "decoders/SndfileDecoder.h"

#include "rawform/audio/Types.h"

#include <sndfile.h>

#include <cstdio>  // SEEK_SET
#include <memory>
#include <string>
#include <utility>

// libsndfile declares its SF_FORMAT_* codes as ENUM values, not preprocessor
// macros, so an #ifdef on one is always false and cannot guard a case label. We
// also want to compile against libsndfile versions old enough to predate the
// Opus enum entirely (a distribution's packaged libsndfile may be one). Both
// are solved by defining the name ourselves when no macro exists: the format
// ABI, so SF_FORMAT_OPUS is 0x0064 everywhere. On a current libsndfile the enum
// already provides the value and this macro simply shadows it with the identical
// number; on an old one it supplies a value the headers lack. Either way the
// Opus case below compiles and matches whatever subtype the runtime reports.
#ifndef SF_FORMAT_OPUS
#define SF_FORMAT_OPUS 0x0064
#endif

namespace rawform::audio {

namespace {

// Maps a libsndfile format word to the source container bit depth. We mask off
// everything but the subtype, then translate the linear-PCM and IEEE subtypes.
// Anything else returns 0, meaning no PCM container depth: the compressed
// subtypes (Vorbis, Opus) are still named by codecForSubtype below, they just
// carry no integer depth, while a genuinely unrecognized subtype ends up
// Codec::Unknown. The decode path itself does not depend on this: sf_readf_float
// converts every supported subtype to float32 regardless, so an unclassified
// subtype still decodes correctly, it just does not get a bit depth or a computed
// bitrate on the status line.
std::uint16_t bitsForSubtype(int sfFormat) noexcept {
    switch (sfFormat & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_S8:
        case SF_FORMAT_PCM_U8:  return 8;
        case SF_FORMAT_PCM_16:  return 16;
        case SF_FORMAT_PCM_24:  return 24;
        case SF_FORMAT_PCM_32:
        case SF_FORMAT_FLOAT:   return 32;
        case SF_FORMAT_DOUBLE:  return 64;
        default:                return 0;
    }
}

// Classifies a libsndfile subtype into a descriptive Codec. PCM and IEEE
// subtypes (depth != 0) are Codec::Pcm. The compressed subtypes libsndfile
// decodes as secondary formats on builds with the matching libraries (Ogg
// Vorbis, Opus) get their own tag so the status line names them instead of
// showing Unknown; they reach this decoder only on a build without FFmpeg,
// since an FFmpeg build declines them in open() below so FfmpegDecoder serves
// them with a live bitrate. There is no container bit depth for these lossy
// formats and libsndfile does not expose an average bitrate for them, so
// bitsPerSample and bitrate stay 0; only the name is recovered. FLAC and MP3
// are intentionally absent: the factory excludes libsndfile for those by
// content, so their reference decoders always win and libsndfile never reports
// them here. Opus relies on the SF_FORMAT_OPUS fallback define above, so it
// classifies even against an older header.
Codec codecForSubtype(int sfFormat, std::uint16_t bits) noexcept {
    switch (sfFormat & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_VORBIS:  return Codec::Vorbis;
        case SF_FORMAT_OPUS:    return Codec::Opus;
        default:                break;
    }
    return bits != 0 ? Codec::Pcm : Codec::Unknown;
}

}  // namespace

// ---------------------------------------------------------------------------
// The pimpl. It owns the SNDFILE handle and closes it in its destructor, so
// SndfileDecoder's own destructor can be the defaulted one (defined below, where
// Impl is a complete type). The descriptive facts are computed once in open()
// and cached here, keeping the accessors branch-free trivial returns.
struct SndfileDecoder::Impl {
    SNDFILE*      snd      = nullptr;
    int           channels = 0;      // cached for the read() interleave math
    AudioFormat   fmt{};
    SourceInfo    src{};
    std::uint64_t total    = 0;
    bool          seekable = false;

    ~Impl() {
        if (snd) {
            sf_close(snd);
        }
    }
};

// ---------------------------------------------------------------------------
// Factory. Non-throwing: returns a fully-opened decoder or nullptr, never a
// half-open object. On failure error (if provided) carries libsndfile's reason.
std::unique_ptr<IDecoder> SndfileDecoder::open(const std::string& path,
                                               std::string* error) {
    SF_INFO info{};  // zero-initialized; libsndfile fills it on a read open
    SNDFILE* snd = sf_open(path.c_str(), SFM_READ, &info);
    if (!snd) {
        if (error) {
            *error = sf_strerror(nullptr);  // global last-error after open failure
        }
        return nullptr;
    }

    // Defensive: a real WAV/AIFF always reports positive values, but reject a
    // degenerate header rather than carry a zero channel count into read().
    if (info.channels <= 0 || info.samplerate <= 0) {
        if (error) {
            *error = "libsndfile reported a non-positive channel or sample rate";
        }
        sf_close(snd);
        return nullptr;
    }

    // Routing policy: Vorbis and Opus are Ogg-contained codecs that a
    // current libsndfile can decode, but the project routes them to FfmpegDecoder
    // (see the format list in FfmpegDecoder.h). FFmpeg wraps the reference
    // libvorbis/libopus and, through its packet-level metering, reports a live VBR
    // bitrate; libsndfile exposes no compressed-stream bitrate, so served here they
    // would read zero. Decline them so the DecoderFactory cascade falls through to
    // its terminal FFmpeg candidate. Gated on FFmpeg actually being in the build:
    // with no FFmpeg to catch them, declining would leave these files unplayable,
    // so a no-FFmpeg build keeps serving them, at a zero bitrate. Every other
    // subtype, the PCM containers (WAV/AIFF/AU/CAF) and any Ogg/FLAC the native
    // FlacDecoder did not sniff, is still handled here.
#if RAWFORM_HAVE_FFMPEG
    {
        const int subtype = info.format & SF_FORMAT_SUBMASK;
        if (subtype == SF_FORMAT_VORBIS || subtype == SF_FORMAT_OPUS) {
            sf_close(snd);
            if (error) {
                *error = "Vorbis/Opus deferred to FfmpegDecoder";
            }
            return nullptr;
        }
    }
#endif

    // Force normalized float reads so integer PCM maps to [-1, 1) deterministically
    // regardless of how this libsndfile was built. Without this, the integer to
    // float scale would depend on the library's compile-time default, which is
    // exactly the kind of hidden variable the project's deterministic-verification
    // philosophy rules out. This is what makes the test's one-LSB tolerance hold.
    sf_command(snd, SFC_SET_NORM_FLOAT, nullptr, SF_TRUE);

    auto impl      = std::make_unique<Impl>();
    impl->snd      = snd;
    impl->channels = info.channels;
    impl->fmt      = AudioFormat{ .sampleRate = static_cast<std::uint32_t>(info.samplerate),
                                  .channels   = static_cast<std::uint16_t>(info.channels) };
    impl->total    = info.frames > 0 ? static_cast<std::uint64_t>(info.frames) : 0;
    impl->seekable = info.seekable != 0;

    const std::uint16_t bits = bitsForSubtype(info.format);
    impl->src.codec          = codecForSubtype(info.format, bits);
    impl->src.decoder        = DecoderKind::Sndfile;
    impl->src.bitsPerSample  = bits;
    // 64-bit intermediate so the multiply cannot overflow before the divide;
    // for PCM this is the exact data rate (see the SourceInfo comment in Types.h).
    impl->src.bitrateKbps    = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(info.samplerate) *
         static_cast<std::uint64_t>(info.channels)   *
         static_cast<std::uint64_t>(bits)) / 1000u);

    // Private constructor: only this factory can call it, so the object is born
    // valid. make_unique cannot reach a private constructor, hence the raw new
    // wrapped immediately into the owning unique_ptr.
    return std::unique_ptr<IDecoder>(new SndfileDecoder(std::move(impl)));
}

SndfileDecoder::SndfileDecoder(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

// Defaulted here, where Impl is complete, so ~Impl (which closes the handle) is
// visible to the unique_ptr deleter.
SndfileDecoder::~SndfileDecoder() = default;

// ---------------------------------------------------------------------------
// Accessors: trivial reads of the facts cached at open time.
AudioFormat   SndfileDecoder::format()      const { return m_impl->fmt; }
SourceInfo    SndfileDecoder::sourceInfo()  const { return m_impl->src; }
std::uint64_t SndfileDecoder::totalFrames() const { return m_impl->total; }
bool          SndfileDecoder::seekable()    const { return m_impl->seekable; }

// ---------------------------------------------------------------------------
// Decode. Honors the "fill fully, short only at EOS, 0 == EOS" contract by
// accumulating across sf_readf_float calls until the request is satisfied or the
// library returns nothing more. For a regular file libsndfile satisfies the
// request in a single call until the final partial block, but looping costs
// nothing and makes the EOS semantics independent of libsndfile's chunking.
std::size_t SndfileDecoder::read(float* dst, std::size_t frames) {
    if (frames == 0) {
        return 0;
    }
    const auto ch = static_cast<std::size_t>(m_impl->channels);
    std::size_t done = 0;
    while (done < frames) {
        const auto want = static_cast<sf_count_t>(frames - done);
        const sf_count_t got = sf_readf_float(m_impl->snd, dst + done * ch, want);
        if (got <= 0) {
            break;  // 0 is end of stream; a negative would be an error, which we
                    // also surface as a short read so the caller stops cleanly.
        }
        done += static_cast<std::size_t>(got);
    }
    return done;
}

// ---------------------------------------------------------------------------
// Seek to an absolute frame. Returns true only when libsndfile lands exactly
// where asked. An unseekable source short-circuits to false up front; sf_seek
// would fail anyway, but the explicit guard documents the contract at the call.
bool SndfileDecoder::seek(std::uint64_t frame) {
    if (!m_impl->seekable) {
        return false;
    }
    const auto target = static_cast<sf_count_t>(frame);
    const sf_count_t result = sf_seek(m_impl->snd, target, SEEK_SET);
    return result == target;
}

}  // namespace rawform::audio
