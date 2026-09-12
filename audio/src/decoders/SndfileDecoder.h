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

// SndfileDecoder.h
//
// IDecoder backed by libsndfile, covering the uncompressed reference formats
// (WAV, AIFF, and the other linear-PCM containers libsndfile reads). The
// DecoderFactory dispatches to it by format; it can also be constructed
// directly through its own factory.
//
// Dependency containment: <sndfile.h> appears in NONE of this header. The
// SNDFILE handle and SF_INFO live behind a pimpl whose definition sits in the
// .cpp, so the library's link to SndFile::sndfile stays PRIVATE and the CLI and
// the rest of the engine can include this header without inheriting the
// libsndfile include path. The cost is one pointer indirection per call, which
// is per-block and not per-sample, and therefore irrelevant next to the decode.
//
// Lifetime: there is no public constructor. open() is the only way to make one,
// and it returns either a fully-opened decoder or nullptr, so a half-open
// SndfileDecoder cannot exist. This mirrors the non-throwing DecoderFactory
// contract.

#pragma once

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/Types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace rawform::audio {

class SndfileDecoder final : public IDecoder {
public:
    /// Non-throwing factory. Opens path through libsndfile and returns a ready
    /// decoder, or nullptr on any failure (file missing, unreadable, or a format
    /// libsndfile cannot decode). When error is non-null it is written with a
    /// human-readable reason on failure, and left untouched on success. The
    /// return type is IDecoder, not SndfileDecoder, so callers program against
    /// the same interface the format-dispatching factory hands them.
    static std::unique_ptr<IDecoder> open(const std::string& path,
                                          std::string* error = nullptr);

    ~SndfileDecoder() override;

    /// Not copyable or movable: it owns an OS file handle through libsndfile and
    /// is always held by unique_ptr behind the IDecoder interface, so copying or
    /// moving the object itself has no meaning.
    SndfileDecoder(const SndfileDecoder&)            = delete;
    SndfileDecoder& operator=(const SndfileDecoder&) = delete;
    SndfileDecoder(SndfileDecoder&&)                 = delete;
    SndfileDecoder& operator=(SndfileDecoder&&)      = delete;

    /// IDecoder surface; the authoritative contract for each lives in IDecoder.h.
    [[nodiscard]] AudioFormat   format()      const override;
    [[nodiscard]] SourceInfo    sourceInfo()  const override;
    [[nodiscard]] std::uint64_t totalFrames() const override;
    std::size_t read(float* dst, std::size_t frames) override;
    [[nodiscard]] bool seekable() const override;
    bool seek(std::uint64_t frame) override;

private:
    /// Defined in the .cpp; holds the SNDFILE*, the channel count, and the facts
    /// computed once at open time so every const accessor stays a trivial return.
    struct Impl;

    /// Private: only open() constructs this, handing over an already-populated
    /// Impl. Taking the Impl by unique_ptr means the object is fully valid the
    /// instant it exists, with no separate init step that could be skipped.
    explicit SndfileDecoder(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> m_impl;
};

}  // namespace rawform::audio
