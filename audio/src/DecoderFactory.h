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

// DecoderFactory.h
//
// The real format-dispatching IDecoderFactory, and the Engine's built-in
// default. Given a path it picks the right decoder (a native reference decoder
// where one owns the format, the FFmpeg long tail otherwise) and returns a
// ready IDecoder, or nullptr with a reason when nothing can open the file.
//
// Dispatch in one line: a magic-byte sniff of the leading bytes is the primary
// signal, the file extension is a hint that reorders the candidates, and a
// try-open cascade across the candidate decoders is the fallback that makes a
// wrong sniff self-correcting. The frozen-decoder rule is enforced structurally:
// a file identified as FLAC or MP3 is never offered to libsndfile (whose own
// optional FLAC/MP3 support would otherwise shadow the reference libraries). The
// full policy, including the one mislabel exception, is documented in the .cpp.
//
// Statelessness: the factory holds nothing, so the Engine can own one by value
// as its default and the CLI can stack-construct one for its info path. It is
// the same one-method IDecoderFactory the Engine injects through
// setDecoderFactory, so nothing above the decoder knows which decoder won.
//
// Dependency shape: this header stays free of every codec library, exactly like
// IDecoderFactory.h. The concrete decoder headers (and therefore the codec
// includes) are pulled in only by DecoderFactory.cpp.

#pragma once

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/IDecoderFactory.h"

#include <memory>
#include <string>

namespace rawform::audio {

class DecoderFactory final : public IDecoderFactory {
public:
    /// Open `path` and return a ready decoder, or nullptr on any failure. On
    /// failure error (when non-null) carries a human-readable reason drawn from
    /// the most likely decoder's own error; on success it is left untouched. A
    /// returned decoder is always fully opened and positioned at frame 0. Matches
    /// the IDecoderFactory contract verbatim.
    std::unique_ptr<IDecoder> open(const std::string& path,
                                   std::string*       error) override;
};

}  // namespace rawform::audio
