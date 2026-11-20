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
// speaker-layout check on four multichannel AC3 blobs (checkLayout), compile in
// only when the library was built with FFmpeg (RAWFORM_HAVE_FFMPEG); without it they
// are skipped with a single notice and the suite still runs the three native
// decoders. The blobs live in tests/fixtures/<Codec>Blob.h, included below;
// each header records the ffmpeg command that produced it, except Mp3Blob.h,
// whose command was not kept. A probe battery (checkProbe) runs beside the
// decode battery on the same fixtures, so MetadataProbe's read of each file is
// held to the decoder's facts.

#include "ChannelPositions.h"
#include "DecoderFactory.h"

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
#include <cmath>
#include <cstddef>
#include <cstdint>  // IWYU pragma: keep (std::int*_t)
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
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

// Float32 WAV via libsndfile, carrying the ramp. The float subtype keeps the
// round trip exact (SndfileDecoder reads it back with SFC_SET_NORM_FLOAT), so the
// WAV fixture is the strictest possible case.
// Writes a known float ramp through libsndfile in the requested major+subtype
// format. WAV/FLOAT yields a lossless fixture (strict tier); the Ogg subtypes
// (Vorbis, Opus) yield lossy fixtures whose exact samples do not survive, so they
// are checked structurally. libsndfile picks the format from SF_INFO, not the
// path extension, so the caller's file name only needs a plausible suffix.
bool writeSndfileFloat(const std::string& path, int channels, int rate,
                       std::uint64_t frames, int sfFormat) {
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
                    pat((written + static_cast<std::uint64_t>(f)) * channels + c);
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
// count's default order; beyond eight channels there is nothing to declare.
bool positionsAre(const AudioFormat& f, std::initializer_list<ChannelPosition> want) {
    return std::ranges::equal(rawform::audio::channelPositionsFor(f), want);
}

void checkChannelPositions() {
    using P = ChannelPosition;
    using rawform::audio::channelPositionsFor;
    using rawform::audio::defaultChannelMask;
    using rawform::audio::sourceChannelMask;
    const auto fmt = [](std::uint16_t channels, std::uint16_t mask) {
        return AudioFormat{
            .sampleRate = 48000, .channels = channels, .channelMask = mask};
    };

    // Default orders.
    CHECK(positionsAre(fmt(1, 0), {P::Mono}));
    CHECK(positionsAre(fmt(6, 0), {P::FrontLeft, P::FrontRight, P::FrontCenter,
                                   P::Lfe, P::BackLeft, P::BackRight}));

    // Masks: 4.0 (FL FR FC BC), and 5.1 with side surrounds.
    CHECK(positionsAre(fmt(4, 0x0107), {P::FrontLeft, P::FrontRight,
                                        P::FrontCenter, P::BackCenter}));
    CHECK(positionsAre(fmt(6, 0x060F), {P::FrontLeft, P::FrontRight, P::FrontCenter,
                                        P::Lfe, P::SideLeft, P::SideRight}));

    // A mask equal to the default order declares what 0 declares.
    CHECK(positionsAre(fmt(6, 0x003F), {P::FrontLeft, P::FrontRight, P::FrontCenter,
                                        P::Lfe, P::BackLeft, P::BackRight}));

    // Ignored masks: four bits on three channels, and front-left-of-center
    // (0x40), which the table cannot name.
    CHECK(positionsAre(fmt(3, 0x0107), {P::FrontLeft, P::FrontRight, P::FrontCenter}));
    CHECK(positionsAre(fmt(3, 0x0043), {P::FrontLeft, P::FrontRight, P::FrontCenter}));

    // Nothing to declare.
    CHECK(channelPositionsFor(fmt(0, 0)).empty());
    CHECK(channelPositionsFor(fmt(9, 0)).empty());

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
    // anything the table cannot declare (an unnamed speaker, a bit count that
    // disagrees with the channel count, mono, more than eight channels).
    CHECK(sourceChannelMask(0x0107, 4) == 0x0107);
    CHECK(sourceChannelMask(0x0033, 4) == 0);
    CHECK(sourceChannelMask(0x0043, 3) == 0);
    CHECK(sourceChannelMask(0x0107, 3) == 0);
    CHECK(sourceChannelMask(0x0004, 1) == 0);
    CHECK(sourceChannelMask(0x073F, 9) == 0);
}

// ---------------------------------------------------------------------------
// Speaker layouts end to end: what each decoded channel holds is what a sink is
// told it is. Each layout fixture carries one tone per source channel, placed by
// speaker: the AC3 streams in fixtures/Ac3LayoutBlob.h, and the FLAC files
// written below. checkDecodedLayout opens one through the factory, takes the
// positions a sink would declare for the decoded format, and listens to every
// decoded channel: the strongest of the nine tones below must be the tone of the
// position declared for that channel.

// The tone each position carries in the fixtures. All nine are listened for, so
// a channel declared at the wrong position hears the wrong tone.
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

// The layout check proper, on a fixture already on disk, which it removes. A
// mask, when given, is also asserted, for the rows whose point is the mask the
// decoder reports.
void checkDecodedLayout(const char* name, const std::string& path,
                        DecoderKind decoder, std::optional<std::uint16_t> mask,
                        std::initializer_list<ChannelPosition> expected) {
    std::unique_ptr<IDecoder> dec = openLayoutFixture(path);
    CHECK(dec != nullptr);
    if (!dec) {
        std::remove(path.c_str());
        return;
    }

    const AudioFormat fmt = dec->format();
    CHECK(dec->sourceInfo().decoder == decoder);
    CHECK(static_cast<std::size_t>(fmt.channels) == expected.size());
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
    for (std::size_t c = 0; c < ch && c < declared.size(); ++c) {
        std::vector<float> x;
        for (std::size_t f = frames / 4; f < (3 * frames) / 4; ++f) {
            x.push_back(all[f * ch + c]);
        }
        const ChannelPosition heard = loudestPosition(x, fmt.sampleRate);
        if (heard != declared[c]) {
            std::fprintf(stderr, "  %s channel %zu: declared %s, holds the %s tone\n",
                         name, c, positionName(declared[c]), positionName(heard));
        }
        CHECK(heard == declared[c]);
    }

    dec.reset();
    std::remove(path.c_str());
}

// A 16-bit 48 kHz FLAC layout fixture, half a second of one tone per channel,
// for the positions listed in order, carrying comment ("NAME=value") unless it
// is null. The decoder must report mask and declare the listed positions, each
// channel holding its own tone.
void checkFlacLayout(const char* name, Container container, const char* comment,
                     std::uint16_t mask,
                     std::initializer_list<ChannelPosition> positions) {
    const std::string path = std::string("rawform_layout_") + name +
                             (container == Container::Ogg ? ".oga" : ".flac");
    constexpr int       rate = 48000;
    std::vector<double> hz;
    for (const ChannelPosition p : positions) {
        hz.push_back(toneHz(p));
    }
    MetadataPtr comments = comment != nullptr
                               ? commentBlock(comment)
                               : MetadataPtr(nullptr, &FLAC__metadata_object_delete);
    CHECK(comment == nullptr || comments != nullptr);
    CHECK(encodeFlac(path, static_cast<int>(hz.size()), rate, 16, 24000, container,
                     comments.get(), [&hz](std::uint64_t frame, int c) {
                         const double t = static_cast<double>(frame) / rate;
                         return static_cast<float>(
                             0.25 * std::sin(2.0 * std::numbers::pi *
                                             hz[static_cast<std::size_t>(c)] * t));
                     }));
    checkDecodedLayout(name, path, DecoderKind::Flac, mask, positions);
}

#if RAWFORM_HAVE_FFMPEG
void checkLayout(const char* name, const unsigned char* bytes, std::size_t size,
                 std::initializer_list<ChannelPosition> expected) {
    const std::string path = std::string("rawform_layout_") + name + ".ac3";
    CHECK(writeBytes(path, bytes, size));
    checkDecodedLayout(name, path, DecoderKind::Ffmpeg, std::nullopt, expected);
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
        // natively and in Ogg, with the field name in either case. Then files
        // whose comment, or its absence, leaves the default order for the count:
        // the default mask itself, a speaker the table cannot name, a bit count
        // that disagrees with the channel count, and a value not written in hex.
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
                        "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=0x0043", 0, p30);
        checkFlacLayout("flac_40_mask_on_3", Container::Native, k40, 0, p30);
        checkFlacLayout("flac_decimal_mask", Container::Native,
                        "WAVEFORMATEXTENSIBLE_CHANNEL_MASK=263", 0, quad);
        checkFlacLayout("flac_no_mask", Container::Native, nullptr, 0, quad);
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
