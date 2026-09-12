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

// FlacDecoderTest.cpp
//
// Standalone unit test for FlacDecoder, in the same shape as SndfileDecoderTest:
// no GoogleTest, no Catch2, just the tiny CHECK macro tallying failures so main()
// returns non-zero if any fired. The test owns both ends of the round trip by
// ENCODING a known ramp to a temp .flac with libFLAC's own stream encoder (the
// FLAC::FLAC dependency ships the encoder alongside the decoder), then reading it
// back through FlacDecoder and asserting the decoded float32 matches.
//
// What is covered, the FLAC acceptance criteria:
//   - 16-bit and 24-bit stereo round-trips match within the int->float tolerance
//   - mono confirms the channel count and interleave
//   - format(), totalFrames(), sourceInfo() report what was encoded (codec Flac,
//     the real container depth, a positive average bitrate)
//   - seek(frame) then read returns the ramp from that offset (the push-to-pull
//     staging buffer's post-seek front-trim is what this exercises)
//   - read at EOS returns 0; a straddling read returns the true remainder; one
//     oversized read returns exactly totalFrames (the "fill fully" rule)
//   - a nonexistent path yields nullptr and a non-empty error
//
// The ramp values are k/2048 for integer k, and 2^(bits-1)/2048 is itself an
// integer at both 16 and 24 bits, so quantizing the ramp to PCM and back is
// lossless: FlacDecoder reproduces the ramp essentially exactly. The tolerance is
// still the 1.5-LSB integer bound used elsewhere, which keeps a boundary sample
// off the knife edge.

#include "decoders/FlacDecoder.h"

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/Types.h"

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cmath>
#include <cstdint>  // NOLINT: std::int*_t
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using rawform::audio::AudioFormat;
using rawform::audio::Codec;
using rawform::audio::FlacDecoder;
using rawform::audio::IDecoder;
using rawform::audio::SourceInfo;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,         \
                         __FILE__, __LINE__);                                  \
        }                                                                      \
    } while (0)

// Same deterministic, exactly-representable, bounded generator the sndfile test
// uses: an integer in [-2048, 2047] over 2048, exact in float32 and in [-1, 1).
// Keyed on the absolute SAMPLE index so each channel gets a distinct value and a
// wrong interleave shows up immediately.
inline float pat(std::uint64_t sampleIndex) noexcept {
    const std::int64_t k =
        static_cast<std::int64_t>(sampleIndex % 4096u) - 2048;
    return static_cast<float>(k) / 2048.0f;
}

// Quantize a [-1, 1) float to a signed integer of the given depth, clamped to
// full scale. For the ramp above this is lossless (k/2048 * 2^(bits-1) is the
// integer k * 2^(bits-1)/2048), so the encode/decode round-trip is exact.
FLAC__int32 quantize(float f, int bits) noexcept {
    const auto   scale = static_cast<double>(std::int64_t{1} << (bits - 1));
    long         v     = std::lround(static_cast<double>(f) * scale);
    const long   maxv  = (std::int64_t{1} << (bits - 1)) - 1;
    const long   minv  = -(std::int64_t{1} << (bits - 1));
    if (v > maxv) v = maxv;
    if (v < minv) v = minv;
    return static_cast<FLAC__int32>(v);
}

// Encodes `frames` frames of the ramp to a real .flac via libFLAC's stream
// encoder. Returns true on success. set_total_samples_estimate makes the encoder
// rewrite STREAMINFO with the true length on finish, so the decoder reports an
// exact totalFrames.
bool writeFlac(const std::string& path, int channels, int rate, int bits,
               std::uint64_t frames) {
    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (!enc) {
        return false;
    }
    FLAC__bool ok = true;
    ok &= FLAC__stream_encoder_set_channels(enc, static_cast<unsigned>(channels));
    ok &= FLAC__stream_encoder_set_bits_per_sample(enc, static_cast<unsigned>(bits));
    ok &= FLAC__stream_encoder_set_sample_rate(enc, static_cast<unsigned>(rate));
    ok &= FLAC__stream_encoder_set_total_samples_estimate(enc, frames);
    ok &= FLAC__stream_encoder_set_compression_level(enc, 5);
    if (!ok) {
        FLAC__stream_encoder_delete(enc);
        return false;
    }

    if (FLAC__stream_encoder_init_file(enc, path.c_str(), nullptr, nullptr) !=
        FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(enc);
        return false;
    }

    constexpr unsigned       block = 1024;
    std::vector<FLAC__int32> buf;
    std::uint64_t            written = 0;
    bool                     good    = true;
    while (written < frames) {
        const unsigned n = static_cast<unsigned>(
            std::min<std::uint64_t>(block, frames - written));
        buf.resize(static_cast<std::size_t>(n) * channels);
        for (unsigned f = 0; f < n; ++f) {
            for (int c = 0; c < channels; ++c) {
                const std::uint64_t sampleIndex =
                    (written + f) * static_cast<std::uint64_t>(channels) +
                    static_cast<std::uint64_t>(c);
                buf[static_cast<std::size_t>(f) * channels + c] =
                    quantize(pat(sampleIndex), bits);
            }
        }
        if (!FLAC__stream_encoder_process_interleaved(enc, buf.data(), n)) {
            good = false;
            break;
        }
        written += n;
    }

    good = good && FLAC__stream_encoder_finish(enc);
    FLAC__stream_encoder_delete(enc);
    return good;
}

// First flat-buffer index more than `tol` from the expected ramp value, or -1 if
// all `n` samples are within tolerance. Flat index i maps to absolute sample
// (baseSample + i).
long firstMismatch(const float* got, std::size_t n, std::uint64_t baseSample,
                   float tol) {
    for (std::size_t i = 0; i < n; ++i) {
        const float expected = pat(baseSample + static_cast<std::uint64_t>(i));
        if (std::fabs(got[i] - expected) > tol) {
            std::fprintf(stderr,
                         "  first mismatch at %zu: got %.9g expected %.9g\n",
                         i, static_cast<double>(got[i]),
                         static_cast<double>(expected));
            return static_cast<long>(i);
        }
    }
    return -1;
}

float integerTolerance(int bits) noexcept {
    const float lsb = 1.0f / static_cast<float>(std::uint32_t{1} << (bits - 1));
    return 1.5f * lsb;
}

// Decodes the whole stream into a flat interleaved float buffer in 1024-frame
// blocks, stopping at the first short/zero read (the EOS signal).
std::vector<float> readAll(IDecoder& dec) {
    const std::size_t ch = dec.format().channels;
    std::vector<float> out;
    std::vector<float> block(1024 * ch);
    for (;;) {
        const std::size_t got = dec.read(block.data(), 1024);
        if (got == 0) {
            break;
        }
        out.insert(out.end(), block.begin(),
                   block.begin() + static_cast<std::ptrdiff_t>(got * ch));
        if (got < 1024) {
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// 16-bit stereo round-trip plus the full property surface.
void testFlac16Stereo() {
    const std::string path = "rf_flac_16_stereo.flac";
    constexpr std::uint64_t frames = 6000;
    CHECK(writeFlac(path, 2, 44100, 16, frames));

    std::string err;
    std::unique_ptr<IDecoder> dec = FlacDecoder::open(path, &err);
    CHECK(dec != nullptr);
    if (dec) {
        CHECK(dec->format() == (AudioFormat{44100, 2}));
        CHECK(dec->totalFrames() == frames);
        CHECK(dec->seekable());

        const SourceInfo si = dec->sourceInfo();
        CHECK(si.codec == Codec::Flac);
        CHECK(si.bitsPerSample == 16);
        CHECK(si.bitrateKbps > 0);  // average rate, lossless has no constant one

        const std::vector<float> all = readAll(*dec);
        CHECK(all.size() == frames * 2);
        CHECK(firstMismatch(all.data(), all.size(), 0, integerTolerance(16)) < 0);

        float scratch[8];
        CHECK(dec->read(scratch, 4) == 0);  // a further read at EOS yields nothing
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 24-bit stereo: confirms the depth is reported and the wider int range decodes.
void testFlac24Stereo() {
    const std::string path = "rf_flac_24_stereo.flac";
    constexpr std::uint64_t frames = 6000;
    CHECK(writeFlac(path, 2, 96000, 24, frames));

    std::unique_ptr<IDecoder> dec = FlacDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        CHECK(dec->format() == (AudioFormat{96000, 2}));
        const SourceInfo si = dec->sourceInfo();
        CHECK(si.codec == Codec::Flac);
        CHECK(si.bitsPerSample == 24);

        const std::vector<float> all = readAll(*dec);
        CHECK(all.size() == frames * 2);
        CHECK(firstMismatch(all.data(), all.size(), 0, integerTolerance(24)) < 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 16-bit mono: channel count and single-channel interleave.
void testFlac16Mono() {
    const std::string path = "rf_flac_16_mono.flac";
    constexpr std::uint64_t frames = 4096;
    CHECK(writeFlac(path, 1, 48000, 16, frames));

    std::unique_ptr<IDecoder> dec = FlacDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        CHECK(dec->format() == (AudioFormat{48000, 1}));
        CHECK(dec->totalFrames() == frames);

        const std::vector<float> all = readAll(*dec);
        CHECK(all.size() == frames);
        CHECK(firstMismatch(all.data(), all.size(), 0, integerTolerance(16)) < 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Seek then read: the ramp must match from the sought offset. This is the
// staging-buffer front-trim under test (libFLAC lands on a block boundary at or
// before the target; FlacDecoder trims to the exact sample).
void testSeek() {
    const std::string path = "rf_flac_seek.flac";
    constexpr std::uint64_t frames = 12000;
    constexpr int ch = 2;
    CHECK(writeFlac(path, ch, 44100, 16, frames));

    std::unique_ptr<IDecoder> dec = FlacDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        constexpr std::size_t readFrames = 1500;
        std::vector<float>    buf(readFrames * ch);
        const float           tol = integerTolerance(16);

        // Into the middle, not on a block boundary, to exercise the trim.
        CHECK(dec->seek(5333));
        CHECK(dec->read(buf.data(), readFrames) == readFrames);
        CHECK(firstMismatch(buf.data(), readFrames * ch, 5333ull * ch, tol) < 0);

        // Back to the start.
        CHECK(dec->seek(0));
        CHECK(dec->read(buf.data(), readFrames) == readFrames);
        CHECK(firstMismatch(buf.data(), readFrames * ch, 0, tol) < 0);

        // One frame before the end: exactly one frame remains.
        CHECK(dec->seek(frames - 1));
        CHECK(dec->read(buf.data(), readFrames) == 1);
        CHECK(firstMismatch(buf.data(), static_cast<std::size_t>(ch),
                            (frames - 1) * ch, tol) < 0);

        // The very end: the next read is at EOS.
        CHECK(dec->seek(frames));
        CHECK(dec->read(buf.data(), readFrames) == 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// EOS and the partial final read, mirroring the sndfile test's contract checks.
void testEosAndPartial() {
    const std::string path = "rf_flac_eos.flac";
    constexpr std::uint64_t frames = 5000;
    constexpr int           ch = 2;
    const float             tol    = integerTolerance(16);
    CHECK(writeFlac(path, ch, 48000, 16, frames));

    std::unique_ptr<IDecoder> dec = FlacDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        std::vector<float> buf(6000 * ch);
        CHECK(dec->read(buf.data(), 4000) == 4000);
        CHECK(firstMismatch(buf.data(), static_cast<std::size_t>(4000 * ch), 0, tol) < 0);

        CHECK(dec->read(buf.data(), 2000) == 1000);  // straddles end, 1000 remain
        CHECK(firstMismatch(buf.data(), static_cast<std::size_t>(1000 * ch),
                            4000ull * ch, tol) < 0);

        CHECK(dec->read(buf.data(), 100) == 0);  // now at EOS
    }

    std::unique_ptr<IDecoder> dec2 = FlacDecoder::open(path);
    CHECK(dec2 != nullptr);
    if (dec2) {
        std::vector<float> buf(static_cast<std::size_t>(frames + 1000) * ch);
        CHECK(dec2->read(buf.data(), frames + 1000) == frames);  // short == EOS
        CHECK(firstMismatch(buf.data(), static_cast<std::size_t>(frames) * ch, 0, tol) < 0);
        CHECK(dec2->read(buf.data(), 1) == 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Open failure: a missing path yields nullptr and a non-empty error.
void testOpenFailure() {
    std::string err;
    std::unique_ptr<IDecoder> dec =
        FlacDecoder::open("rf_flac_does_not_exist.flac", &err);
    CHECK(dec == nullptr);
    CHECK(!err.empty());
}

}  // namespace

int main() {
    testFlac16Stereo();
    testFlac24Stereo();
    testFlac16Mono();
    testSeek();
    testEosAndPartial();
    testOpenFailure();

    if (g_failures == 0) {
        std::printf("FlacDecoderTest: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "FlacDecoderTest: %d check(s) failed\n", g_failures);
    return 1;
}
