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

// SndfileDecoderTest.cpp
//
// Standalone, dependency-free-except-libsndfile unit test for SndfileDecoder. No
// GoogleTest, no Catch2: the same tiny CHECK macro the RingBuffer test uses,
// tallying failures so main() returns non-zero if any fired, which is all CTest
// needs. The test owns both ends of the round trip: it WRITES a known file with
// libsndfile, then reads it back through SndfileDecoder and asserts the decoded
// float32 matches what was written.
//
// What is covered, the acceptance criteria:
//   - float WAV round-trip is bit-exact (FLOAT subtype is a lossless passthrough)
//   - integer WAV round-trip (PCM_16 and PCM_24) matches within 1.5 LSB; the
//     headroom over an exact one-LSB bound keeps a boundary sample off the knife
//     edge of the float comparison
//   - mono and stereo: the pattern is keyed on the absolute SAMPLE index, so a
//     wrong interleave or channel count shows up as a mismatch immediately
//   - format(), totalFrames(), sourceInfo() report what was written
//   - seek(frame) then read returns the pattern from that offset
//   - read at EOS returns 0; a read straddling the end returns the true remainder
//   - an AIFF round-trip exercises a second container through the same path
//
// The pattern values are exactly representable in float32 and bounded to [-1, 1),
// so the same generator serves the exact float test and the bounded integer
// tests. The integer tolerance assumes libsndfile's normalized float scaling,
// which open() pins with SFC_SET_NORM_FLOAT, so this is a guarantee, not a hope.

#include "decoders/SndfileDecoder.h"

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/Types.h"

#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <cstdint>  // NOLINT: std::int*_t
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using rawform::audio::AudioFormat;
using rawform::audio::Codec;
using rawform::audio::IDecoder;
using rawform::audio::SndfileDecoder;
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

// Deterministic, exactly-representable, bounded sample generator. An integer in
// [-2048, 2047] over 2048 is exact in float32 and lies in [-1, 1), so the FLOAT
// round-trip can compare with operator== while the integer-PCM round-trip stays
// inside full scale. Keying on the absolute SAMPLE index (not the frame) means
// each channel gets a distinct value, which is what exercises interleave order.
inline float pat(std::uint64_t sampleIndex) noexcept {
    const std::int64_t k =
        static_cast<std::int64_t>(sampleIndex % 4096u) - 2048;
    return static_cast<float>(k) / 2048.0f;
}

// Writes a file of `frames` frames in the given format/channels/rate using the
// pattern above, via libsndfile. Returns true on success. Normalization is set
// on the write side too so the float-to-integer scaling matches the read side.
bool writeFile(const std::string& path, int sfFormat, int channels,
               int sampleRate, std::uint64_t frames) {
    SF_INFO info{};
    info.samplerate = sampleRate;
    info.channels   = channels;
    info.format     = sfFormat;

    SNDFILE* snd = sf_open(path.c_str(), SFM_WRITE, &info);
    if (!snd) {
        std::fprintf(stderr, "writeFile: sf_open(%s) failed: %s\n",
                     path.c_str(), sf_strerror(nullptr));
        return false;
    }
    sf_command(snd, SFC_SET_NORM_FLOAT, nullptr, SF_TRUE);

    constexpr sf_count_t blockFrames = 1024;
    std::vector<float> buf;
    std::uint64_t written = 0;
    bool ok = true;
    while (written < frames) {
        const sf_count_t n = std::min<sf_count_t>(
            blockFrames, static_cast<sf_count_t>(frames - written));
        buf.resize(static_cast<std::size_t>(n) * static_cast<std::size_t>(channels));
        for (sf_count_t f = 0; f < n; ++f) {
            for (int c = 0; c < channels; ++c) {
                const std::uint64_t sampleIndex =
                    (written + static_cast<std::uint64_t>(f)) *
                        static_cast<std::uint64_t>(channels) +
                    static_cast<std::uint64_t>(c);
                buf[static_cast<std::size_t>(f) * channels + c] = pat(sampleIndex);
            }
        }
        if (sf_writef_float(snd, buf.data(), n) != n) {
            ok = false;
            break;
        }
        written += static_cast<std::uint64_t>(n);
    }
    sf_close(snd);
    return ok;
}

// Returns the flat-buffer index of the first sample more than `tol` away from
// the expected pattern value, or -1 if all `n` samples are within tolerance. The
// flat index i maps to absolute sample (baseSample + i). tol == 0 means an exact
// comparison. Taking a pointer plus count lets callers check a sub-range of a
// buffer without constructing a temporary vector (and sidesteps the braced-init
// range-vs-initializer-list ambiguity that a vector argument would invite).
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

// One quantization step for an n-bit signed PCM sample, expressed in the [-1, 1)
// normalized float domain, with 1.5x headroom so a sample that quantizes a full
// LSB off does not flake on the float comparison.
float integerTolerance(int bits) noexcept {
    const float lsb = 1.0f / static_cast<float>(std::uint32_t{1} << (bits - 1));
    return 1.5f * lsb;
}

// Decodes the whole stream into a flat interleaved float buffer, in 1024-frame
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
            break;  // short read == end of stream reached
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Exact float round-trip, stereo. FLOAT subtype is lossless, so tol is 0.
void testFloatStereoExact() {
    const std::string path = "rf_sndfile_float_stereo.wav";
    constexpr std::uint64_t frames = 5000;
    CHECK(writeFile(path, SF_FORMAT_WAV | SF_FORMAT_FLOAT, 2, 48000, frames));

    std::string err;
    std::unique_ptr<IDecoder> dec = SndfileDecoder::open(path, &err);
    CHECK(dec != nullptr);
    if (dec) {
        CHECK(dec->format() == (AudioFormat{48000, 2}));
        CHECK(dec->totalFrames() == frames);
        CHECK(dec->seekable());

        const SourceInfo si = dec->sourceInfo();
        CHECK(si.codec == Codec::Pcm);
        CHECK(si.bitsPerSample == 32);
        CHECK(si.bitrateKbps == 3072);  // 48000 * 2 * 32 / 1000

        const std::vector<float> all = readAll(*dec);
        CHECK(all.size() == frames * 2);
        CHECK(firstMismatch(all.data(), all.size(), 0, 0.0f) < 0);

        // A further read at EOS yields nothing.
        float scratch[8];
        CHECK(dec->read(scratch, 4) == 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Exact float round-trip, mono: confirms channel count and the single-channel
// interleave (trivial, but the reported channels and frame math must be right).
void testFloatMonoExact() {
    const std::string path = "rf_sndfile_float_mono.wav";
    constexpr std::uint64_t frames = 4096;
    CHECK(writeFile(path, SF_FORMAT_WAV | SF_FORMAT_FLOAT, 1, 44100, frames));

    std::unique_ptr<IDecoder> dec = SndfileDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        CHECK(dec->format() == (AudioFormat{44100, 1}));
        CHECK(dec->totalFrames() == frames);
        CHECK(dec->sourceInfo().bitrateKbps == 1411);  // 44100 * 1 * 32 / 1000

        const std::vector<float> all = readAll(*dec);
        CHECK(all.size() == frames);
        CHECK(firstMismatch(all.data(), all.size(), 0, 0.0f) < 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 16-bit integer WAV, stereo: match within 1.5 LSB of 16-bit full scale.
void testPcm16Stereo() {
    const std::string path = "rf_sndfile_pcm16_stereo.wav";
    constexpr std::uint64_t frames = 6000;
    CHECK(writeFile(path, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 2, 44100, frames));

    std::unique_ptr<IDecoder> dec = SndfileDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        const SourceInfo si = dec->sourceInfo();
        CHECK(si.codec == Codec::Pcm);
        CHECK(si.bitsPerSample == 16);
        CHECK(si.bitrateKbps == 1411);  // 44100 * 2 * 16 / 1000

        const std::vector<float> all = readAll(*dec);
        CHECK(all.size() == frames * 2);
        CHECK(firstMismatch(all.data(), all.size(), 0, integerTolerance(16)) < 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// 24-bit integer WAV, stereo: match within 1.5 LSB of 24-bit full scale.
void testPcm24Stereo() {
    const std::string path = "rf_sndfile_pcm24_stereo.wav";
    constexpr std::uint64_t frames = 6000;
    CHECK(writeFile(path, SF_FORMAT_WAV | SF_FORMAT_PCM_24, 2, 44100, frames));

    std::unique_ptr<IDecoder> dec = SndfileDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        const SourceInfo si = dec->sourceInfo();
        CHECK(si.bitsPerSample == 24);
        CHECK(si.bitrateKbps == 2116);  // 44100 * 2 * 24 / 1000

        const std::vector<float> all = readAll(*dec);
        CHECK(all.size() == frames * 2);
        CHECK(firstMismatch(all.data(), all.size(), 0, integerTolerance(24)) < 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Seek then read: samples must match the pattern from the sought offset. Uses a
// float file so the comparison is exact and the only thing under test is seek.
void testSeek() {
    const std::string path = "rf_sndfile_seek.wav";
    constexpr std::uint64_t frames = 8000;
    constexpr int ch = 2;
    CHECK(writeFile(path, SF_FORMAT_WAV | SF_FORMAT_FLOAT, ch, 48000, frames));

    std::unique_ptr<IDecoder> dec = SndfileDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        constexpr std::size_t readFrames = 1000;
        std::vector<float> buf(readFrames * ch);

        // Seek into the middle and read.
        CHECK(dec->seek(2000));
        CHECK(dec->read(buf.data(), readFrames) == readFrames);
        CHECK(firstMismatch(buf.data(), readFrames * ch, 2000ull * ch, 0.0f) < 0);

        // Seek back to the start and read again.
        CHECK(dec->seek(0));
        CHECK(dec->read(buf.data(), readFrames) == readFrames);
        CHECK(firstMismatch(buf.data(), readFrames * ch, 0, 0.0f) < 0);

        // Seek to one frame before the end, read exactly that frame.
        CHECK(dec->seek(frames - 1));
        CHECK(dec->read(buf.data(), readFrames) == 1);  // only one frame remains
        CHECK(firstMismatch(buf.data(), static_cast<std::size_t>(ch),
                            (frames - 1) * ch, 0.0f) < 0);

        // Seek to the very end: the next read is at EOS.
        CHECK(dec->seek(frames));
        CHECK(dec->read(buf.data(), readFrames) == 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// EOS and the partial final read. A read straddling the end returns the true
// remainder, the next read returns 0, and one oversized read of the whole file
// returns exactly totalFrames (the "fill fully, short only at EOS" rule).
void testEosAndPartial() {
    const std::string path = "rf_sndfile_eos.wav";
    constexpr std::uint64_t frames = 5000;
    constexpr int ch = 2;
    CHECK(writeFile(path, SF_FORMAT_WAV | SF_FORMAT_FLOAT, ch, 48000, frames));

    std::unique_ptr<IDecoder> dec = SndfileDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        std::vector<float> buf(6000 * ch);

        // First chunk fully satisfied.
        CHECK(dec->read(buf.data(), 4000) == 4000);
        CHECK(firstMismatch(buf.data(), static_cast<std::size_t>(4000 * ch), 0, 0.0f) < 0);

        // Second chunk straddles the end: only the 1000-frame remainder comes back.
        CHECK(dec->read(buf.data(), 2000) == 1000);
        CHECK(firstMismatch(buf.data(), static_cast<std::size_t>(1000 * ch),
                            4000ull * ch, 0.0f) < 0);

        // Now at EOS.
        CHECK(dec->read(buf.data(), 100) == 0);
    }

    // A separate decoder, one oversized read of the entire file.
    std::unique_ptr<IDecoder> dec2 = SndfileDecoder::open(path);
    CHECK(dec2 != nullptr);
    if (dec2) {
        std::vector<float> buf(static_cast<std::size_t>(frames + 1000) * ch);
        CHECK(dec2->read(buf.data(), frames + 1000) == frames);  // short == EOS
        CHECK(firstMismatch(buf.data(), static_cast<std::size_t>(frames) * ch, 0, 0.0f) < 0);
        CHECK(dec2->read(buf.data(), 1) == 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// A second container through the same path: AIFF, 16-bit stereo.
void testAiffPcm16() {
    const std::string path = "rf_sndfile_pcm16_stereo.aiff";
    constexpr std::uint64_t frames = 4000;
    CHECK(writeFile(path, SF_FORMAT_AIFF | SF_FORMAT_PCM_16, 2, 44100, frames));

    std::unique_ptr<IDecoder> dec = SndfileDecoder::open(path);
    CHECK(dec != nullptr);
    if (dec) {
        CHECK(dec->format() == (AudioFormat{44100, 2}));
        CHECK(dec->totalFrames() == frames);
        CHECK(dec->sourceInfo().bitsPerSample == 16);

        const std::vector<float> all = readAll(*dec);
        CHECK(all.size() == frames * 2);
        CHECK(firstMismatch(all.data(), all.size(), 0, integerTolerance(16)) < 0);
    }
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Open failure: a path that does not exist yields nullptr and a non-empty error.
void testOpenFailure() {
    std::string err;
    std::unique_ptr<IDecoder> dec =
        SndfileDecoder::open("rf_sndfile_does_not_exist.wav", &err);
    CHECK(dec == nullptr);
    CHECK(!err.empty());
}

}  // namespace

int main() {
    testFloatStereoExact();
    testFloatMonoExact();
    testPcm16Stereo();
    testPcm24Stereo();
    testSeek();
    testEosAndPartial();
    testAiffPcm16();
    testOpenFailure();

    if (g_failures == 0) {
        std::printf("SndfileDecoderTest: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "SndfileDecoderTest: %d check(s) failed\n", g_failures);
    return 1;
}
