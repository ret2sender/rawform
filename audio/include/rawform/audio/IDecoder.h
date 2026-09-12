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

// IDecoder.h
//
// The single abstraction every codec in the rawform engine implements. A decoder
// owns one opened source and hands the rest of the engine interleaved float32
// and nothing else: each concrete decoder converts its native sample format to
// float32 internally (libsndfile's SndfileDecoder does it with sf_readf_float,
// for instance), so there is no separate normalization stage anywhere
// downstream of the decoder.
//
// This header is intentionally as dependency-free as Types.h: it pulls in the
// shared value types and the standard size/integer types, and nothing else. In
// particular it never sees a codec library header, so the engine, the CLI, and
// the tests can program against the decoder contract without inheriting any
// third-party include. Concrete decoders keep their library headers private to
// their own .cpp via pimpl.
//
// Construction is deliberately NOT part of this interface. Decoders are created
// through a concrete non-throwing factory (SndfileDecoder::open, or the
// format-dispatching DecoderFactory) that returns either a ready-to-use
// IDecoder or nullptr, so a live IDecoder is always fully opened and valid.
// There are no throwing constructors and no half-open objects to guard against.

#pragma once

#include "rawform/audio/Types.h"

#include <cstddef>
#include <cstdint>

namespace rawform::audio {

class IDecoder {
public:
    virtual ~IDecoder() = default;

    /// Native format of the decoded stream: source sample rate and channel count.
    /// This is the format the engine configures the sink for (the device follows
    /// the track, the bit-perfect rule), so it reports the source's own rate and
    /// never a resampled one. The samples themselves are always interleaved
    /// float32, which is why sample format is not part of AudioFormat.
    [[nodiscard]] virtual AudioFormat format() const = 0;

    /// Descriptive facts for the user-facing status line: codec family, source
    /// container bit depth, bitrate. Nothing the pipeline math depends on; the
    /// pipeline-critical facts are the typed methods around this one. See the
    /// SourceInfo comment in Types.h for the rationale behind the split.
    [[nodiscard]] virtual SourceInfo sourceInfo() const = 0;

    /// Length of the source in frames, or 0 when unknown (an unseekable stream,
    /// say). A frame is one sample per channel, matching the rest of the engine.
    [[nodiscard]] virtual std::uint64_t totalFrames() const = 0;

    /// Decode interleaved float32 into dst, whose capacity must be at least
    /// frames*channels floats. Returns the number of frames actually produced.
    /// The decoder fills as many frames as the request asks for while data
    /// remains; a return value less than frames means end of stream was reached
    /// on this call, and a return of 0 means the stream was already at EOS. This
    /// "fill fully, short only at EOS" rule is what lets the decode thread detect
    /// end of stream from the read count alone, with no separate EOS query.
    virtual std::size_t read(float* dst, std::size_t frames) = 0;

    /// Whether seek() may be issued at all. For a regular file this is true; for
    /// an unseekable stream it is false and seek() will always fail.
    [[nodiscard]] virtual bool seekable() const = 0;

    /// Reposition so the next read() begins at the given absolute frame. Returns
    /// false if seeking is unsupported or the seek failed, in which case the read
    /// position is left unspecified: callers that care should treat a false
    /// return as "stop", not "retry from a known spot".
    virtual bool seek(std::uint64_t frame) = 0;

    /// Approximate bitrate (kbps) of the audio being decoded RIGHT NOW, for the
    /// live VBR readout in the player status line. This is distinct
    /// from sourceInfo().bitrateKbps, which is the fixed nominal/average figure
    /// computed once at open: this one tracks the moment, so a variable-rate
    /// stream's value rises and falls as decoding moves through the file.
    /// It is a short trailing-window average (smooth, not per-frame jittery) and
    /// naturally reflects the decoder's read-ahead position, a fraction of a
    /// second past what is being heard, which is imperceptible in a status readout.
    ///
    /// The DEFAULT returns 0, meaning "I publish no live figure; use the nominal".
    /// The engine treats 0 as "fall back to sourceInfo().bitrateKbps". Decoders
    /// whose rate genuinely varies (FLAC, MP3, the FFmpeg fallback) override this;
    /// constant-rate PCM (SndfileDecoder) and the test ramp leave the default,
    /// since for uncompressed audio the nominal already IS the instantaneous rate.
    ///
    /// Threading: called on the engine thread only, alongside read() and seek()
    /// (never the RT thread, never concurrently with read()), so an implementation
    /// needs no locking of its own.
    [[nodiscard]] virtual std::uint32_t currentBitrateKbps() const { return 0; }
};

}  // namespace rawform::audio
