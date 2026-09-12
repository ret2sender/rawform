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

// FfmpegDecoder.cpp
//
// FFmpeg implementation of the IDecoder contract, the terminal fallback.
// Everything that touches a libav* header is confined to this translation unit:
// the format context, the codec context, the resampler, the reusable frame and
// packet, and the send/receive pump all live here, so the static library links
// the four FFmpeg targets privately while the header stays free of the
// dependency. The whole file is compiled only when RAWFORM_HAVE_FFMPEG == 1; the
// build leaves it out entirely otherwise (see CMakeLists.txt), so a native-only
// build never references a libav* symbol.
//
// Two design problems this file solves, both already familiar from the other
// decoders:
//
//   1. Push-shaped output bridged to a pull contract. avcodec_receive_frame
//      hands back one decoded AVFrame at a time, its length chosen by the codec,
//      while IDecoder::read() asks for exactly N frames. The bridge is the same
//      staging buffer FlacDecoder uses: read() drains what is staged, and when it
//      runs dry it pumps one more decoded frame, converts it to interleaved
//      float32, and appends it. The "fill fully, short only at EOS, 0 == EOS"
//      rule falls out of that loop unchanged.
//
//   2. Many native sample formats bridged to our one canonical format. FFmpeg
//      decoders emit planar or packed samples in s16/s32/fltp/dbl and so on.
//      libswresample converts each frame to PACKED float32. It is configured to
//      convert the sample format and the channel LAYOUT only: the output rate is
//      pinned equal to the input rate (never resampled) and the output layout is
//      pinned equal to the input layout (never downmixed), so the bit-perfect,
//      native-rate, native-channel invariant holds exactly as for sndfile, FLAC,
//      and mpg123. With equal in/out rates and a packed-float target, swresample
//      runs one-to-one with zero internal delay, which keeps the frame-count math
//      exact and means there is no resampler tail to drain.
//
// Seek mirrors FlacDecoder's shape: a raw seek lands at or before the target, we
// flush the decoder, then decode-and-discard forward to the exact target frame
// and front-trim the landing block so read() resumes at `frame`. Position is
// frame-accurate; it is NOT bit-exact for lossy codecs, because inter-frame
// filter state differs across the seek boundary (the same caveat libmpg123 has).

#include "decoders/FfmpegDecoder.h"

#include "LiveBitrateMeter.h"
#include "rawform/audio/Types.h"

// The exact-duration helper for index-less raw streams (raw AC3/DTS/ADTS) was
// lifted out of this file so the metadata probe and the decoder compute the same
// length and cannot drift. It is a PRIVATE engine header that pulls the libav*
// headers it needs itself; including it here is safe because this whole TU is
// already inside the RAWFORM_HAVE_FFMPEG guard.
#include "decoders/FfmpegDuration.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace rawform::audio {

namespace {

// ---------------------------------------------------------------------------
// Process-wide log quieting. FFmpeg writes warnings and progress chatter to
// stderr by default ("Estimating duration from bitrate" and similar), which
// would scribble over the CLI status line and the test output. We drop the level
// to ERROR once, the same once-only process global pattern Mpg123Decoder uses for
// mpg123_init: genuine errors still surface, the noise does not. No registration
// call is needed on modern FFmpeg (av_register_all was removed in 4.0), so this
// is the only global the decoder touches.
void quietFfmpegLogOnce() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

// Human-readable libav* error string for the open-path diagnostics.
std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

// Map an AVCodecID to our descriptive Codec tag. The named families are the ones
// the status line knows how to label; everything else is Other (it still plays,
// the label is just generic). FLAC and MP3 map back to the existing entries so a
// build that for some reason decodes one through FFmpeg still reports it
// truthfully rather than as Other; in a normal build the native cascade catches
// those long before FFmpeg is reached.
Codec mapCodec(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_AAC_LATM:   return Codec::Aac;
        case AV_CODEC_ID_AC3:
        case AV_CODEC_ID_EAC3:       return Codec::Ac3;
        case AV_CODEC_ID_DTS:        return Codec::Dts;
        case AV_CODEC_ID_OPUS:       return Codec::Opus;
        case AV_CODEC_ID_VORBIS:     return Codec::Vorbis;
        case AV_CODEC_ID_ALAC:       return Codec::Alac;
        case AV_CODEC_ID_WMAV1:
        case AV_CODEC_ID_WMAV2:
        case AV_CODEC_ID_WMAPRO:
        case AV_CODEC_ID_WMALOSSLESS:
        case AV_CODEC_ID_WMAVOICE:   return Codec::Wma;
        case AV_CODEC_ID_FLAC:       return Codec::Flac;
        case AV_CODEC_ID_MP1:
        case AV_CODEC_ID_MP2:
        case AV_CODEC_ID_MP3:        return Codec::Mp3;
        default:                     return Codec::Other;
    }
}

// Container bit depth for the status line, following the engine rule: a real
// depth for lossless codecs, 0 for lossy ones (there is no meaningful PCM
// container depth behind a lossy stream). We ask FFmpeg's own codec descriptor
// whether the codec is lossless and not lossy, then read the raw sample depth;
// this generalizes ALAC and WMA Lossless without enumerating them by hand, and
// returns 0 for every lossy family.
std::uint16_t losslessDepth(AVCodecID id, const AVCodecParameters* par) {
    const AVCodecDescriptor* desc = avcodec_descriptor_get(id);
    const bool lossless = desc != nullptr &&
                          (desc->props & AV_CODEC_PROP_LOSSLESS) != 0 &&
                          (desc->props & AV_CODEC_PROP_LOSSY) == 0;
    if (!lossless) {
        return 0;
    }
    int bits = par->bits_per_raw_sample;
    if (bits <= 0) {
        bits = par->bits_per_coded_sample;
    }
    return bits > 0 ? static_cast<std::uint16_t>(bits) : 0;
}

// The exact-duration demux scan (scanExactFrames) lives in
// decoders/FfmpegDuration.h as detail::scanExactFrames, shared with the
// metadata probe so both compute identical lengths for raw AC3/DTS. The call
// site below uses the detail:: name.

}  // namespace

// ---------------------------------------------------------------------------
// The pimpl. Owns every FFmpeg object (freed in its destructor so FfmpegDecoder's
// own destructor stays the defaulted one), the staging buffer that bridges the
// variable frame size to read(), and the facts cached at open time. The running
// outputPos is the absolute frame index the next read() will return; it is the
// backbone of both the post-seek front-trim and the EOS bookkeeping.
struct FfmpegDecoder::Impl {
    // ---- FFmpeg objects -----------------------------------------------------
    AVFormatContext* fmtCtx      = nullptr;
    AVCodecContext*  codecCtx    = nullptr;
    SwrContext*      swr         = nullptr;
    AVFrame*         frame       = nullptr;  // reused across receive calls
    AVPacket*        pkt         = nullptr;  // reused across read calls
    int              streamIndex = -1;
    AVRational       timeBase{0, 1};         // the audio stream's time base

    // The audio stream's start_time in stream time-base units, 0 when the
    // container reports none (AV_NOPTS_VALUE). Offset-timestamped containers
    // (transport streams, edit-listed MP4s, some Ogg chains) begin every pts at
    // this value rather than 0, so seek() must ADD it when rescaling a sample
    // index into pts space and SUBTRACT it when rescaling a frame's pts back to
    // a sample index; without both, every seek on such a container lands early
    // by exactly this many samples and the frame-accuracy contract silently
    // breaks. Music-library files usually carry 0, which is why the skew
    // never showed in listening tests; the long tail is where it bites. Cached
    // at open; duration math is untouched (st->duration is a span, not a
    // position).
    std::int64_t     startTime = 0;

    // ---- facts cached once at open -----------------------------------------
    AudioFormat   audioFormat{};
    SourceInfo    src{};
    std::uint64_t total    = 0;     // frames, 0 == unknown (or estimate, lossy)
    unsigned      channels = 0;     // cached for the interleave math
    bool          seekable_ = false;

    // ---- decode/pull state --------------------------------------------------
    // staging holds interleaved float32 produced by the converter but not yet
    // handed to read(); stagePos is the read cursor in FLOATS from the front,
    // exactly the FlacDecoder layout. outputPos is the absolute frame index of
    // staging's front, so it is also the index the next read() returns.
    std::vector<float> staging;
    std::size_t        stagePos  = 0;
    std::uint64_t      outputPos = 0;

    bool swrReady  = false;  // resampler configured from the first decoded frame
    bool eof       = false;  // decoder fully drained (receive returned EOF)
    bool draining  = false;  // the NULL flush packet has been sent

    // True when pkt still holds a packet the codec refused with
    // AVERROR(EAGAIN); supplyPacket re-sends it before reading further, so a
    // refused packet is never dropped. Cleared on acceptance, on hard send
    // failure, and on seek (a retained packet is stale after the flush).
    bool resend    = false;
    bool forcedEos = false;  // positioned exactly at the end by a prior seek

    // Live VBR bitrate. Fed in read() from the compressed bytes
    // accumulated per packet (see pendingBytes) paired with the decoded frame's
    // sample count; read back by currentBitrateKbps(). Engine-thread only, like
    // every other field here, so it needs no synchronization.
    LiveBitrateMeter meter;

    // Compressed bytes consumed since the last meter push. supplyPacket()
    // accumulates pkt->size here; read() pairs the total with the decoded frame's
    // sample count and pushes it, then clears it. Pairing bytes with the OUTPUT
    // sample count (always known) rather than pkt->duration makes the meter work
    // for codecs whose demuxer leaves packet durations unset (Vorbis, sometimes
    // Opus), which otherwise read 0.
    std::uint64_t pendingBytes = 0;

    // First-push guard for the meter. The decoder primes by buffering several
    // packets before it emits the first frame, so that frame's pendingBytes lumps
    // those packets together and reads as an inflated one-off spike. We discard
    // that first push after every reset (open and each seek) so the trailing
    // window fills with clean one-packet-per-frame samples instead, which removes
    // the visible startup swing on constant-rate streams (e.g. WMA). Set true once
    // the priming frame has been skipped.
    bool meterPrimed = false;

    // Re-seed the meter to the nominal and clear both the byte accumulator and the
    // priming guard. Used at open and at every seek so the live readout restarts
    // cleanly rather than blending across the discontinuity.
    void resetMeter() {
        meter.reset(src.bitrateKbps);
        pendingBytes = 0;
        meterPrimed  = false;
    }

    ~Impl() {
        if (swr)    { swr_free(&swr); }
        if (codecCtx) { avcodec_free_context(&codecCtx); }
        if (fmtCtx) { avformat_close_input(&fmtCtx); }
        if (frame)  { av_frame_free(&frame); }
        if (pkt)    { av_packet_free(&pkt); }
    }

    void clearStaging() {
        staging.clear();
        stagePos = 0;
    }

    // Configure the resampler from the FIRST decoded frame, which is the
    // authoritative source of the actual sample format and layout (these match
    // the codec context for every real file, but the frame is what swr_convert
    // will be fed, so we read its parameters to be exact). Output is pinned to
    // the input: same rate (no resample), same layout (no downmix), packed
    // float32 target. A frame that reports an unspecified channel order (some
    // raw AAC streams do) gets a default positional layout for its channel count,
    // which preserves the count without inventing a remix.
    bool configureSwr() {
        AVChannelLayout inLayout;
        if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC ||
            frame->ch_layout.nb_channels == 0) {
            av_channel_layout_default(&inLayout, static_cast<int>(channels));
        } else {
            av_channel_layout_copy(&inLayout, &frame->ch_layout);
        }
        AVChannelLayout outLayout;
        av_channel_layout_copy(&outLayout, &inLayout);  // never downmix

        const int rate = frame->sample_rate > 0
                             ? frame->sample_rate
                             : static_cast<int>(audioFormat.sampleRate);

        SwrContext* s = nullptr;
        const int rc  = swr_alloc_set_opts2(
            &s, &outLayout, AV_SAMPLE_FMT_FLT, rate,  // out: packed float, in rate
            &inLayout, static_cast<AVSampleFormat>(frame->format), rate,  // in
            0, nullptr);

        av_channel_layout_uninit(&inLayout);
        av_channel_layout_uninit(&outLayout);

        if (rc < 0 || s == nullptr) {
            return false;
        }
        if (swr_init(s) < 0) {
            swr_free(&s);
            return false;
        }
        swr      = s;
        swrReady = true;
        return true;
    }

    // Drive the send-packet / receive-frame state machine until ONE decoded
    // frame is available in `frame`, returning true. Returns false when the
    // stream is fully drained or an unrecoverable decode error occurs (both
    // surface to read() as a clean short read, the engine-wide rule). The caller
    // owns the frame on a true return and must unref it (directly, or via
    // appendCurrentFrame which leaves the unref to the caller).
    //
    // The loop receives first and only feeds a packet when the decoder reports
    // it is hungry (EAGAIN). Because a packet is sent only right after the
    // decoder said it had no output queued, a send-EAGAIN is nearly impossible
    // for audio codecs; supplyPacket still handles it by retaining the packet
    // for resend rather than unreffing it, so nothing is ever dropped.
    bool receiveOneFrame() {
        if (eof) {
            return false;
        }
        for (;;) {
            const int r = avcodec_receive_frame(codecCtx, frame);
            if (r == 0) {
                return true;  // a frame is ready in `frame`
            }
            if (r == AVERROR_EOF) {
                eof = true;
                return false;
            }
            if (r != AVERROR(EAGAIN)) {
                eof = true;  // genuine decode error: stop cleanly, like the others
                return false;
            }
            // EAGAIN: the decoder needs input. Supply one packet, or enter drain.
            if (!supplyPacket()) {
                // Input exhausted and the flush packet has been sent; the next
                // receive will hand back the remaining frames and then EOF. Loop.
                if (eof) {
                    return false;
                }
            }
        }
    }

    // Read packets until one belongs to our stream and send it, skipping packets
    // from other streams (video, the attached-picture cover art, extra audio
    // tracks). On input EOF, send a single NULL packet to put the decoder in
    // drain mode and return false. Returns true once a packet was accepted (or
    // retained for resend, see below). The send return is checked: the
    // receive-first design in read() makes send-EAGAIN close to impossible for
    // audio codecs (the decoder is always drained before being fed), so the
    // EAGAIN arm is a correctness backstop rather than a hot path, and any other
    // negative return means a wedged or broken codec, which ends the track
    // cleanly instead of looping against it.
    bool supplyPacket() {
        if (draining) {
            return false;  // flush packet already sent; nothing left to feed
        }
        for (;;) {
            if (!resend) {
                const int r = av_read_frame(fmtCtx, pkt);
                if (r < 0) {
                    avcodec_send_packet(codecCtx, nullptr);  // enter drain mode
                    draining = true;
                    return false;
                }
                if (pkt->stream_index != streamIndex) {
                    av_packet_unref(pkt);
                    continue;
                }
            }
            const int sent = avcodec_send_packet(codecCtx, pkt);
            if (sent == AVERROR(EAGAIN)) {
                if (resend) {
                    // A second consecutive refusal with an intervening receive
                    // that also came up empty (that is the only way read() calls
                    // here again) means both sides of the send/receive contract
                    // are refusing at once, which the API forbids. Treat the
                    // decoder as wedged rather than risk a livelock.
                    resend = false;
                    av_packet_unref(pkt);
                    avcodec_send_packet(codecCtx, nullptr);
                    draining = true;
                    return false;
                }
                // Decoder full: retain the packet (NOT unreffed) and re-send it
                // on the next call; the receive that follows drains a frame and
                // makes room. Nothing is dropped.
                resend = true;
                return true;
            }
            if (sent < 0) {
                // Hard send failure: the codec context is broken for this
                // stream. Flush what it may still hold and end the track; read()
                // drains the remainder and reports EOS, and the engine advances.
                resend = false;
                av_packet_unref(pkt);
                avcodec_send_packet(codecCtx, nullptr);
                draining = true;
                return false;
            }
            // Accepted. Live VBR bitrate: accumulate this packet's compressed
            // size, AFTER acceptance so a packet that went around the retain
            // loop is counted exactly once. read() pairs the running total with
            // the decoded frame's sample count and pushes it to the meter, so the
            // figure does not depend on pkt->duration (which the Ogg/Vorbis
            // demuxer, and sometimes Opus, leaves at 0). Read before the unref
            // clears pkt's fields.
            resend = false;
            if (pkt->size > 0) {
                pendingBytes += static_cast<std::uint64_t>(pkt->size);
            }
            av_packet_unref(pkt);
            return true;
        }
    }

    // Convert the frame currently held in `frame` to packed float32 and append
    // it to staging. Returns false only on a conversion failure (treated as a
    // clean stop). An empty frame appends nothing and is not an error. swr runs
    // one-to-one here (equal in/out rate, packed-float out), so the produced
    // count equals nb_samples; we trim to the actual count defensively.
    bool appendCurrentFrame() {
        if (!swrReady && !configureSwr()) {
            return false;
        }
        const int inSamples = frame->nb_samples;
        if (inSamples <= 0) {
            return true;
        }
        const std::size_t base = staging.size();
        staging.resize(base + static_cast<std::size_t>(inSamples) * channels);

        std::uint8_t* outPlanes[1] = {
            reinterpret_cast<std::uint8_t*>(staging.data() + base)};
        const int got = swr_convert(
            swr, outPlanes, inSamples,
            const_cast<const std::uint8_t**>(frame->extended_data), inSamples);
        if (got < 0) {
            staging.resize(base);
            return false;
        }
        staging.resize(base + static_cast<std::size_t>(got) * channels);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Factory. Non-throwing: returns a fully-opened decoder or nullptr, never a
// half-open object. Opens the container, finds the best audio stream, opens its
// decoder, and caches every fact format()/sourceInfo()/totalFrames()/seekable()
// report, so all of them are valid before any audio is pulled. The resampler is
// configured lazily from the first decoded frame, since its parameters are not
// needed until read() runs.
std::unique_ptr<IDecoder> FfmpegDecoder::open(const std::string& path,
                                              std::string* error) {
    quietFfmpegLogOnce();

    auto impl = std::make_unique<Impl>();

    int rc = avformat_open_input(&impl->fmtCtx, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        if (error) {
            *error = "ffmpeg could not open '" + path + "': " + avErr(rc);
        }
        return nullptr;  // fmtCtx left null by avformat_open_input on failure
    }

    rc = avformat_find_stream_info(impl->fmtCtx, nullptr);
    if (rc < 0) {
        if (error) {
            *error = "ffmpeg could not read stream info: " + avErr(rc);
        }
        return nullptr;
    }

    // Best audio stream, decoder chosen for us. AVMEDIA_TYPE_AUDIO skips video
    // and the attached-picture cover-art stream automatically.
    const AVCodec* codec = nullptr;
    const int      si =
        av_find_best_stream(impl->fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (si < 0 || codec == nullptr) {
        if (error) {
            *error = "ffmpeg found no decodable audio stream in '" + path + "'";
        }
        return nullptr;
    }
    impl->streamIndex      = si;
    AVStream* st           = impl->fmtCtx->streams[si];
    impl->timeBase         = st->time_base;
    // Pts origin for the seek math (see the Impl comment): 0 when unreported.
    impl->startTime =
        (st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;

    impl->codecCtx = avcodec_alloc_context3(codec);
    if (impl->codecCtx == nullptr) {
        if (error) {
            *error = "ffmpeg could not allocate a codec context";
        }
        return nullptr;
    }
    rc = avcodec_parameters_to_context(impl->codecCtx, st->codecpar);
    if (rc < 0) {
        if (error) {
            *error = "ffmpeg could not copy codec parameters: " + avErr(rc);
        }
        return nullptr;
    }
    rc = avcodec_open2(impl->codecCtx, codec, nullptr);
    if (rc < 0) {
        if (error) {
            *error = "ffmpeg could not open the decoder: " + avErr(rc);
        }
        return nullptr;
    }

    impl->frame = av_frame_alloc();
    impl->pkt   = av_packet_alloc();
    if (impl->frame == nullptr || impl->pkt == nullptr) {
        if (error) {
            *error = "ffmpeg could not allocate the frame/packet";
        }
        return nullptr;
    }

    // ---- native format -----------------------------------------------------
    const auto rate     = static_cast<std::uint32_t>(impl->codecCtx->sample_rate);
    const auto channels = static_cast<unsigned>(impl->codecCtx->ch_layout.nb_channels);
    if (rate == 0 || channels == 0) {
        if (error) {
            *error = "ffmpeg: audio stream reports no rate or channels";
        }
        return nullptr;
    }
    impl->audioFormat = AudioFormat{.sampleRate = rate,
                                    .channels   = static_cast<std::uint16_t>(channels)};
    impl->channels    = channels;

    // ---- length in frames --------------------------------------------------
    // Prefer the stream duration (in the stream time base); fall back to the
    // container duration (in AV_TIME_BASE). For a container with an authoritative
    // duration (.m4a, .ogg) this is exact. For an index-less raw stream (raw
    // ADTS-AAC, raw AC3/DTS) FFmpeg can only ESTIMATE it from the bitrate, which
    // is off by seconds (CBR comes out short, VBR long), so when that is the case
    // and the file is seekable we refine to an exact count with a demux-only scan
    // and rewind. read() and seek() still behave against the real decoded EOF, so
    // the refinement only improves the reported length and the seek scale. 0 means
    // unknown.
    std::uint64_t total = 0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
        total = static_cast<std::uint64_t>(av_rescale_q(
            st->duration, st->time_base, AVRational{1, static_cast<int>(rate)}));
    } else if (impl->fmtCtx->duration != AV_NOPTS_VALUE &&
               impl->fmtCtx->duration > 0) {
        const double seconds =
            static_cast<double>(impl->fmtCtx->duration) / AV_TIME_BASE;
        total = static_cast<std::uint64_t>(std::round(seconds * rate));
    }

    // Seekability proxy (computed here because the refine below needs it): the I/O
    // context must be normally seekable AND we must have a usable duration to
    // rescale against. A container with no usable duration reports unseekable
    // rather than seeking coarsely; a per-call seek still returns false on its own
    // failure even when this is true.
    const bool pbSeekable = impl->fmtCtx->pb != nullptr &&
                            (impl->fmtCtx->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    impl->seekable_ = pbSeekable && total > 0;

    // Refine the estimate to an exact count for the bitrate-estimated raw case.
    if (impl->fmtCtx->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE &&
        impl->seekable_) {
        const std::uint64_t exact = detail::scanExactFrames(impl->fmtCtx, si, rate);
        if (exact > 0) {
            total = exact;
        }
        // The scan ran the demuxer to EOF; rewind to the start so the first read()
        // begins at frame 0. The codec is already open, so flush it too.
        av_seek_frame(impl->fmtCtx, si, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(impl->codecCtx);
    }
    impl->total = total;

    // ---- descriptive facts -------------------------------------------------
    impl->src.codec         = mapCodec(codec->id);
    impl->src.decoder       = DecoderKind::Ffmpeg;
    impl->src.bitsPerSample = losslessDepth(codec->id, st->codecpar);
    std::int64_t br         = st->codecpar->bit_rate;
    if (br <= 0) {
        br = impl->fmtCtx->bit_rate;  // container average when the stream omits it
    }
    if (br <= 0 && impl->fmtCtx->pb != nullptr && total > 0) {
        // Last resort for raw streams that expose no bitrate at all (raw DTS is a
        // common one): the file-size over duration average, the same figure a tag
        // reader would compute. Uses the refined duration above, so it is accurate
        // for the very streams that needed the refinement.
        const std::int64_t sizeBytes = avio_size(impl->fmtCtx->pb);
        const double       seconds   =
            static_cast<double>(total) / static_cast<double>(rate);
        if (sizeBytes > 0 && seconds > 0.0) {
            br = static_cast<std::int64_t>(
                static_cast<double>(sizeBytes) * 8.0 / seconds);
        }
    }
    impl->src.bitrateKbps = br > 0 ? static_cast<std::uint32_t>(br / 1000) : 0;

    // Seed the live meter with the nominal so currentBitrateKbps() reads sensibly
    // before the first frame is measured, and arm the priming guard.
    impl->resetMeter();

    return std::unique_ptr<IDecoder>(new FfmpegDecoder(std::move(impl)));
}

FfmpegDecoder::FfmpegDecoder(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

// Defaulted here, where Impl is complete, so ~Impl (which frees every FFmpeg
// object) is visible to the unique_ptr deleter.
FfmpegDecoder::~FfmpegDecoder() = default;

// ---------------------------------------------------------------------------
// Accessors: trivial reads of the facts cached at open time.
AudioFormat   FfmpegDecoder::format()      const { return m_impl->audioFormat; }
SourceInfo    FfmpegDecoder::sourceInfo()  const { return m_impl->src; }
std::uint64_t FfmpegDecoder::totalFrames() const { return m_impl->total; }
bool          FfmpegDecoder::seekable()    const { return m_impl->seekable_; }

// Live VBR bitrate: the meter's trailing-window average, fed from read().
std::uint32_t FfmpegDecoder::currentBitrateKbps() const {
    return m_impl->meter.value();
}

// ---------------------------------------------------------------------------
// Decode. Drains the staging buffer first; when it runs dry, pumps one more
// decoded+converted frame and tries again. Honors "fill fully, short only at
// EOS, 0 == EOS": the loop ends only when the request is satisfied or the
// decoder reaches end of stream / an error with nothing left to hand back.
std::size_t FfmpegDecoder::read(float* dst, std::size_t frames) {
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
            m_impl->stagePos  += take * ch;
            done              += take;
            m_impl->outputPos += take;
            if (m_impl->stagePos >= m_impl->staging.size()) {
                m_impl->clearStaging();
            }
            continue;
        }

        // 2) Staging empty: decode one more frame, or stop at EOS.
        if (m_impl->eof) {
            break;
        }
        if (!m_impl->receiveOneFrame()) {
            break;  // EOF or error: receiveOneFrame set eof
        }
        const std::size_t before = m_impl->staging.size();
        const bool ok = m_impl->appendCurrentFrame();
        av_frame_unref(m_impl->frame);
        if (!ok) {
            m_impl->eof = true;
            break;
        }
        // Pair the compressed bytes consumed for this frame (summed in
        // supplyPacket since the last push) with the audio it produced. The output
        // sample count is reliable for every codec, so this reads correctly even
        // when packet durations are absent. Pushing here (not in appendCurrentFrame)
        // keeps seek's decode-forward out of the meter; pendingBytes is cleared
        // across seeks so the first post-seek frame does not over-count.
        const std::size_t produced = (m_impl->staging.size() - before) / ch;
        if (produced > 0 && m_impl->pendingBytes > 0 &&
            m_impl->audioFormat.sampleRate > 0) {
            if (m_impl->meterPrimed) {
                const double seconds =
                    static_cast<double>(produced) / m_impl->audioFormat.sampleRate;
                m_impl->meter.push(
                    static_cast<double>(m_impl->pendingBytes) * 8.0, seconds);
            } else {
                // Discard this first frame after a reset: it carries the decoder's
                // priming packets lumped together and would spike the window.
                m_impl->meterPrimed = true;
            }
            m_impl->pendingBytes = 0;
        }
    }
    return done;
}

// ---------------------------------------------------------------------------
// Seek to an absolute frame. A raw seek with the BACKWARD bias lands on a packet
// at or before the target; we flush the decoder, decode forward discarding whole
// frames that end before the target, then front-trim the landing frame so the
// next read() resumes exactly at `frame`. Frame-accurate, not bit-exact for
// lossy codecs (inter-frame filter state), the same caveat libmpg123 carries.
bool FfmpegDecoder::seek(std::uint64_t frame) {
    if (!m_impl->seekable_) {
        return false;
    }

    // Seek-to-end parity, mirroring FlacDecoder's forcedEos idiom. The contract
    // treats seek(totalFrames) as a valid "go to EOS"; strictly past the end is a
    // genuine failure. Because total is a container estimate for lossy formats,
    // this is the structural end position, not a bit-exact one.
    if (m_impl->total > 0 && frame >= m_impl->total) {
        m_impl->clearStaging();
        m_impl->resetMeter();  // at EOS, read the nominal; arm the priming guard
        if (frame == m_impl->total) {
            m_impl->forcedEos = true;
            m_impl->eof       = true;
            m_impl->outputPos = m_impl->total;
            return true;
        }
        m_impl->forcedEos = false;
        return false;
    }
    m_impl->forcedEos = false;

    // Raw seek to the target timestamp: the target frame rescaled to the stream
    // time base, then offset by the stream's start_time so an offset-timestamped
    // container is addressed in its own pts space (see the Impl comment).
    // Biased to land at or before the target so we can decode forward to the
    // exact frame.
    const std::int64_t ts =
        m_impl->startTime +
        av_rescale_q(
            static_cast<std::int64_t>(frame),
            AVRational{1, static_cast<int>(m_impl->audioFormat.sampleRate)},
            m_impl->timeBase);
    if (av_seek_frame(m_impl->fmtCtx, m_impl->streamIndex, ts,
                      AVSEEK_FLAG_BACKWARD) < 0) {
        // Position is unspecified on a false return (the IDecoder contract allows
        // it); clear staging so a later seek starts clean.
        m_impl->clearStaging();
        return false;
    }

    avcodec_flush_buffers(m_impl->codecCtx);
    m_impl->clearStaging();
    m_impl->eof      = false;
    m_impl->draining = false;
    // A packet retained for resend belongs to the pre-seek position; drop
    // it, or the first post-seek supply would feed stale compressed data into
    // the freshly flushed codec.
    if (m_impl->resend) {
        m_impl->resend = false;
        av_packet_unref(m_impl->pkt);
    }
    // Discard the pre-seek window so the live readout reflects the new position
    // rather than blending across the jump; also clears the byte accumulator and
    // re-arms the priming guard for the post-seek decode.
    m_impl->resetMeter();

    // Decode forward to the target. Each decoded frame carries a presentation
    // timestamp we rescale to an absolute sample index; a frame ending before the
    // target is discarded, the frame containing the target is converted and
    // front-trimmed. A frame with no usable timestamp is taken as the landing
    // frame at the requested position (the coarse fallback for streams that omit
    // timestamps); this is the lossy structural-accuracy case.
    for (;;) {
        if (!m_impl->receiveOneFrame()) {
            // Reached real EOF before the target: the estimate placed the target
            // past the true end. Position at the end and report success, the
            // structural seek-to-end behavior.
            m_impl->eof       = true;
            m_impl->forcedEos = true;
            m_impl->outputPos = m_impl->total > 0 ? m_impl->total : frame;
            return true;
        }

        const std::int64_t pts = m_impl->frame->best_effort_timestamp;
        const auto nb = static_cast<std::uint64_t>(m_impl->frame->nb_samples);

        std::uint64_t start;
        bool          haveStart = false;
        if (pts != AV_NOPTS_VALUE) {
            // Rebase the pts to sample-index space: subtract the stream's
            // start_time BEFORE rescaling, so an offset-timestamped container
            // compares in the same 0-based space as `frame`. The clamp
            // covers the pathological pts below start_time.
            const std::int64_t s = av_rescale_q(
                pts - m_impl->startTime, m_impl->timeBase,
                AVRational{1, static_cast<int>(m_impl->audioFormat.sampleRate)});
            start     = s < 0 ? 0 : static_cast<std::uint64_t>(s);
            haveStart = true;
        } else {
            start = frame;  // coarse: treat this frame as the landing frame
        }

        // Whole frame ends before the target: discard and keep decoding.
        if (haveStart && start + nb <= frame) {
            av_frame_unref(m_impl->frame);
            continue;
        }

        // This frame contains (or, for the no-timestamp case, is taken as) the
        // target. Convert it and front-trim to the requested sample.
        const bool ok = m_impl->appendCurrentFrame();
        av_frame_unref(m_impl->frame);
        if (!ok) {
            m_impl->eof = true;
            return true;  // landed, but no audio could be produced; read() stops
        }
        const std::uint64_t skip = frame > start ? frame - start : 0;
        const std::size_t   skipFloats =
            static_cast<std::size_t>(skip) * m_impl->channels;
        m_impl->stagePos  = std::min(skipFloats, m_impl->staging.size());
        m_impl->outputPos = frame;
        m_impl->pendingBytes = 0;  // decode-forward bytes are not a real read frame
        return true;
    }
}

}  // namespace rawform::audio
