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

// Types.h
//
// Plain-old-data value types shared across the rawform audio engine. This
// header is deliberately dependency-free: no Qt, no codec headers, only the
// standard integer types and std::string for the one higher-level
// payload that needs it. Everything that crosses a thread boundary on the hot
// path stays trivially copyable and cheap to pass by value, which is what lets
// the engine, the decoders, and the sink agree on a common vocabulary without
// dragging heavier headers across the thread boundaries.
//
// Pipeline invariant worth restating here, since these types encode it: the
// canonical PCM format flowing through the whole engine is interleaved float32
// and nothing else. Sample format is therefore NOT a field anyone can vary;
// AudioFormat carries sample rate and channel count only, and the sink is the
// single place that converts float32 to a device format.

#pragma once

#include <cstdint>
#include <string>

namespace rawform::audio {

/// Describes the shape of an interleaved float32 stream. There is intentionally
/// no sample-format member: the only PCM representation in the engine is float32,
/// so the sole degrees of freedom are how fast frames arrive (sampleRate) and how
/// many samples make up one frame (channels). A "frame" throughout the engine
/// means one sample per channel; a stereo frame is two contiguous floats.
struct AudioFormat {
    std::uint32_t sampleRate = 0;  ///< frames per second, e.g. 44100, 48000, 96000
    std::uint16_t channels   = 0;  ///< samples per frame, e.g. 2 for stereo

    /// A format is usable only once a decoder has reported real values. Zero in
    /// either field means "not yet known", so a default-constructed AudioFormat
    /// doubles as the unconfigured state.
    [[nodiscard]] constexpr bool isValid() const noexcept {
        return sampleRate != 0 && channels != 0;
    }
};

/// Two formats are equal when both fields match. The engine compares formats to
/// decide whether a track change requires reconfiguring the output device for a
/// new native sample rate (the bit-perfect rule: the device follows the track,
/// the track is never resampled to fit the device).
constexpr bool operator==(const AudioFormat& a, const AudioFormat& b) noexcept {
    return a.sampleRate == b.sampleRate && a.channels == b.channels;
}
constexpr bool operator!=(const AudioFormat& a, const AudioFormat& b) noexcept {
    return !(a == b);
}

/// Transport state of the engine. This is the lean set the state machine needs
/// to express to the controlling thread. Transitional or error states
/// (buffering, seeking, error) are not modeled; the engine reports errors through
/// the listener's onError, not through a state. Kept to a single byte because it
/// is published through an atomic back to the controlling thread.
enum class State : std::uint8_t {
    Stopped,  ///< no source playing; sink idle. A track may still be REMEMBERED
              /// here (see Engine: stop rewinds, play replays it from frame 0).
    Playing,  ///< decoding and pulling; the sink RT thread is consuming frames
    Paused,   ///< source loaded and positioned, but the sink is not consuming
};

/// Which point in the real-time pull chain the visualization tap (the spectrum
/// analyzer's feed) samples. This selects WHERE in EngineRingSource::pull() the
/// scope tap captures its mono block, relative to the master-gain multiply, and
/// is set from the controlling thread (Engine::setScopeSource) and read once per
/// pull on the RT thread, exactly like the master gain beside it.
///
///   - PostGain (the default): tap AFTER the gain multiply, so the bars show the
///     signal as it is actually heard. Lowering the volume visibly lowers the
///     spectrum. At unity the gain multiply is skipped, so this is byte-identical
///     to the decoded signal and the bit-perfect path still feeds the visualizer
///     untouched.
///   - PreGain: tap BEFORE the gain multiply, so the bars reflect the decoded
///     source at full scale regardless of the volume setting (a steady display
///     while you ride the fader).
///
/// It is purely a tap-position choice: it changes nothing about the audio sent to
/// the device, only which copy of the block is handed to the analyzer. A single
/// byte, trivially copyable, so it publishes cleanly through a relaxed atomic. The
/// Qt layer maps these two members to friendlier Settings labels (Output / Source);
/// the engine keeps the precise gain-relative names.
enum class ScopeSource : std::uint8_t {
    PostGain,  ///< tap as heard (volume-dependent); the default
    PreGain,   ///< tap the decoded source (volume-independent)
};

/// The encoding family a decoder reports for an opened source. A descriptive
/// tag for the user-facing status line, deliberately NOT used for any pipeline
/// decision: every decoder converts to interleaved float32 before the audio
/// reaches the engine. An enum rather than a string keeps SourceInfo trivially
/// copyable and keeps the tag-to-display-name mapping out of this zero-Qt
/// library (the Qt layer owns it; the headless CLI carries its own).
///
/// Family, not implementation: the tag names what the source IS, not which
/// library decoded it. Which decoder ran is recorded separately in
/// SourceInfo.decoder (DecoderKind). In a normal build the families are
/// disjoint across the decoder cascade (libsndfile owns Pcm, libFLAC owns Flac,
/// libmpg123 owns Mp3, FFmpeg owns the rest), and FfmpegDecoder maps a FLAC or
/// MP3 source back onto Flac/Mp3 so the tag stays truthful in a build where a
/// native decoder is absent.
///
/// The lossless/lossy split lives in the SourceInfo fields beside this tag, not
/// in the enum: a lossless member (Pcm, Flac, Alac, and Wma when the source is
/// WMA Lossless) reports its real container depth in bitsPerSample, every lossy
/// member reports 0 (there is no meaningful PCM container depth behind a lossy
/// stream), and bitrateKbps is exact for Pcm and stream- or container-derived
/// otherwise.
///
/// One accepted loss: libsndfile's WAV/AIFF integer-PCM and IEEE-float subtypes
/// all report Pcm, so {codec = Pcm, bitsPerSample = 32} cannot by itself tell
/// 32-bit integer from 32-bit float. The pipeline decodes both to float32
/// regardless, so nothing depends on the distinction; if a status line ever
/// needs to render "32-bit float" distinctly, the fix is one more field on
/// SourceInfo or a PcmInt/PcmFloat split. Recorded here so the loss is a
/// decision, not an accident.
enum class Codec : std::uint8_t {
    Unknown,  ///< no source opened, or a subtype this build cannot classify
    Pcm,      ///< uncompressed linear PCM, integer or IEEE float (libsndfile)
    Flac,     ///< FLAC lossless (libFLAC); bitsPerSample is the real 16/24 depth
    Mp3,      ///< MPEG-1/2 Audio Layer III (libmpg123); bitsPerSample == 0 (lossy)
    Aac,      ///< Advanced Audio Coding (FFmpeg); lossy, bitsPerSample == 0
    Ac3,      ///< Dolby Digital AC-3 / E-AC-3 (FFmpeg); lossy, bitsPerSample == 0
    Dts,      ///< DTS Coherent Acoustics (FFmpeg); lossy, bitsPerSample == 0
    Opus,     ///< Opus (FFmpeg; libsndfile only on a build without FFmpeg); lossy
    Vorbis,   ///< Ogg Vorbis (FFmpeg; libsndfile only on a build without FFmpeg); lossy
    Alac,     ///< Apple Lossless (FFmpeg); LOSSLESS, real container depth reported
    Wma,      ///< Windows Media Audio family (FFmpeg); lossy, or lossless for WMAL
    Other,    ///< any other FFmpeg-decoded codec; plays, status line shows generic
};

/// Which concrete decoder produced a source. This is provenance (HOW a file was
/// decoded), distinct from Codec (WHAT the source encoding is): a FLAC file is
/// always Codec::Flac, but it is DecoderKind::Flac when the reference libFLAC path
/// took it and would be DecoderKind::Ffmpeg only in a build where that native
/// decoder was absent. It rides up to the UI alongside Codec so the player can
/// surface the decode path, and consolidates what the DecoderFactory's debug log
/// shows into queryable metadata. Trivially copyable; names map in the Qt layer
/// and the CLI. Unknown is the default for a source no real decoder stamped (the
/// injected test ramp, for instance).
enum class DecoderKind : std::uint8_t {
    Unknown,  ///< not stamped by a real decoder
    Sndfile,  ///< libsndfile (linear PCM and its secondary formats)
    Flac,     ///< libFLAC reference decoder
    Mpg123,   ///< libmpg123 reference decoder
    Ffmpeg,   ///< FFmpeg terminal fallback (the long tail)
};

/// What a decoder reports about an opened source beyond its float32 pipeline
/// shape: the descriptive facts that feed the user-facing status line, and
/// nothing the pipeline math depends on. The pipeline-critical facts (native rate
/// and channel count, length in frames, seek capability) are first-class methods
/// on IDecoder (format(), totalFrames(), seekable()), not fields here, so this
/// struct deliberately does NOT duplicate them.
///
/// bitsPerSample is the source CONTAINER depth (16/24/32), not the float32 the
/// stream is decoded to. bitrateKbps for PCM is the exact data rate
/// sampleRate*channels*bitsPerSample/1000; compressed codecs instead source it
/// from the stream itself: the MPEG frame header for MP3, and the container or a
/// file-size-over-duration average for the FLAC and FFmpeg paths.
struct SourceInfo {
    Codec         codec         = Codec::Unknown;
    std::uint16_t bitsPerSample = 0;  ///< source container depth; 0 when unknown
    std::uint32_t bitrateKbps   = 0;  ///< exact for PCM; stream/container-derived for compressed
    /// Which concrete decoder produced this source. Provenance, not source
    /// content: it records HOW the file was decoded, not WHAT it is. It rides up
    /// to the UI the same way codec does, so the player can show, for example,
    /// that an AAC file was handled by the FFmpeg fallback while a FLAC went to the
    /// reference decoder. Each decoder stamps its own kind at open; the name
    /// mapping lives in the Qt layer (and the CLI), like the Codec mapping.
    DecoderKind   decoder       = DecoderKind::Unknown;
};

/// One selectable output device, the portable descriptor both platform sinks
/// populate for enumeration. `id` is the PERSISTENT platform identity (the
/// CoreAudio device UID string on macOS, the PipeWire node.name on Linux),
/// never a transient numeric handle, so a remembered selection survives
/// reboots, hot-plugs, and re-enumeration. `name` is the human label for a
/// device picker; `isDefault` marks the system default output as of the
/// enumeration that produced this entry.
struct AudioDeviceInfo {
    std::string id;
    std::string name;
    bool        isDefault = false;
};

/// The higher-level, engine-to-controller description of whatever the engine has
/// loaded: identity (the path) plus the descriptive facts and a derived duration.
/// This is the payload of Engine::Listener::onTrackChanged, built once when a
/// track becomes current, and the value Engine::currentTrackInfo() returns. Like
/// AudioDeviceInfo it is NOT trivially copyable (it owns a std::string), which is
/// fine: it never travels on the RT path, only across the engine-thread ->
/// controller notification, where a string copy is irrelevant.
///
/// Duration is intentionally a DERIVED accessor rather than a stored field, so
/// totalFrames stays the single source of truth and the two can never disagree.
/// The pipeline-critical facts (format, totalFrames) are first-class; codec/depth
/// /bitrate ride along in `source` for the status line. Nothing here is needed by
/// the RT thread, by design.
struct TrackInfo {
    std::string   path;             ///< identity: the file that produced this stream
    AudioFormat   format;           ///< native rate + channels (the sink's target)
    SourceInfo    source;           ///< codec / bit-depth / bitrate, status line only
    std::uint64_t totalFrames = 0;  ///< length in frames; 0 when unknown

    /// Whether the live decoder can reposition within this track. Surfaced
    /// here, alongside the other descriptive facts, so a UI can
    /// enable or disable its scrubber up front rather than discovering an
    /// unseekable stream only by a refused seek (Engine::seek -> onError). It is
    /// pure provenance of what the decoder reported at open; nothing in the
    /// pipeline math reads it, and it keeps TrackInfo's value semantics (a bool
    /// adds no ownership). Set in the engine from IDecoder::seekable(); false on a
    /// default/empty TrackInfo (the Stopped readout).
    bool seekable = false;

    /// Length in seconds, derived from the two fields above. Returns 0.0 when the
    /// rate is unknown (a default/empty TrackInfo) so callers never divide by
    /// zero; a 0-frame but valid-rate track correctly yields 0.0 as well.
    [[nodiscard]] double durationSeconds() const noexcept {
        return format.sampleRate == 0
                   ? 0.0
                   : static_cast<double>(totalFrames) / format.sampleRate;
    }
};

}  // namespace rawform::audio
