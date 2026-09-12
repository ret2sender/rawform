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

// ReplayGainScanner.h
//
// The loudness-measurement half of the ReplayGain story: the UI reads, applies,
// edits, writes, and clears the REPLAYGAIN_* tags that live in a file; this
// class generates those numbers from the audio itself. It decodes a track to
// end of stream through the engine's existing decoder abstraction, feeds the
// interleaved float32 to libebur128 (the standard ITU-R BS.1770
// implementation), and reports the track's integrated loudness, the gain that
// brings it to the reference target, and its sample peak. It can also combine
// every scanned track of one album into a single album loudness and peak.
//
// Layering. This is a rawform_audio engine library, the same tier as the decoders
// and the Engine: it sees IDecoderFactory and the shared value types and nothing
// above them. Crucially it does NOT see a Qt header and, like every decoder, it keeps
// its third-party library header (<ebur128.h>) private to the .cpp. That is enforced
// here with a pimpl: libebur128 typedefs an anonymous struct for its state, which
// cannot be forward-declared, so the only way to keep the include out of this header
// is to hide the state vector behind an opaque Impl. The thin Qt controller that
// drives this off the GUI thread, the progress dialog, and the menu wiring all live
// one layer up in the UI.
//
// Decoder injection. The factory is injected by reference exactly like the
// Engine's, for the same two reasons: production passes a real DecoderFactory so
// every format the player can play is also scannable, and the headless test
// passes a synthetic factory that emits a known signal so the computed gain can be
// asserted against a tolerance with no codec dependency. The reference is
// non-owning and must outlive the scanner.
//
// Reference level and the gain formula live HERE, not in the controller, by
// design: the -18 LUFS reference is a named constant on this class and the
// gain is computed as referenceLufs minus the measured loudness, so the gain
// math is what the headless test exercises and the controller upstream only
// has to format the resulting number. -18 LUFS is the ReplayGain 2.0 default;
// it is deliberately NOT the EBU R128 broadcast target of -23 LUFS, because the
// numbers feed the dB-string REPLAYGAIN_* tags the UI writes, never the Q7.8
// R128_* tags.
//
// Threading. A scanner instance is single-threaded by contract: the controller
// runs the whole batch on one worker (a single QtConcurrent::run task that walks
// the tracks in order), so addTrack, albumResult, and reset are never called
// concurrently and the class needs no locking of its own. The progress callback
// is invoked on that same worker thread.

#pragma once

#include "rawform/audio/IDecoderFactory.h"

#include <functional>
#include <memory>
#include <string>

namespace rawform::audio {

class ReplayGainScanner {
public:
    /// The ReplayGain 2.0 reference target in LUFS. A track measured at exactly
    /// this loudness gets a 0 dB gain; quieter tracks get a positive (boost) gain,
    /// louder tracks a negative (attenuate) one. Named here so the one place the
    /// reference is chosen is also the place the gain is computed.
    static constexpr double kReferenceLufs = -18.0;

    /// Loudness floor. libebur128 returns negative infinity (or a value far below
    /// any real program level) for digital silence or a clip too short to fill the
    /// gating window. Anything at or below this floor is reported as unmeasurable
    /// rather than turned into a nonsensical gain like referenceLufs minus minus
    /// infinity. -70 LUFS sits well under the quietest real music and above the
    /// library's silence sentinel.
    static constexpr double kLoudnessFloorLufs = -70.0;

    /// One track's measured result. gainDb and integratedLufs are meaningful only
    /// when measurable is true; for an unmeasurable (silent or too-short) track
    /// gainDb is 0 and integratedLufs is 0, while peak still carries the real
    /// sample peak (which for true silence is 0).
    struct TrackResult {
        double gainDb         = 0.0;  ///< referenceLufs - integratedLufs, in dB
        double peak           = 0.0;  ///< linear sample peak, max abs over channels
        double integratedLufs = 0.0;  ///< raw BS.1770 integrated loudness, for logs
        bool   measurable     = false;
    };

    /// The whole album's measured result, combined across every measurable track
    /// scanned so far. peak is the maximum sample peak over all tracks (measurable
    /// or not). Valid only when albumResult returned true.
    struct AlbumResult {
        double gainDb         = 0.0;
        double peak           = 0.0;
        double integratedLufs = 0.0;
        bool   measurable     = true;  ///< always true when albumResult returns true
    };

    /// Per-file outcome. OpenFailed means the factory could not open the path (the
    /// error string carries the reason); Canceled means the progress callback asked
    /// to abort mid-decode, in which case nothing is retained for the album combine.
    enum class Status { Ok, OpenFailed, Canceled };

    /// Progress + cancel hook, called periodically during a track's decode with a
    /// 0..1 fraction of that track (0 when the source length is unknown). Returning
    /// false aborts the decode; addTrack then returns Canceled and discards the
    /// in-flight partial measurement. The callback should be cheap and self-throttle
    /// if it crosses a thread boundary, since it fires once per decode chunk.
    using ProgressFn = std::function<bool(double fraction)>;

    explicit ReplayGainScanner(IDecoderFactory& factory);
    ~ReplayGainScanner();

    ReplayGainScanner(const ReplayGainScanner&)            = delete;
    ReplayGainScanner& operator=(const ReplayGainScanner&) = delete;

    /// Open @p path through the injected factory, decode it to end of stream while
    /// feeding libebur128, and write the per-track result to @p out. On success the
    /// track's loudness state is retained internally so a later albumResult can
    /// combine it. On OpenFailed @p error (when non-null) carries the reason; on
    /// Canceled nothing is retained and @p out is left unmodified.
    Status addTrack(const std::string& path, const ProgressFn& onProgress,
                    TrackResult* out, std::string* error);

    /// Combine every measurable track added so far into one album loudness and one
    /// album peak, writing them to @p out. Returns false when no measurable track
    /// has been added (an all-silence or empty batch), in which case @p out is left
    /// untouched and the caller writes no album value. Unmeasurable tracks are
    /// excluded from the loudness combine but their (zero) peaks are harmless to the
    /// peak maximum.
    [[nodiscard]] bool albumResult(AlbumResult* out) const;

    /// Drop every retained track state and reset the album peak, so one scanner can
    /// be reused for a fresh batch. The destructor frees the states regardless.
    void reset();

private:
    struct Impl;                 ///< owns the libebur128 states; defined in the .cpp
    IDecoderFactory&      m_factory;  ///< non-owning, must outlive this scanner
    std::unique_ptr<Impl> m_impl;
};

}  // namespace rawform::audio
