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

// FlacDecoder.cpp
//
// libFLAC implementation of the IDecoder contract. Everything that touches
// <FLAC/stream_decoder.h> is confined to this translation unit: the decoder
// handle, the staging buffer, and the libFLAC callbacks all live here, so the
// static library links FLAC::FLAC privately while the header stays free of the
// dependency.
//
// The central design problem this file solves is impedance: libFLAC is a PUSH
// decoder (it calls our write callback once per FLAC frame, handing us a whole
// block of samples), but IDecoder::read() is a PULL contract (the engine asks
// for exactly N frames). The bridge is a staging buffer. read() pumps libFLAC's
// process loop until the staging buffer holds enough, then drains it into the
// caller's destination. Seeking reuses the same machinery: libFLAC's
// seek_absolute lands by firing the write callback once for the frame containing
// the target sample, and we trim the front of that block to start exactly at the
// requested sample.
//
// Live VBR bitrate: FLAC has no per-frame bitrate field, so we
// measure it. FLAC__stream_decoder_get_decode_position reports the byte offset in
// the file; we sample it either side of a single-frame pump in read() and feed
// (bytesConsumed*8, framesProduced/sampleRate) into a LiveBitrateMeter. That is
// the exact compressed size of one FLAC frame over the audio it produced, so the
// trailing-window average is an accurate "current bitrate". The static
// sourceInfo().bitrateKbps stays the whole-file average.

#include "decoders/FlacDecoder.h"

#include "LiveBitrateMeter.h"
#include "rawform/audio/Types.h"

#include <FLAC/stream_decoder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>  // NOLINT: std::uintN_t
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rawform::audio {

// ---------------------------------------------------------------------------
// The pimpl. Owns the libFLAC handle (freed in its destructor so FlacDecoder's
// own destructor can be the defaulted one), the staging buffer that bridges
// push to pull, and the facts cached from STREAMINFO at open time.
struct FlacDecoder::Impl {
    FLAC__StreamDecoder* dec = nullptr;

    // ---- facts cached once from STREAMINFO ----------------------------------
    AudioFormat   fmt{};
    SourceInfo    src{};
    std::uint64_t total      = 0;      // total samples (frames), 0 == unknown
    unsigned      channels   = 0;      // cached for the interleave math
    unsigned      bits       = 0;      // STREAMINFO bits-per-sample (16/24/...)
    float         invScale   = 0.0f;   // 1 / 2^(bits-1): int sample -> [-1, 1)
    bool          seekable   = false;

    // ---- the push-to-pull staging buffer ------------------------------------
    // staging holds interleaved float32 produced by the write callback but not
    // yet handed to read(). stagePos is the read cursor in FLOATS from the
    // front: [stagePos, staging.size()) is what remains. When the cursor reaches
    // the end the buffer is cleared, so it never grows without bound across a
    // long decode (one FLAC block at a time).
    std::vector<float> staging;
    std::size_t        stagePos = 0;

    // Absolute sample number where the most recently staged FLAC frame begins.
    // Captured in the write callback and consulted ONLY right after a seek, to
    // trim the landed block so read() resumes exactly at the requested sample.
    std::uint64_t lastFrameStart = 0;

    // Sticky decode-error flag set by the error callback. read() treats a set
    // flag as end of stream (a short read), matching the "errors surface as a
    // clean stop" rule SndfileDecoder follows for a negative sf_readf return.
    bool errored = false;

    // Set when seek() lands exactly on totalFrames (a "go to end" request, which
    // the contract allows since libsndfile permits seeking to the length).
    // libFLAC rejects seek_absolute(total) as out of range, so we emulate the end
    // position with this flag: read() returns 0 while it is set. Any seek to a
    // real frame clears it.
    bool forcedEos = false;

    // Live VBR bitrate. Fed in read() from the decode-position byte
    // delta across one frame; read back by currentBitrateKbps(). Engine-thread
    // only, like every other field here, so it needs no synchronization.
    LiveBitrateMeter meter;

    ~Impl() {
        if (dec) {
            FLAC__stream_decoder_finish(dec);  // flush internal state, close file
            FLAC__stream_decoder_delete(dec);
        }
    }

    // --- libFLAC callbacks --------------------------------------------------
    // Static members rather than free functions so they may name Impl and reach
    // its fields while Impl stays a fully private nested type (no codec types
    // escape into the header). client_data is the Impl* handed to init_file, so
    // each recovers the decoder state with one cast. Their types match libFLAC's
    // callback typedefs exactly, so open() passes &Impl::<name> with no cast.

    static FLAC__StreamDecoderWriteStatus writeCallback(
        const FLAC__StreamDecoder* /*dec*/, const FLAC__Frame* frame,
        const FLAC__int32* const buffer[], void* client) {
        auto* impl = static_cast<Impl*>(client);

        const unsigned ch    = frame->header.channels;
        const unsigned block = frame->header.blocksize;

        // Record where this block starts, for the post-seek front-trim. libFLAC
        // reports a sample number for a fixed-blocksize stream (the common case)
        // and a frame number otherwise; only the sample-number form is
        // meaningful for the trim, so we derive a sample position from it when
        // available.
        if (frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER) {
            impl->lastFrameStart = frame->header.number.sample_number;
        } else {
            // Variable-blocksize: fall back to frame-number * blocksize, a best
            // effort. Standard encoders emit fixed blocksize, so this branch is
            // effectively unreached for files this engine will see.
            impl->lastFrameStart =
                static_cast<std::uint64_t>(frame->header.number.frame_number) * block;
        }

        // Interleave the planar libFLAC channels into staging as float32. We
        // append at the end; read() drains from stagePos at the front. The int
        // samples are sign-extended to the stream depth already (24-bit lives
        // correctly in the FLAC__int32), so a single multiply by invScale lands
        // them in [-1, 1).
        const std::size_t base = impl->staging.size();
        impl->staging.resize(base + static_cast<std::size_t>(block) * ch);
        float* out = impl->staging.data() + base;
        for (unsigned i = 0; i < block; ++i) {
            for (unsigned c = 0; c < ch; ++c) {
                out[static_cast<std::size_t>(i) * ch + c] =
                    static_cast<float>(buffer[c][i]) * impl->invScale;
            }
        }
        return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    }

    static void metadataCallback(const FLAC__StreamDecoder* /*dec*/,
                                 const FLAC__StreamMetadata* meta, void* client) {
        auto* impl = static_cast<Impl*>(client);
        if (meta->type != FLAC__METADATA_TYPE_STREAMINFO) {
            return;  // we only care about STREAMINFO; ignore VORBIS_COMMENT etc.
        }
        const auto& si = meta->data.stream_info;
        impl->channels = si.channels;
        impl->bits     = si.bits_per_sample;
        impl->fmt      = AudioFormat{ .sampleRate = static_cast<std::uint32_t>(si.sample_rate),
                                      .channels   = static_cast<std::uint16_t>(si.channels) };
        impl->total    = si.total_samples;  // 0 means unknown (streamed FLAC)
        // 1 / 2^(bits-1) as the int-to-normalized-float scale. Generic over
        // depth, so 16, 24, and the rarer 8/12/20-bit FLAC all map correctly
        // with no special case; 24-bit sign extension is already done by libFLAC.
        if (si.bits_per_sample > 0) {
            const std::int64_t fullScale =
                std::int64_t{1} << (si.bits_per_sample - 1);
            impl->invScale =
                static_cast<float>(1.0 / static_cast<double>(fullScale));
        }
    }

    static void errorCallback(const FLAC__StreamDecoder* /*dec*/,
                              FLAC__StreamDecoderErrorStatus /*status*/,
                              void* client) {
        // Mid-stream decode error: flag it so read() stops cleanly on the next
        // pump. We do not try to recover or resync; the contract turns this into
        // a short read, exactly like SndfileDecoder surfacing a negative read as
        // EOS.
        static_cast<Impl*>(client)->errored = true;
    }
};

namespace {

// File size in bytes via a binary ifstream opened at end. Used only for the
// average-bitrate figure; a failure (returns 0) simply leaves bitrate at 0,
// which the status line renders as unknown. No <filesystem> dependency.
std::uint64_t fileSizeBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return 0;
    }
    const std::streamoff end = f.tellg();
    return end > 0 ? static_cast<std::uint64_t>(end) : 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Factory. Non-throwing: returns a fully-opened decoder or nullptr, never a
// half-open object. STREAMINFO is read here (process_until_end_of_metadata) so
// format(), totalFrames(), and sourceInfo() are all valid before any audio is
// pulled.
std::unique_ptr<IDecoder> FlacDecoder::open(const std::string& path,
                                            std::string* error) {
    auto impl = std::make_unique<Impl>();

    impl->dec = FLAC__stream_decoder_new();
    if (!impl->dec) {
        if (error) {
            *error = "could not allocate a libFLAC stream decoder";
        }
        return nullptr;
    }

    // We do not need MD5 verification (it would buffer the whole stream to hash
    // it at the end); we only decode forward and seek, so disable it.
    FLAC__stream_decoder_set_md5_checking(impl->dec, false);

    const FLAC__StreamDecoderInitStatus st = FLAC__stream_decoder_init_file(
        impl->dec, path.c_str(), &Impl::writeCallback, &Impl::metadataCallback,
        &Impl::errorCallback, impl.get());
    if (st != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        if (error) {
            *error = std::string("libFLAC init failed: ") +
                     FLAC__StreamDecoderInitStatusString[st];
        }
        return nullptr;  // Impl dtor deletes the handle
    }

    // Drive the decoder through every metadata block so metadataCallback has run
    // and STREAMINFO is cached. This positions the decoder at the first audio
    // frame, so a fresh decoder reads from sample 0 with an empty staging buffer.
    if (!FLAC__stream_decoder_process_until_end_of_metadata(impl->dec)) {
        if (error) {
            *error = "libFLAC failed to read stream metadata";
        }
        return nullptr;
    }

    // A real FLAC always carries a STREAMINFO with positive rate and channels.
    // Reject a degenerate header rather than carry a zero channel count into the
    // interleave math.
    if (!impl->fmt.isValid() || impl->channels == 0 || impl->bits == 0) {
        if (error) {
            *error = "FLAC stream is missing a usable STREAMINFO";
        }
        return nullptr;
    }

    // SourceInfo. FLAC is lossless, so there is no constant bitrate; we report
    // the AVERAGE rate (file bytes over duration).
    // 0 when the length is unknown, since then there is no duration to divide by.
    impl->src.codec         = Codec::Flac;
    impl->src.decoder       = DecoderKind::Flac;
    impl->src.bitsPerSample = static_cast<std::uint16_t>(impl->bits);
    impl->src.bitrateKbps   = 0;
    if (impl->total > 0 && impl->fmt.sampleRate > 0) {
        const std::uint64_t bytes = fileSizeBytes(path);
        if (bytes > 0) {
            const double seconds =
                static_cast<double>(impl->total) / impl->fmt.sampleRate;
            impl->src.bitrateKbps = static_cast<std::uint32_t>(
                (static_cast<double>(bytes) * 8.0 / seconds) / 1000.0);
        }
    }

    // Seed the live meter with the file-average so currentBitrateKbps() reads
    // sensibly before the first frame is measured and right after a seek.
    impl->meter.reset(impl->src.bitrateKbps);

    // A file-backed libFLAC decoder is always seekable; seek() reports libFLAC's
    // own result per call, so an actual failure still returns false there.
    impl->seekable = true;

    return std::unique_ptr<IDecoder>(new FlacDecoder(std::move(impl)));
}

FlacDecoder::FlacDecoder(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

// Defaulted here, where Impl is complete, so ~Impl (which finishes and deletes
// the libFLAC handle) is visible to the unique_ptr deleter.
FlacDecoder::~FlacDecoder() = default;

// ---------------------------------------------------------------------------
// Accessors: trivial reads of the facts cached at open time.
AudioFormat   FlacDecoder::format()      const { return m_impl->fmt; }
SourceInfo    FlacDecoder::sourceInfo()  const { return m_impl->src; }
std::uint64_t FlacDecoder::totalFrames() const { return m_impl->total; }
bool          FlacDecoder::seekable()    const { return m_impl->seekable; }

// Live VBR bitrate: the meter's trailing-window average, fed from read() below.
std::uint32_t FlacDecoder::currentBitrateKbps() const {
    return m_impl->meter.value();
}

// ---------------------------------------------------------------------------
// Decode. Drains the staging buffer first; when it runs dry, pumps libFLAC for
// one more frame and tries again. Honors "fill fully, short only at EOS, 0 ==
// EOS": the loop ends only when the request is satisfied or the decoder reaches
// end of stream / an error with nothing left to hand back.
std::size_t FlacDecoder::read(float* dst, std::size_t frames) {
    if (frames == 0) {
        return 0;
    }
    if (m_impl->forcedEos) {
        return 0;  // positioned exactly at the end by a prior seek(totalFrames)
    }
    const std::size_t ch   = m_impl->channels;
    std::size_t       done = 0;

    while (done < frames) {
        // 1) Hand over whatever is already staged.
        if (m_impl->stagePos < m_impl->staging.size()) {
            const std::size_t availFloats = m_impl->staging.size() - m_impl->stagePos;
            const std::size_t availFrames = availFloats / ch;
            const std::size_t take        = std::min(frames - done, availFrames);
            const float* srcPtr = m_impl->staging.data() + m_impl->stagePos;
            float*       dstPtr = dst + done * ch;
            for (std::size_t i = 0; i < take * ch; ++i) {
                dstPtr[i] = srcPtr[i];
            }
            m_impl->stagePos += take * ch;
            done             += take;
            if (m_impl->stagePos >= m_impl->staging.size()) {
                m_impl->staging.clear();
                m_impl->stagePos = 0;
            }
            continue;
        }

        // 2) Staging is empty. A prior error or a terminal decoder state means
        //    end of stream: stop and return the short count.
        if (m_impl->errored) {
            break;
        }
        const FLAC__StreamDecoderState state =
            FLAC__stream_decoder_get_state(m_impl->dec);
        if (state >= FLAC__STREAM_DECODER_END_OF_STREAM) {
            break;  // END_OF_STREAM or any of the error states
        }

        // 3) Pump exactly one frame. The write callback appends to staging; a
        //    false return is a hard error, which we also turn into a clean stop.
        //
        //    Bracket the pump with the decode-position byte offset so
        //    we know how many compressed bytes this one frame cost, and pair that
        //    with the samples it produced. get_decode_position can decline (a
        //    false return) in some states; when it does we simply skip the
        //    measurement for this frame and the meter holds its last value.
        FLAC__uint64 posBefore   = 0;
        const bool   havePos     = (FLAC__stream_decoder_get_decode_position(
                                       m_impl->dec, &posBefore) != 0);
        const std::size_t stageBefore = m_impl->staging.size();

        if (!FLAC__stream_decoder_process_single(m_impl->dec)) {
            break;
        }

        if (havePos && m_impl->fmt.sampleRate > 0) {
            FLAC__uint64 posAfter = 0;
            if (FLAC__stream_decoder_get_decode_position(m_impl->dec, &posAfter) != 0
                && posAfter > posBefore) {
                const std::size_t framesGot =
                    (m_impl->staging.size() - stageBefore) / ch;
                if (framesGot > 0) {
                    const double seconds =
                        static_cast<double>(framesGot) / m_impl->fmt.sampleRate;
                    m_impl->meter.push(
                        static_cast<double>(posAfter - posBefore) * 8.0, seconds);
                }
            }
        }
    }
    return done;
}

// ---------------------------------------------------------------------------
// Seek to an absolute frame. libFLAC's seek_absolute lands by firing the write
// callback once for the FLAC frame that contains the target sample, so after the
// call the staging buffer holds that whole block. We then trim the front so the
// next read() begins exactly at the requested sample, not at the block boundary.
bool FlacDecoder::seek(std::uint64_t frame) {
    if (!m_impl->seekable) {
        return false;
    }

    // Start clean: drop anything staged so the only block present afterwards is
    // the one the seek lands on, which keeps lastFrameStart unambiguous.
    m_impl->staging.clear();
    m_impl->stagePos = 0;
    m_impl->errored  = false;

    // Seek-to-end parity. When the length is known and the target is at or past
    // it, libFLAC's seek_absolute would fail (total is one past the last valid
    // sample), but the contract treats seek(totalFrames) as a valid "go to EOS"
    // (libsndfile allows seeking to the length). Emulate it: an exact-end seek
    // marks EOS and succeeds; strictly past the end is a genuine failure.
    if (m_impl->total > 0 && frame >= m_impl->total) {
        if (frame == m_impl->total) {
            m_impl->forcedEos = true;
            // Discard the pre-seek window; at EOS the meter reads the nominal.
            m_impl->meter.reset(m_impl->src.bitrateKbps);
            return true;
        }
        m_impl->forcedEos = false;
        return false;
    }
    m_impl->forcedEos = false;

    if (!FLAC__stream_decoder_seek_absolute(m_impl->dec, frame)) {
        // A failed seek leaves the decoder in SEEK_ERROR; flush clears that so
        // the object is not permanently dead, though the position is now
        // unspecified (which the IDecoder contract allows on a false return).
        FLAC__stream_decoder_flush(m_impl->dec);
        m_impl->staging.clear();
        m_impl->stagePos = 0;
        return false;
    }

    // Discard the pre-seek window so the live readout reflects the new position
    // rather than blending across the jump.
    m_impl->meter.reset(m_impl->src.bitrateKbps);

    // Trim the landed block down to the requested sample. The write callback ran
    // during the seek, so staging begins at lastFrameStart <= frame; advance the
    // read cursor by the difference so read() resumes at `frame`.
    if (!m_impl->staging.empty() && m_impl->lastFrameStart <= frame) {
        const std::uint64_t skipFrames = frame - m_impl->lastFrameStart;
        const std::size_t   skipFloats =
            static_cast<std::size_t>(skipFrames) * m_impl->channels;
        m_impl->stagePos = std::min(skipFloats, m_impl->staging.size());
    }
    return true;
}

}  // namespace rawform::audio
