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

// cli/main.cpp
//
// Headless harness for the rawform audio engine. Five modes:
//
//   rawform_audio_cli <file> [--dump <out.raw>]
//       The info/hash path: open one file, print format and the descriptive
//       facts, decode the whole stream to interleaved float32, print a stable
//       64-bit FNV-1a hash of the raw decoded bytes, and optionally write the
//       float32 PCM to a file. The hash is the regression hook: a given input
//       decodes to the same hash on a given machine, no reference file needed.
//
//   rawform_audio_cli play [--resample|--force-rate] [--no-gapless] <file>
//       The transport one-shot path: hand the file to a real Engine (its engine
//       thread owns the decoder and ring, its command queue drives transport, the
//       sink runs the RT pull), play to end of stream, then let the Engine's
//       destructor tear everything down in the order the lifetime invariant
//       demands. Available where a platform sink is compiled in (CoreAudio on
//       macOS, PipeWire on Linux); elsewhere it reports that no sink exists.
//
//   rawform_audio_cli interactive [--resample|--force-rate] [--no-gapless]
//       The transport REPL: one long-lived Engine, driven by typed commands.
//       Transport: play [file], pause, stop, seek <m:ss|seconds>, pos, and the
//       relative nudges > < >> <<. Playlist (a CLI-side cursor over an engine
//       queue): enqueue|add <file...>, next, previous|prev, first, last,
//       list|pl, clear. Devices: devices, device <n|default>. quit|exit or
//       Ctrl-C tears down cleanly. This is the manual harness for the transport
//       contract: instant pause/resume with no flush, stop that releases the
//       device, queue advance and clear, seek with a live m:ss / m:ss readout,
//       and output device selection with the track resuming where it was.
//
//   rawform_audio_cli rates [--device <id|n>] [--resample|--force-rate] [file]
//       The rate-policy diagnostic: print what an output device advertises and,
//       given a file, the RateDecision the engine would make for it under the
//       chosen mode, without opening the device.
//
//   rawform_audio_cli probe <file>
//       The metadata-probe diagnostic: what MetadataProbe reads for one file
//       (codec, rate, duration, tags), with no sink and no decode.
//
// Exit status: 0 on success; 2 on a usage error; 1 on an open or I/O failure.

#include "rawform/audio/Engine.h"
#include "rawform/audio/IDecoder.h"
#include "rawform/audio/MetadataProbe.h"
#include "rawform/audio/RateManager.h"
#include "rawform/audio/Types.h"

#include "DecoderFactory.h"

#if RAWFORM_HAVE_COREAUDIO || RAWFORM_HAVE_PIPEWIRE
#if RAWFORM_HAVE_COREAUDIO
#include "sinks/CoreAudioSink.h"
#else
#include "sinks/PipeWireSink.h"
#endif
#include <atomic>
#include <chrono>
#include <signal.h>  // NOLINT: sigaction/SA_RESTART are POSIX, not in <csignal>
#include <iostream>
#include <mutex>
#include <thread>
#endif

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>  // std::move (startAt's setQueue handoff)
#include <vector>

namespace {

// How the play/interactive subcommands should reconcile the file rate with the
// device. Kept platform-independent (defined unconditionally) so argument
// parsing in main() does not depend on whether a sink is compiled in; the play
// path maps it to the sink's own RateMode where one exists.
enum class PlayRateFlag {
    Default,    // bit-perfect when the device supports the rate, else resample
    Resample,   // never switch the device; always resample
    ForceRate,  // switch the device regardless of advertised rates (diagnostic)
};

using rawform::audio::AudioFormat;
using rawform::audio::Codec;
using rawform::audio::DecoderFactory;
using rawform::audio::DecoderKind;
using rawform::audio::IDecoder;
using rawform::audio::SourceInfo;

// 64-bit FNV-1a with the standard offset basis and prime (Fowler, Noll, Vo).
// No crypto dependency, deterministic, and good enough to flag any change in the
// decoded stream. The two tests that hash decoded audio (Mp3DecoderTest,
// ConformanceTest) carry the same constants; each hash is compared only
// against hashes from the same process, so a shared header would buy nothing.
constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime  = 0x100000001b3ULL;

void fnv1aUpdate(std::uint64_t& h, const void* data, std::size_t bytes) noexcept {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

const char* codecName(Codec c) noexcept {
    switch (c) {
        case Codec::Pcm:     return "PCM";
        case Codec::Flac:    return "FLAC";
        case Codec::Mp3:     return "MP3";
        case Codec::Aac:     return "AAC";
        case Codec::Ac3:     return "AC3";
        case Codec::Dts:     return "DTS";
        case Codec::Opus:    return "Opus";
        case Codec::Vorbis:  return "Vorbis";
        case Codec::Alac:    return "ALAC";
        case Codec::Wma:     return "WMA";
        case Codec::Other:   return "Other";
        case Codec::Unknown: break;
    }
    return "Unknown";
}

// Display name for which decoder handled the source. Exhaustive over DecoderKind
// so it stays -Wswitch clean as the enum grows.
const char* decoderKindName(DecoderKind d) noexcept {
    switch (d) {
        case DecoderKind::Sndfile: return "sndfile";
        case DecoderKind::Flac:    return "libFLAC";
        case DecoderKind::Mpg123:  return "mpg123";
        case DecoderKind::Ffmpeg:  return "FFmpeg";
        case DecoderKind::Unknown: break;
    }
    return "unknown";
}

// Formats a frame count and sample rate as m:ss for the status line.
std::string durationString(std::uint64_t frames, std::uint32_t sampleRate) {
    if (sampleRate == 0 || frames == 0) {
        return "0:00";
    }
    const std::uint64_t totalSeconds = frames / sampleRate;
    const std::uint64_t minutes = totalSeconds / 60;
    const std::uint64_t seconds = totalSeconds % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                  static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(seconds));
    return buf;
}

// Formats a position in seconds as m:ss for the live readout. Guards NaN and a
// negative value to a clean 0:00.
std::string secondsToMMSS(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {  // NaN, inf, or negative
        seconds = 0.0;
    }
    const auto total   = static_cast<std::uint64_t>(seconds);
    const std::uint64_t minutes = total / 60;
    const std::uint64_t secs    = total % 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                  static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(secs));
    return buf;
}

// Parses a seek time as either "m:ss" (minutes and seconds, each possibly
// fractional) or a raw seconds value. Returns false on a malformed input.
bool parseTime(const std::string& s, double& out) {
    if (s.empty()) {
        return false;
    }
    const std::size_t colon = s.find(':');
    if (colon == std::string::npos) {
        char*        end = nullptr;
        const double v   = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) {
            return false;  // no digits consumed
        }
        out = v;
        return true;
    }
    const std::string mm = s.substr(0, colon);
    const std::string ss = s.substr(colon + 1);
    char* e1 = nullptr;
    char* e2 = nullptr;
    const double minutes = std::strtod(mm.c_str(), &e1);
    const double secs    = std::strtod(ss.c_str(), &e2);
    if (e1 == mm.c_str() || e2 == ss.c_str()) {
        return false;
    }
    out = minutes * 60.0 + secs;
    return true;
}

// Shared header block for the info/hash path. Reads only the decoder's cached facts, so
// it is cheap and side-effect free.
void printSourceInfo(const std::string& path, const IDecoder& dec) {
    const AudioFormat   fmt    = dec.format();
    const SourceInfo    si     = dec.sourceInfo();
    const std::uint64_t frames = dec.totalFrames();

    std::printf("File:        %s\n", path.c_str());
    std::printf("Codec:       %s\n", codecName(si.codec));
    std::printf("Sample rate: %u Hz\n", fmt.sampleRate);
    std::printf("Channels:    %u\n", static_cast<unsigned>(fmt.channels));
    std::printf("Bit depth:   %u bits\n", static_cast<unsigned>(si.bitsPerSample));
    std::printf("Bitrate:     %u kbps\n", si.bitrateKbps);
    std::printf("Frames:      %llu\n", static_cast<unsigned long long>(frames));
    std::printf("Duration:    %s\n", durationString(frames, fmt.sampleRate).c_str());
    std::printf("Seekable:    %s\n", dec.seekable() ? "yes" : "no");
}

void printUsage(const char* argv0) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  %s <file> [--dump <out.raw>]        decode, hash, optional PCM dump\n"
                 "  %s play [--resample|--force-rate] <file>\n"
                 "                                      play one file through the sink\n"
                 "  %s interactive [--resample|--force-rate]\n"
                 "                                      transport REPL (play/pause/stop/seek/...)\n"
                 "  %s rates [--device <id|n>] [--resample|--force-rate] [file]\n"
                 "                                      print an output device's rates, and\n"
                 "                                      the rate decision for a file if given\n"
                 "  %s probe <file>                     read metadata via the engine's\n"
                 "                                      FFmpeg probe (codec, rate, tags)\n"
                 "\n"
                 "  play (default)   bit-perfect when the device supports the file's\n"
                 "                   rate, otherwise resample to the device\n"
                 "  --resample       never switch the device; always resample\n"
                 "  --force-rate     switch the device regardless of advertised rates\n"
                 "                   (diagnostic; may produce garbled output)\n"
                 "  --no-gapless     disable cross-track gapless playback (drain and\n"
                 "                   reconfigure at every track boundary; for A/B)\n"
                 "  --device <id|n>  (rates only) report the named device instead of the\n"
                 "                   default: a persistent device id, or a 1-based index\n"
                 "                   into the enumeration (the same numbering the\n"
                 "                   interactive `devices` listing prints)\n",
                 argv0, argv0, argv0, argv0, argv0);
}

// ---------------------------------------------------------------------------
// Info/hash/dump path: decode fully, hash, optional PCM dump. Returns a process
// exit code. This opens through the format-dispatching DecoderFactory
// rather than SndfileDecoder directly, so it is codec-agnostic: WAV/AIFF, FLAC,
// and MP3 all report the correct codec name, bit depth, bitrate, and duration
// through the same surface. The play/interactive paths get this for free because
// they go through the Engine, whose default opener is the same factory.
int runInfo(const std::string& inputPath, const std::string& dumpPath) {
    std::string    err;
    DecoderFactory factory;
    std::unique_ptr<IDecoder> dec = factory.open(inputPath, &err);
    if (!dec) {
        std::fprintf(stderr, "error: could not open '%s': %s\n", inputPath.c_str(),
                     err.c_str());
        return 1;
    }

    printSourceInfo(inputPath, *dec);

    std::FILE* dumpFile = nullptr;
    if (!dumpPath.empty()) {
        dumpFile = std::fopen(dumpPath.c_str(), "wb");
        if (!dumpFile) {
            std::fprintf(stderr, "error: could not open dump file '%s'\n",
                         dumpPath.c_str());
            return 1;
        }
    }

    const std::size_t ch              = dec->format().channels;
    constexpr std::size_t blockFrames = 4096;
    std::vector<float> block(blockFrames * ch);

    std::uint64_t hash          = kFnvOffset;
    std::uint64_t framesDecoded = 0;
    bool          ioError       = false;
    for (;;) {
        const std::size_t got = dec->read(block.data(), blockFrames);
        if (got == 0) {
            break;
        }
        const std::size_t floats = got * ch;
        const std::size_t bytes  = floats * sizeof(float);
        fnv1aUpdate(hash, block.data(), bytes);
        framesDecoded += got;

        if (dumpFile) {
            if (std::fwrite(block.data(), 1, bytes, dumpFile) != bytes) {
                std::fprintf(stderr, "error: short write to dump file\n");
                ioError = true;
                break;
            }
        }
        if (got < blockFrames) {
            break;  // short read == end of stream
        }
    }

    if (dumpFile) {
        std::fclose(dumpFile);
    }
    if (ioError) {
        return 1;
    }

    std::printf("Decoded:     %llu frames (%llu samples)\n",
                static_cast<unsigned long long>(framesDecoded),
                static_cast<unsigned long long>(framesDecoded * ch));  // NOLINT(bugprone-misplaced-widening-cast)
    std::printf("FNV-1a64:    0x%016llx\n",
                static_cast<unsigned long long>(hash));
    if (!dumpPath.empty()) {
        std::printf("Dumped float32 PCM to %s (%llu bytes)\n", dumpPath.c_str(),
                    static_cast<unsigned long long>(framesDecoded * ch *  // NOLINT(bugprone-misplaced-widening-cast)
                                                    sizeof(float)));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The "probe" subcommand: the engine's FFmpeg metadata reader (MetadataProbe),
// surfaced as a diagnostic. Unlike runInfo it decodes nothing; it reports exactly
// what the read-side fallback sees for a file, which is the same data the UI
// scanner consumes for AC3/DTS and the rest of the long tail. Pure engine, no
// sink, no real-time thread. On a native-only build the probe declines with a
// clear message (the symbol still links, so this path needs no #if of its own).
int runProbe(const std::string& path) {
    std::string err;
    const auto  md = rawform::audio::probeAudioMetadata(path, &err);
    if (!md) {
        std::fprintf(stderr, "error: could not probe '%s': %s\n", path.c_str(),
                     err.empty() ? "unknown reason" : err.c_str());
        return 1;
    }

    // Duration is reported in milliseconds; convert to frames for the shared
    // m:ss formatter so the line matches the rest of the CLI. Guard a zero rate.
    const std::uint64_t frames =
        md->sampleRate > 0
            ? static_cast<std::uint64_t>(md->durationMs) *
                  md->sampleRate / 1000ULL
            : 0;

    std::printf("File:        %s\n", path.c_str());
    std::printf("Codec:       %s\n",
                md->codecName.empty() ? "(unknown)" : md->codecName.c_str());
    std::printf("Sample rate: %u Hz\n", md->sampleRate);
    std::printf("Channels:    %u\n", static_cast<unsigned>(md->channels));
    std::printf("Bit depth:   %u bits\n",
                static_cast<unsigned>(md->bitsPerSample));
    std::printf("Bitrate:     %u kbps\n", md->bitrateKbps);
    std::printf("Duration:    %s\n", durationString(frames, md->sampleRate).c_str());

    // Tags verbatim in libavformat's native key spelling, the same bytes the UI
    // then maps onto its promoted fields. Most raw .ac3 / .dts carry none.
    if (md->tags.empty()) {
        std::printf("Tags:        (none)\n");
    } else {
        std::printf("Tags:\n");
        for (const auto& kv : md->tags) {
            std::printf("  %s = %s\n", kv.first.c_str(), kv.second.c_str());
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Transport paths: play and interactive, both driving a real Engine.
#if RAWFORM_HAVE_COREAUDIO || RAWFORM_HAVE_PIPEWIRE

// The one platform seam in this file. The transport code below is sink-
// agnostic: it constructs a sink, hands ownership to the engine, and reads the
// two measured facts both platform sinks expose identically (currentFormat and
// bitPerfect). One alias therefore covers every construction site, and the
// capability defines are mutually exclusive by construction (CoreAudio is
// Apple-only, PipeWire is non-Apple UNIX), so exactly one branch exists.
#if RAWFORM_HAVE_COREAUDIO
using PlatformSink = rawform::audio::CoreAudioSink;
#else
using PlatformSink = rawform::audio::PipeWireSink;
#endif

using rawform::audio::AudioDeviceInfo; // RunRates' --device resolution
using rawform::audio::Engine;
using rawform::audio::RateDecision;
using rawform::audio::RateManager;
using rawform::audio::RateMode;
using rawform::audio::RateRange;
using rawform::audio::SinkCapabilities;
using rawform::audio::State;
using rawform::audio::TrackInfo;

// Set by SIGINT/SIGTERM, polled by the loops. The handler does the only
// async-signal-safe thing (a flag write); the device teardown and rate restore
// happen on the main thread, through the Engine's destructor, when the loop
// notices the flag. That is what makes Ctrl-C restore the device rate rather
// than leaving it stuck.
std::atomic<bool> g_interrupted{false};

void onInterrupt(int /*sig*/) {
    g_interrupted.store(true, std::memory_order_relaxed);
}

// Install the handlers with sigaction and SA_RESTART deliberately CLEARED,
// not with std::signal: glibc's signal() installs BSD semantics (SA_RESTART
// set), which transparently restarts the terminal read the REPL blocks in, so
// on Linux Ctrl-C only ever echoed and the flag was never polled (an early finding).
// The Mac exiting cleanly was its libc's differing legacy behavior, not
// something the code had actually arranged. Clearing SA_RESTART makes the
// interrupted read fail with EINTR on both platforms by construction, which is
// the contract the REPL's "getline returns failing" exit path was written
// against. runPlay's poll loop never depended on this, but installs the same
// way for one set of semantics everywhere.
void installInterruptHandlers() {
    struct sigaction sa{};
    sa.sa_handler = &onInterrupt;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // deliberately no SA_RESTART; see above
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// ---------------------------------------------------------------------------
// Interactive-mode playlist controller (CLI-only). The engine is deliberately a
// forward-queue transport with a single remembered track, so a PERSISTENT
// playlist and its cursor are a controller concern; they live here, exactly
// where a UI controller would own them. `items` is the ordered list the user
// builds up (kept across playback, unlike the engine's self-consuming queue) and
// `cursor` is the index the engine is currently playing. The cursor is advanced
// authoritatively by onTrackChanged, which fires once as each track starts
// including across a gapless auto-advance, and set optimistically by a navigation
// command. Every field is mutex-guarded because onTrackChanged runs on the engine
// thread while the REPL runs on the main thread.
struct PlaylistState {
    mutable std::mutex       mtx;
    std::vector<std::string> items;
    int                      cursor = -1;  // index the engine is playing; -1 == none yet

    void append(const std::string& p) {
        std::lock_guard<std::mutex> lock(mtx);
        items.push_back(p);
    }
    void clearAll() {
        std::lock_guard<std::mutex> lock(mtx);
        items.clear();
        cursor = -1;
    }
    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return items.size();
    }
    int cursorIndex() const {
        std::lock_guard<std::mutex> lock(mtx);
        return cursor;
    }
    void setCursor(int i) {
        std::lock_guard<std::mutex> lock(mtx);
        cursor = i;
    }
    std::string at(int i) const {
        std::lock_guard<std::mutex> lock(mtx);
        return (i >= 0 && i < static_cast<int>(items.size())) ? items[i]
                                                              : std::string{};
    }
    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lock(mtx);
        return items;
    }
    // Called on the engine thread as each track starts; resolves the playing path
    // to its index so the cursor follows a gapless auto-advance with no polling.
    // Duplicate-path aware: a path can appear several times in the list, and a
    // plain first-occurrence scan would snap the cursor BACK to the earliest
    // duplicate, overwriting a navigation command's optimistic set and making
    // `next` orbit the first two entries of [a b a c]. Resolution order:
    // (1) the cursor already points at this path, so the optimistic set was
    // right: keep it (also what makes `previous` onto a duplicate work);
    // (2) the nearest occurrence AT or AFTER the cursor, because an engine
    // auto-advance only ever moves forward; (3) the first occurrence anywhere,
    // for a start the controller did not position.
    void onEngineTrack(const std::string& path) {
        std::lock_guard<std::mutex> lock(mtx);
        const int n = static_cast<int>(items.size());
        if (cursor >= 0 && cursor < n && items[cursor] == path) {
            return;
        }
        const int from = (cursor >= 0 && cursor < n) ? cursor : 0;
        for (int i = from; i < n; ++i) {
            if (items[i] == path) {
                cursor = i;
                return;
            }
        }
        for (int i = 0; i < from; ++i) {
            if (items[i] == path) {
                cursor = i;
                return;
            }
        }
    }
};

// Parse the argument of enqueue/add/play into one or more paths. A bare argument
// is a SINGLE path, so a path with spaces still works unquoted (the previous
// behavior). A bracketed argument, [a b c], is a LIST split on whitespace, with
// double quotes to protect any element that itself contains spaces:
// ["a b.flac" c.flac].
std::vector<std::string> parsePathList(const std::string& arg) {
    std::vector<std::string> out;
    if (arg.size() >= 2 && arg.front() == '[' && arg.back() == ']') {
        const std::string inner = arg.substr(1, arg.size() - 2);
        std::string       cur;
        bool              inQuotes = false;
        for (const char c : inner) {
            if (c == '"') {
                inQuotes = !inQuotes;
            } else if (!inQuotes && (c == ' ' || c == '\t')) {
                if (!cur.empty()) {
                    out.push_back(cur);
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) {
            out.push_back(cur);
        }
    } else if (!arg.empty()) {
        out.push_back(arg);
    }
    return out;
}

// Map the CLI flag to the engine's portable RateMode (the mode lives in
// RateManager, not in the sink, and the engine owns it via setRateMode).
RateMode toRateMode(PlayRateFlag flag) {
    switch (flag) {
        case PlayRateFlag::Resample:  return RateMode::AlwaysResample;
        case PlayRateFlag::ForceRate: return RateMode::ForceDeviceRate;
        case PlayRateFlag::Default:   break;
    }
    return RateMode::BitPerfectWhenAvailable;
}

const char* rateModeName(RateMode m) noexcept {
    switch (m) {
        case RateMode::AlwaysResample:          return "always-resample";
        case RateMode::ForceDeviceRate:         return "force-device-rate";
        case RateMode::BitPerfectWhenAvailable: break;
    }
    return "bit-perfect-when-available";
}

const char* stateName(State s) noexcept {
    switch (s) {
        case State::Stopped: return "Stopped";
        case State::Playing: return "Playing";
        case State::Paused:  return "Paused";
    }
    return "?";
}

// Prints the engine's notifications. Fired on the engine thread; the writes here
// are atomics the main loop reads to know when a track has started or the run
// has ended (so the one-shot loop never mistakes the initial Stopped for a
// finish, and never spins forever when an open fails). It also keeps the current
// track's duration so a position readout can render "m:ss / m:ss".
//
// The live in-place position readout (a leading carriage return, no newline, so
// 10 Hz updates do not flood the scrollback) is for the ONE-SHOT play mode,
// which has no prompt to fight. In the interactive REPL it is disabled, because
// repainting the current line at 10 Hz overwrites whatever you are typing on the
// prompt; there, position is shown on demand via the `pos` command. setLiveReadout
// selects between the two, and it is off by default so a stray listener never
// clobbers the prompt.
class CliListener final : public Engine::Listener {
public:
    void onStateChanged(State s) override {
        if (s == State::Playing) {
            m_everPlayed.store(true, std::memory_order_relaxed);
        }
        endPosLine();
        std::printf("[state] %s\n", stateName(s));
        std::fflush(stdout);
    }
    void onTrackChanged(const TrackInfo& t) override {
        m_durationFrames.store(t.totalFrames, std::memory_order_relaxed);
        m_rate.store(t.format.sampleRate, std::memory_order_relaxed);
        endPosLine();
        std::printf("[track] %s  (%s via %s, %u kbps, %u Hz, %u ch, %s)\n",
                    t.path.c_str(),
                    codecName(t.source.codec),
                    decoderKindName(t.source.decoder),
                    t.source.bitrateKbps,
                    t.format.sampleRate,
                    static_cast<unsigned>(t.format.channels),
                    durationString(t.totalFrames, t.format.sampleRate).c_str());
        std::fflush(stdout);
        // Keep the interactive playlist cursor on the track now playing. Fires on
        // the engine thread, including each gapless auto-advance, so navigation
        // commands always compute from the real current index. Null in one-shot
        // play mode, which has no playlist.
        if (m_playlist != nullptr) {
            m_playlist->onEngineTrack(t.path);
        }
    }
    void onPositionChanged(double seconds) override {
        // Interactive mode leaves this off so the readout never overwrites the
        // prompt; one-shot play turns it on. See the class comment.
        if (!m_liveReadout.load(std::memory_order_relaxed)) {
            return;
        }
        const std::string total =
            durationString(m_durationFrames.load(std::memory_order_relaxed),
                           m_rate.load(std::memory_order_relaxed));
        const std::uint32_t kbps =
            (m_engine != nullptr) ? m_engine->liveBitrateKbps() : 0;
        std::printf("\r[pos] %s / %s  [%4u kbps]   ",
                    secondsToMMSS(seconds).c_str(), total.c_str(), kbps);
        m_posPending.store(true, std::memory_order_relaxed);
        std::fflush(stdout);
    }
    void onError(const std::string& message) override {
        m_errored.store(true, std::memory_order_relaxed);
        endPosLine();
        std::fprintf(stderr, "[error] %s\n", message.c_str());
    }

    // The reply to `devices`. Print the numbered listing and keep the
    // ids so `device <n>` can map an index back to a persistent id. Fires on
    // the engine thread, like everything here.
    void onOutputDevices(
        const std::vector<rawform::audio::AudioDeviceInfo>& devices) override {
        endPosLine();
        {
            std::lock_guard<std::mutex> lock(m_devMtx);
            m_devices = devices;
        }
        if (devices.empty()) {
            std::puts("[devices] none reported");
        } else {
            std::puts("[devices]");
            for (std::size_t i = 0; i < devices.size(); ++i) {
                std::printf("  %zu. %s%s\n      id: %s\n", i + 1,
                            devices[i].name.c_str(),
                            devices[i].isDefault ? "  [system default]" : "",
                            devices[i].id.c_str());
            }
            std::puts("  (select with: device <n>, or device default)");
        }
        std::fflush(stdout);
    }

    // Index (1-based, from the printed listing) -> persistent id; empty when
    // out of range or no listing has been requested yet.
    std::string deviceIdAt(std::size_t oneBased) const {
        std::lock_guard<std::mutex> lock(m_devMtx);
        if (oneBased == 0 || oneBased > m_devices.size()) {
            return {};
        }
        return m_devices[oneBased - 1].id;
    }

    // Terminate an in-place position readout with a newline, if one is pending,
    // so a following line prints cleanly. Safe to call from any thread; it is a
    // single print guarded by an atomic exchange.
    void endPosLine() {
        if (m_posPending.exchange(false, std::memory_order_relaxed)) {
            std::printf("\n");
            std::fflush(stdout);
        }
    }

    // Enable the live in-place readout. On for the one-shot play mode (no
    // prompt), off for the interactive REPL (where it would clobber the line you
    // are typing). Off by default.
    void setLiveReadout(bool on) noexcept {
        m_liveReadout.store(on, std::memory_order_relaxed);
    }

    // Attach the interactive playlist so the cursor can follow track changes. Set
    // once before playback starts; left null for one-shot play mode.
    void setPlaylist(PlaylistState* pl) noexcept { m_playlist = pl; }

    // Attach the engine so the live readout can pull the moment-to-moment decode
    // bitrate. Set once before playback; left null in modes that do
    // not paint the live readout.
    void setEngine(const Engine* e) noexcept { m_engine = e; }

    std::uint64_t durationFrames() const noexcept {
        return m_durationFrames.load(std::memory_order_relaxed);
    }
    std::uint32_t durationRate() const noexcept {
        return m_rate.load(std::memory_order_relaxed);
    }
    bool everPlayed() const noexcept { return m_everPlayed.load(std::memory_order_relaxed); }
    bool errored()    const noexcept { return m_errored.load(std::memory_order_relaxed); }

private:
    std::atomic<bool>          m_everPlayed{false};
    std::atomic<bool>          m_errored{false};
    std::atomic<bool>          m_posPending{false};
    mutable std::mutex         m_devMtx; // guards the last `devices` listing
    std::vector<rawform::audio::AudioDeviceInfo> m_devices;
    std::atomic<bool>          m_liveReadout{false};
    std::atomic<std::uint64_t> m_durationFrames{0};
    std::atomic<std::uint32_t> m_rate{0};
    PlaylistState*             m_playlist = nullptr;  // interactive only; null in one-shot play
    const Engine*              m_engine   = nullptr;  // for the live bitrate readout
};

// One-shot: enqueue one file, play, and wait for natural completion (queue
// exhaustion returns the engine to Stopped) or Ctrl-C. Teardown is the Engine
// destructor at scope exit, which closes the sink, joining the RT thread and
// restoring the device rate.
int runPlay(const std::string& inputPath, PlayRateFlag rateFlag) {
    installInterruptHandlers();

    Engine       engine;
    CliListener  listener;
    engine.setListener(&listener);
    engine.setRateMode(toRateMode(rateFlag)); // Policy is engine-owned now
    listener.setEngine(&engine);    // Live bitrate for the readout
    listener.setLiveReadout(true);  // one-shot play has no prompt; show the live readout

    // Capture the sink pointer before handing ownership to the engine, so we can
    // report the granted device format once playback is up. Reading it after we
    // observe Playing is safe: the engine opens the sink and then publishes
    // Playing with a release store, so the acquire load of state() that lets us
    // through orders the sink's open writes ahead of our read.
    auto          sink    = std::make_unique<PlatformSink>();
    PlatformSink* sinkPtr = sink.get();
    engine.setSink(std::move(sink));

    engine.enqueue(inputPath);
    engine.play();

    bool printedOutput = false;
    bool interrupted   = false;
    for (;;) {
        if (g_interrupted.load(std::memory_order_relaxed)) {
            interrupted = true;
            break;
        }
        // An open failure before anything ever played: stop waiting.
        if (listener.errored() && !listener.everPlayed()) {
            return 1;
        }
        const State s = engine.state();
        if (s == State::Playing && !printedOutput) {
            listener.endPosLine();
            const AudioFormat granted = sinkPtr->currentFormat();
            std::printf("Output:      %u Hz, %u ch (%s)\n", granted.sampleRate,
                        static_cast<unsigned>(granted.channels),
                        sinkPtr->bitPerfect() ? "bit-perfect" : "resampled by device");
            std::fflush(stdout);
            printedOutput = true;
        }
        // Completion: we played and the engine has settled back to Stopped.
        if (listener.everPlayed() && s == State::Stopped) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    listener.endPosLine();
    std::printf(interrupted ? "Interrupted.\n" : "Done.\n");
    return 0;
    // engine destructs here: stop + close the sink (restoring the rate), then
    // drop the ring and decoder. No explicit teardown needed.
}

// Reads "command [argument]" from a line, where the argument is everything after
// the first token, trimmed, so file paths may contain spaces.
void splitCommand(const std::string& line, std::string& cmd, std::string& arg) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    std::size_t start = i;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
        ++i;
    }
    cmd = line.substr(start, i - start);
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    std::size_t end = line.size();
    while (end > i && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                       line[end - 1] == '\n' || line[end - 1] == '\r')) {
        --end;
    }
    arg = line.substr(i, end - i);
}

// The transport REPL. One Engine, many commands. The blocking getline is
// interrupted by SIGINT (the handler installation clears SA_RESTART, so the
// read fails with EINTR rather than restarting), and Ctrl-C falls through to
// the flag check and a clean exit.
int runInteractive(PlayRateFlag rateFlag) {
    installInterruptHandlers();

    Engine        engine;
    CliListener   listener;
    PlaylistState playlist;
    listener.setPlaylist(&playlist);
    engine.setListener(&listener);
    engine.setRateMode(toRateMode(rateFlag)); // Policy is engine-owned now
    engine.setSink(std::make_unique<PlatformSink>());

    std::puts("rawform interactive (gapless playlist). commands:");
    std::puts("  play [file]      play; with no file, resume/continue the playlist");
    std::puts("  pause    stop");
    std::puts("  enqueue <file>   add to the playlist (kept after it plays)");
    std::puts("  enqueue [a b c]  add several; quote spaces: [\"a b.flac\" c.flac]");
    std::puts("  next    previous    first    last    list    clear");
    std::puts("  seek <m:ss|seconds>      absolute seek");
    std::puts("  >  <   seek +/- 1s    >>  <<   seek +/- 5s   (optional arg: > 3)");
    std::puts("  devices          list output devices");
    std::puts("  device <n|default>   select output device (live if playing)");
    std::puts("  pos    quit");

    // Start playback at playlist index j, with the remainder queued behind it so a
    // same-format album plays gaplessly from there.
    //
    // This is now a single, state-independent gesture. The engine's
    // playNow/setQueue primitives replace the old four-branch dance that had to
    // reason about Playing vs Paused vs fresh vs Stopped-on-this vs
    // Stopped-on-another, and they eliminate its documented wrinkle: jumping from a
    // Stopped state onto a DIFFERENT track used to briefly play the remembered
    // track before cutting. setQueue replaces the pending queue with the tail after
    // j (what auto-advances, gaplessly when the format matches); playNow makes j
    // current and plays it immediately, from any state, with no remembered-replay
    // blip. The two land in one engine-thread command pass, so no advance slips
    // between them.
    const auto startAt = [&](int j) {
        const int n = static_cast<int>(playlist.size());
        if (j < 0 || j >= n) {
            std::puts("playlist: nothing to play there");
            return;
        }
        std::vector<std::string> tail;
        tail.reserve(static_cast<std::size_t>(n - j - 1));
        for (int k = j + 1; k < n; ++k) {
            tail.push_back(playlist.at(k));
        }
        engine.setQueue(std::move(tail));  // what plays AFTER j (gapless if same format)
        engine.playNow(playlist.at(j));    // cut/start to j now; no remembered-replay blip
        playlist.setCursor(j);             // optimistic; onTrackChanged confirms it
    };

    // Relative seek for the arrow commands, clamped at 0.
    const auto relSeek = [&](double delta) {
        const State s = engine.state();
        if (s == State::Stopped) {
            std::puts("not playing");
            return;
        }
        double target = engine.position() + delta;
        if (target < 0.0) {
            target = 0.0;
        }
        engine.seek(target);
        listener.endPosLine();
        std::printf("[seek] -> %s\n", secondsToMMSS(target).c_str());
        std::fflush(stdout);
    };

    std::string line;
    for (;;) {
        std::fputs("> ", stdout);
        std::fflush(stdout);

        if (!std::getline(std::cin, line)) {
            // EOF, or a signal interrupted the read. Either way, leave.
            break;
        }
        if (g_interrupted.load(std::memory_order_relaxed)) {
            break;
        }

        std::string cmd, arg;
        splitCommand(line, cmd, arg);
        if (cmd.empty()) {
            continue;
        }

        if (cmd == "quit" || cmd == "exit") {
            break;
        } else if (cmd == "play") {
            if (!arg.empty()) {
                // Add the given file(s) to the playlist and jump to the first.
                const std::vector<std::string> paths = parsePathList(arg);
                if (paths.empty()) {
                    std::puts("usage: play <file>");
                } else {
                    const int firstNew = static_cast<int>(playlist.size());
                    for (const std::string& p : paths) {
                        playlist.append(p);
                    }
                    startAt(firstNew);
                }
            } else {
                const State s = engine.state();
                if (s == State::Paused) {
                    engine.play();  // resume in place
                } else if (s == State::Playing) {
                    // already playing; nothing to do
                } else if (playlist.size() == 0) {
                    std::puts("playlist is empty; enqueue a file first");
                } else {
                    const int c = playlist.cursorIndex();
                    startAt((listener.everPlayed() && c >= 0) ? c : 0);
                }
            }
        } else if (cmd == "pause") {
            engine.pause();
        } else if (cmd == "stop") {
            engine.stop();
        } else if (cmd == "enqueue" || cmd == "add") {
            const std::vector<std::string> paths = parsePathList(arg);
            if (paths.empty()) {
                std::puts("usage: enqueue <file>   |   enqueue [a b c]");
            } else {
                for (const std::string& p : paths) {
                    playlist.append(p);
                }
                // If a session is live, feed the new tracks to the engine too so
                // it auto-advances into them; if Stopped, they are queued on the
                // next start, so the playlist stays the single source of truth.
                const State s = engine.state();
                if (s == State::Playing || s == State::Paused) {
                    for (const std::string& p : paths) {
                        engine.enqueue(p);
                    }
                }
                std::printf("playlist: added %zu, %zu total\n", paths.size(),
                            playlist.size());
            }
        } else if (cmd == "next") {
            const int n = static_cast<int>(playlist.size());
            if (n == 0) {
                std::puts("playlist is empty");
            } else {
                const int t = playlist.cursorIndex() + 1;
                if (t < n) {
                    startAt(t);
                } else {
                    std::puts("already at the last track");
                }
            }
        } else if (cmd == "previous" || cmd == "prev") {
            const int n = static_cast<int>(playlist.size());
            if (n == 0) {
                std::puts("playlist is empty");
            } else {
                const int c = playlist.cursorIndex();
                startAt(c <= 0 ? 0 : c - 1);
            }
        } else if (cmd == "first") {
            if (playlist.size() == 0) {
                std::puts("playlist is empty");
            } else {
                startAt(0);
            }
        } else if (cmd == "last") {
            const int n = static_cast<int>(playlist.size());
            if (n == 0) {
                std::puts("playlist is empty");
            } else {
                startAt(n - 1);
            }
        } else if (cmd == "list" || cmd == "pl") {
            const std::vector<std::string> items = playlist.snapshot();
            const int                      c     = playlist.cursorIndex();
            listener.endPosLine();
            if (items.empty()) {
                std::puts("(playlist empty)");
            } else {
                for (int i = 0; i < static_cast<int>(items.size()); ++i) {
                    std::printf("  %c %2d  %s\n", (i == c ? '>' : ' '), i + 1,
                                items[i].c_str());
                }
            }
            std::fflush(stdout);
        } else if (cmd == "clear") {
            engine.stop();
            engine.clearQueue();
            playlist.clearAll();
            std::puts("playlist cleared");
        } else if (cmd == "seek") {
            double t = 0.0;
            if (!parseTime(arg, t)) {
                std::puts("usage: seek <m:ss | seconds>");
            } else {
                engine.seek(t);
            }
        } else if (cmd == ">" || cmd == "<" || cmd == ">>" || cmd == "<<") {
            // Relative seek. A doubled glyph is the 5 s step, a single is 1 s; an
            // optional numeric argument overrides the magnitude (for example
            // "> 3" jumps forward 3 s).
            double magnitude = (cmd.size() == 2) ? 5.0 : 1.0;
            double explicitMag = 0.0;
            if (!arg.empty() && parseTime(arg, explicitMag) && explicitMag > 0.0) {
                magnitude = explicitMag;
            }
            relSeek((cmd[0] == '>') ? magnitude : -magnitude);
        } else if (cmd == "devices") {
            engine.requestOutputDevices();
        } else if (cmd == "device") {
            // device <n> picks from the last `devices` listing; `device
            // default` (or `device 0`) returns to following the system
            // default. The engine applies it live (reopen-at-position) when
            // something is playing.
            if (arg.empty()) {
                std::puts("usage: device <n | default>   (run `devices` first)");
            } else if (arg == "default" || arg == "0") {
                engine.selectOutputDevice("");
            } else {
                std::size_t n = 0;
                try {
                    n = static_cast<std::size_t>(std::stoul(arg));
                } catch (...) {
                    n = 0;
                }
                const std::string id = listener.deviceIdAt(n);
                if (id.empty()) {
                    std::puts("no such entry; run `devices` and pick an index");
                } else {
                    engine.selectOutputDevice(id);
                }
            }
        } else if (cmd == "pos") {
            listener.endPosLine();
            std::printf("[pos] %s / %s  [%u kbps]\n",
                        secondsToMMSS(engine.position()).c_str(),
                        durationString(listener.durationFrames(),
                                       listener.durationRate()).c_str(),
                        engine.liveBitrateKbps());
            std::fflush(stdout);
        } else {
            std::printf("unknown command: %s\n", cmd.c_str());
        }
    }

    std::puts("bye.");
    return 0;
    // engine destructs here: clean teardown, device rate restored.
}

// The "rates" / "device" diagnostic. Prints what an output device advertises,
// exactly as the RateManager sees it through capabilities(), and, if a file is
// given, the decision the policy would make for that file under the chosen
// mode (as if from a fresh start). By default the report describes the system
// default device; --device pins the report to a specific one,
// resolved through the SAME persistent-id matching the engine's
// device selection uses, with a 1-based enumeration index accepted as a
// convenience (the same numbering the interactive `devices` listing prints,
// since persistent ids are long). It changes no device state: the pin lives
// and dies with this process's sink, capabilities() is a pure query and
// decide() is pure, so nothing here switches the device or plays audio. This
// is the tool for confirming the logged decision against the platform's own
// view (Audio MIDI Setup on macOS, pw-metadata's clock settings on PipeWire).
int runRates(const std::string& filePath, PlayRateFlag rateFlag,
             const std::string& deviceSel) {
    PlatformSink sink;  // not opened; capabilities() is independent of open()

    if (!deviceSel.empty()) {
        const std::vector<AudioDeviceInfo> devices = sink.enumerateDevices();
        std::string id;
        for (const AudioDeviceInfo& d : devices) {
            if (d.id == deviceSel) {
                id = d.id;
                break;
            }
        }
        if (id.empty()) {
            // Not a persistent id: accept a 1-based index into the
            // enumeration, but only when the WHOLE argument is a number in
            // range, so a mistyped id can never silently pick device #4.
            char*               end = nullptr;
            const unsigned long n = std::strtoul(deviceSel.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && n >= 1 &&
                n <= devices.size()) {
                id = devices[n - 1].id;
            }
        }
        if (id.empty() || !sink.selectDevice(id)) {
            std::fprintf(stderr, "error: no output device matches '%s'\n",
                         deviceSel.c_str());
            std::fprintf(stderr, "available:\n");
            for (std::size_t i = 0; i < devices.size(); ++i) {
                std::fprintf(stderr, "  %zu. %s%s\n      id: %s\n", i + 1,
                             devices[i].name.c_str(),
                             devices[i].isDefault ? "  [system default]" : "",
                             devices[i].id.c_str());
            }
            return 2;
        }
    }

    const SinkCapabilities caps = sink.capabilities();

    std::printf("Device:       %s\n", caps.deviceName.c_str());
    std::printf("Current rate: %u Hz\n", caps.currentRate);
    std::printf("Can switch:   %s\n", caps.canSwitchRate ? "yes" : "no");

    std::printf("Advertised:   ");
    if (caps.rates.empty()) {
        std::printf("(none reported)");
    } else {
        for (std::size_t i = 0; i < caps.rates.size(); ++i) {
            const RateRange& r = caps.rates[i];
            if (i != 0) {
                std::printf(", ");
            }
            if (r.min == r.max) {
                std::printf("%u", r.min);
            } else {
                std::printf("%u-%u", r.min, r.max);
            }
        }
    }
    std::printf(" Hz\n");

    if (filePath.empty()) {
        return 0;  // device-only report
    }

    // Decision for a specific file, from a fresh start (invalid currentOpen).
    DecoderFactory            factory;
    std::string               err;
    std::unique_ptr<IDecoder> dec = factory.open(filePath, &err);
    if (!dec) {
        std::fprintf(stderr, "error: could not open '%s': %s\n", filePath.c_str(),
                     err.c_str());
        return 1;
    }

    const AudioFormat  fmt  = dec->format();
    const RateMode     mode = toRateMode(rateFlag);
    constexpr RateManager rm;
    const RateDecision d    = rm.decide(fmt, caps, mode, AudioFormat{});

    std::printf("\nFile:         %s\n", filePath.c_str());
    std::printf("Source:       %u Hz, %u ch\n", fmt.sampleRate,
                static_cast<unsigned>(fmt.channels));
    std::printf("Mode:         %s\n", rateModeName(mode));
    std::printf("Decision:\n");
    std::printf("  device rate: %u Hz\n", d.deviceRate);
    std::printf("  switch:      %s\n", d.switchDevice ? "yes" : "no");
    std::printf("  resample:    %s (predicted)\n", d.resampleNeeded ? "yes" : "no");
    std::printf("  reconfigure: %s\n", d.needsDeviceReconfigure ? "yes" : "no");
    return 0;
}

#else  // neither RAWFORM_HAVE_COREAUDIO nor RAWFORM_HAVE_PIPEWIRE

// Compiled only when NO platform sink exists: a non-Apple, non-UNIX build, or
// a UNIX box where libpipewire-0.3 was missing at configure time (the CMake
// warning already named the missing -devel package there).

int runPlay(const std::string& /*inputPath*/, PlayRateFlag /*rateFlag*/) {
    std::fprintf(stderr,
                 "error: no audio sink was compiled in (CoreAudio is "
                 "macOS-only; PipeWire needs libpipewire-0.3 at configure "
                 "time)\n");
    return 1;
}

int runInteractive(PlayRateFlag /*rateFlag*/) {
    std::fprintf(stderr,
                 "error: no audio sink was compiled in (CoreAudio is "
                 "macOS-only; PipeWire needs libpipewire-0.3 at configure "
                 "time)\n");
    return 1;
}

int runRates(const std::string& /*filePath*/, PlayRateFlag /*rateFlag*/,
             const std::string& /*deviceSel*/) {
    std::fprintf(stderr,
                 "error: no audio device to query; no sink was compiled in "
                 "(CoreAudio is macOS-only; PipeWire needs libpipewire-0.3 "
                 "at configure time)\n");
    return 1;
}

#endif  // RAWFORM_HAVE_COREAUDIO || RAWFORM_HAVE_PIPEWIRE

// Parses the optional rate flags shared by play and interactive. Returns false
// on an unrecognized option, having printed an error. On success, `positional`
// receives any single non-flag argument (the file for play; unused otherwise),
// and `noGapless` is set if --no-gapless was given (the caller maps it to the
// RAWFORM_NO_GAPLESS environment knob before constructing the Engine).
// deviceSel: where --device is meaningful (the rates report), the caller
// passes a string to receive the selector; callers that pass nullptr reject
// the flag with a clear message rather than silently ignoring it.
bool parseRateArgs(int argc, char** argv, int firstArg, const char* argv0,
                   PlayRateFlag& rateFlag, std::string& positional,
                   bool wantPositional, bool& noGapless,
                   std::string* deviceSel) {
    for (int i = firstArg; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--resample") {
            rateFlag = PlayRateFlag::Resample;
        } else if (a == "--force-rate") {
            rateFlag = PlayRateFlag::ForceRate;
        } else if (a == "--no-gapless") {
            noGapless = true; // Mapped to RAWFORM_NO_GAPLESS before the Engine is built
        } else if (a == "--device") {
            if (deviceSel == nullptr) {
                std::fprintf(stderr,
                             "error: --device is only valid with 'rates'\n");
                printUsage(argv0);
                return false;
            }
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --device requires a value\n");
                printUsage(argv0);
                return false;
            }
            *deviceSel = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", a.c_str());
            printUsage(argv0);
            return false;
        } else if (wantPositional && positional.empty()) {
            positional = a;
        } else {
            std::fprintf(stderr, "error: unexpected argument '%s'\n", a.c_str());
            printUsage(argv0);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // Subcommand dispatch. "play ... <file>" and "interactive ..." route to the
    // transport paths; anything else is the info/hash path with --dump.
    if (argc >= 2 && std::strcmp(argv[1], "play") == 0) {
        auto rateFlag = PlayRateFlag::Default;
        std::string  playPath;
        bool         noGapless = false;
        if (!parseRateArgs(argc, argv, 2, argv[0], rateFlag, playPath, true,
                           noGapless, nullptr)) {
            return 2;
        }
        if (playPath.empty()) {
            std::fprintf(stderr, "error: 'play' requires a file\n");
            printUsage(argv[0]);
            return 2;
        }
        // The Engine reads RAWFORM_NO_GAPLESS once at construction, which
        // happens inside runPlay, so set it here first. No public interface
        // change; --no-gapless is purely an env mapping.
        if (noGapless) {
            ::setenv("RAWFORM_NO_GAPLESS", "1", 1);
        }
        return runPlay(playPath, rateFlag);
    }

    if (argc >= 2 && std::strcmp(argv[1], "interactive") == 0) {
        auto rateFlag = PlayRateFlag::Default;
        std::string  unused;
        bool         noGapless = false;
        if (!parseRateArgs(argc, argv, 2, argv[0], rateFlag, unused, false,
                           noGapless, nullptr)) {
            return 2;
        }
        if (noGapless) {
            ::setenv("RAWFORM_NO_GAPLESS", "1", 1);
        }
        return runInteractive(rateFlag);
    }

    // "rates" (alias "device"): print the output device's capabilities and,
    // optionally, the rate decision for a file. The file positional is optional
    // here, so an empty positional is fine (device-only report).
    if (argc >= 2 && (std::strcmp(argv[1], "rates") == 0 ||
                      std::strcmp(argv[1], "device") == 0)) {
        auto rateFlag = PlayRateFlag::Default;
        std::string  filePath;
        std::string deviceSel; // --device pins the report
        bool         noGapless = false;  // accepted but irrelevant to the rates report
        if (!parseRateArgs(argc, argv, 2, argv[0], rateFlag, filePath, true,
                           noGapless, &deviceSel)) {
            return 2;
        }
        return runRates(filePath, rateFlag, deviceSel);
    }

    // "probe": the engine's metadata reader for one file. No rate flags, no sink,
    // exactly one positional argument.
    if (argc >= 2 && std::strcmp(argv[1], "probe") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "error: 'probe' requires a file\n");
            printUsage(argv[0]);
            return 2;
        }
        if (argc > 3) {
            std::fprintf(stderr, "error: unexpected extra argument '%s'\n",
                         argv[3]);
            printUsage(argv[0]);
            return 2;
        }
        return runProbe(argv[2]);
    }

    std::string inputPath;
    std::string dumpPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: --dump requires a path\n");
                printUsage(argv[0]);
                return 2;
            }
            dumpPath = argv[++i];
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
            printUsage(argv[0]);
            return 2;
        } else if (inputPath.empty()) {
            inputPath = arg;
        } else {
            std::fprintf(stderr, "error: unexpected extra argument '%s'\n",
                         arg.c_str());
            printUsage(argv[0]);
            return 2;
        }
    }
    if (inputPath.empty()) {
        printUsage(argv[0]);
        return 2;
    }

    return runInfo(inputPath, dumpPath);
}
