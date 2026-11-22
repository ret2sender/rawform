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

// ConformanceTest.cpp
//
// The cross-decoder conformance suite. Every concrete decoder faces the same
// boundary-contract questions (what a read at EOS returns, whether
// seek-to-length succeeds, what the final short read hands back). This target
// states that contract ONCE as a battery and runs it, unchanged, against every
// real decoder, so the boundary behavior is provably uniform. The per-format
// tests (SndfileDecoderTest, FlacDecoderTest, Mp3DecoderTest) keep their
// format-specific fidelity and round-trip assertions; this suite owns the shared
// boundary contract, and the speaker positions a sink declares for a decoded
// format (ChannelPositions.h).
//
// Same hand-rolled CHECK harness as the other tests: no GoogleTest, no Catch2, a
// failure tally that makes main() return non-zero. Each fixture is materialized
// to a temp file (WAV via libsndfile, native and Ogg FLAC via libFLAC, MP3 and AAC
// from embedded byte blobs that need no encoder), then opened THROUGH THE REAL
// DecoderFactory. Routing through the factory makes this a dispatch test as well:
// every fixture asserts it landed on the decoder it should have, which is the
// direct proof that the FFmpeg fallback never shadows a native reference decoder,
// that Ogg FLAC reaches libFLAC under an .oga or a .flac name, and that an
// ADTS-AAC stream (whose frame sync sniffs as MP3) correctly falls through
// libmpg123 to FFmpeg.
//
// Two tiers, because lossy and lossless differ in what can be asserted:
//   - STRICT (PCM, FLAC): exact frame math. The fixture is a known ramp; decoded
//     values match it, the decoded length equals totalFrames exactly, the final
//     short read returns the exact remainder, and a post-seek read lands on the
//     exact sample.
//   - STRUCTURAL (MP3, AAC): position-accurate, not bit-exact. totalFrames is a
//     header/container figure (an estimate for the FFmpeg-decoded long tail), and
//     inter-frame filter state makes a post-seek decode differ bit-for-bit from a
//     linear one. So the structural tier asserts the contract STRUCTURALLY:
//     decode is deterministic within the process, seek-to-end terminates at a
//     zero read, the remainder shrinks monotonically as the seek target grows,
//     and a repeated seek reproduces the same decode.
//
// The FFmpeg-decoded cases (AAC, AC3, DTS, WMA, ALAC as embedded byte blobs;
// Vorbis and Opus generated through libsndfile's Ogg writers), and the
// speaker-layout checks on four multichannel AC3 blobs (checkLayout, and
// checkLayoutChange on two of them joined into one stream), compile in only when
// the library was built with FFmpeg (RAWFORM_HAVE_FFMPEG); without it they are
// skipped with a single notice and the suite still runs the three native
// decoders. The blobs live in tests/fixtures/<Codec>Blob.h, included below;
// each header records the ffmpeg command that produced it, except Mp3Blob.h,
// whose command was not kept. A probe battery (checkProbe) runs beside the
// decode battery on the same fixtures, so MetadataProbe's read of each file is
// held to the decoder's facts.

#include "ChannelPositions.h"
#include "DecoderFactory.h"
#if RAWFORM_HAVE_FFMPEG
#include "decoders/FfmpegDecoder.h"
#endif

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/MetadataProbe.h"
#include "rawform/audio/Types.h"

#include <sndfile.h>

// Some libsndfile headers predate the Opus subtype constant; define it to its
// stable ABI value so SF_FORMAT_OPUS below compiles even there. Mirrors the same
// guard in SndfileDecoder.
#ifndef SF_FORMAT_OPUS
#define SF_FORMAT_OPUS 0x0064
#endif

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>  // IWYU pragma: keep (std::int*_t)
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using rawform::audio::AudioFormat;
using rawform::audio::ChannelPosition;
using rawform::audio::Codec;
using rawform::audio::DecoderFactory;
using rawform::audio::DecoderKind;
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

// ---------------------------------------------------------------------------
// The same deterministic, exactly-representable ramp the sndfile and FLAC tests
// use: an integer in [-2048, 2047] over 2048, exact in float32 and in [-1, 1),
// keyed on the absolute SAMPLE index so each channel of each frame gets a
// distinct value and a wrong interleave shows up at once.
float pat(std::uint64_t sampleIndex) noexcept {
    const std::int64_t k = static_cast<std::int64_t>(sampleIndex % 4096u) - 2048;
    return static_cast<float>(k) / 2048.0f;
}

// FNV-1a 64-bit over the raw bytes of a float buffer. Compared only WITHIN one
// process against output from the same build, so cross-version float differences
// never reach an assertion; it is a determinism probe, not a golden hash.
std::uint64_t fnv1a(std::uint64_t h, const float* data, std::size_t floats) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(data);
    const std::size_t bytes = floats * sizeof(float);
    for (std::size_t i = 0; i < bytes; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;

// ---------------------------------------------------------------------------
// Fixture writers.

// A float file via libsndfile in the requested major+subtype format. libsndfile
// picks the format from SF_INFO, not the path extension, so the caller's file
// name only needs a plausible suffix. sampleAt(frame, channel) gives each
// sample; a non-empty map is handed to SFC_SET_CHANNEL_MAP_INFO before any
// audio, and a refusal fails the write.
template <typename SampleAt>
bool encodeSndfile(const std::string& path, int channels, int rate,
                   std::uint64_t frames, int sfFormat, std::span<const int> map,
                   SampleAt sampleAt) {
    SF_INFO info;
    info.frames     = 0;
    info.samplerate = rate;
    info.channels   = channels;
    info.format     = sfFormat;
    info.sections   = 0;
    info.seekable   = 0;

    SNDFILE* sf = sf_open(path.c_str(), SFM_WRITE, &info);
    if (sf == nullptr) {
        return false;
    }
    if (!map.empty()) {
        std::vector<int> copy(map.begin(), map.end());
        if (sf_command(sf, SFC_SET_CHANNEL_MAP_INFO, copy.data(),
                       static_cast<int>(copy.size() * sizeof(int))) != SF_TRUE) {
            sf_close(sf);
            return false;
        }
    }
    constexpr sf_count_t block = 1024;
    std::vector<float>   buf(static_cast<std::size_t>(block) * channels);
    std::uint64_t        written = 0;
    bool                 good    = true;
    while (written < frames) {
        const sf_count_t n = static_cast<sf_count_t>(
            std::min<std::uint64_t>(static_cast<std::uint64_t>(block),
                                    frames - written));
        for (sf_count_t f = 0; f < n; ++f) {
            for (int c = 0; c < channels; ++c) {
                buf[static_cast<std::size_t>(f) * channels + c] =
                    sampleAt(written + static_cast<std::uint64_t>(f), c);
            }
        }
        if (sf_writef_float(sf, buf.data(), n) != n) {
            good = false;
            break;
        }
        written += static_cast<std::uint64_t>(n);
    }
    sf_close(sf);
    return good;
}

// Float32 via libsndfile, carrying the ramp. WAV/FLOAT keeps the round trip
// exact (SndfileDecoder reads it back with SFC_SET_NORM_FLOAT), so the WAV
// fixture is the strictest possible case (strict tier); the Ogg subtypes
// (Vorbis, Opus) yield lossy fixtures whose exact samples do not survive, so
// they are checked structurally.
bool writeSndfileFloat(const std::string& path, int channels, int rate,
                       std::uint64_t frames, int sfFormat) {
    return encodeSndfile(path, channels, rate, frames, sfFormat, {},
                         [channels](std::uint64_t frame, int c) {
                             return pat(frame * static_cast<std::uint64_t>(channels) +
                                        static_cast<std::uint64_t>(c));
                         });
}

// Quantize the [-1, 1) ramp to a signed integer of the given depth. Lossless for
// this ramp (k/2048 * 2^(bits-1) is an integer), so the FLAC round trip is exact.
FLAC__int32 quantize(float f, int bits) noexcept {
    const auto scale = static_cast<double>(std::int64_t{1} << (bits - 1));
    long v = std::lround(static_cast<double>(f) * scale);
    const long maxv = static_cast<long>((std::int64_t{1} << (bits - 1)) - 1);
    const long minv = static_cast<long>(-(std::int64_t{1} << (bits - 1)));
    if (v > maxv) {
        v = maxv;
    }
    if (v < minv) {
        v = minv;
    }
    return static_cast<FLAC__int32>(v);
}

// The container the FLAC encoder writes: a native FLAC stream, or FLAC in Ogg.
enum class Container { Native, Ogg };

using MetadataPtr =
    std::unique_ptr<FLAC__StreamMetadata, decltype(&FLAC__metadata_object_delete)>;

// A VORBIS_COMMENT block holding one comment, given whole as "NAME=value" and
// written verbatim, so a test controls both the field name's case and the
// value's form; null when libFLAC cannot build it.
MetadataPtr commentBlock(const char* comment) {
    MetadataPtr block(FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT),
                      &FLAC__metadata_object_delete);
    std::string                              text(comment);
    FLAC__StreamMetadata_VorbisComment_Entry entry{};
    entry.length = static_cast<FLAC__uint32>(text.size());
    entry.entry  = reinterpret_cast<FLAC__byte*>(text.data());
    if (!block ||
        !FLAC__metadata_object_vorbiscomment_append_comment(block.get(), entry,
                                                            /*copy=*/true)) {
        return MetadataPtr(nullptr, &FLAC__metadata_object_delete);
    }
    return block;
}

// FLAC via libFLAC's stream encoder. sampleAt(frame, channel) gives each sample
// in [-1, 1); a non-null comments block is written after STREAMINFO.
template <typename SampleAt>
bool encodeFlac(const std::string& path, int channels, int rate, int bits,
                std::uint64_t frames, Container container,
                FLAC__StreamMetadata* comments, SampleAt sampleAt) {
    FLAC__StreamEncoder* enc = FLAC__stream_encoder_new();
    if (enc == nullptr) {
        return false;
    }
    FLAC__bool ok = true;
    ok &= FLAC__stream_encoder_set_channels(enc, static_cast<unsigned>(channels));
    ok &= FLAC__stream_encoder_set_bits_per_sample(enc, static_cast<unsigned>(bits));
    ok &= FLAC__stream_encoder_set_sample_rate(enc, static_cast<unsigned>(rate));
    ok &= FLAC__stream_encoder_set_total_samples_estimate(enc, frames);
    ok &= FLAC__stream_encoder_set_compression_level(enc, 5);
    if (comments != nullptr) {
        ok &= FLAC__stream_encoder_set_metadata(enc, &comments, 1);
    }
    if (!ok) {
        FLAC__stream_encoder_delete(enc);
        return false;
    }
    const FLAC__StreamEncoderInitStatus st =
        container == Container::Ogg
            ? FLAC__stream_encoder_init_ogg_file(enc, path.c_str(), nullptr, nullptr)
            : FLAC__stream_encoder_init_file(enc, path.c_str(), nullptr, nullptr);
    if (st != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
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
                buf[static_cast<std::size_t>(f) * channels + c] =
                    quantize(sampleAt(written + f, c), bits);
            }
        }
        if (!FLAC__stream_encoder_process_interleaved(enc, buf.data(), n)) {
            good = false;
            break;
        }
        written += n;
    }
    good = (FLAC__stream_encoder_finish(enc) != 0) && good;
    FLAC__stream_encoder_delete(enc);
    return good;
}

// FLAC carrying the ramp.
bool writeFlac(const std::string& path, int channels, int rate, int bits,
               std::uint64_t frames, Container container = Container::Native) {
    return encodeFlac(path, channels, rate, bits, frames, container, nullptr,
                      [channels](std::uint64_t frame, int c) {
                          return pat(frame * static_cast<std::uint64_t>(channels) +
                                     static_cast<std::uint64_t>(c));
                      });
}

// Raw bytes to a file, for the encoder-free MP3 and AAC fixtures.
bool writeBytes(const std::string& path, const unsigned char* bytes,
                std::size_t len) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    const std::size_t wrote = std::fwrite(bytes, 1, len, f);
    std::fclose(f);
    return wrote == len;
}

// ---------------------------------------------------------------------------
// Embedded fixtures.
//
// kMp3's bytes and provenance live in fixtures/Mp3Blob.h.
#include "fixtures/Mp3Blob.h"

bool createWav(const std::string& p)  { return writeSndfileFloat(p, 2, 44100, 50000, SF_FORMAT_WAV | SF_FORMAT_FLOAT); }
bool createFlac(const std::string& p) { return writeFlac(p, 2, 44100, 16, 50000); }
bool createMp3(const std::string& p)  { return writeBytes(p, kMp3, sizeof(kMp3)); }

// Ogg FLAC needs no FFmpeg: libFLAC writes it here and reads it back, so a
// native-only build runs these rows too.
bool createOggFlac(const std::string& p) {
    return writeFlac(p, 2, 44100, 16, 50000, Container::Ogg);
}

#if RAWFORM_HAVE_FFMPEG
#include "fixtures/AacBlob.h"

bool createAac(const std::string& p) { return writeBytes(p, kAacAdts, sizeof(kAacAdts)); }

#include "fixtures/Ac3Blob.h"
bool createAc3(const std::string& p) { return writeBytes(p, kAc3, sizeof(kAc3)); }

#include "fixtures/DtsBlob.h"
bool createDts(const std::string& p) { return writeBytes(p, kDts, sizeof(kDts)); }

#include "fixtures/WmaBlob.h"
bool createWma(const std::string& p) { return writeBytes(p, kWma, sizeof(kWma)); }

#include "fixtures/AlacBlob.h"
bool createAlac(const std::string& p) { return writeBytes(p, kAlac, sizeof(kAlac)); }

// kAc3Layout40, kAc3LayoutQuadSide, kAc3Layout21 and kAc3Layout51Side, for
// checkLayout below.
#include "fixtures/Ac3LayoutBlob.h"

// Vorbis and Opus are generated programmatically: libsndfile writes both Ogg
// subtypes. They still decode through FFmpeg because SndfileDecoder declines those
// subtypes on read (it defers them), so the cascade falls through; the fixtures
// assert exactly that via expectedDecoder. Opus is fixed at 48 kHz. These writers
// live inside the guard so a native-only build (which cannot decode either) does
// not carry them as unused functions.
bool createVorbis(const std::string& p) {
    return writeSndfileFloat(p, 2, 44100, 50000, SF_FORMAT_OGG | SF_FORMAT_VORBIS);
}
bool createOpus(const std::string& p) {
    return writeSndfileFloat(p, 2, 48000, 50000, SF_FORMAT_OGG | SF_FORMAT_OPUS);
}
#endif

// ---------------------------------------------------------------------------
// Battery building blocks.

enum class Tier { Strict, Structural };

struct Fixture {
    const char* name;
    const char* ext;
    bool (*create)(const std::string&);
    Codec         expectedCodec;
    DecoderKind   expectedDecoder;  // proves WHICH decoder ran, not just the codec
    Tier          tier;
    unsigned      channels;
    std::uint32_t rate;
    float         eps;  // strict value tolerance; unused for structural fixtures
};

// Decode from the current position to EOS, returning the frame count and folding
// every handed-back sample into the running hash.
std::uint64_t decodeToEos(IDecoder& dec, unsigned ch, std::uint64_t& hashOut) {
    std::vector<float> buf(static_cast<std::size_t>(4096) * ch);
    std::uint64_t      hash   = kFnvOffset;
    std::uint64_t      frames = 0;
    for (;;) {
        const std::size_t got = dec.read(buf.data(), 4096);
        if (got == 0) {
            break;
        }
        hash = fnv1a(hash, buf.data(), got * ch);
        frames += got;
    }
    hashOut = hash;
    return frames;
}

// Strict: drain from frame 0 to EOS, verifying every sample against the ramp,
// the total decoded count against totalFrames (which proves the final short read
// returned the exact remainder), and the post-EOS read returning 0.
void strictDrainVerify(IDecoder& dec, unsigned ch, std::uint64_t total, float eps) {
    std::vector<float> buf(static_cast<std::size_t>(4096) * ch);
    std::uint64_t      frame  = 0;
    float              maxErr = 0.0f;
    for (;;) {
        const std::size_t got = dec.read(buf.data(), 4096);
        if (got == 0) {
            break;
        }
        for (std::size_t f = 0; f < got; ++f) {
            for (unsigned c = 0; c < ch; ++c) {
                const float exp = pat((frame + f) * ch + c);
                maxErr = std::max(maxErr, std::fabs(buf[f * ch + c] - exp));
            }
        }
        frame += got;
    }
    CHECK(frame == total);
    CHECK(maxErr <= eps);
    CHECK(dec.read(buf.data(), 4096) == 0);  // read at EOS returns 0
}

// Strict: from the current position, read framesToCheck frames and verify each
// against the ramp keyed at startFrame. Used to prove a post-seek read lands on
// the exact sample.
void strictVerifyFrom(IDecoder& dec, unsigned ch, std::uint64_t startFrame,
                      std::uint64_t framesToCheck, float eps) {
    std::vector<float> buf(static_cast<std::size_t>(4096) * ch);
    std::uint64_t      checked = 0;
    float              maxErr  = 0.0f;
    while (checked < framesToCheck) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(4096, framesToCheck - checked));
        const std::size_t got = dec.read(buf.data(), want);
        if (got == 0) {
            break;
        }
        for (std::size_t f = 0; f < got; ++f) {
            for (unsigned c = 0; c < ch; ++c) {
                const float exp = pat((startFrame + checked + f) * ch + c);
                maxErr = std::max(maxErr, std::fabs(buf[f * ch + c] - exp));
            }
        }
        checked += got;
    }
    CHECK(checked == framesToCheck);
    CHECK(maxErr <= eps);
}

// The full battery, run identically against every fixture.
void runBattery(const Fixture& fx) {
    const std::string path = std::string("rawform_conf_") + fx.name + fx.ext;
    if (!fx.create(path)) {
        std::fprintf(stderr, "fixture creation failed for %s\n", path.c_str());
        CHECK(false);
        std::remove(path.c_str());
        return;
    }

    DecoderFactory            factory;
    std::string               err;
    std::unique_ptr<IDecoder> dec = factory.open(path, &err);
    if (!dec) {
        std::fprintf(stderr, "open failed for %s: %s\n", path.c_str(), err.c_str());
        CHECK(dec != nullptr);
        std::remove(path.c_str());
        return;
    }

    const SourceInfo    si    = dec->sourceInfo();
    const AudioFormat   fmt   = dec->format();
    const std::uint64_t total = dec->totalFrames();
    const unsigned      ch    = fmt.channels;

    // Routing and basic facts. The codec says WHAT was decoded; the decoder says
    // WHICH decoder did it. Both are needed: for Vorbis/Opus the codec alone does
    // not prove the route (libsndfile would report the same codec if it stopped
    // declining them), so the decoder check is what actually guards the cascade.
    CHECK(si.codec == fx.expectedCodec);
    CHECK(si.decoder == fx.expectedDecoder);
    CHECK(fmt.sampleRate == fx.rate);
    CHECK(ch == fx.channels);
    CHECK(total > 0);
    CHECK(dec->seekable());
    if (ch == 0) {  // guard the per-channel math below against a bad open
        std::remove(path.c_str());
        return;
    }

    // Forward decode plus within-process determinism.
    std::uint64_t hashOpen = 0;
    if (fx.tier == Tier::Strict) {
        strictDrainVerify(*dec, ch, total, fx.eps);
        CHECK(dec->seek(0));
        const std::uint64_t f1 = decodeToEos(*dec, ch, hashOpen);
        CHECK(f1 == total);
        CHECK(dec->seek(0));
        std::uint64_t       h2 = 0;
        const std::uint64_t f2 = decodeToEos(*dec, ch, h2);
        CHECK(f2 == total);
        CHECK(h2 == hashOpen);
    } else {
        // Lossy: decode bytes are not contractually reproducible across arbitrary
        // seek preconditions, and the very START of the stream is the fragile
        // region (mpg123's gapless encoder-delay trimming interacts with the
        // filterbank warm-up), so here the structural tier asserts only COUNT and
        // BOUNDARY behavior. Bit-exact reproducibility is asserted below with a
        // steady-state mid-stream seek, the way Mp3DecoderTest checks it.
        std::uint64_t       hCold = 0;
        const std::uint64_t f0    = decodeToEos(*dec, ch, hCold);
        CHECK(f0 > 0);
        (void)hCold;
        std::vector<float> tmp(ch);
        CHECK(dec->read(tmp.data(), 1) == 0);  // read at EOS returns 0
        CHECK(dec->read(tmp.data(), 1) == 0);  // and stays 0

        CHECK(dec->seek(0));                    // rewind to the start
        std::uint64_t       hRewind = 0;
        const std::uint64_t fRewind = decodeToEos(*dec, ch, hRewind);

        // Opus carries encoder-delay preskip that FFmpeg trims at the start; decoding
        // from a fresh open vs. after seek(0) primes that trim slightly differently, so
        // the from-start count wobbles by the preskip (a few ms). That is the start
        // fragility this tier already declines to assert bit-exactly, here reaching the
        // count. Every other lossy codec has a stable rewind count and keeps the exact
        // check; the mid-stream reproducibility below is the real bit-exact guard.
        if (fx.expectedCodec == Codec::Opus) {
            CHECK(fRewind > 0);
            const std::uint64_t d = fRewind > f0 ? fRewind - f0 : f0 - fRewind;
            // <= 100 ms; preskip is a few ms, so this is generous.
            CHECK(d <= fx.rate / 10);
        } else {
            CHECK(fRewind == f0);
        }
        (void)hRewind;
    }

    // seek(totalFrames): valid "go to EOS", then a read reaches 0. Strict hits it
    // on the very next read; structural reaches it within a bounded number.
    CHECK(dec->seek(total));
    {
        std::vector<float> tmp(static_cast<std::size_t>(4096) * ch);
        if (fx.tier == Tier::Strict) {
            CHECK(dec->read(tmp.data(), 4096) == 0);
        } else {
            bool reachedZero = false;
            for (int i = 0; i < 64; ++i) {
                if (dec->read(tmp.data(), 4096) == 0) {
                    reachedZero = true;
                    break;
                }
            }
            CHECK(reachedZero);
        }
    }

    // Strictly past the end. Exact-length decoders (PCM, FLAC, and FfmpegDecoder)
    // reject it outright; a lossy library with an estimated length may instead
    // clamp to the end and succeed (libmpg123 does, returning a non-negative
    // achieved offset), so the exact-failure guarantee is a STRICT-tier property.
    // The structural tier has already proven no audio is produced past the true
    // end via the seek(total) reach-zero check above.
    if (fx.tier == Tier::Strict) {
        CHECK(dec->seek(total + fx.rate) == false);
    }

    // Post-seek landing.
    if (fx.tier == Tier::Strict) {
        const std::uint64_t mid  = total / 2;
        CHECK(dec->seek(mid));
        const std::uint64_t span = std::min<std::uint64_t>(2000, total - mid);
        strictVerifyFrom(*dec, ch, mid, span, fx.eps);
    } else {
        // Structural landing ("lands at frame structurally for lossy"): seeking
        // to a larger target leaves fewer frames to decode, so the remaining count
        // tracks the playhead. This is a RELATIVE check across well-separated
        // targets, which makes it robust to the two lossy realities at play: the
        // totalFrames figure is an estimate, and a format with no container
        // index (raw ADTS-AAC) seeks approximately, its landing depending on the
        // pre-seek position. The targets are a full quarter-stream apart, far
        // wider than any per-seek wobble, so the ordering holds with only a small
        // boundary slack. Absolute landing accuracy and bit-exact reproducibility
        // are deliberately NOT asserted here: the former couples to the estimated
        // length, the latter is an indexing property that belongs to the codec's
        // own per-format test (Mp3DecoderTest verifies it for MP3), not the shared
        // boundary contract.
        const std::uint64_t slack    = fx.rate / 100 + 8192;  // boundary tolerance
        const std::uint64_t targets[] = {total / 4, total / 2, (3 * total) / 4};
        std::uint64_t       prevRemaining = total + slack + 1;
        std::uint64_t       firstRemaining = 0;
        std::uint64_t       lastRemaining  = 0;
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(dec->seek(targets[i]));
            std::uint64_t       hJunk     = 0;
            const std::uint64_t remaining = decodeToEos(*dec, ch, hJunk);
            (void)hJunk;
            // Monotonic: a later target never leaves meaningfully more frames.
            CHECK(remaining <= prevRemaining + slack);
            prevRemaining = remaining;
            if (i == 0) {
                firstRemaining = remaining;
            }
            if (i == 2) {
                lastRemaining = remaining;
            }
        }
        // And the playhead actually moved: a quarter-stream-later target leaves
        // clearly fewer frames than the earlier one (the seek is not a no-op).
        CHECK(firstRemaining > lastRemaining);
    }

    dec.reset();
    std::remove(path.c_str());
}

#if RAWFORM_HAVE_FFMPEG
// Probe conformance: the read-side mirror of runBattery. Materialize the fixture,
// run the engine metadata probe (no decode), and assert it reports the stream's
// facts: a value at all, the right sample rate and channel count, the expected
// codec LABEL (the probe returns a presentation string, not the Codec enum), and
// a positive duration. Tags are intentionally not asserted, since a raw
// elementary stream carries none. Only invoked when the probe is genuinely
// available (FFmpeg present and the blob embedded), so a missing value is a real
// failure rather than an unbuilt feature.
void checkProbe(const Fixture& fx, const char* expectedLabel) {
    const std::string path = std::string("rawform_probe_") + fx.name + fx.ext;
    if (!fx.create(path)) {
        std::fprintf(stderr, "probe fixture creation failed for %s\n", path.c_str());
        CHECK(false);
        std::remove(path.c_str());
        return;
    }
    std::string perr;
    const std::optional<rawform::audio::ProbedMetadata> md =
        rawform::audio::probeAudioMetadata(path, &perr);
    CHECK(md.has_value());
    if (md) {
        CHECK(md->sampleRate == fx.rate);
        CHECK(md->channels == fx.channels);
        CHECK(md->codecName == expectedLabel);
        CHECK(md->durationMs > 0);
    }
    std::remove(path.c_str());
}
#endif

// ---------------------------------------------------------------------------
// The positions a sink declares for a format (ChannelPositions.h). A mask names
// the speakers in ascending bit order. A zero mask, a mask with a bit the table
// cannot name, or one whose bit count disagrees with the channel count gets the
// count's default order. Beyond eight channels there is no default order, so
// only a mask declares positions; beyond eighteen nothing does.
bool positionsAre(const AudioFormat& f, std::initializer_list<ChannelPosition> want) {
    return std::ranges::equal(rawform::audio::channelPositionsFor(f), want);
}

void checkChannelPositions() {
    using P = ChannelPosition;
    using rawform::audio::channelPositionsFor;
    using rawform::audio::defaultChannelMask;
    using rawform::audio::sourceChannelMask;
    const auto fmt = [](std::uint16_t channels, std::uint32_t mask) {
        return AudioFormat{
            .sampleRate = 48000, .channels = channels, .channelMask = mask};
    };

    // Default orders.
    CHECK(positionsAre(fmt(1, 0), {P::Mono}));
    CHECK(positionsAre(fmt(6, 0), {P::FrontLeft, P::FrontRight, P::FrontCenter,
                                   P::Lfe, P::BackLeft, P::BackRight}));

    // Masks: 4.0 (FL FR FC BC), 5.1 with side surrounds, 7.1(wide) with the
    // front left and right of center, 5.1.2(back) with the top front pair, the
    // remaining height speakers over a stereo pair, 7.1.4 with twelve speakers,
    // and the top back row over a stereo pair.
    CHECK(positionsAre(fmt(4, 0x0107), {P::FrontLeft, P::FrontRight,
                                        P::FrontCenter, P::BackCenter}));
    CHECK(positionsAre(fmt(6, 0x060F), {P::FrontLeft, P::FrontRight, P::FrontCenter,
                                        P::Lfe, P::SideLeft, P::SideRight}));
    CHECK(positionsAre(fmt(8, 0x00FF),
                       {P::FrontLeft, P::FrontRight, P::FrontCenter, P::Lfe,
                        P::BackLeft, P::BackRight, P::FrontLeftOfCenter,
                        P::FrontRightOfCenter}));
    CHECK(positionsAre(fmt(8, 0x503F),
                       {P::FrontLeft, P::FrontRight, P::FrontCenter, P::Lfe,
                        P::BackLeft, P::BackRight, P::TopFrontLeft,
                        P::TopFrontRight}));
    CHECK(positionsAre(fmt(5, 0xA803), {P::FrontLeft, P::FrontRight, P::TopCenter,
                                        P::TopFrontCenter, P::TopBackLeft}));
    CHECK(positionsAre(fmt(3, 0x0043),
                       {P::FrontLeft, P::FrontRight, P::FrontLeftOfCenter}));
    CHECK(positionsAre(fmt(12, 0x2D63F),
                       {P::FrontLeft, P::FrontRight, P::FrontCenter, P::Lfe,
                        P::BackLeft, P::BackRight, P::SideLeft, P::SideRight,
                        P::TopFrontLeft, P::TopFrontRight, P::TopBackLeft,
                        P::TopBackRight}));
    CHECK(positionsAre(fmt(5, 0x38003), {P::FrontLeft, P::FrontRight, P::TopBackLeft,
                                         P::TopBackCenter, P::TopBackRight}));

    // A mask equal to the default order declares what 0 declares.
    CHECK(positionsAre(fmt(6, 0x003F), {P::FrontLeft, P::FrontRight, P::FrontCenter,
                                        P::Lfe, P::BackLeft, P::BackRight}));

    // Ignored masks: four bits on three channels, and a speaker the table cannot
    // name (bit 18).
    CHECK(positionsAre(fmt(3, 0x0107), {P::FrontLeft, P::FrontRight, P::FrontCenter}));
    CHECK(positionsAre(fmt(3, 0x40003), {P::FrontLeft, P::FrontRight, P::FrontCenter}));

    // Nothing to declare: no channels, more than eight with no mask or an
    // ignored one, more than eighteen.
    CHECK(channelPositionsFor(fmt(0, 0)).empty());
    CHECK(channelPositionsFor(fmt(9, 0)).empty());
    CHECK(channelPositionsFor(fmt(12, 0)).empty());
    CHECK(channelPositionsFor(fmt(12, 0x063F)).empty());
    CHECK(channelPositionsFor(fmt(19, 0x7FFFF)).empty());

    // The default masks a decoder compares a source's mask against.
    CHECK(defaultChannelMask(1) == 0);
    CHECK(defaultChannelMask(2) == 0x0003);
    CHECK(defaultChannelMask(4) == 0x0033);
    CHECK(defaultChannelMask(6) == 0x003F);
    CHECK(defaultChannelMask(7) == 0x070F);
    CHECK(defaultChannelMask(8) == 0x063F);
    CHECK(defaultChannelMask(9) == 0);

    // The mask a decoder reports for a source's own mask: the mask when it
    // differs from the default order, 0 for the default order itself, and 0 for
    // anything the table cannot declare (an unnamed speaker, bit 18, a bit count
    // that disagrees with the channel count, mono, more than eighteen channels).
    // Nine to eighteen named speakers have no default order to match, so their
    // mask is always reported.
    CHECK(sourceChannelMask(0x0107, 4) == 0x0107);
    CHECK(sourceChannelMask(0x0033, 4) == 0);
    CHECK(sourceChannelMask(0x0043, 3) == 0x0043);
    CHECK(sourceChannelMask(0x40003, 3) == 0);
    CHECK(sourceChannelMask(0x0107, 3) == 0);
    CHECK(sourceChannelMask(0x0004, 1) == 0);
    CHECK(sourceChannelMask(0x073F, 9) == 0x073F);
    CHECK(sourceChannelMask(0x2D63F, 12) == 0x2D63F);
    CHECK(sourceChannelMask(0x3FFFF, 18) == 0x3FFFF);
    CHECK(sourceChannelMask(0x7FFFF, 19) == 0);

    // A source that names a speaker per channel, in stream order. 4.0 in
    // ascending order needs no reorder. 4.0 as C L R Cs is moved: output FL
    // takes source channel 1, FR 2, FC 0, BC 3. 5.1 as L C R Ls Rs LFE is moved
    // to the default order, so it reports no mask. FLC ahead of L R is moved
    // behind them. 7.1.4 with its four height speakers first is moved behind
    // the eight at ear level. The default order itself, an unnamed speaker
    // (bit 18), a speaker with no bit and a repeated speaker get no mask, and
    // none of them moves.
    const auto orderOf = [](std::initializer_list<std::uint64_t> speakers) {
        return rawform::audio::channelOrderFor(
            std::span<const std::uint64_t>(speakers.begin(), speakers.size()));
    };
    const auto ascending = orderOf({0x001, 0x002, 0x004, 0x100});
    CHECK(ascending.mask == 0x0107 && !ascending.reorder);
    const auto c40b = orderOf({0x004, 0x001, 0x002, 0x100});
    CHECK(c40b.mask == 0x0107 && c40b.reorder);
    CHECK(c40b.from[0] == 1 && c40b.from[1] == 2 && c40b.from[2] == 0 &&
          c40b.from[3] == 3);
    const auto c51c = orderOf({0x001, 0x004, 0x002, 0x010, 0x020, 0x008});
    CHECK(c51c.mask == 0 && c51c.reorder);
    const auto defaultQuad = orderOf({0x001, 0x002, 0x010, 0x020});
    CHECK(defaultQuad.mask == 0 && !defaultQuad.reorder);
    const auto flc = orderOf({0x040, 0x001, 0x002});
    CHECK(flc.mask == 0x0043 && flc.reorder);
    CHECK(flc.from[0] == 1 && flc.from[1] == 2 && flc.from[2] == 0);
    const auto c714 = orderOf({0x01000, 0x04000, 0x08000, 0x20000, 0x001, 0x002,
                               0x004, 0x008, 0x010, 0x020, 0x200, 0x400});
    CHECK(c714.mask == 0x2D63F && c714.reorder && c714.channels == 12);
    const std::array<std::uint8_t, 12> c714From = {4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3};
    CHECK(c714.from.size() >= c714From.size() &&
          std::ranges::equal(c714From,
                             std::span(c714.from.data(), c714From.size())));
    const auto unnamed = orderOf({0x40000, 0x001, 0x002});
    CHECK(unnamed.mask == 0 && !unnamed.reorder);
    const auto noBit = orderOf({0x004, 0x001, 0x000});
    CHECK(noBit.mask == 0 && !noBit.reorder);
    const auto repeated = orderOf({0x004, 0x001, 0x001});
    CHECK(repeated.mask == 0 && !repeated.reorder);

    // The reorder moves samples within each frame and nothing else.
    std::array<float, 8> frames = {0.7f, 0.3f, 0.5f, 0.13f, -0.7f, -0.3f, -0.5f, -0.13f};
    rawform::audio::reorderChannels(frames.data(), 2, c40b);
    CHECK((frames == std::array<float, 8>{0.3f, 0.5f, 0.7f, 0.13f, -0.3f, -0.5f, -0.7f,
                                          -0.13f}));
    std::array<float, 12> frame714 = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    rawform::audio::reorderChannels(frame714.data(), 1, c714);
    CHECK((frame714 == std::array<float, 12>{4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3}));
}

// Routing onto a device's speaker configuration (routeToDevice), the channel map
// CoreAudioSink sets on the AUHAL: one entry per device channel, holding the
// source channel played there, or -1.
void checkDeviceRouting() {
    using P        = ChannelPosition;
    using Speakers = std::vector<std::optional<P>>;
    using Map      = std::vector<std::int32_t>;
    const auto route = [](std::uint16_t channels, std::uint32_t mask,
                          const Speakers& device) {
        Map map(device.size());
        rawform::audio::routeToDevice(
            rawform::audio::channelPositionsFor(AudioFormat{
                .sampleRate = 48000, .channels = channels, .channelMask = mask}),
            device, map);
        return map;
    };
    // A device whose channel n carries CoreAudio label n, BlackHole 16ch's default:
    // the first 16 speakers in WAVE order.
    const auto& order = rawform::audio::detail::kMaskOrder;
    const Speakers wave16(order.begin(), order.begin() + 16);

    // 4.0's back center goes to the channel configured for it; the LFE stays silent.
    CHECK((route(4, 0x0107, wave16) ==
           Map{0, 1, 2, -1, -1, -1, -1, -1, 3, -1, -1, -1, -1, -1, -1, -1}));
    // 7.1.4: the sides and the height row go to their channels; the top back right,
    // which the device does not name, keeps its own index, channel 12.
    CHECK((route(12, 0x2D63F, wave16) ==
           Map{0, 1, 2, 3, 4, 5, -1, -1, -1, 6, 7, 11, 8, -1, 9, 10}));
    // Every speaker named, in reverse order: each goes to its own label.
    CHECK((route(6, 0x3F,
                 {P::BackRight, P::BackLeft, P::Lfe, P::FrontCenter, P::FrontRight,
                  P::FrontLeft}) == Map{5, 4, 3, 2, 1, 0}));
    // A device that names nothing gets the identity.
    CHECK((route(6, 0, Speakers(8)) == Map{0, 1, 2, 3, 4, 5, -1, -1}));
    // A source wider than the device keeps its first channels.
    CHECK((route(8, 0, Speakers(4)) == Map{0, 1, 2, 3}));
    // An unnamed speaker whose own channel is taken is dropped: 4.0's FC here.
    CHECK((route(4, 0x0107,
                 {P::BackCenter, P::FrontRight, P::FrontLeft, std::nullopt}) ==
           Map{3, 1, 0, -1}));
    // A speaker the device names twice keeps its index; the others find theirs.
    CHECK((route(6, 0x3F,
                 {P::FrontLeft, P::FrontRight, P::FrontLeft, P::Lfe, P::BackLeft,
                  P::BackRight, P::FrontCenter}) == Map{0, 1, -1, 3, 4, 5, 2}));
}

// The move between two orders of the same speakers (channelOrderBetween), which
// FfmpegDecoder applies to a block whose layout lists the declared speakers in
// another order.
void checkChannelOrderBetween() {
    using P            = ChannelPosition;
    const auto between = [](std::initializer_list<P> source,
                            std::initializer_list<P> target) {
        return rawform::audio::channelOrderBetween(
            std::span<const P>(source.begin(), source.size()),
            std::span<const P>(target.begin(), target.size()));
    };

    // C L R Cs to FL FR FC BC: each output channel takes the source channel
    // holding its speaker, and the samples follow.
    const auto cs = between({P::FrontCenter, P::FrontLeft, P::FrontRight, P::BackCenter},
                            {P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackCenter});
    CHECK(cs.has_value());
    if (cs) {
        CHECK(cs->reorder);
        CHECK(cs->channels == 4);
        CHECK(cs->mask == 0);
        CHECK(cs->from[0] == 1 && cs->from[1] == 2 && cs->from[2] == 0 &&
              cs->from[3] == 3);
        std::array<float, 4> frame{2, 0, 1, 3};
        rawform::audio::reorderChannels(frame.data(), 1, *cs);
        CHECK((frame == std::array<float, 4>{0, 1, 2, 3}));
    }
    // The same order needs no move.
    const auto same = between({P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackCenter},
                              {P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackCenter});
    CHECK(same.has_value() && !same->reorder);
    // Other speakers at the same count: 4.0 against quad(side).
    CHECK(!between({P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackCenter},
                   {P::FrontLeft, P::FrontRight, P::SideLeft, P::SideRight}));
    // A repeated speaker, in either list or in both.
    CHECK(!between({P::FrontLeft, P::FrontLeft, P::FrontRight, P::BackCenter},
                   {P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackCenter}));
    CHECK(!between({P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackCenter},
                   {P::FrontLeft, P::FrontLeft, P::FrontRight, P::BackCenter}));
    CHECK(!between({P::FrontLeft, P::FrontLeft}, {P::FrontLeft, P::FrontLeft}));
    // Lists of different lengths.
    CHECK(!between({P::FrontLeft, P::FrontRight},
                   {P::FrontLeft, P::FrontRight, P::FrontCenter}));

    // Any comparable id works, as FFmpeg's channel ids do: 18 reversed is the
    // largest move a ChannelOrder holds, and 19 is past it.
    const auto reversed = [](int count) {
        std::vector<int> source(static_cast<std::size_t>(count));
        std::vector<int> target(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            source[static_cast<std::size_t>(i)] = count - 1 - i;
            target[static_cast<std::size_t>(i)] = i;
        }
        return rawform::audio::channelOrderBetween(std::span<const int>(source),
                                                   std::span<const int>(target));
    };
    const auto wide = reversed(18);
    CHECK(wide.has_value() && wide->from[0] == 17 && wide->from[17] == 0);
    CHECK(!reversed(19));
}

// ---------------------------------------------------------------------------
// Speaker layouts end to end: what each decoded channel holds is what a sink is
// told it is. Each layout fixture carries one tone per source channel, placed by
// speaker: the AC3 streams in fixtures/Ac3LayoutBlob.h, and the FLAC files
// written below. checkDecodedLayout opens one through the factory, takes the
// positions a sink would declare for the decoded format, and listens to every
// decoded channel: the strongest of the tones below must be the tone of the
// position declared for that channel.

// The tone each position carries in the fixtures. All of them are listened for,
// so a channel declared at the wrong position hears the wrong tone.
struct PositionTone {
    ChannelPosition position;
    double          hz;
};
constexpr PositionTone kPositionTones[] = {
    {ChannelPosition::FrontLeft, 300.0},  {ChannelPosition::FrontRight, 500.0},
    {ChannelPosition::FrontCenter, 700.0}, {ChannelPosition::Lfe, 60.0},
    {ChannelPosition::BackLeft, 900.0},   {ChannelPosition::BackRight, 1100.0},
    {ChannelPosition::BackCenter, 1300.0}, {ChannelPosition::SideLeft, 1500.0},
    {ChannelPosition::SideRight, 1700.0},
    {ChannelPosition::FrontLeftOfCenter, 1900.0},
    {ChannelPosition::FrontRightOfCenter, 2100.0},
    {ChannelPosition::TopCenter, 2300.0},
    {ChannelPosition::TopFrontLeft, 2500.0},
    {ChannelPosition::TopFrontCenter, 2700.0},
    {ChannelPosition::TopFrontRight, 2900.0},
    {ChannelPosition::TopBackLeft, 3100.0},
    {ChannelPosition::TopBackCenter, 3300.0},
    {ChannelPosition::TopBackRight, 3500.0},
};

const char* positionName(ChannelPosition p) noexcept {
    switch (p) {
        case ChannelPosition::Mono:        return "Mono";
        case ChannelPosition::FrontLeft:   return "FL";
        case ChannelPosition::FrontRight:  return "FR";
        case ChannelPosition::FrontCenter: return "FC";
        case ChannelPosition::Lfe:         return "LFE";
        case ChannelPosition::BackLeft:    return "BL";
        case ChannelPosition::BackRight:   return "BR";
        case ChannelPosition::BackCenter:  return "BC";
        case ChannelPosition::SideLeft:    return "SL";
        case ChannelPosition::SideRight:   return "SR";
        case ChannelPosition::FrontLeftOfCenter:  return "FLC";
        case ChannelPosition::FrontRightOfCenter: return "FRC";
        case ChannelPosition::TopCenter:          return "TC";
        case ChannelPosition::TopFrontLeft:       return "TFL";
        case ChannelPosition::TopFrontCenter:     return "TFC";
        case ChannelPosition::TopFrontRight:      return "TFR";
        case ChannelPosition::TopBackLeft:        return "TBL";
        case ChannelPosition::TopBackCenter:      return "TBC";
        case ChannelPosition::TopBackRight:       return "TBR";
    }
    return "?";
}

// Goertzel power of one frequency over a channel's samples.
double tonePower(const std::vector<float>& x, double hz, double rate) {
    const double c = 2.0 * std::cos(2.0 * std::numbers::pi * hz / rate);
    double       a = 0.0;
    double       b = 0.0;
    for (const float v : x) {
        const double next = static_cast<double>(v) + c * a - b;
        b = a;
        a = next;
    }
    return a * a + b * b - c * a * b;
}

// The position whose tone is strongest in one channel.
ChannelPosition loudestPosition(const std::vector<float>& x, double rate) {
    ChannelPosition best      = ChannelPosition::Mono;
    double          bestPower = -1.0;
    for (const PositionTone& t : kPositionTones) {
        const double p = tonePower(x, t.hz, rate);
        if (p > bestPower) {
            bestPower = p;
            best      = t.position;
        }
    }
    return best;
}

// Open a layout fixture through the factory, saying why when nothing opens it.
std::unique_ptr<IDecoder> openLayoutFixture(const std::string& path) {
    DecoderFactory            factory;
    std::string               err;
    std::unique_ptr<IDecoder> dec = factory.open(path, &err);
    if (!dec) {
        std::fprintf(stderr, "open failed for %s: %s\n", path.c_str(), err.c_str());
    }
    return dec;
}

// The tone a position carries in the fixtures.
double toneHz(ChannelPosition p) noexcept {
    const auto* it = std::ranges::find(kPositionTones, p, &PositionTone::position);
    return it != std::ranges::end(kPositionTones) ? it->hz : 0.0;
}

// The fixtures this file writes for the layout check: half a second at 48 kHz.
constexpr int           kLayoutRate   = 48000;
constexpr std::uint64_t kLayoutFrames = 24000;

// The samples of a written layout fixture: channel c carries, at a quarter of
// full scale, the tone of the c-th position listed.
auto positionTones(std::initializer_list<ChannelPosition> positions) {
    std::vector<double> hz;
    for (const ChannelPosition p : positions) {
        hz.push_back(toneHz(p));
    }
    return [hz](std::uint64_t frame, int c) {
        const double t     = static_cast<double>(frame) / kLayoutRate;
        const double omega = 2.0 * std::numbers::pi * hz[static_cast<std::size_t>(c)];
        return static_cast<float>(0.25 * std::sin(omega * t));
    };
}

// The layout check proper, on a decoder opened on a fixture, which it removes.
// A mask, when given, is also asserted, for the rows whose point is the mask
// the decoder reports. Each decoded channel must hold the tone of its declared
// position, or, when holds is given, the tone holds names for it: a source
// with no positions keeps its channels where they were stored. An empty
// expected list means the stream is declared unpositioned, and holds then
// gives the channel count.
void checkLayoutOf(const char* name, const std::string& path,
                   std::unique_ptr<IDecoder> dec, DecoderKind decoder,
                   std::optional<std::uint32_t>           mask,
                   std::initializer_list<ChannelPosition> expected,
                   std::initializer_list<ChannelPosition> holds = {}) {
    CHECK(dec != nullptr);
    if (!dec) {
        std::remove(path.c_str());
        return;
    }

    const AudioFormat fmt = dec->format();
    CHECK(dec->sourceInfo().decoder == decoder);
    const std::size_t channels = expected.size() != 0 ? expected.size() : holds.size();
    CHECK(static_cast<std::size_t>(fmt.channels) == channels);
    if (mask) {
        if (fmt.channelMask != *mask) {
            std::fprintf(stderr, "  %s: mask 0x%04X, expected 0x%04X\n", name,
                         static_cast<unsigned>(fmt.channelMask),
                         static_cast<unsigned>(*mask));
        }
        CHECK(fmt.channelMask == *mask);
    }
    const auto declared = rawform::audio::channelPositionsFor(fmt);
    CHECK(std::ranges::equal(declared, expected));

    // Decode it all, then listen to the middle half of each channel, clear of
    // the encoder's start and end.
    const std::size_t  ch = fmt.channels;
    std::vector<float> all;
    std::vector<float> buf(4096 * ch);
    for (;;) {
        const std::size_t got = dec->read(buf.data(), 4096);
        if (got == 0) {
            break;
        }
        all.insert(all.end(), buf.begin(),
                   buf.begin() + static_cast<std::ptrdiff_t>(got * ch));
    }
    const std::size_t frames = ch == 0 ? 0 : all.size() / ch;
    CHECK(frames > 0);
    const std::size_t heldCount = holds.size() == 0 ? declared.size() : holds.size();
    for (std::size_t c = 0; c < ch && c < heldCount; ++c) {
        std::vector<float> x;
        for (std::size_t f = frames / 4; f < (3 * frames) / 4; ++f) {
            x.push_back(all[f * ch + c]);
        }
        const ChannelPosition want =
            holds.size() == 0 ? declared[c] : *(holds.begin() + c);
        const ChannelPosition heard = loudestPosition(x, fmt.sampleRate);
        if (heard != want) {
            std::fprintf(stderr,
                         "  %s channel %zu: declared %s, expected the %s tone, "
                         "holds the %s tone\n",
                         name, c, c < declared.size() ? positionName(declared[c]) : "-",
                         positionName(want), positionName(heard));
        }
        CHECK(heard == want);
    }

    dec.reset();
    std::remove(path.c_str());
}

// The same check on a fixture opened through the factory.
void checkDecodedLayout(const char* name, const std::string& path,
                        DecoderKind decoder, std::optional<std::uint32_t> mask,
                        std::initializer_list<ChannelPosition> expected,
                        std::initializer_list<ChannelPosition> holds = {}) {
    checkLayoutOf(name, path, openLayoutFixture(path), decoder, mask, expected, holds);
}

// A 16-bit 48 kHz FLAC layout fixture, half a second of one tone per channel,
// for the positions listed in order, carrying comment ("NAME=value") unless it
// is null. The decoder must report mask and declare the listed positions, each
// channel holding its own tone.
void checkFlacLayout(const char* name, Container container, const char* comment,
                     std::uint32_t mask,
                     std::initializer_list<ChannelPosition> positions) {
    const std::string path = std::string("rawform_layout_") + name +
                             (container == Container::Ogg ? ".oga" : ".flac");
    MetadataPtr comments = comment != nullptr
                               ? commentBlock(comment)
                               : MetadataPtr(nullptr, &FLAC__metadata_object_delete);
    CHECK(comment == nullptr || comments != nullptr);
    CHECK(encodeFlac(path, static_cast<int>(positions.size()), kLayoutRate, 16,
                     kLayoutFrames, container, comments.get(),
                     positionTones(positions)));
    checkDecodedLayout(name, path, DecoderKind::Flac, mask, positions);
}

// The channel map libsndfile reports for a file, empty when it reports none.
std::vector<int> channelMapOf(const std::string& path) {
    SF_INFO  info{};
    SNDFILE* sf = sf_open(path.c_str(), SFM_READ, &info);
    if (sf == nullptr) {
        return {};
    }
    std::vector<int> map(static_cast<std::size_t>(std::max(info.channels, 0)));
    const bool       has =
        sf_command(sf, SFC_GET_CHANNEL_MAP_INFO, map.data(),
                   static_cast<int>(map.size() * sizeof(int))) == SF_TRUE;
    sf_close(sf);
    return has ? map : std::vector<int>{};
}

// A 48 kHz float libsndfile layout fixture in the given format, half a second
// of one tone per channel for the source positions, in stream order, carrying
// map unless it is empty. libsndfile must read back the map it wrote, since that
// is all a decoder learns the speakers from.
void writeSndfileLayout(const char* name, const std::string& path, int sfFormat,
                        std::initializer_list<int>             map,
                        std::initializer_list<ChannelPosition> source) {
    const bool written = encodeSndfile(
        path, static_cast<int>(source.size()), kLayoutRate, kLayoutFrames, sfFormat,
        std::span<const int>(map.begin(), map.size()), positionTones(source));
    if (!written) {
        std::fprintf(stderr, "  %s: libsndfile did not write the file with its map\n",
                     name);
    }
    CHECK(written);
    const std::vector<int> back = channelMapOf(path);
    if (!std::ranges::equal(back, map)) {
        std::fprintf(stderr, "  %s: libsndfile reads back a map of %zu entries:", name,
                     back.size());
        for (const int v : back) {
            std::fprintf(stderr, " %d", v);
        }
        std::fprintf(stderr, "\n");
    }
    CHECK(std::ranges::equal(back, map));
}

// A libsndfile layout fixture (writeSndfileLayout) through the factory. The
// decoder must report mask and declare the declared positions, each decoded
// channel holding its own tone: the same list as the source when the map is
// in ascending order, the source's speakers in ascending order when it is not.
void checkSndfileLayout(const char* name, const char* ext, int sfFormat,
                        std::initializer_list<int> map, std::uint32_t mask,
                        std::initializer_list<ChannelPosition> source,
                        std::initializer_list<ChannelPosition> declared) {
    const std::string path = std::string("rawform_layout_") + name + ext;
    writeSndfileLayout(name, path, sfFormat, map, source);
    checkDecodedLayout(name, path, DecoderKind::Sndfile, mask, declared);
}

// ---------------------------------------------------------------------------
// Layout fixtures libsndfile cannot write, assembled byte by byte: a W64 whose
// format chunk carries a WAVEFORMATEXTENSIBLE mask, as FFmpeg writes one, and
// CAF and AIFF channel layout chunks that name the speakers with a channel
// bitmap or with one description per channel instead of a layout tag. Each
// holds 16-bit PCM at kLayoutRate, one tone per source channel.

// Integers appended in either byte order.
struct ByteWriter {
    std::vector<unsigned char> bytes;

    void put(std::uint64_t v, int size, bool bigEndian) {
        for (int i = 0; i < size; ++i) {
            const int shift = 8 * (bigEndian ? size - 1 - i : i);
            bytes.push_back(static_cast<unsigned char>((v >> shift) & 0xFFu));
        }
    }
    void be16(std::uint64_t v) { put(v, 2, true); }
    void be32(std::uint64_t v) { put(v, 4, true); }
    void be64(std::uint64_t v) { put(v, 8, true); }
    void le16(std::uint64_t v) { put(v, 2, false); }
    void le32(std::uint64_t v) { put(v, 4, false); }
    void le64(std::uint64_t v) { put(v, 8, false); }
    void tag(const char* fourcc) { bytes.insert(bytes.end(), fourcc, fourcc + 4); }
    void raw(std::span<const unsigned char> b) {
        bytes.insert(bytes.end(), b.begin(), b.end());
    }
};

// kLayoutFrames frames of 16-bit PCM, channel c carrying the tone of the c-th
// source position.
std::vector<unsigned char> pcm16(std::initializer_list<ChannelPosition> source,
                                 bool bigEndian) {
    const auto tone = positionTones(source);
    const int  ch   = static_cast<int>(source.size());
    ByteWriter w;
    for (std::uint64_t f = 0; f < kLayoutFrames; ++f) {
        for (int c = 0; c < ch; ++c) {
            const long v = std::lround(static_cast<double>(tone(f, c)) * 32767.0);
            w.put(static_cast<std::uint16_t>(static_cast<std::int16_t>(v)), 2, bigEndian);
        }
    }
    return std::move(w.bytes);
}

// A W64 with a WAVE_FORMAT_EXTENSIBLE format chunk naming mask. W64 chunks are
// identified by GUIDs, the first four bytes of each spelling the RIFF name.
bool writeW64(const std::string& path, std::uint32_t mask,
              std::initializer_list<ChannelPosition> source) {
    constexpr std::array<unsigned char, 12> kRiffTail = {
        0x2E, 0x91, 0xCF, 0x11, 0xA5, 0xD6, 0x28, 0xDB, 0x04, 0xC1, 0x00, 0x00};
    constexpr std::array<unsigned char, 12> kWaveTail = {
        0xF3, 0xAC, 0xD3, 0x11, 0x8C, 0xD1, 0x00, 0xC0, 0x4F, 0x8E, 0xDB, 0x8A};
    constexpr std::array<unsigned char, 16> kPcmSubformat = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
        0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    const auto                       ch      = static_cast<std::uint32_t>(source.size());
    const std::vector<unsigned char> samples = pcm16(source, /*bigEndian=*/false);
    constexpr std::uint64_t          kHeader = 24;  // GUID and 64-bit size
    constexpr std::uint64_t          kFmt    = 40;  // WAVEFORMATEXTENSIBLE

    ByteWriter w;
    w.tag("riff");
    w.raw(kRiffTail);
    w.le64(kHeader + 16 + kHeader + kFmt + kHeader + samples.size());
    w.tag("wave");
    w.raw(kWaveTail);
    w.tag("fmt ");
    w.raw(kWaveTail);
    w.le64(kHeader + kFmt);
    w.le16(0xFFFE);
    w.le16(ch);
    w.le32(kLayoutRate);
    w.le32(kLayoutRate * ch * 2);
    w.le16(ch * 2);
    w.le16(16);
    w.le16(22);
    w.le16(16);
    w.le32(mask);
    w.raw(kPcmSubformat);
    w.tag("data");
    w.raw(kWaveTail);
    w.le64(kHeader + samples.size());
    w.raw(samples);
    return writeBytes(path, w.bytes.data(), w.bytes.size());
}

// CoreAudio's channel labels and channel bitmap bits (CoreAudioBaseTypes.h) for
// the speakers these fixtures name, and its two layout tags that name speakers
// one by one rather than by layout.
constexpr std::uint32_t kLabelLeft                 = 1;
constexpr std::uint32_t kLabelRight                = 2;
constexpr std::uint32_t kLabelCenter               = 3;
constexpr std::uint32_t kLabelCenterSurround       = 9;
constexpr std::uint32_t kLabelTopCenterSurround    = 12;
constexpr std::uint32_t kLabelVerticalHeightCenter = 14;
constexpr std::uint32_t kLabelTopBackLeft          = 16;
constexpr std::uint32_t kLabelUnknown              = 0xFFFFFFFFu;
constexpr std::uint32_t kBitLeft                   = 1u << 0;
constexpr std::uint32_t kBitRight                  = 1u << 1;
constexpr std::uint32_t kBitCenter                 = 1u << 2;
constexpr std::uint32_t kBitLFEScreen              = 1u << 3;
constexpr std::uint32_t kBitLeftSurround           = 1u << 4;
constexpr std::uint32_t kBitRightSurround          = 1u << 5;
constexpr std::uint32_t kBitLeftCenter             = 1u << 6;
constexpr std::uint32_t kBitRightCenter            = 1u << 7;
constexpr std::uint32_t kBitCenterSurround         = 1u << 8;
constexpr std::uint32_t kTagUseChannelDescriptions = 0;
constexpr std::uint32_t kTagUseChannelBitmap       = 1u << 16;

// The payload of a CAF or AIFF channel layout chunk (CoreAudio's
// AudioChannelLayout, big-endian) naming the speakers with a bitmap.
ByteWriter bitmapLayout(std::uint32_t bitmap) {
    ByteWriter w;
    w.be32(kTagUseChannelBitmap);
    w.be32(bitmap);
    w.be32(0);
    return w;
}

// The same payload naming one speaker label per channel, in stream order.
ByteWriter descriptionLayout(std::initializer_list<std::uint32_t> labels) {
    ByteWriter w;
    w.be32(kTagUseChannelDescriptions);
    w.be32(0);
    w.be32(labels.size());
    for (const std::uint32_t label : labels) {
        w.be32(label);
        w.be32(0);           // flags
        w.be32(0);           // three float coordinates, unused
        w.be32(0);
        w.be32(0);
    }
    return w;
}

// A CAF holding big-endian 16-bit PCM and the channel layout chunk layout.
bool writeCaf(const std::string& path, const ByteWriter& layout,
              std::initializer_list<ChannelPosition> source) {
    const auto                       ch      = static_cast<std::uint32_t>(source.size());
    const std::vector<unsigned char> samples = pcm16(source, /*bigEndian=*/true);
    ByteWriter                       w;
    w.tag("caff");
    w.be16(1);  // version
    w.be16(0);  // flags
    w.tag("desc");
    w.be64(32);
    w.be64(std::bit_cast<std::uint64_t>(static_cast<double>(kLayoutRate)));
    w.tag("lpcm");
    w.be32(0);  // format flags: big-endian integer
    w.be32(ch * 2);
    w.be32(1);
    w.be32(ch);
    w.be32(16);
    w.tag("chan");
    w.be64(layout.bytes.size());
    w.raw(layout.bytes);
    w.tag("data");
    w.be64(4 + samples.size());
    w.be32(0);  // edit count
    w.raw(samples);
    return writeBytes(path, w.bytes.data(), w.bytes.size());
}

// An AIFF holding big-endian 16-bit PCM and the channel layout chunk layout.
bool writeAiff(const std::string& path, const ByteWriter& layout,
               std::initializer_list<ChannelPosition> source) {
    static_assert(kLayoutRate == 48000, "the COMM chunk spells 48000 below");
    constexpr std::array<unsigned char, 10> kRate48000 = {  // 80-bit IEEE extended
        0x40, 0x0E, 0xBB, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const auto                       ch      = static_cast<std::uint32_t>(source.size());
    const std::vector<unsigned char> samples = pcm16(source, /*bigEndian=*/true);
    ByteWriter                       body;
    body.tag("AIFF");
    body.tag("COMM");
    body.be32(18);
    body.be16(ch);
    body.be32(kLayoutFrames);
    body.be16(16);
    body.raw(kRate48000);
    body.tag("CHAN");  // every layout payload has an even size, so no pad byte
    body.be32(layout.bytes.size());
    body.raw(layout.bytes);
    body.tag("SSND");
    body.be32(8 + samples.size());
    body.be32(0);  // offset
    body.be32(0);  // block size
    body.raw(samples);
    ByteWriter w;
    w.tag("FORM");
    w.be32(body.bytes.size());
    w.raw(body.bytes);
    return writeBytes(path, w.bytes.data(), w.bytes.size());
}

// A fixture write(path) assembles, opened through the factory, which hands
// these containers to libsndfile. In FFmpeg builds a second copy goes to FFmpeg
// directly, which reads the same headers on its own, so the two readings of a
// fixture are held to one answer. Each must report mask and declare the
// declared positions, with the tones checkLayoutOf expects.
template <typename Write>
void checkByteLayout(const char* name, const char* ext, Write write, std::uint32_t mask,
                     std::initializer_list<ChannelPosition> declared,
                     std::initializer_list<ChannelPosition> holds = {}) {
    const std::string path = std::string("rawform_layout_") + name + ext;
    CHECK(write(path));
    checkDecodedLayout(name, path, DecoderKind::Sndfile, mask, declared, holds);
#if RAWFORM_HAVE_FFMPEG
    const std::string ffName = std::string("ffmpeg_") + name;
    const std::string ffPath = std::string("rawform_layout_") + ffName + ext;
    CHECK(write(ffPath));
    std::string               err;
    std::unique_ptr<IDecoder> dec = rawform::audio::FfmpegDecoder::open(ffPath, &err);
    if (!dec) {
        std::fprintf(stderr, "open failed for %s: %s\n", ffPath.c_str(), err.c_str());
    }
    checkLayoutOf(ffName.c_str(), ffPath, std::move(dec), DecoderKind::Ffmpeg, mask,
                  declared, holds);
#endif
}

// An Ogg Vorbis or Opus layout fixture, written by libsndfile with its channels
// in Vorbis order, the order libvorbis and libopus take them in; no channel map,
// since Ogg carries none. Without FFmpeg libsndfile decodes it. With FFmpeg the
// factory hands it to FFmpeg, which reorders Vorbis order itself, so there the
// same row proves the fixture carries true Vorbis order.
void checkVorbisOrderLayout(const char* name, const char* ext, int sfFormat,
                            std::initializer_list<ChannelPosition> source,
                            std::initializer_list<ChannelPosition> declared) {
    const std::string path = std::string("rawform_layout_") + name + ext;
    writeSndfileLayout(name, path, sfFormat, {}, source);
#if RAWFORM_HAVE_FFMPEG
    constexpr DecoderKind decoder = DecoderKind::Ffmpeg;
#else
    constexpr DecoderKind decoder = DecoderKind::Sndfile;
#endif
    checkDecodedLayout(name, path, decoder, 0, declared);
}

// A whole file's bytes, empty when it cannot be read.
std::vector<unsigned char> readBytes(const std::string& path) {
    std::vector<unsigned char> bytes;
    std::FILE*                 f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return bytes;
    }
    std::array<unsigned char, 4096> buf{};
    for (;;) {
        const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
        bytes.insert(bytes.end(), buf.begin(),
                     buf.begin() + static_cast<std::ptrdiff_t>(got));
        if (got < buf.size()) {
            break;
        }
    }
    std::fclose(f);
    return bytes;
}

// The Ogg page checksum: CRC-32 with polynomial 0x04C11DB7, no reflection and
// a zero initial value, over a page whose own checksum field reads zero.
std::uint32_t oggCrc(std::span<const unsigned char> page) {
    std::uint32_t crc = 0;
    for (const unsigned char b : page) {
        crc ^= static_cast<std::uint32_t>(b) << 24;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x80000000u) != 0 ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

// Sets an Ogg Opus file's channel mapping family: byte 18 of its ID header
// (OpusHead), which RFC 7845 puts alone on the first page, whose checksum is
// then computed again. Families 1 and 255 share the mapping table's layout, so
// the streams still decode as before; only the positions the family promises
// change.
bool setOpusMappingFamily(const std::string& path, unsigned char family) {
    std::vector<unsigned char> bytes = readBytes(path);
    if (bytes.size() < 27 || std::memcmp(bytes.data(), "OggS", 4) != 0) {
        return false;
    }
    const std::size_t segments = bytes[26];
    const std::size_t start    = 27 + segments;
    if (bytes.size() < start) {
        return false;
    }
    std::size_t length = 0;
    for (std::size_t i = 0; i < segments; ++i) {
        length += bytes[27 + i];
    }
    if (length < 19 || bytes.size() < start + length ||
        std::memcmp(bytes.data() + start, "OpusHead", 8) != 0) {
        return false;
    }
    bytes[start + 18] = family;
    std::fill_n(bytes.begin() + 22, 4, static_cast<unsigned char>(0));
    const std::uint32_t crc = oggCrc(std::span(bytes).first(start + length));
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[22 + i] = static_cast<unsigned char>((crc >> (8 * i)) & 0xFFu);
    }
    return writeBytes(path, bytes.data(), bytes.size());
}

// A 5.1 Ogg Opus written by libsndfile in Vorbis order, then given mapping
// family 255, which defines no positions. Without FFmpeg libsndfile decodes it;
// with FFmpeg the factory hands it to FFmpeg, so the same row holds the two
// builds to one answer: no mask, the default order's positions declared, and
// the channels left where they were stored.
void checkOpusUndefinedMapping(std::initializer_list<ChannelPosition> stored,
                               std::initializer_list<ChannelPosition> declared) {
    const std::string path = "rawform_layout_opus_51_m255.opus";
    writeSndfileLayout("opus_51_m255", path, SF_FORMAT_OGG | SF_FORMAT_OPUS, {}, stored);
    CHECK(setOpusMappingFamily(path, 255));
#if RAWFORM_HAVE_FFMPEG
    constexpr DecoderKind decoder = DecoderKind::Ffmpeg;
#else
    constexpr DecoderKind decoder = DecoderKind::Sndfile;
#endif
    checkDecodedLayout("opus_51_m255", path, decoder, 0, declared, stored);
}

// A float ramp in a CAF whose map is out of ascending order (C L R Cs). The
// decoder must deliver every sample bit for bit, only moved to its speaker's
// place: output channel i holds source channel from[i].
void checkReorderExact() {
    const std::string                      path   = "rawform_reorder_exact.caf";
    constexpr int                          ch     = 4;
    constexpr std::uint64_t                frames = 5000;
    constexpr std::array<std::uint64_t, 4> from   = {1, 2, 0, 3};
    const std::initializer_list<int>       map    = {
        SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
        SF_CHANNEL_MAP_REAR_CENTER};
    CHECK(encodeSndfile(path, ch, kLayoutRate, frames, SF_FORMAT_CAF | SF_FORMAT_FLOAT,
                        std::span<const int>(map.begin(), map.size()),
                        [](std::uint64_t frame, int c) {
                            return pat(frame * ch + static_cast<std::uint64_t>(c));
                        }));
    std::unique_ptr<IDecoder> dec = openLayoutFixture(path);
    CHECK(dec != nullptr);
    if (!dec) {
        std::remove(path.c_str());
        return;
    }
    CHECK(dec->format().channelMask == 0x0107);
    std::vector<float> all;
    std::vector<float> buf(1024 * ch);
    for (;;) {
        const std::size_t got = dec->read(buf.data(), 1024);
        if (got == 0) {
            break;
        }
        all.insert(all.end(), buf.begin(),
                   buf.begin() + static_cast<std::ptrdiff_t>(got * ch));
    }
    CHECK(all.size() == frames * ch);
    std::size_t moved = 0;
    for (std::uint64_t f = 0; f < frames && (f + 1) * ch <= all.size(); ++f) {
        for (std::uint64_t i = 0; i < ch; ++i) {
            const float want = pat(f * ch + from[i]);
            if (std::bit_cast<std::uint32_t>(all[f * ch + i]) !=
                std::bit_cast<std::uint32_t>(want)) {
                ++moved;
            }
        }
    }
    if (moved != 0) {
        std::fprintf(stderr, "  reorder_exact: %zu samples not where expected\n", moved);
    }
    CHECK(moved == 0);
    dec.reset();
    std::remove(path.c_str());
}

#if RAWFORM_HAVE_FFMPEG
// A libsndfile layout fixture (writeSndfileLayout) opened by FFmpeg directly,
// since the factory hands these containers to libsndfile. FFmpeg must report
// mask and declare the declared positions, each channel holding its own tone.
void checkFfmpegSndfileLayout(const char* name, const char* ext, int sfFormat,
                              std::initializer_list<int> map, std::uint32_t mask,
                              std::initializer_list<ChannelPosition> source,
                              std::initializer_list<ChannelPosition> declared) {
    const std::string path = std::string("rawform_layout_") + name + ext;
    writeSndfileLayout(name, path, sfFormat, map, source);
    std::string               err;
    std::unique_ptr<IDecoder> dec = rawform::audio::FfmpegDecoder::open(path, &err);
    if (!dec) {
        std::fprintf(stderr, "open failed for %s: %s\n", path.c_str(), err.c_str());
    }
    checkLayoutOf(name, path, std::move(dec), DecoderKind::Ffmpeg, mask, declared);
}

// The out-of-order CAF (C L R Cs) through FFmpeg, which reads such a layout as
// a custom order.
void checkFfmpegCustomOrder() {
    using P = ChannelPosition;
    checkFfmpegSndfileLayout(
        "ffmpeg_custom", ".caf", SF_FORMAT_CAF | SF_FORMAT_FLOAT,
        {SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
         SF_CHANNEL_MAP_REAR_CENTER},
        0x0107, {P::FrontCenter, P::FrontLeft, P::FrontRight, P::BackCenter},
        {P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackCenter});
}

void checkLayout(const char* name, const unsigned char* bytes, std::size_t size,
                 std::initializer_list<ChannelPosition> expected) {
    const std::string path = std::string("rawform_layout_") + name + ".ac3";
    CHECK(writeBytes(path, bytes, size));
    checkDecodedLayout(name, path, DecoderKind::Ffmpeg, std::nullopt, expected);
}

// Every frame a decoder delivers, interleaved.
std::vector<float> decodeAll(IDecoder& dec) {
    const std::size_t  ch = dec.format().channels;
    std::vector<float> all;
    std::vector<float> buf(4096 * ch);
    for (;;) {
        const std::size_t got = dec.read(buf.data(), 4096);
        if (got == 0) {
            break;
        }
        all.insert(all.end(), buf.begin(),
                   buf.begin() + static_cast<std::ptrdiff_t>(got * ch));
    }
    return all;
}

// One raw AC3 stream whose layout changes partway at the same channel count: the
// 4.0 blob written kLeadCopies times, then the quad(side) blob once. The lead-in
// outlasts FFmpeg's default 5 s stream analysis, so open() declares 4.0 however
// far the probe reads; a lead-in of one copy is declared quad(side), the last
// layout a probe of the whole file saw. The quad(side) frames name other speakers
// (SL SR where 4.0 has FC BC), so each one is dropped and counted: what plays is
// the 4.0 part alone, and its channels hold their own tones up to the end.
void checkLayoutChange() {
    constexpr int           kLeadCopies     = 32;    // 6 AC3 frames each, 6.1 s
    constexpr std::uint64_t kQuadSideFrames = 7;     // AC3 frames in that blob
    constexpr std::size_t   kAc3FrameLength = 1536;  // samples per AC3 frame
    const std::string       leadPath        = "rawform_layout_change_lead.ac3";
    const std::string       path            = "rawform_layout_change.ac3";

    std::vector<unsigned char> bytes;
    for (int i = 0; i < kLeadCopies; ++i) {
        bytes.insert(bytes.end(), std::begin(kAc3Layout40), std::end(kAc3Layout40));
    }
    CHECK(writeBytes(leadPath, bytes.data(), bytes.size()));
    bytes.insert(bytes.end(), std::begin(kAc3LayoutQuadSide),
                 std::end(kAc3LayoutQuadSide));
    CHECK(writeBytes(path, bytes.data(), bytes.size()));

    std::unique_ptr<IDecoder> lead = openLayoutFixture(leadPath);
    std::unique_ptr<IDecoder> dec  = openLayoutFixture(path);
    CHECK(lead != nullptr);
    CHECK(dec != nullptr);
    if (lead && dec) {
        const AudioFormat fmt = dec->format();
        CHECK(dec->sourceInfo().decoder == DecoderKind::Ffmpeg);
        CHECK(fmt.channels == 4);
        if (fmt.channelMask != 0x0107) {
            std::fprintf(stderr, "  layout_change: mask 0x%04X, expected 0x0107\n",
                         static_cast<unsigned>(fmt.channelMask));
        }
        CHECK(fmt.channelMask == 0x0107);

        const std::vector<float> leadPcm    = decodeAll(*lead);
        const std::vector<float> all        = decodeAll(*dec);
        const std::size_t        frames     = all.size() / 4;
        const std::size_t        leadFrames = leadPcm.size() / 4;
        if (frames != leadFrames || dec->recoveredErrors() != kQuadSideFrames) {
            std::fprintf(stderr,
                         "  layout_change: %zu frames, the 4.0 part alone %zu; %llu "
                         "frames dropped, expected %llu\n",
                         frames, leadFrames,
                         static_cast<unsigned long long>(dec->recoveredErrors()),
                         static_cast<unsigned long long>(kQuadSideFrames));
        }
        CHECK(frames == leadFrames);
        CHECK(dec->recoveredErrors() == kQuadSideFrames);

        // The last frames delivered, short of the very last one: where the
        // quad(side) frames would land if they were let through.
        const auto declared = rawform::audio::channelPositionsFor(fmt);
        CHECK(declared.size() == 4);
        CHECK(frames >= 6 * kAc3FrameLength);
        if (declared.size() == 4 && frames >= 6 * kAc3FrameLength) {
            for (std::size_t c = 0; c < 4; ++c) {
                std::vector<float> x;
                for (std::size_t f = frames - 6 * kAc3FrameLength;
                     f < frames - kAc3FrameLength; ++f) {
                    x.push_back(all[f * 4 + c]);
                }
                const ChannelPosition heard = loudestPosition(x, fmt.sampleRate);
                if (heard != declared[c]) {
                    std::fprintf(stderr,
                                 "  layout_change channel %zu: declared %s, holds the "
                                 "%s tone at the end\n",
                                 c, positionName(declared[c]), positionName(heard));
                }
                CHECK(heard == declared[c]);
            }
        }
    }
    lead.reset();
    dec.reset();
    std::remove(leadPath.c_str());
    std::remove(path.c_str());
}

// A source FFmpeg decodes in the default order for its channel count reports
// no mask, so it keeps the table's positions and can still join a stream of a
// native-decoder source with the same count.
void checkLayoutControl(const char* name, int channels,
                        std::initializer_list<ChannelPosition> expected) {
    const std::string path = std::string("rawform_layout_") + name + ".ogg";
    CHECK(writeSndfileFloat(path, channels, 44100, 50000,
                            SF_FORMAT_OGG | SF_FORMAT_VORBIS));
    std::unique_ptr<IDecoder> dec = openLayoutFixture(path);
    CHECK(dec != nullptr);
    if (dec) {
        const AudioFormat fmt = dec->format();
        CHECK(dec->sourceInfo().decoder == DecoderKind::Ffmpeg);
        CHECK(fmt.channelMask == 0);
        CHECK(positionsAre(fmt, expected));
        dec.reset();
    }
    std::remove(path.c_str());
}
#endif

}  // namespace

int main() {
    std::printf("[conformance] channel positions\n");
    checkChannelPositions();
    checkDeviceRouting();
    checkChannelOrderBetween();

    // The fixture table is positional on purpose: nine columns per row read as a
    // table, and designated initializers would triple the row width.
    // NOLINTBEGIN(modernize-use-designated-initializers)
    constexpr Fixture fixtures[] = {
        {"wav",  ".wav",  &createWav,  Codec::Pcm,  DecoderKind::Sndfile, Tier::Strict,     2, 44100, 1.0e-5f},
        {"flac", ".flac", &createFlac, Codec::Flac, DecoderKind::Flac,    Tier::Strict,     2, 44100, 2.0e-4f},
        {"mp3",  ".mp3",  &createMp3,  Codec::Mp3,  DecoderKind::Mpg123,  Tier::Structural, 1, 44100, 0.0f},
        // Ogg FLAC routes to libFLAC by content, under its own name and under
        // the .flac name such files often carry.
        {"oggflac",         ".oga",  &createOggFlac, Codec::Flac, DecoderKind::Flac, Tier::Strict, 2, 44100, 2.0e-4f},
        {"oggflac_as_flac", ".flac", &createOggFlac, Codec::Flac, DecoderKind::Flac, Tier::Strict, 2, 44100, 2.0e-4f},
    };
    for (const Fixture& fx : fixtures) {
        std::printf("[conformance] %s\n", fx.name);
        runBattery(fx);
    }

    {
        // FLAC files that name 4.0 in a WAVEFORMATEXTENSIBLE_CHANNEL_MASK comment,
        // natively and in Ogg, with the field name in either case, and one that
        // names FL FR FLC. Then files whose comment, or its absence, leaves the
        // default order for the count: the default mask itself, a speaker the
        // table cannot name (bit 18), a bit count that disagrees with the
        // channel count, and a value not written in hex.
        using P = ChannelPosition;
        constexpr const char* k40 = "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0107";
        const std::initializer_list<P> p40 = {P::FrontLeft, P::FrontRight,
                                              P::FrontCenter, P::BackCenter};
        const std::initializer_list<P> quad = {P::FrontLeft, P::FrontRight,
                                               P::BackLeft, P::BackRight};
        const std::initializer_list<P> p30 = {P::FrontLeft, P::FrontRight,
                                              P::FrontCenter};
        std::printf("[conformance] native layouts\n");
        checkFlacLayout("flac_40", Container::Native, k40, 0x0107, p40);
        checkFlacLayout("oggflac_40", Container::Ogg, k40, 0x0107, p40);
        checkFlacLayout("flac_40_lowercase", Container::Native,
                        "waveformatextensible_channel_mask=0x0107", 0x0107, p40);
        checkFlacLayout("flac_quad_mask", Container::Native,
                        "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0033", 0, quad);
        checkFlacLayout("flac_flc_mask", Container::Native,
                        "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0043", 0x0043,
                        {P::FrontLeft, P::FrontRight, P::FrontLeftOfCenter});
        checkFlacLayout("flac_unnamed_mask", Container::Native,
                        "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x40003", 0, p30);
        checkFlacLayout("flac_40_mask_on_3", Container::Native, k40, 0, p30);
        checkFlacLayout("flac_decimal_mask", Container::Native,
                        "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=263", 0, quad);
        checkFlacLayout("flac_no_mask", Container::Native, nullptr, 0, quad);

        // Files whose container carries a channel map naming 4.0: WAVE's channel
        // mask in WAVEX and RF64, a channel layout chunk in CAF and AIFF.
        // libsndfile spells the front speakers with Apple's names in every one
        // of them. Then a WAVE mask naming the default order, a WAV with no map
        // at all, and a W64, into which libsndfile writes no map.
        const std::initializer_list<int> map40 = {
            SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_CENTER,
            SF_CHANNEL_MAP_REAR_CENTER};
        const std::initializer_list<int> mapQuad = {
            SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT, SF_CHANNEL_MAP_REAR_LEFT,
            SF_CHANNEL_MAP_REAR_RIGHT};
        checkSndfileLayout("wavex_40", ".wav", SF_FORMAT_WAVEX | SF_FORMAT_FLOAT, map40,
                           0x0107, p40, p40);
        checkSndfileLayout("rf64_40", ".rf64", SF_FORMAT_RF64 | SF_FORMAT_FLOAT, map40,
                           0x0107, p40, p40);
        checkSndfileLayout("caf_40", ".caf", SF_FORMAT_CAF | SF_FORMAT_FLOAT, map40,
                           0x0107, p40, p40);
        checkSndfileLayout("aiff_40", ".aiff", SF_FORMAT_AIFF | SF_FORMAT_FLOAT, map40,
                           0x0107, p40, p40);
        checkSndfileLayout("wavex_quad_map", ".wav", SF_FORMAT_WAVEX | SF_FORMAT_FLOAT,
                           mapQuad, 0, quad, quad);
        checkSndfileLayout("wav_no_map", ".wav", SF_FORMAT_WAV | SF_FORMAT_FLOAT, {}, 0,
                           quad, quad);
        checkSndfileLayout("w64_no_map", ".w64", SF_FORMAT_W64 | SF_FORMAT_FLOAT, {}, 0,
                           quad, quad);

        // Layouts whose channels are not in ascending speaker order, from the
        // layout table CAF and AIFF share: MPEG 4.0 B (C L R Cs) is delivered as
        // 4.0, MPEG 5.1 C (L C R Ls Rs LFE) as the default 5.1, so with no mask.
        // Then the exact check that the reorder only moves samples.
        const std::initializer_list<int> map40b = {
            SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
            SF_CHANNEL_MAP_REAR_CENTER};
        const std::initializer_list<P> src40b = {P::FrontCenter, P::FrontLeft,
                                                 P::FrontRight, P::BackCenter};
        checkSndfileLayout("caf_40b", ".caf", SF_FORMAT_CAF | SF_FORMAT_FLOAT, map40b,
                           0x0107, src40b, p40);
        checkSndfileLayout("aiff_40b", ".aiff", SF_FORMAT_AIFF | SF_FORMAT_FLOAT, map40b,
                           0x0107, src40b, p40);
        checkSndfileLayout(
            "caf_51c", ".caf", SF_FORMAT_CAF | SF_FORMAT_FLOAT,
            {SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_CENTER, SF_CHANNEL_MAP_RIGHT,
             SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT, SF_CHANNEL_MAP_LFE},
            0,
            {P::FrontLeft, P::FrontCenter, P::FrontRight, P::BackLeft, P::BackRight,
             P::Lfe},
            {P::FrontLeft, P::FrontRight, P::FrontCenter, P::Lfe, P::BackLeft,
             P::BackRight});
        checkReorderExact();

        // Headers libsndfile cannot write, assembled here. A W64 whose format
        // chunk names 4.0 in its mask, as FFmpeg writes one. CAF and AIFF
        // layout chunks naming 4.0 with a channel bitmap, and C L R Cs with one
        // description per channel, which is moved to 4.0. Then descriptions
        // with a label that is no speaker, which leave the channels where they
        // are, under the default order's positions.
        checkByteLayout(
            "w64_40", ".w64",
            [&](const std::string& p) { return writeW64(p, 0x0107, p40); }, 0x0107, p40);
        const std::uint32_t bitmap40 = kBitLeft | kBitRight | kBitCenter |
                                       kBitCenterSurround;
        const std::initializer_list<std::uint32_t> labels40b = {
            kLabelCenter, kLabelLeft, kLabelRight, kLabelCenterSurround};
        checkByteLayout(
            "caf_bitmap_40", ".caf",
            [&](const std::string& p) {
                return writeCaf(p, bitmapLayout(bitmap40), p40);
            },
            0x0107, p40);
        checkByteLayout(
            "caf_desc_40b", ".caf",
            [&](const std::string& p) {
                return writeCaf(p, descriptionLayout(labels40b), src40b);
            },
            0x0107, p40);
        checkByteLayout(
            "aiff_bitmap_40", ".aiff",
            [&](const std::string& p) {
                return writeAiff(p, bitmapLayout(bitmap40), p40);
            },
            0x0107, p40);
        checkByteLayout(
            "aiff_desc_40b", ".aiff",
            [&](const std::string& p) {
                return writeAiff(p, descriptionLayout(labels40b), src40b);
            },
            0x0107, p40);
        checkByteLayout(
            "caf_desc_unknown", ".caf",
            [&](const std::string& p) {
                return writeCaf(p,
                                descriptionLayout({kLabelLeft, kLabelRight, kLabelCenter,
                                                   kLabelUnknown}),
                                p40);
            },
            0, quad, p40);

        // Layouts with the speakers left and right of center and the height
        // layer: 7.1(wide) and 5.1.2(back) in WAVEX, 7.1(wide) as a CAF channel
        // bitmap, and TFC L R TBL TC as CAF descriptions, moved to ascending
        // order. libsndfile's CAF and AIFF layout tags name none of them.
        const std::initializer_list<P> p71wide = {
            P::FrontLeft, P::FrontRight, P::FrontCenter,       P::Lfe,
            P::BackLeft,  P::BackRight,  P::FrontLeftOfCenter, P::FrontRightOfCenter};
        const std::initializer_list<P> p512back = {
            P::FrontLeft, P::FrontRight, P::FrontCenter,  P::Lfe,
            P::BackLeft,  P::BackRight,  P::TopFrontLeft, P::TopFrontRight};
        const std::initializer_list<int> map71wide = {
            SF_CHANNEL_MAP_LEFT,      SF_CHANNEL_MAP_RIGHT,
            SF_CHANNEL_MAP_CENTER,    SF_CHANNEL_MAP_LFE,
            SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT,
            SF_CHANNEL_MAP_FRONT_LEFT_OF_CENTER, SF_CHANNEL_MAP_FRONT_RIGHT_OF_CENTER};
        const std::initializer_list<int> map512back = {
            SF_CHANNEL_MAP_LEFT,          SF_CHANNEL_MAP_RIGHT,
            SF_CHANNEL_MAP_CENTER,        SF_CHANNEL_MAP_LFE,
            SF_CHANNEL_MAP_REAR_LEFT,     SF_CHANNEL_MAP_REAR_RIGHT,
            SF_CHANNEL_MAP_TOP_FRONT_LEFT, SF_CHANNEL_MAP_TOP_FRONT_RIGHT};
        checkSndfileLayout("wavex_71wide", ".wav", SF_FORMAT_WAVEX | SF_FORMAT_FLOAT,
                           map71wide, 0x00FF, p71wide, p71wide);
        checkSndfileLayout("wavex_512back", ".wav", SF_FORMAT_WAVEX | SF_FORMAT_FLOAT,
                           map512back, 0x503F, p512back, p512back);
        checkByteLayout(
            "caf_bitmap_71wide", ".caf",
            [&](const std::string& p) {
                return writeCaf(p,
                                bitmapLayout(kBitLeft | kBitRight | kBitCenter |
                                             kBitLFEScreen | kBitLeftSurround |
                                             kBitRightSurround | kBitLeftCenter |
                                             kBitRightCenter),
                                p71wide);
            },
            0x00FF, p71wide);
        checkByteLayout(
            "caf_desc_height", ".caf",
            [&](const std::string& p) {
                return writeCaf(p,
                                descriptionLayout({kLabelVerticalHeightCenter, kLabelLeft,
                                                   kLabelRight, kLabelTopBackLeft,
                                                   kLabelTopCenterSurround}),
                                {P::TopFrontCenter, P::FrontLeft, P::FrontRight,
                                 P::TopBackLeft, P::TopCenter});
            },
            0xA803,
            {P::FrontLeft, P::FrontRight, P::TopCenter, P::TopFrontCenter,
             P::TopBackLeft});

        // More than eight channels: 7.1.4 in WAVEX, twelve named speakers with
        // the top back pair, and the top back row over a stereo pair. A
        // 12-channel WAV with no map has no default order to fall back on, so
        // it is declared unpositioned and keeps its channels as stored.
        const std::initializer_list<P> p714 = {
            P::FrontLeft,    P::FrontRight,    P::FrontCenter, P::Lfe,
            P::BackLeft,     P::BackRight,     P::SideLeft,    P::SideRight,
            P::TopFrontLeft, P::TopFrontRight, P::TopBackLeft, P::TopBackRight};
        const std::initializer_list<int> map714 = {
            SF_CHANNEL_MAP_LEFT,           SF_CHANNEL_MAP_RIGHT,
            SF_CHANNEL_MAP_CENTER,         SF_CHANNEL_MAP_LFE,
            SF_CHANNEL_MAP_REAR_LEFT,      SF_CHANNEL_MAP_REAR_RIGHT,
            SF_CHANNEL_MAP_SIDE_LEFT,      SF_CHANNEL_MAP_SIDE_RIGHT,
            SF_CHANNEL_MAP_TOP_FRONT_LEFT, SF_CHANNEL_MAP_TOP_FRONT_RIGHT,
            SF_CHANNEL_MAP_TOP_REAR_LEFT,  SF_CHANNEL_MAP_TOP_REAR_RIGHT};
        checkSndfileLayout("wavex_714", ".wav", SF_FORMAT_WAVEX | SF_FORMAT_FLOAT,
                           map714, 0x2D63F, p714, p714);
        const std::initializer_list<P> pTopBack = {P::FrontLeft, P::FrontRight,
                                                   P::TopBackLeft, P::TopBackCenter,
                                                   P::TopBackRight};
        checkSndfileLayout("wavex_top_back", ".wav", SF_FORMAT_WAVEX | SF_FORMAT_FLOAT,
                           {SF_CHANNEL_MAP_LEFT, SF_CHANNEL_MAP_RIGHT,
                            SF_CHANNEL_MAP_TOP_REAR_LEFT,
                            SF_CHANNEL_MAP_TOP_REAR_CENTER,
                            SF_CHANNEL_MAP_TOP_REAR_RIGHT},
                           0x38003, pTopBack, pTopBack);
        const std::string wav12 = "rawform_layout_wav_12_no_map.wav";
        writeSndfileLayout("wav_12_no_map", wav12, SF_FORMAT_WAV | SF_FORMAT_FLOAT, {},
                           p714);
        checkDecodedLayout("wav_12_no_map", wav12, DecoderKind::Sndfile, 0, {}, p714);
#if RAWFORM_HAVE_FFMPEG
        // FFmpeg names these WAVEX layouts natively; opened directly, it agrees.
        checkFfmpegSndfileLayout("ffmpeg_wavex_71wide", ".wav",
                                 SF_FORMAT_WAVEX | SF_FORMAT_FLOAT, map71wide, 0x00FF,
                                 p71wide, p71wide);
        checkFfmpegSndfileLayout("ffmpeg_wavex_512back", ".wav",
                                 SF_FORMAT_WAVEX | SF_FORMAT_FLOAT, map512back, 0x503F,
                                 p512back, p512back);
        checkFfmpegSndfileLayout("ffmpeg_wavex_714", ".wav",
                                 SF_FORMAT_WAVEX | SF_FORMAT_FLOAT, map714, 0x2D63F,
                                 p714, p714);
#endif

        // Ogg Vorbis and Opus in Vorbis order (3.0 as L C R, 5.1 as L C R BL BR
        // LFE), delivered in the default order for their count, so with no mask.
        const std::initializer_list<P> vorbis51 = {P::FrontLeft, P::FrontCenter,
                                                   P::FrontRight, P::BackLeft,
                                                   P::BackRight, P::Lfe};
        const std::initializer_list<P> p51 = {P::FrontLeft, P::FrontRight,
                                              P::FrontCenter, P::Lfe,
                                              P::BackLeft, P::BackRight};
        checkVorbisOrderLayout("vorbis_30", ".ogg", SF_FORMAT_OGG | SF_FORMAT_VORBIS,
                               {P::FrontLeft, P::FrontCenter, P::FrontRight}, p30);
        checkVorbisOrderLayout("vorbis_51", ".ogg", SF_FORMAT_OGG | SF_FORMAT_VORBIS,
                               vorbis51, p51);
        checkVorbisOrderLayout("opus_51", ".opus", SF_FORMAT_OGG | SF_FORMAT_OPUS,
                               vorbis51, p51);

        // The same Opus stream under mapping family 255, which defines no order.
        checkOpusUndefinedMapping(vorbis51, p51);
    }

#if RAWFORM_HAVE_FFMPEG
    // The FFmpeg fixtures. Each runs the decode battery, whose si.codec and
    // si.decoder checks together prove the factory reached FfmpegDecoder and not a
    // native decoder (the routing guard, the exact failure the DTS/ASF sniff
    // carve-outs fixed), plus the probe check on the read side. AAC/AC3/DTS/WMA/
    // ALAC are embedded blobs; Vorbis and Opus are libsndfile-generated yet still
    // decode through FFmpeg because SndfileDecoder declines those Ogg subtypes.
    {
        constexpr Fixture aac{"aac", ".aac", &createAac, Codec::Aac, DecoderKind::Ffmpeg,
                              Tier::Structural, 2, 44100, 0.0f};
        std::printf("[conformance] %s\n", aac.name);
        runBattery(aac);
        checkProbe(aac, "AAC");
    }
    {
        constexpr Fixture ac3{"ac3", ".ac3", &createAc3, Codec::Ac3, DecoderKind::Ffmpeg,
                              Tier::Structural, 2, 44100, 0.0f};
        std::printf("[conformance] %s\n", ac3.name);
        runBattery(ac3);
        checkProbe(ac3, "AC3");
    }
    {
        constexpr Fixture dts{"dts", ".dts", &createDts, Codec::Dts, DecoderKind::Ffmpeg,
                              Tier::Structural, 2, 44100, 0.0f};
        std::printf("[conformance] %s\n", dts.name);
        runBattery(dts);
        checkProbe(dts, "DTS");
    }
    {
        constexpr Fixture vorbis{"vorbis", ".ogg", &createVorbis, Codec::Vorbis,
                                 DecoderKind::Ffmpeg, Tier::Structural, 2, 44100, 0.0f};
        std::printf("[conformance] %s\n", vorbis.name);
        runBattery(vorbis);
        checkProbe(vorbis, "Vorbis");
    }
    {
        // An Ogg Vorbis file named .flac: the content says Ogg, not FLAC, so
        // libsndfile stays a candidate, declines Vorbis, and FFmpeg decodes it.
        constexpr Fixture vorbisAsFlac{"vorbis_as_flac", ".flac", &createVorbis,
                                       Codec::Vorbis, DecoderKind::Ffmpeg,
                                       Tier::Structural, 2, 44100, 0.0f};
        std::printf("[conformance] %s\n", vorbisAsFlac.name);
        runBattery(vorbisAsFlac);
    }
    {
        constexpr Fixture opus{"opus", ".opus", &createOpus, Codec::Opus,
                               DecoderKind::Ffmpeg, Tier::Structural, 2, 48000, 0.0f};
        std::printf("[conformance] %s\n", opus.name);
        runBattery(opus);
        checkProbe(opus, "Opus");
    }
    {
        constexpr Fixture wma{"wma", ".wma", &createWma, Codec::Wma, DecoderKind::Ffmpeg,
                              Tier::Structural, 2, 44100, 0.0f};
        std::printf("[conformance] %s\n", wma.name);
        runBattery(wma);
        checkProbe(wma, "WMA");
    }
    {
        constexpr Fixture alac{"alac", ".m4a", &createAlac, Codec::Alac, DecoderKind::Ffmpeg,
                               Tier::Structural, 2, 44100, 0.0f};
        std::printf("[conformance] %s\n", alac.name);
        runBattery(alac);
        checkProbe(alac, "ALAC");
    }
    {
        // The four layouts FFmpeg declares differently from the default order
        // for their channel count, then two sources in the default order.
        using P = ChannelPosition;
        std::printf("[conformance] layouts\n");
        checkLayout("40", kAc3Layout40, sizeof(kAc3Layout40),
                    {P::FrontLeft, P::FrontRight, P::FrontCenter, P::BackCenter});
        checkLayout("quadside", kAc3LayoutQuadSide, sizeof(kAc3LayoutQuadSide),
                    {P::FrontLeft, P::FrontRight, P::SideLeft, P::SideRight});
        checkLayout("21", kAc3Layout21, sizeof(kAc3Layout21),
                    {P::FrontLeft, P::FrontRight, P::Lfe});
        checkLayout("51side", kAc3Layout51Side, sizeof(kAc3Layout51Side),
                    {P::FrontLeft, P::FrontRight, P::FrontCenter, P::Lfe,
                     P::SideLeft, P::SideRight});
        checkLayoutControl("vorbis3", 3, {P::FrontLeft, P::FrontRight, P::FrontCenter});
        checkLayoutControl("vorbis4", 4,
                           {P::FrontLeft, P::FrontRight, P::BackLeft, P::BackRight});
        // A custom order, delivered in ascending speaker order.
        checkFfmpegCustomOrder();
        // A layout change partway through one stream.
        checkLayoutChange();
    }
#else
    std::printf("[conformance] aac/ac3/dts/vorbis/opus/wma/alac/layouts SKIPPED: built "
                "without FFmpeg (RAWFORM_HAVE_FFMPEG=0)\n");
#endif
    // NOLINTEND(modernize-use-designated-initializers)

    if (g_failures == 0) {
        std::printf("ALL CONFORMANCE CHECKS PASSED\n");
    } else {
        std::fprintf(stderr, "%d CONFORMANCE CHECK(S) FAILED\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}
