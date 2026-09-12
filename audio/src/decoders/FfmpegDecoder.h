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

// FfmpegDecoder.h
//
// IDecoder backed by FFmpeg (libavformat, libavcodec, libavutil, libswresample),
// the terminal fallback for the long tail of formats no native reference
// decoder handles: AAC/M4A, AC3/E-AC3, DTS, Opus, Vorbis, ALAC, WMA, and the
// rest. It is the fourth and last concrete decoder in the cascade. The frozen
// native decoders (SndfileDecoder, FlacDecoder, Mpg123Decoder) are always tried
// first; DecoderFactory reaches this one only when every native candidate has
// returned nullptr, so FFmpeg never shadows a reference library and the common
// path never pays its open cost.
//
// Optional dependency: this whole translation unit, the DecoderFactory hook that
// registers it, and the conformance suite's FFmpeg case are compiled only when
// the build found the four libav* libraries (RAWFORM_HAVE_FFMPEG == 1). Without
// FFmpeg the engine builds native-only: the same decoders, the same factory,
// no long-tail fallback. The header is still safe to include unconditionally:
// the guard lives in the .cpp and in DecoderFactory.cpp, not here, so nothing
// that names FfmpegDecoder needs its own #if around the include.
//
// Dependency containment: NONE of <libavformat/...>, <libavcodec/...>, or
// <libswresample/...> appears in this header. The AVFormatContext, the codec
// context, the resampler, the staging buffer, and the cached facts all live
// behind a pimpl whose definition sits in the .cpp, so the library's link to the
// four FFmpeg targets stays PRIVATE and callers include this header without
// inheriting any libav* include path. The cost is one pointer indirection per
// call, which is per-block and not per-sample, and irrelevant next to the decode.
//
// The pull bridge: FFmpeg's avcodec_receive_frame hands back variable-size
// frames (one decoded AVFrame at a time, its length chosen by the codec), but
// IDecoder::read() is a fixed-N pull contract. The Impl owns a staging buffer in
// the same shape FlacDecoder uses: read() drains whatever is staged, and when it
// runs dry it pumps the send-packet / receive-frame loop for one more frame,
// converts it to interleaved float32 through libswresample, and appends it.
// swresample converts ONLY the sample format and the planar/packed layout; it
// never changes the sample rate and never remixes channels, so the bit-perfect,
// native-rate, native-channel invariant holds exactly as for every other decoder.
//
// Lifetime: there is no public constructor. open() is the only way to make one,
// and it returns either a fully-opened decoder or nullptr, so a half-open
// FfmpegDecoder cannot exist. This mirrors the other three decoders and the
// DecoderFactory contract exactly.

#pragma once

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/Types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rawform::audio {

class FfmpegDecoder final : public IDecoder {
public:
    /// Non-throwing factory. Opens path through libavformat, selects the best
    /// AUDIO stream (ignoring video and attached-picture cover art), opens the
    /// matching decoder, and configures a format-only resampler to interleaved
    /// float32 at the source's native rate and channel count. Returns a ready
    /// decoder, or nullptr on any failure (file missing, no audio stream, an
    /// unsupported codec, or a libav* error). When error is non-null it is
    /// written with a human-readable reason on failure and left untouched on
    /// success. The return type is IDecoder, so callers program against the same
    /// interface the factory hands them.
    static std::unique_ptr<IDecoder> open(const std::string& path,
                                          std::string* error = nullptr);

    ~FfmpegDecoder() override;

    /// Not copyable or movable: it owns an AVFormatContext, a codec context, a
    /// resampler, and an open file, and is always held by unique_ptr behind
    /// IDecoder, so moving the object itself has no meaning.
    FfmpegDecoder(const FfmpegDecoder&)            = delete;
    FfmpegDecoder& operator=(const FfmpegDecoder&) = delete;
    FfmpegDecoder(FfmpegDecoder&&)                 = delete;
    FfmpegDecoder& operator=(FfmpegDecoder&&)      = delete;

    /// IDecoder surface; the authoritative contract for each lives in IDecoder.h.
    [[nodiscard]] AudioFormat   format()      const override;
    [[nodiscard]] SourceInfo    sourceInfo()  const override;
    [[nodiscard]] std::uint64_t totalFrames() const override;
    std::size_t read(float* dst, std::size_t frames) override;
    [[nodiscard]] bool seekable() const override;
    bool seek(std::uint64_t frame) override;

    /// Live VBR bitrate. The FFmpeg fallback handles the inherently
    /// variable-rate families (AAC, Opus, Vorbis, ...); this reports the trailing-
    /// window average of recent packets' compressed sizes, distinct from the static
    /// container-average figure in sourceInfo().
    [[nodiscard]] std::uint32_t currentBitrateKbps() const override;

private:
    /// Defined in the .cpp; holds the AVFormatContext*, AVCodecContext*,
    /// SwrContext*, the reusable AVFrame/AVPacket, the staging buffer, the running
    /// output position used by seek and the remainder math, and the facts cached
    /// at open time so every const accessor stays a trivial return.
    struct Impl;

    /// Private: only open() constructs this, handing over an already-populated
    /// Impl. Taking it by unique_ptr means the object is fully valid the instant
    /// it exists, with no separate init step that could be skipped.
    explicit FfmpegDecoder(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

}  // namespace rawform::audio
