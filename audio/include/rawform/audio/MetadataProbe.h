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

// MetadataProbe.h
//
// A descriptive-metadata reader for the long tail of files the native tag
// reader cannot parse. It is the read-side mirror of the decode-side fallback:
// just as DecoderFactory tries the reference decoders first and reaches
// FfmpegDecoder only for what they decline, the UI scanner reads tags with
// TagLib first and reaches this probe only for what TagLib declines (AC3, DTS,
// and any other container libavformat understands but TagLib does not). The probe
// reads facts WITHOUT decoding audio: it opens the container, finds the best
// audio stream, and reports its rate, channels, bitrate, bit depth, duration,
// codec label, and container tags.
//
// Why this lives in the engine and not the UI. The engine already links the four
// libav* libraries (PRIVATE), so the probe reuses a dependency that is physically
// present rather than introducing a second, independent avformat link in the Qt
// layer. Because rawform_audio is a STATIC library, that PRIVATE link threads
// onto the final executable's link line automatically, so a consumer (the UI)
// calls probeAudioMetadata() with NO avformat link or include of its own: this
// header names no libav* type, exactly like FfmpegDecoder.h. The Engine.h public
// surface is untouched; this is a separate, additive header.
//
// Build-configuration behavior. The implementation is compiled UNCONDITIONALLY
// (unlike FfmpegDecoder.cpp), but its body is guarded: with FFmpeg it does the
// real avformat read, without FFmpeg it returns std::nullopt. The symbol
// therefore exists in every build, so callers link unconditionally, and
// kMetadataProbeAvailable below lets a caller branch at compile time without its
// own #if on the engine's private FFmpeg state.
//
// Threading. probeAudioMetadata() is self-contained per call: it opens its own
// AVFormatContext, holds no shared state, and frees everything before returning.
// It is therefore safe to call concurrently across a thread pool, which is how
// the UI scanner runs it (QtConcurrent::mapped over the file list).

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rawform::audio {

/// Descriptive facts about one audio file, read without decoding. Plain value
/// type, no Qt, no libav* in sight, keeping with the engine's dependency-free
/// vocabulary. A zero / empty field means "unknown", the same convention the
/// UI's TrackData already uses, so the adapter is a straight copy.
struct ProbedMetadata {
    std::uint32_t sampleRate    = 0;  ///< frames per second; 0 if unknown
    std::uint16_t channels      = 0;  ///< 0 if unknown
    std::uint32_t bitrateKbps   = 0;  ///< stream, else container, else size/duration
    std::uint16_t bitsPerSample = 0;  ///< lossless depth only; 0 for lossy / unknown
    std::int64_t  durationMs    = 0;  ///< 0 if unknown

    /// Presentation-ready codec label ("AC3", "E-AC3", "DTS", "AAC", ...), derived
    /// from the stream's codec id, not from the file extension. Empty only when the
    /// codec id maps to nothing nameable, in which case the caller keeps whatever
    /// provisional label it had.
    std::string codecName;

    /// Container and stream tags, verbatim in libavformat's NATIVE key spelling
    /// (lower-case "title", "artist", "album", "track", "date", "genre",
    /// "album_artist", "encoder", ...). The probe stays vocabulary-neutral on
    /// purpose: mapping these onto a promoted-field vocabulary is presentation
    /// policy and belongs in the UI, which already owns that vocabulary. Order is
    /// format-level entries first, then stream-level. Typically EMPTY for raw
    /// elementary streams (a raw .ac3 / .dts carries no metadata at all).
    std::vector<std::pair<std::string, std::string>> tags;
};

/// Read descriptive metadata for the file at `path` without decoding its audio.
/// Returns a filled ProbedMetadata on success; std::nullopt when the file cannot
/// be opened or carries no decodable audio stream, AND when the build has no
/// FFmpeg (kMetadataProbeAvailable == false). On std::nullopt, *error (when
/// non-null) is written with a human-readable reason; on success it is left
/// untouched. Thread-safe per call (see the file header).
[[nodiscard]] std::optional<ProbedMetadata> probeAudioMetadata(const std::string& path,
                                                               std::string* error = nullptr);

/// Compile-time capability mirror of the engine's PUBLIC RAWFORM_HAVE_FFMPEG, so a
/// caller can skip the probe on a native-only build without naming that macro
/// itself. When RAWFORM_HAVE_FFMPEG is not visible to the including TU it defaults
/// to 0 here, which is the safe answer ("assume unavailable").
constexpr bool kMetadataProbeAvailable =
#if defined(RAWFORM_HAVE_FFMPEG) && RAWFORM_HAVE_FFMPEG
    true;
#else
    false;
#endif

}  // namespace rawform::audio
