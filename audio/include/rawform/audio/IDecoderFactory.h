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

// IDecoderFactory.h
//
// The seam by which the Engine turns a path into a decoder. The Engine takes a
// path through enqueue() and must open it, but it must NOT hard-wire a concrete
// decoder: tests need to substitute an in-memory source (so the portable
// transport soak stays free of every codec library, the way RingBufferTest and
// EngineTest are), and the default opener needs to be swappable without
// touching the transport. This one-method interface is exactly that injection
// point, and it deliberately mirrors the non-throwing factory contract every
// concrete decoder already follows (open returns a ready IDecoder or nullptr,
// never a half-open object, never an exception).
//
// Like the other engine interface headers this one is dependency-free past the
// shared types: it pulls in IDecoder (for the return type) and the standard
// string/memory headers, and nothing else. No codec library include reaches it.
//
// The Engine ships with a built-in default: the format-dispatching
// DecoderFactory, held by value inside Engine::Impl, so callers who
// do not inject anything still play every native format (WAV/AIFF via
// libsndfile, FLAC via libFLAC, MP3 via libmpg123, plus the FFmpeg long tail on
// builds that have it). setDecoderFactory overrides it; the pointer is
// non-owning and must outlive the Engine, the same ownership shape as
// setListener.

#pragma once

#include "rawform/audio/IDecoder.h"

#include <memory>
#include <string>

namespace rawform::audio {

struct IDecoderFactory {
    virtual ~IDecoderFactory() = default;

    /// Open `path` and return a ready-to-use decoder, or nullptr on any failure.
    /// When error is non-null it is written with a human-readable reason on
    /// failure and left untouched on success, the same contract every concrete
    /// decoder's own open() follows. A returned decoder is always fully opened
    /// and positioned at frame 0.
    virtual std::unique_ptr<IDecoder> open(const std::string& path,
                                           std::string*       error) = 0;
};

}  // namespace rawform::audio
