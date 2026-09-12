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

// Mpg123Decoder.h
//
// IDecoder backed by libmpg123, the native reference decoder for MP3. MP3 goes
// through the reference library rather than libsndfile's optional MP3 support
// so the decode is the canonical one; the DecoderFactory enforces that
// structurally and dispatches here by content/extension. Like every decoder it
// is constructed only through its own non-throwing open().
//
// Dependency containment: <mpg123.h> appears in NONE of this header. The mpg123
// handle and the cached facts live behind a pimpl whose definition sits in the
// .cpp, so the library's link to MPG123::libmpg123 stays PRIVATE and callers can
// include this header without inheriting the libmpg123 include path.
//
// The one global in the codec libraries lives behind this decoder: older
// libmpg123 requires a process-wide mpg123_init() before any handle is
// created. open() guards that with a std::once_flag on the versions that need
// it (newer libmpg123 handles self-initialize and the call is skipped) and the
// library deliberately NEVER calls mpg123_exit(), treating the init as
// process-lifetime. This is documented in the .cpp; it is the single
// unavoidable shared-state exception in an engine that is otherwise free of
// process globals.
//
// Pull model: unlike libFLAC, libmpg123 is already a pull decoder (mpg123_read
// hands back as many bytes as asked for), so there is no staging buffer here.
// open() forces float32 output at the source's native rate and channel count and
// enables gapless trimming, so read() is a thin wrapper over mpg123_read.
//
// Lifetime: there is no public constructor. open() is the only way to make one,
// and it returns either a fully-opened decoder or nullptr, so a half-open
// Mpg123Decoder cannot exist. This mirrors the other decoders exactly.

#pragma once

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/Types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rawform::audio {

class Mpg123Decoder final : public IDecoder {
public:
    /// Non-throwing factory. Opens path through libmpg123, locks float32 output at
    /// the file's native rate/channels, enables gapless trimming, and returns a
    /// ready decoder or nullptr on any failure (file missing, not an MPEG audio
    /// stream, or a libmpg123 error). When error is non-null it is written with a
    /// human-readable reason on failure and left untouched on success.
    static std::unique_ptr<IDecoder> open(const std::string& path,
                                          std::string* error = nullptr);

    ~Mpg123Decoder() override;

    /// Not copyable or movable: it owns an mpg123 handle and an open file, and is
    /// always held by unique_ptr behind IDecoder, so moving the object itself has
    /// no meaning.
    Mpg123Decoder(const Mpg123Decoder&)            = delete;
    Mpg123Decoder& operator=(const Mpg123Decoder&) = delete;
    Mpg123Decoder(Mpg123Decoder&&)                 = delete;
    Mpg123Decoder& operator=(Mpg123Decoder&&)      = delete;

    /// IDecoder surface; the authoritative contract for each lives in IDecoder.h.
    [[nodiscard]] AudioFormat   format()      const override;
    [[nodiscard]] SourceInfo    sourceInfo()  const override;
    [[nodiscard]] std::uint64_t totalFrames() const override;
    std::size_t   read(float* dst, std::size_t frames) override;
    [[nodiscard]] bool seekable() const override;
    bool seek(std::uint64_t frame) override;

    /// Live VBR bitrate: the trailing-window average of the per-frame
    /// bitrates libmpg123 reports as decoding moves through the file. Overridden
    /// here because MP3 is the canonical variable-rate case.
    [[nodiscard]] std::uint32_t currentBitrateKbps() const override;

private:
    /// Defined in the .cpp; holds the mpg123_handle*, the channel count for the
    /// interleave math, the live-bitrate meter, and the facts cached at open time.
    struct Impl;

    /// Private: only open() constructs this, handing over an already-populated
    /// Impl, so the object is fully valid the instant it exists.
    explicit Mpg123Decoder(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

}  // namespace rawform::audio
