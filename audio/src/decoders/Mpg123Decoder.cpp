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

// Mpg123Decoder.cpp
//
// libmpg123 implementation of the IDecoder contract. Everything that touches
// <mpg123.h> is confined to this translation unit: the handle and the cached
// facts live in Impl, so the static library links MPG123::libmpg123 privately
// while the header stays free of the dependency.
//
// libmpg123 is already a pull decoder, so unlike FlacDecoder there is no staging
// buffer: open() forces float32 output at the source's native rate and channel
// count and turns on gapless trimming, after which read() is a thin loop over
// mpg123_read. The two MP3-specific decisions encoded here:
//
//   - Gapless: MPG123_GAPLESS trims the encoder delay and end padding LAME and
//     friends write, so totalFrames() and the playhead reflect the real audio,
//     not the codec's framing slop.
//
//   - Length without a full scan: mpg123_length() reads the VBR/Xing header when
//     present (fast and accurate) and estimates otherwise; we deliberately do
//     NOT call mpg123_scan(), which would stream the whole file on open just to
//     index it. The cost is small seek imprecision on headerless VBR, which the
//     MP3 path accepts on purpose (lossy seek is inherently approximate anyway).
//
// Live VBR bitrate: mpg123_info() reports the bitrate of the most recently
// decoded frame, which for a VBR file changes frame to frame. After each
// read() we feed that figure into a LiveBitrateMeter (see read()), and
// currentBitrateKbps() returns the meter's trailing-window average. The static
// sourceInfo().bitrateKbps stays the first frame's value, the nominal.

#include "decoders/Mpg123Decoder.h"

#include "LiveBitrateMeter.h"
#include "rawform/audio/Types.h"

#include <mpg123.h>

#include <sys/types.h>  // off_t, the width mpg123_length/seek speak in

#include <cstddef>
#include <cstdint>  // NOLINT: std::uintN_t
#include <cstdio>  // SEEK_SET
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace rawform::audio {

namespace {

// The one unavoidable process global in the codec libraries. Older libmpg123
// (pre-1.32) requires a single mpg123_init() before any handle is created;
// we guard it with call_once and NEVER call mpg123_exit(), treating the init as
// process-lifetime (a matching exit would create a teardown-ordering hazard for
// no benefit). libmpg123 1.32 and newer (API version >= 47) made init/exit
// unnecessary and deprecated them, so on those builds we skip the call entirely,
// which also keeps the compile free of a deprecation warning. Either way this
// returns true when the library is ready to create handles.
bool ensureMpg123Init() {
#if defined(MPG123_API_VERSION) && MPG123_API_VERSION >= 47
    return true;  // init not needed on mpg123 >= 1.32; handles self-initialize
#else
    static std::once_flag once;
    static bool           ok = false;
    std::call_once(once, [] { ok = (mpg123_init() == MPG123_OK); });
    return ok;
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// The pimpl. Owns the mpg123 handle (closed and deleted in its destructor so
// Mpg123Decoder's own destructor can be defaulted), and caches the facts read at
// open time so every const accessor stays a trivial return.
struct Mpg123Decoder::Impl {
    mpg123_handle* mh       = nullptr;
    AudioFormat    fmt{};
    SourceInfo     src{};
    std::uint64_t  total    = 0;   // gapless-trimmed length in frames, 0 unknown
    int            channels = 0;   // cached for the read() frame<->byte math
    bool           seekable = false;

    // Live VBR bitrate. Fed per read() from mpg123_info()'s current
    // frame bitrate; read back by currentBitrateKbps(). Engine-thread only, like
    // every other field here, so it needs no synchronization.
    LiveBitrateMeter meter;

    ~Impl() {
        if (mh) {
            mpg123_close(mh);
            mpg123_delete(mh);
        }
    }
};

// ---------------------------------------------------------------------------
// Factory. Non-throwing: returns a fully-opened decoder or nullptr, never a
// half-open object. Forces float32 output at the native rate/channels and leaves
// the decoder positioned at frame 0.
std::unique_ptr<IDecoder> Mpg123Decoder::open(const std::string& path,
                                              std::string* error) {
    if (!ensureMpg123Init()) {
        if (error) {
            *error = "libmpg123 process initialization failed";
        }
        return nullptr;
    }

    int            mperr = MPG123_OK;
    mpg123_handle* mh    = mpg123_new(nullptr, &mperr);
    if (!mh) {
        if (error) {
            *error = std::string("mpg123_new failed: ") +
                     mpg123_plain_strerror(mperr);
        }
        return nullptr;
    }

    // Own the handle immediately, so every early return below frees it through
    // the Impl destructor with no manual cleanup at each exit.
    auto impl = std::make_unique<Impl>();
    impl->mh  = mh;

    // Gapless trimming on; force float decoding; quiet (no diagnostics to
    // stderr). FORCE_FLOAT is belt-and-suspenders alongside the explicit format
    // lock below, so the default path is float32 even before the lock applies.
    mpg123_param(mh, MPG123_ADD_FLAGS,
                 MPG123_GAPLESS | MPG123_FORCE_FLOAT | MPG123_QUIET, 0.0);

    if ((mperr = mpg123_open(mh, path.c_str())) != MPG123_OK) {
        if (error) {
            *error = std::string("mpg123_open failed: ") + mpg123_strerror(mh);
        }
        return nullptr;
    }

    // getformat reads the first frame header and reports the source's native
    // rate, channel count, and default encoding. We discard the encoding and
    // lock our own (float32) below.
    long rate     = 0;
    int  channels = 0;
    int  enc      = 0;
    if ((mperr = mpg123_getformat(mh, &rate, &channels, &enc)) != MPG123_OK) {
        if (error) {
            *error = std::string("mpg123_getformat failed: ") + mpg123_strerror(mh);
        }
        return nullptr;
    }
    if (rate <= 0 || channels <= 0) {
        if (error) {
            *error = "libmpg123 reported a non-positive rate or channel count";
        }
        return nullptr;
    }

    // Lock the output format to float32 at the native rate/channels. Clearing all
    // formats first and allowing exactly one means libmpg123 never silently hands
    // back a different encoding mid-stream.
    mpg123_format_none(mh);
    if ((mperr = mpg123_format(mh, rate, channels, MPG123_ENC_FLOAT_32))
            != MPG123_OK) {
        if (error) {
            *error = std::string("mpg123_format(float32) failed: ") +
                     mpg123_strerror(mh);
        }
        return nullptr;
    }

    impl->channels = channels;
    impl->fmt      = AudioFormat{ .sampleRate = static_cast<std::uint32_t>(rate),
                                  .channels   = static_cast<std::uint16_t>(channels) };

    // Length in (gapless-trimmed) frames. Xing/Info/VBRI header based when
    // present, an estimate otherwise; negative means unknown, which we report as
    // 0 per the IDecoder contract. No mpg123_scan(): see the file header.
    const off_t len = mpg123_length(mh);
    impl->total     = len > 0 ? static_cast<std::uint64_t>(len) : 0;

    // Bitrate from the parsed frame header: exact for CBR, the first frame's
    // value for VBR (lossy has no single true bitrate; this is the nominal the
    // status line shows). bitsPerSample stays 0, the lossy convention.
    mpg123_frameinfo fr{};
    std::uint32_t    kbps = 0;
    if (mpg123_info(mh, &fr) == MPG123_OK && fr.bitrate > 0) {
        kbps = static_cast<std::uint32_t>(fr.bitrate);
    }
    impl->src.codec         = Codec::Mp3;
    impl->src.decoder       = DecoderKind::Mpg123;
    impl->src.bitsPerSample = 0;
    impl->src.bitrateKbps   = kbps;

    // Seed the live meter with the nominal so currentBitrateKbps() reads sensibly
    // before the first read() and right after a seek.
    impl->meter.reset(kbps);

    // Probe seekability and, as a side effect, rewind to frame 0: mpg123_length
    // and mpg123_info may have moved the read position while reading headers, so
    // this both answers seekable() and guarantees the first read() starts at 0.
    impl->seekable = (mpg123_seek(mh, 0, SEEK_SET) >= 0);

    return std::unique_ptr<IDecoder>(new Mpg123Decoder(std::move(impl)));
}

Mpg123Decoder::Mpg123Decoder(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

// Defaulted here, where Impl is complete, so ~Impl (which closes and deletes the
// handle) is visible to the unique_ptr deleter.
Mpg123Decoder::~Mpg123Decoder() = default;

// ---------------------------------------------------------------------------
// Accessors: trivial reads of the facts cached at open time.
AudioFormat   Mpg123Decoder::format()      const { return m_impl->fmt; }
SourceInfo    Mpg123Decoder::sourceInfo()  const { return m_impl->src; }
std::uint64_t Mpg123Decoder::totalFrames() const { return m_impl->total; }
bool          Mpg123Decoder::seekable()    const { return m_impl->seekable; }

// Live VBR bitrate: the meter's trailing-window average, fed from read() below.
std::uint32_t Mpg123Decoder::currentBitrateKbps() const {
    return m_impl->meter.value();
}

// ---------------------------------------------------------------------------
// Decode. mpg123_read fills as many bytes as we ask while data remains, so the
// loop accumulates until the request is satisfied or the stream ends. We honor
// "fill fully, short only at EOS, 0 == EOS" by converting the byte count back to
// whole frames at the end and stopping on MPG123_DONE, on a hard error (surfaced
// as a clean short read), or on a no-progress return.
std::size_t Mpg123Decoder::read(float* dst, std::size_t frames) {
    if (frames == 0) {
        return 0;
    }
    const auto ch                = static_cast<std::size_t>(m_impl->channels);
    const std::size_t wantBytes  = frames * ch * sizeof(float);
    auto*             out        = reinterpret_cast<unsigned char*>(dst);
    std::size_t       totalBytes = 0;

    while (totalBytes < wantBytes) {
        std::size_t got = 0;
        const int   err = mpg123_read(m_impl->mh, out + totalBytes,
                                      wantBytes - totalBytes, &got);
        totalBytes += got;

        if (err == MPG123_OK) {
            if (got == 0) {
                break;  // no progress with no error: defensive stop
            }
            continue;  // partial fill, keep going
        }
        if (err == MPG123_NEW_FORMAT) {
            // Cannot happen with a single locked format, but if libmpg123 ever
            // signals it, the already-copied bytes are valid; stop if it carried
            // no data so we never spin.
            if (got == 0) {
                break;
            }
            continue;
        }
        // MPG123_DONE (end of stream) or any error: stop. Any bytes returned on
        // this final call are already counted in totalBytes.
        break;
    }

    const std::size_t framesGot = totalBytes / (ch * sizeof(float));

    // Feed the live meter. mpg123_info() reports the bitrate of the
    // most recently decoded frame; for a VBR file this changes as we move through
    // the stream. A read() may span a few MPEG frames, so we attribute this
    // chunk's whole duration to that latest frame's bitrate, a fine approximation
    // the meter's trailing window smooths out across calls. CBR simply reports a
    // steady figure.
    if (framesGot > 0 && m_impl->fmt.sampleRate > 0) {
        mpg123_frameinfo fr{};
        if (mpg123_info(m_impl->mh, &fr) == MPG123_OK && fr.bitrate > 0) {
            const double seconds =
                static_cast<double>(framesGot) / m_impl->fmt.sampleRate;
            m_impl->meter.push(static_cast<double>(fr.bitrate) * 1000.0 * seconds,
                               seconds);
        }
    }

    return framesGot;
}

// ---------------------------------------------------------------------------
// Seek to an absolute frame. mpg123_seek returns the achieved sample offset, or
// a negative value on failure. For VBR without a full scan the achieved offset
// can differ slightly from the request; the MP3 path accepts that imprecision,
// so any non-negative result is a success. A negative result leaves the
// position unspecified, which the IDecoder contract allows on a false return.
bool Mpg123Decoder::seek(std::uint64_t frame) {
    if (!m_impl->seekable) {
        return false;
    }
    const off_t result = mpg123_seek(m_impl->mh,
                                     static_cast<off_t>(frame),
                                     SEEK_SET);
    if (result < 0) {
        return false;
    }
    // Discard the pre-seek window so the live readout reflects the new position
    // rather than blending across the jump.
    m_impl->meter.reset(m_impl->src.bitrateKbps);
    return true;
}

}  // namespace rawform::audio
