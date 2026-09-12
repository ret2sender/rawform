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

// MetadataProbe.cpp
//
// Implementation of the read-side FFmpeg fallback declared in MetadataProbe.h.
// This translation unit is compiled UNCONDITIONALLY (see CMakeLists.txt), so the
// probeAudioMetadata symbol exists in every build and callers link without an #if.
// Everything that touches a libav* header is confined to the RAWFORM_HAVE_FFMPEG
// branch below; the native-only build compiles the #else and returns std::nullopt,
// referencing no libav* symbol, exactly as the decoder does when FFmpeg is absent.
//
// The shape deliberately mirrors FfmpegDecoder::open()'s opening half: open the
// container, find the best audio stream, then read facts. It stops where the
// decoder would diverge: it never allocates a codec context, never calls
// avcodec_open2, and never configures a resampler, because nothing here decodes a
// single sample. The duration refinement for index-less raw streams reuses the
// SAME helper the decoder uses (detail::scanExactFrames), so a raw .ac3 / .dts
// reports the identical length in the metadata pane and during playback.

#include "rawform/audio/MetadataProbe.h"

#if RAWFORM_HAVE_FFMPEG

#include "decoders/FfmpegDuration.h"

extern "C" {
#include <libavcodec/avcodec.h>          // AVCodecParameters, avcodec_get_name, descriptors
#include <libavformat/avformat.h>        // AVFormatContext, av_find_best_stream, avio_size
#include <libavutil/channel_layout.h>    // ch_layout.nb_channels
#include <libavutil/dict.h>              // av_dict_get, AVDictionaryEntry
#include <libavutil/error.h>             // av_strerror
#include <libavutil/mathematics.h>       // av_rescale_q
}

#include <cctype>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>

namespace rawform::audio {

namespace {

// Process-wide log quieting, the same once-only pattern the decoder uses. Raw
// AC3/DTS make libavformat print "Estimating duration from bitrate" to stderr;
// during a folder scan that would scribble across thousands of files. A separate
// std::once_flag from the decoder's is fine: both only lower the level, so
// whichever runs first wins and the other is a no-op.
void quietFfmpegLogOnce() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

// Human-readable libav* error string for the diagnostics out-param.
std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

// Presentation label from the stream's codec id. The named families are the ones
// worth spelling nicely in the pane; everything else falls back to libav's own
// short name, upper-cased, so an unanticipated codec still shows a sensible label
// rather than nothing. This mirrors how the TagLib path returns ready labels from
// codecForFile, keeping the two readers visually consistent.
std::string codecLabel(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_AC3:         return "AC3";
        case AV_CODEC_ID_EAC3:        return "E-AC3";
        case AV_CODEC_ID_DTS:         return "DTS";
        case AV_CODEC_ID_TRUEHD:      return "TrueHD";
        case AV_CODEC_ID_MLP:         return "MLP";
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_AAC_LATM:    return "AAC";
        case AV_CODEC_ID_OPUS:        return "Opus";
        case AV_CODEC_ID_VORBIS:      return "Vorbis";
        case AV_CODEC_ID_ALAC:        return "ALAC";
        case AV_CODEC_ID_WMAV1:
        case AV_CODEC_ID_WMAV2:
        case AV_CODEC_ID_WMAPRO:
        case AV_CODEC_ID_WMALOSSLESS:
        case AV_CODEC_ID_WMAVOICE:    return "WMA";
        case AV_CODEC_ID_FLAC:        return "FLAC";
        case AV_CODEC_ID_MP1:         return "MP1";
        case AV_CODEC_ID_MP2:         return "MP2";
        case AV_CODEC_ID_MP3:         return "MP3";
        default: {
            const char* name = avcodec_get_name(id);  // never null
            std::string out(name != nullptr ? name : "");
            for (char& c : out) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return out;
        }
    }
}

// Container bit depth, following the engine rule the decoder already encodes: a
// real depth for lossless codecs, 0 for lossy ones (there is no meaningful PCM
// container depth behind a lossy stream). Asks libav's own codec descriptor
// whether the codec is lossless-and-not-lossy, then reads the raw sample depth.
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

// Closes and frees an AVFormatContext on scope exit, so every early return path
// below is leak-free without repeating the cleanup. avformat_close_input takes a
// pointer-to-pointer and nulls it; handing it the address of the by-value param
// satisfies that contract.
struct FmtCloser {
    void operator()(AVFormatContext* c) const noexcept {
        if (c != nullptr) {
            avformat_close_input(&c);
        }
    }
};
using FmtPtr = std::unique_ptr<AVFormatContext, FmtCloser>;

// Copy every entry of one AVDictionary into the verbatim tag list. The
// IGNORE_SUFFIX empty-key query is the long-standing idiom for "iterate all".
void harvestDict(const AVDictionary* dict,
                 std::vector<std::pair<std::string, std::string>>& out) {
    const AVDictionaryEntry* e = nullptr;
    while ((e = av_dict_get(dict, "", e, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        out.emplace_back(e->key != nullptr ? e->key : "",
                         e->value != nullptr ? e->value : "");
    }
}

}  // namespace

std::optional<ProbedMetadata> probeAudioMetadata(const std::string& path,
                                                 std::string* error) {
    quietFfmpegLogOnce();

    AVFormatContext* raw = nullptr;
    int rc = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (rc < 0) {
        if (error) {
            *error = "ffmpeg probe could not open '" + path + "': " + avErr(rc);
        }
        return std::nullopt;  // raw left null by avformat_open_input on failure
    }
    FmtPtr fmt(raw);  // from here every return path frees the context

    rc = avformat_find_stream_info(fmt.get(), nullptr);
    if (rc < 0) {
        if (error) {
            *error = "ffmpeg probe could not read stream info: " + avErr(rc);
        }
        return std::nullopt;
    }

    // Best audio stream; AVMEDIA_TYPE_AUDIO skips video and attached-picture
    // cover-art streams automatically. No decoder is opened: we read the stream's
    // codec PARAMETERS directly, which carry rate, channels, depth, and bitrate
    // without the cost of allocating and opening an AVCodecContext.
    const int si =
        av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si < 0) {
        if (error) {
            *error = "ffmpeg probe found no audio stream in '" + path + "'";
        }
        return std::nullopt;
    }
    AVStream*                 st  = fmt->streams[si];
    const AVCodecParameters*  par = st->codecpar;

    const auto rate     = static_cast<std::uint32_t>(par->sample_rate);
    const auto channels = static_cast<unsigned>(par->ch_layout.nb_channels);
    if (rate == 0 || channels == 0) {
        if (error) {
            *error = "ffmpeg probe: audio stream reports no rate or channels";
        }
        return std::nullopt;
    }

    ProbedMetadata md;
    md.sampleRate    = rate;
    md.channels      = static_cast<std::uint16_t>(channels);
    md.bitsPerSample = losslessDepth(par->codec_id, par);
    md.codecName     = codecLabel(par->codec_id);

    // ---- length ------------------------------------------------------------
    // Prefer the stream duration (stream time base), fall back to the container
    // duration (AV_TIME_BASE). For a container with an authoritative duration this
    // is exact; for an index-less raw stream FFmpeg can only estimate it from the
    // bitrate, so when that is the case and the file is seekable we refine to an
    // exact count with the shared demux-only scan. We do NOT rewind afterward: the
    // context is closed at scope exit, nothing reads from it again.
    std::uint64_t totalFrames = 0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
        totalFrames = static_cast<std::uint64_t>(av_rescale_q(
            st->duration, st->time_base, AVRational{1, static_cast<int>(rate)}));
    } else if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        const double seconds = static_cast<double>(fmt->duration) / AV_TIME_BASE;
        totalFrames = static_cast<std::uint64_t>(std::round(seconds * rate));
    }

    const bool pbSeekable = fmt->pb != nullptr &&
                            (fmt->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    if (fmt->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE &&
        pbSeekable && totalFrames > 0) {
        const std::uint64_t exact = detail::scanExactFrames(fmt.get(), si, rate);
        if (exact > 0) {
            totalFrames = exact;
        }
    }
    md.durationMs = rate > 0
                        ? static_cast<std::int64_t>(totalFrames * 1000ULL / rate)
                        : 0;

    // ---- bitrate -----------------------------------------------------------
    // Stream bitrate, else the container average, else (for raw streams that
    // expose neither, raw DTS being the common one) the file-size over duration
    // average, the same figure a tag reader would compute. Uses the refined
    // duration above, so the last-resort figure is accurate for exactly the
    // streams that needed the refinement.
    std::int64_t br = par->bit_rate;
    if (br <= 0) {
        br = fmt->bit_rate;
    }
    if (br <= 0 && fmt->pb != nullptr && totalFrames > 0) {
        const std::int64_t sizeBytes = avio_size(fmt->pb);
        const double       seconds =
            static_cast<double>(totalFrames) / static_cast<double>(rate);
        if (sizeBytes > 0 && seconds > 0.0) {
            br = static_cast<std::int64_t>(
                static_cast<double>(sizeBytes) * 8.0 / seconds);
        }
    }
    md.bitrateKbps = br > 0 ? static_cast<std::uint32_t>(br / 1000) : 0;

    // ---- tags --------------------------------------------------------------
    // Format-level entries first, then stream-level, verbatim in libav's native
    // keys. Most raw .ac3 / .dts carry none; tagged containers (an AC3 inside an
    // .mka, say) carry the usual title/artist/album set, which the UI maps onto
    // its promoted fields.
    harvestDict(fmt->metadata, md.tags);
    harvestDict(st->metadata, md.tags);

    return md;
}

}  // namespace rawform::audio

#else  // RAWFORM_HAVE_FFMPEG

#include <string>

namespace rawform::audio {

// Native-only build: the probe is a no-op that always declines. The caller checks
// kMetadataProbeAvailable (false here) and so never reaches a useful result, but
// the symbol must still exist for the UI to link against.
std::optional<ProbedMetadata> probeAudioMetadata(const std::string& path,
                                                 std::string* error) {
    (void)path;
    if (error) {
        *error = "metadata probe unavailable: rawform_audio was built without FFmpeg";
    }
    return std::nullopt;
}

}  // namespace rawform::audio

#endif  // RAWFORM_HAVE_FFMPEG
