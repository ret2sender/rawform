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

// ReplayGainScanner.cpp
//
// The implementation, and the only translation unit that sees <ebur128.h>. The
// shape is: open through the injected factory, init one libebur128 state from the
// decoder's native rate and channel count, pull the whole stream in chunks while
// feeding ebur128_add_frames_float, then read back the integrated loudness and the
// sample peak. Per-track loudness states are retained so albumResult can hand the
// whole set to ebur128_loudness_global_multiple, which is the correct gated
// combine over the album rather than an average of per-track numbers.
//
// Canonical format. The engine's decoders always hand out interleaved float32 at
// the source's native sample rate (the bit-perfect rule), which is exactly the
// input libebur128 expects, so there is no normalization or resampling stage here:
// the state's rate and channel count come straight from decoder->format().
//
// Channel weighting. ebur128_init installs the standard default channel map
// (channel 0 LEFT, 1 RIGHT, 2 CENTER, 4 LEFT_SURROUND, 5 RIGHT_SURROUND), which
// is correct for the overwhelmingly common mono and stereo cases and for
// canonical 5.1 ordering. The engine surfaces no per-channel speaker map, so
// exotic layouts ride the default; refining that would need channel-position
// metadata from the decoder, which is outside this class's contract.

#include "rawform/audio/ReplayGainScanner.h"

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/Types.h"

#include <ebur128.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rawform::audio {

namespace {

// One decode chunk, in frames. Large enough that the per-chunk overhead (the
// virtual read, the ebur128 call, the progress callback) is negligible against
// the DSP, small enough that cancel latency stays well under a frame of UI time:
// 8192 frames is under 0.2 s of audio at 44.1 kHz.
constexpr std::size_t kChunkFrames = 8192;

}  // namespace

// The retained-state store. Each measured track keeps its libebur128 state alive
// (the integrated-loudness combine needs every track's gating blocks), tagged with
// whether it was measurable so the combine can exclude silence. The running album
// peak is the max sample peak over all tracks, measurable or not.
struct ReplayGainScanner::Impl {
    struct Entry {
        ebur128_state* st         = nullptr;
        bool           measurable = false;
    };

    std::vector<Entry> entries;
    double             albumPeak = 0.0;

    ~Impl() {
        for (Entry& e : entries) {
            if (e.st) {
                ebur128_destroy(&e.st);
            }
        }
    }
};

ReplayGainScanner::ReplayGainScanner(IDecoderFactory& factory)
    : m_factory(factory), m_impl(std::make_unique<Impl>()) {}

ReplayGainScanner::~ReplayGainScanner() = default;

ReplayGainScanner::Status ReplayGainScanner::addTrack(
    const std::string& path, const ProgressFn& onProgress, TrackResult* out,
    std::string* error) {

    // 1. Open through the injected factory. A null return is the factory's "could
    // not open" signal, with the reason already written into error.
    std::unique_ptr<IDecoder> dec = m_factory.open(path, error);
    if (!dec) {
        return Status::OpenFailed;
    }

    const AudioFormat fmt = dec->format();
    if (!fmt.isValid()) {
        if (error) {
            *error = "decoder reported an invalid format (zero rate or channels)";
        }
        return Status::OpenFailed;
    }

    // 2. One state per track, mode I for integrated loudness plus sample peak.
    // HISTOGRAM mode bounds the state's memory: without it, MODE_I retains
    // every 100 ms block energy in a growing list for the life of the state, and
    // the album combine keeps every track's state alive until the group closes,
    // so a "scan as one album" over a very long selection grows without bound.
    // The histogram replaces the list with a fixed table per state, is supported
    // by ebur128_loudness_global_multiple (the combine below), and quantizes
    // block energies at 0.087 LU granularity, far below anything that matters at
    // ReplayGain precision.
    const auto channels   = static_cast<unsigned int>(fmt.channels);
    const auto sampleRate = static_cast<unsigned long>(fmt.sampleRate);
    ebur128_state*      st         = ebur128_init(
             channels, sampleRate,
             EBUR128_MODE_I | EBUR128_MODE_HISTOGRAM | EBUR128_MODE_SAMPLE_PEAK);
    if (!st) {
        if (error) {
            *error = "ebur128_init failed (unsupported format or out of memory)";
        }
        return Status::OpenFailed;
    }

    // 3. Decode to end of stream, feeding every frame. The IDecoder contract is
    // "fill fully, return short only at EOS, return 0 when already at EOS", so a
    // read of fewer than the requested frames is the end marker. Progress and the
    // cancel check ride each chunk; on cancel the partial state is destroyed and
    // never retained, so it cannot corrupt a later album combine.
    const std::uint64_t total = dec->totalFrames();
    std::vector<float>  buf(kChunkFrames * channels);
    std::uint64_t       pulled = 0;

    for (;;) {
        const std::size_t got = dec->read(buf.data(), kChunkFrames);
        if (got > 0) {
            const int rc = ebur128_add_frames_float(st, buf.data(), got);
            if (rc != EBUR128_SUCCESS) {
                if (error) {
                    *error = "ebur128_add_frames_float failed during decode";
                }
                ebur128_destroy(&st);
                return Status::OpenFailed;
            }
            pulled += got;
        }

        if (onProgress) {
            double frac = (total > 0)
                              ? static_cast<double>(pulled) /
                                    static_cast<double>(total)
                              : 0.0;
            if (frac > 1.0) {
                frac = 1.0;
            }
            if (!onProgress(frac)) {
                ebur128_destroy(&st);
                return Status::Canceled;
            }
        }

        if (got < kChunkFrames)  // end of stream reached on this read
            break;
    }

    // 4. Integrated loudness. The library returns a value at or below the silence
    // floor (or a non-finite sentinel) for digital silence or a too-short clip; we
    // map that to "unmeasurable" rather than to a garbage gain.
    double    lufs       = 0.0;
    const int loudnessRc = ebur128_loudness_global(st, &lufs);
    const bool measurable = (loudnessRc == EBUR128_SUCCESS) &&
                            std::isfinite(lufs) && lufs > kLoudnessFloorLufs;

    // 5. Sample peak: the max absolute sample over all channels, cumulative across
    // the whole track. A channel query failure just leaves that channel out of the
    // max; it never aborts the measurement.
    double peak = 0.0;
    for (unsigned int c = 0; c < channels; ++c) {
        double chPeak = 0.0;
        if (ebur128_sample_peak(st, c, &chPeak) == EBUR128_SUCCESS &&
            chPeak > peak) {
            peak = chPeak;
        }
    }

    if (out) {
        out->measurable     = measurable;
        out->integratedLufs = measurable ? lufs : 0.0;
        out->gainDb         = measurable ? (kReferenceLufs - lufs) : 0.0;
        out->peak           = peak;
    }

    // 6. Retain for the album combine and fold the peak into the album maximum.
    m_impl->entries.push_back(Impl::Entry{st, measurable});
    if (peak > m_impl->albumPeak) {
        m_impl->albumPeak = peak;
    }

    return Status::Ok;
}

bool ReplayGainScanner::albumResult(AlbumResult* out) const {
    // The gated combine runs over the measurable tracks only; silence contributes
    // no loudness (and would only drag the integration toward the floor).
    std::vector<ebur128_state*> live;
    live.reserve(m_impl->entries.size());
    for (const Impl::Entry& e : m_impl->entries) {
        if (e.measurable && e.st) {
            live.push_back(e.st);
        }
    }
    if (live.empty()) {
        return false;  // nothing to measure; caller writes no album gain
    }

    double    lufs = 0.0;
    const int rc =
        ebur128_loudness_global_multiple(live.data(), live.size(), &lufs);
    if (rc != EBUR128_SUCCESS || !std::isfinite(lufs) ||
        lufs <= kLoudnessFloorLufs) {
        return false;
    }

    if (out) {
        out->measurable     = true;
        out->integratedLufs = lufs;
        out->gainDb         = kReferenceLufs - lufs;
        out->peak           = m_impl->albumPeak;
    }
    return true;
}

void ReplayGainScanner::reset() {
    for (Impl::Entry& e : m_impl->entries) {
        if (e.st) {
            ebur128_destroy(&e.st);
        }
    }
    m_impl->entries.clear();
    m_impl->albumPeak = 0.0;
}

}  // namespace rawform::audio
