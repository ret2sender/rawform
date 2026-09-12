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

// ReplayGainScannerTest.cpp
//
// The codec-free test for the ReplayGain scanner, in the spirit of EngineTest: a
// synthetic IDecoderFactory emits a known signal (a sine of a chosen amplitude, or
// digital silence) so the scanner can be exercised with no libsndfile, no FLAC, no
// MP3, and no audio hardware. libebur128 still runs (inside the library), so this
// is a real measurement, but the inputs are exact and reproducible.
//
// Loudness is a DSP result, so the assertions are STRUCTURAL (bounded deltas), not
// STRICT equality, deliberately. The strongest
// checks are level-relative and therefore independent of the K-weighting curve and
// the reference calibration: halving a track's amplitude must raise its gain by
// about 6.02 dB, and an album's combined gain must fall strictly between its
// loudest and quietest members'. A single generous absolute band guards against a
// sign flip or an order-of-magnitude scale error without over-fitting libebur128's
// exact numbers. The remaining checks pin the exact-fact corners: silence is
// unmeasurable and excluded from the album combine, a failed open is reported, and
// a cancel discards the in-flight track.

#include "rawform/audio/IDecoder.h"
#include "rawform/audio/IDecoderFactory.h"
#include "rawform/audio/ReplayGainScanner.h"
#include "rawform/audio/Types.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using rawform::audio::AudioFormat;
using rawform::audio::IDecoder;
using rawform::audio::IDecoderFactory;
using rawform::audio::ReplayGainScanner;
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

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// A synthetic decoder emitting a steady sine of a given amplitude and frequency
// into every channel, for a fixed number of frames, then end of stream. Amplitude
// 0 yields digital silence, which is how the "silence" spec is built. The sine is
// identical across channels, so a stereo source is two correlated channels (the
// realistic music-ish case for a loudness measurement, and the case for which the
// L and R channel weights of 1.0 sum cleanly).
class SineDecoder final : public IDecoder {
public:
    SineDecoder(std::uint32_t rate, std::uint16_t channels, double freqHz,
                double amplitude, std::uint64_t totalFrames)
        : m_fmt{rate, channels}, m_freq(freqHz), m_amp(amplitude),
          m_total(totalFrames) {}

    [[nodiscard]] AudioFormat   format() const override { return m_fmt; }
    [[nodiscard]] SourceInfo    sourceInfo() const override { return SourceInfo{}; }
    [[nodiscard]] std::uint64_t totalFrames() const override { return m_total; }
    [[nodiscard]] bool          seekable() const override { return true; }

    bool seek(std::uint64_t frame) override {
        m_pos = (frame > m_total) ? m_total : frame;
        return true;
    }

    std::size_t read(float* dst, std::size_t frames) override {
        const std::size_t ch = m_fmt.channels;
        const double      w  = 2.0 * kPi * m_freq /
                         static_cast<double>(m_fmt.sampleRate);
        std::size_t produced = 0;
        while (produced < frames && m_pos < m_total) {
            const double s = m_amp * std::sin(w * static_cast<double>(m_pos));
            for (std::size_t c = 0; c < ch; ++c)
                dst[produced * ch + c] = static_cast<float>(s);
            ++m_pos;
            ++produced;
        }
        return produced;
    }

private:
    AudioFormat   m_fmt;
    double        m_freq;
    double        m_amp;
    std::uint64_t m_total;
    std::uint64_t m_pos = 0;
};

// Split a colon-delimited spec, the same readable path-spec style EngineTest uses
// for its ramp factory.
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string              cur;
    for (const char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// The injected factory. Specs:
//   "sine:<rate>:<channels>:<freqHz>:<amplitude>:<frames>"  -> steady sine
//   "silence:<rate>:<channels>:<frames>"                    -> all zeros
//   "fail"                                                   -> open failure
class SyntheticFactory final : public IDecoderFactory {
public:
    std::unique_ptr<IDecoder> open(const std::string& path,
                                   std::string*       error) override {
        const std::vector<std::string> t = split(path, ':');
        if (!t.empty() && t[0] == "fail") {
            if (error)
                *error = "synthetic open failure";
            return nullptr;
        }
        if (t.size() == 6 && t[0] == "sine") {
            return std::make_unique<SineDecoder>(
                static_cast<std::uint32_t>(std::stoul(t[1])),
                static_cast<std::uint16_t>(std::stoul(t[2])), std::stod(t[3]),
                std::stod(t[4]), std::stoull(t[5]));
        }
        if (t.size() == 4 && t[0] == "silence") {
            return std::make_unique<SineDecoder>(
                static_cast<std::uint32_t>(std::stoul(t[1])),
                static_cast<std::uint16_t>(std::stoul(t[2])), 1000.0, 0.0,
                std::stoull(t[3]));
        }
        if (error)
            *error = "unrecognized synthetic spec: " + path;
        return nullptr;
    }
};

// A 1 kHz stereo sine at 44.1 kHz, three seconds long: comfortably more than the
// integrated-loudness gating window needs, fast to generate and measure.
std::string sineSpec(double amplitude) {
    return "sine:44100:2:1000:" + std::to_string(amplitude) + ":132300";
}

// The progress callback that never cancels.
bool noCancel(double) { return true; }

// ---------------------------------------------------------------------------

// A failed open is reported as OpenFailed with a reason, and nothing is retained.
void testOpenFailedReported() {
    SyntheticFactory  factory;
    ReplayGainScanner scanner(factory);

    ReplayGainScanner::TrackResult r;
    std::string                    err;
    const auto st = scanner.addTrack("fail", noCancel, &r, &err);

    CHECK(st == ReplayGainScanner::Status::OpenFailed);
    CHECK(!err.empty());

    ReplayGainScanner::AlbumResult album;
    CHECK(scanner.albumResult(&album) == false);
}

// The sample peak of a 0.5-amplitude sine is 0.5, and the track is measurable.
void testSamplePeak() {
    SyntheticFactory  factory;
    ReplayGainScanner scanner(factory);

    ReplayGainScanner::TrackResult r;
    std::string                    err;
    const auto st = scanner.addTrack(sineSpec(0.5), noCancel, &r, &err);

    CHECK(st == ReplayGainScanner::Status::Ok);
    CHECK(r.measurable == true);
    CHECK(std::fabs(r.peak - 0.5) < 1e-3);
}

// Halving the amplitude (exactly -6.02 dB quieter) must raise the track gain by
// about 6.02 dB. This is the K-weighting- and calibration-independent check.
void testLevelRelativeGain() {
    SyntheticFactory  factory;
    ReplayGainScanner scanner(factory);

    ReplayGainScanner::TrackResult loud;
    ReplayGainScanner::TrackResult quiet;
    std::string                    err;

    CHECK(scanner.addTrack(sineSpec(0.5), noCancel, &loud, &err) ==
          ReplayGainScanner::Status::Ok);
    CHECK(scanner.addTrack(sineSpec(0.25), noCancel, &quiet, &err) ==
          ReplayGainScanner::Status::Ok);

    CHECK(loud.measurable && quiet.measurable);
    const double delta = quiet.gainDb - loud.gainDb;
    CHECK(std::fabs(delta - 6.02) < 0.2);  // half level -> ~+6.02 dB gain
}

// A single absolute sanity band: a near-half-scale 1 kHz sine lands in a plausible
// gain range. Wide on purpose; it only has to catch a sign flip or a 10x error.
void testAbsoluteGainBand() {
    SyntheticFactory  factory;
    ReplayGainScanner scanner(factory);

    ReplayGainScanner::TrackResult r;
    std::string                    err;
    CHECK(scanner.addTrack(sineSpec(0.5), noCancel, &r, &err) ==
          ReplayGainScanner::Status::Ok);

    CHECK(r.measurable == true);
    CHECK(r.gainDb > -14.0 && r.gainDb < -8.0);
}

// An album of a loud and a moderately quieter track: combined gain falls strictly
// between the two members', and the album peak is the louder track's peak. The
// level gap is kept to 6 dB on purpose: BS.1770 integrated loudness applies a
// relative gate that drops blocks more than 10 LU below the surviving mean, so a
// much quieter track (say 20 dB down) would be gated out of the album integration
// entirely and the album gain would collapse onto the loud track's, which is
// correct ReplayGain behavior but not a "strictly between" case. At 6 dB both
// tracks stay inside the gate and genuinely combine.
void testAlbumCombineBetween() {
    SyntheticFactory  factory;
    ReplayGainScanner scanner(factory);

    ReplayGainScanner::TrackResult loud;
    ReplayGainScanner::TrackResult quiet;
    std::string                    err;

    CHECK(scanner.addTrack(sineSpec(0.5), noCancel, &loud, &err) ==
          ReplayGainScanner::Status::Ok);
    CHECK(scanner.addTrack(sineSpec(0.25), noCancel, &quiet, &err) ==
          ReplayGainScanner::Status::Ok);

    ReplayGainScanner::AlbumResult album;
    CHECK(scanner.albumResult(&album) == true);
    CHECK(album.measurable == true);

    // The loud track is louder, so it carries the lower (more negative) gain; the
    // album sits between, and its peak is the loud track's 0.5.
    CHECK(album.gainDb > loud.gainDb);
    CHECK(album.gainDb < quiet.gainDb);
    CHECK(std::fabs(album.peak - 0.5) < 1e-3);
}

// Digital silence is unmeasurable: Ok status, measurable false, zero gain and
// peak, and it forms no album on its own.
void testSilenceUnmeasurable() {
    SyntheticFactory  factory;
    ReplayGainScanner scanner(factory);

    ReplayGainScanner::TrackResult r;
    std::string                    err;
    const auto st = scanner.addTrack("silence:44100:2:132300", noCancel, &r, &err);

    CHECK(st == ReplayGainScanner::Status::Ok);
    CHECK(r.measurable == false);
    CHECK(r.gainDb == 0.0);
    CHECK(r.peak == 0.0);

    ReplayGainScanner::AlbumResult album;
    CHECK(scanner.albumResult(&album) == false);  // no measurable track
}

// A silent track mixed into an album is excluded from the loudness combine, so the
// album gain matches scanning the lone real track on its own.
void testSilenceExcludedFromAlbum() {
    SyntheticFactory factory;

    // The real track's gain, measured alone.
    ReplayGainScanner              solo(factory);
    ReplayGainScanner::TrackResult soloTrack;
    std::string                    err;
    CHECK(solo.addTrack(sineSpec(0.5), noCancel, &soloTrack, &err) ==
          ReplayGainScanner::Status::Ok);

    // The same real track plus a silent one, combined as an album.
    ReplayGainScanner mixed(factory);
    ReplayGainScanner::TrackResult discard;
    CHECK(mixed.addTrack("silence:44100:2:132300", noCancel, &discard, &err) ==
          ReplayGainScanner::Status::Ok);
    CHECK(mixed.addTrack(sineSpec(0.5), noCancel, &discard, &err) ==
          ReplayGainScanner::Status::Ok);

    ReplayGainScanner::AlbumResult album;
    CHECK(mixed.albumResult(&album) == true);
    CHECK(album.measurable == true);
    CHECK(std::fabs(album.gainDb - soloTrack.gainDb) < 0.05);
}

// A cancel during decode returns Canceled and retains nothing; a later normal scan
// on the same instance still works and is retained.
void testCancelDiscardsTrack() {
    SyntheticFactory  factory;
    ReplayGainScanner scanner(factory);

    int        calls    = 0;
    const auto cancelAt = [&calls](double) -> bool {
        ++calls;
        return calls < 2;  // allow the first chunk, abort on the second
    };

    ReplayGainScanner::TrackResult r;
    std::string                    err;
    const auto st = scanner.addTrack(sineSpec(0.5), cancelAt, &r, &err);
    CHECK(st == ReplayGainScanner::Status::Canceled);

    // Nothing retained, so no album yet.
    ReplayGainScanner::AlbumResult album;
    CHECK(scanner.albumResult(&album) == false);

    // The scanner is still usable for a real scan afterward.
    ReplayGainScanner::TrackResult good;
    CHECK(scanner.addTrack(sineSpec(0.5), noCancel, &good, &err) ==
          ReplayGainScanner::Status::Ok);
    CHECK(good.measurable == true);
    CHECK(scanner.albumResult(&album) == true);
}

// reset clears retained tracks so a reused scanner starts a fresh album.
void testResetClearsBatch() {
    SyntheticFactory  factory;
    ReplayGainScanner scanner(factory);

    ReplayGainScanner::TrackResult r;
    std::string                    err;
    CHECK(scanner.addTrack(sineSpec(0.5), noCancel, &r, &err) ==
          ReplayGainScanner::Status::Ok);

    ReplayGainScanner::AlbumResult album;
    CHECK(scanner.albumResult(&album) == true);

    scanner.reset();
    CHECK(scanner.albumResult(&album) == false);  // empty again
}

}  // namespace

int main() {
    testOpenFailedReported();
    testSamplePeak();
    testLevelRelativeGain();
    testAbsoluteGainBand();
    testAlbumCombineBetween();
    testSilenceUnmeasurable();
    testSilenceExcludedFromAlbum();
    testCancelDiscardsTrack();
    testResetClearsBatch();

    if (g_failures == 0) {
        std::puts("ReplayGainScannerTest: all checks passed");
        return 0;
    }
    std::fprintf(stderr, "ReplayGainScannerTest: %d check(s) failed\n",
                 g_failures);
    return 1;
}
