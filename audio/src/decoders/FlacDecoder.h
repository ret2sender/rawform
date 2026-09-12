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

// FlacDecoder.h
//
// IDecoder backed by libFLAC, the native reference decoder for FLAC. FLAC goes
// through the reference library rather than libsndfile's optional FLAC
// support so the decode is the canonical one; the DecoderFactory enforces that
// structurally and dispatches here by content/extension. Like every decoder it
// is constructed only through its own non-throwing open().
//
// Dependency containment: <FLAC/stream_decoder.h> appears in none of this
// header. libFLAC's stream decoder handle, the staging buffer that bridges its
// push-model write callback to our pull-model read(), and the cached facts all
// live behind a pimpl whose definition sits in the .cpp, so the library's link
// to FLAC::FLAC stays PRIVATE and the CLI and engine can include this header
// without inheriting the libFLAC include path. The cost is one pointer
// indirection per call, which is per-block, not per-sample, and irrelevant next
// to the decode.
//
// The push-to-pull bridge: libFLAC delivers decoded audio by CALLING us (a write
// callback fires once per FLAC frame), but IDecoder::read() is a PULL contract
// (the engine asks for N frames). The Impl owns a staging buffer; read() pumps
// the libFLAC process loop until enough frames have accumulated, then drains the
// staging buffer into the caller's destination. This is the same "fill fully,
// short only at EOS, 0 == EOS" shape every decoder presents; the bridging is an
// implementation detail invisible above this header.
//
// Lifetime: there is no public constructor. open() is the only way to make one,
// and it returns either a fully-opened decoder or nullptr, so a half-open
// FlacDecoder cannot exist. This mirrors SndfileDecoder and the DecoderFactory
// contract exactly.

#pragma once

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/Types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rawform::audio {

class FlacDecoder final : public IDecoder {
public:
    /// Non-throwing factory. Opens path through libFLAC's stream decoder, reads
    /// the STREAMINFO so format/depth/length are known before any audio is asked
    /// for, and returns a ready decoder or nullptr on any failure (file missing,
    /// not a FLAC stream, or a libFLAC init error). When error is non-null it is
    /// written with a human-readable reason on failure and left untouched on
    /// success. The return type is IDecoder, so callers program against the same
    /// interface the factory hands them.
    static std::unique_ptr<IDecoder> open(const std::string& path,
                                          std::string* error = nullptr);

    ~FlacDecoder() override;

    /// Not copyable or movable: it owns a libFLAC decoder handle and an open file,
    /// and is always held by unique_ptr behind IDecoder, so moving the object has
    /// no meaning.
    FlacDecoder(const FlacDecoder&)            = delete;
    FlacDecoder& operator=(const FlacDecoder&) = delete;
    FlacDecoder(FlacDecoder&&)                 = delete;
    FlacDecoder& operator=(FlacDecoder&&)      = delete;

    /// IDecoder surface; the authoritative contract for each lives in IDecoder.h.
    [[nodiscard]] AudioFormat   format()      const override;
    [[nodiscard]] SourceInfo    sourceInfo()  const override;
    [[nodiscard]] std::uint64_t totalFrames() const override;
    std::size_t read(float* dst, std::size_t frames) override;
    [[nodiscard]] bool seekable() const override;
    bool seek(std::uint64_t frame) override;

    /// Live VBR bitrate. FLAC is lossless and inherently variable
    /// rate, so this reports the trailing-window average of recent frames'
    /// compressed bytes, distinct from the static file-average sourceInfo()
    /// figure. See the .cpp read() for how the per-frame bytes are measured.
    [[nodiscard]] std::uint32_t currentBitrateKbps() const override;

private:
    /// Defined in the .cpp; holds the FLAC__StreamDecoder*, the staging buffer,
    /// the channel/depth facts from STREAMINFO, the int-to-float scale, and the
    /// live-bitrate meter, so every const accessor stays a trivial return.
    struct Impl;

    /// Private: only open() constructs this, handing over an already-populated
    /// Impl. Taking it by unique_ptr means the object is fully valid the instant
    /// it exists, with no separate init step that could be skipped.
    explicit FlacDecoder(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

}  // namespace rawform::audio
