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

// FfmpegDuration.h
//
// One small piece of FFmpeg logic shared by the decoder and the metadata probe:
// the exact frame count of an index-less raw elementary stream whose duration
// libavformat could only ESTIMATE from the bitrate (raw ADTS-AAC, raw AC3, raw
// DTS). Both FfmpegDecoder (for its totalFrames / seek scale) and MetadataProbe
// (for the duration it reports to the scanner) need the SAME number, so the
// logic lives here once rather than being copied into each; the rationale for
// the scan itself sits on the function below.
//
// This is a PRIVATE engine header. It names libav* types in its signature, so it
// is included ONLY by translation units that already compile inside the
// RAWFORM_HAVE_FFMPEG guard, namely FfmpegDecoder.cpp and MetadataProbe.cpp. It
// is header-only (an inline function), so it needs no separate source file and no
// CMake entry; it simply rides along with whichever guarded TU includes it. The
// library's link to the four libav* targets stays PRIVATE, because nothing
// public includes this header.

#pragma once

extern "C" {
#include <libavcodec/packet.h>      // AVPacket, av_packet_alloc / unref / free
#include <libavformat/avformat.h>   // AVFormatContext, AVStream, av_read_frame
#include <libavutil/mathematics.h>  // av_rescale_q
#include <libavutil/rational.h>     // AVRational
}

#include <cstdint>  // NOLINT: std::uintN_t

namespace rawform::audio::detail {

/// Exact frame count by a demux-only scan, for index-less raw streams whose
/// duration FFmpeg could only ESTIMATE from the bitrate (raw ADTS-AAC, raw AC3,
/// raw DTS). It reads every packet of the chosen stream WITHOUT decoding any
/// audio, sums their durations (the demuxer fills each ADTS/AC3/DTS frame's
/// duration from the frame header), and rescales once to a frame count. This is
/// I/O over the whole file but no decode, so it is cheap relative to playback; the
/// caller gates it to the bitrate-estimated case only, so containers with an
/// authoritative duration (.m4a, .ogg, .mka) never pay it. Returns 0 if any packet
/// lacks a duration (the count would be wrong), so the caller keeps the estimate.
/// The caller is responsible for rewinding the demuxer afterward if it intends to
/// keep reading from fmtCtx (the decoder does; the probe closes and so does not).
inline std::uint64_t scanExactFrames(AVFormatContext* fmtCtx,
                                     int streamIndex,
                                     std::uint32_t rate) {
    AVPacket* pkt = av_packet_alloc();
    if (pkt == nullptr) {
        return 0;
    }
    const AVStream* st     = fmtCtx->streams[streamIndex];
    std::int64_t    sumDur = 0;  // in stream time-base units
    bool            allHaveDuration = true;
    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == streamIndex) {
            if (pkt->duration > 0) {
                sumDur += pkt->duration;
            } else {
                allHaveDuration = false;
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (!allHaveDuration || sumDur <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(av_rescale_q(
        sumDur, st->time_base, AVRational{1, static_cast<int>(rate)}));
}

}  // namespace rawform::audio::detail
