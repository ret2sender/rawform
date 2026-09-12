# This file is part of rawform.
# Copyright (C) 2026 Etienne Fleurant
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
#
# SPDX-License-Identifier: GPL-3.0-or-later

# audio/CMakeLists.txt
#
# Standalone build for the rawform_audio engine library. The pieces, in
# dependency order: the SPSC RingBuffer (fully isolated); SndfileDecoder behind
# libsndfile; the platform output sinks (CoreAudioSink on macOS, PipeWireSink on
# Linux); and the Engine itself: an engine thread, a command queue, a transport
# state machine, and a play queue, driving any IAudioSink through a small
# internal real-time source (EngineRingSource). NullSink is the portable
# headless sink used by the engine soak test (and a legitimate null output in
# its own right). The two native reference decoders, FlacDecoder (libFLAC) and
# Mpg123Decoder (libmpg123), sit behind a format-dispatching DecoderFactory
# that is the Engine's built-in default. RateManager centralizes the
# sample-rate policy as a portable, codec-free, CoreAudio-free decision
# function (src/RateManager.cpp) that the Engine consults per track and the
# sink merely executes; RateManagerTest links the library but needs no codec or
# platform dependency, since the policy is pure.
#
# FfmpegDecoder is the OPTIONAL terminal fallback for the long tail of
# formats no native decoder handles, behind the tri-state RAWFORM_FFMPEG option
# and the PUBLIC RAWFORM_HAVE_FFMPEG capability define; the four libav* libraries
# link PRIVATE like every other codec. ConformanceTest is a
# cross-decoder boundary-contract battery run through the real DecoderFactory.
#
# Dependency layering: libsndfile, libFLAC, libmpg123, and
# the CoreAudio frameworks all link PRIVATE to the library (the public headers
# stay free of every one of them, thanks to the pimpls), so for this STATIC
# library CMake threads their symbols onto consumers' link lines and the CLI links
# cleanly without naming any of them.
#
# ReplayGainScanner is the zero-Qt loudness measurement class (decode through
# the same factory, measure with libebur128). libebur128 is a HARD dependency of
# the library, linked PRIVATE like every other codec, with its header kept out
# of the public interface by the scanner's pimpl. ReplayGainScannerTest drives
# the scanner with a synthetic in-memory signal through an injected factory, so
# it needs no codec dependency.
#
# Sanitizers: configure with -DRAWFORM_SANITIZE=thread for the concurrency
# soaks (RingBufferTest and EngineTest); =address builds ASan+UBSan together,
# the pairing the decoders care about. The profile block below is directory-
# scoped, so instrumentation covers the library, the CLI and every test.
#
# Typical use from this directory:
#   cmake -S . -B build -DRAWFORM_SANITIZE=thread
#   cmake --build build
#   ctest --test-dir build --output-on-failure

cmake_minimum_required(VERSION 3.20)
project(rawform_audio LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Sanitizer profiles. -DRAWFORM_SANITIZE=thread builds under TSan;
# =address builds ASan+UBSan together (their usual pairing; TSan is
# incompatible with ASan and gets its own profile).
#
# POSITION IS LOAD-BEARING: add_compile_options() affects only targets defined
# AFTER it, so this block sits directly under project(), before any target.
# The scenario this prevents: with the block placed
# below the library target, only the test executables are instrumented, and a
# partially instrumented TSan build FABRICATES deterministic races: the
# runtime's memcpy interceptor still sees every payload copy inside the
# uninstrumented library, while the library's atomic release/acquire operations
# never reach the runtime, so the synchronization is invisible and the payload
# transfer looks unordered. The provably-correct SPSC RingBuffer was reported
# racy on every run, under GCC and clang alike, until the placement was fixed.
# Every TU must be instrumented for a sanitizer verdict to mean anything.
set(RAWFORM_SANITIZE "off" CACHE STRING
    "Sanitizer profile: off, thread, or address (ASan+UBSan)")
if(RAWFORM_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
    message(STATUS "rawform_audio: ThreadSanitizer profile")
elseif(RAWFORM_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    message(STATUS "rawform_audio: AddressSanitizer+UBSan profile")
elseif(NOT RAWFORM_SANITIZE STREQUAL "off")
    message(WARNING "rawform_audio: unknown RAWFORM_SANITIZE '${RAWFORM_SANITIZE}' (off, thread, address)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared warning flags, applied per-target below.
set(_rawform_warnings "")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(_rawform_warnings -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Threads. The library owns the engine thread (Engine) and the headless sink's
# pull thread (NullSink), so the threads runtime is a library dependency, not
# just a test one. Discover it before the library and link it PUBLIC so the
# symbol requirement propagates to every consumer's link line (a static archive
# does not carry its own link deps).
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# libsndfile discovery. Two supported paths, both ending at a uniform
# SndFile::sndfile target so the rest of this file does not care which fired:
#   - CONFIG mode: modern libsndfile ships SndFileConfig.cmake (Homebrew on macOS,
#     which is the dev environment, installs exactly this).
#   - pkg-config fallback: Debian/Ubuntu's libsndfile1-dev ships only a .pc file,
#     so when the config package is absent we wrap PkgConfig::SNDFILE in an alias
#     of the same name.
find_package(SndFile CONFIG QUIET)
if(NOT SndFile_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
    add_library(SndFile::sndfile ALIAS PkgConfig::SNDFILE)
endif()

# ---------------------------------------------------------------------------
# libFLAC discovery. Same two-path shape as libsndfile, normalized to a
# FLAC::FLAC target. The config package's FLAC::FLAC carries the encoder too, so
# FlacDecoderTest (which encodes its own fixtures) needs no extra package.
#   - CONFIG mode: Homebrew's flac ships flac-config.cmake with FLAC::FLAC.
#   - pkg-config fallback: Debian's libflac-dev ships only flac.pc, wrapped in an
#     alias of the same name.
find_package(FLAC CONFIG QUIET)
if(NOT TARGET FLAC::FLAC)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
    add_library(FLAC::FLAC ALIAS PkgConfig::FLAC)
endif()

# ---------------------------------------------------------------------------
# libmpg123 discovery, normalized to a MPG123::libmpg123 target.
#   - CONFIG mode: Homebrew's mpg123 ships libmpg123 CMake config exporting
#     MPG123::libmpg123.
#   - pkg-config fallback: Debian's libmpg123-dev ships only libmpg123.pc.
find_package(mpg123 CONFIG QUIET)
if(NOT TARGET MPG123::libmpg123)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MPG123 REQUIRED IMPORTED_TARGET libmpg123)
    add_library(MPG123::libmpg123 ALIAS PkgConfig::MPG123)
endif()

# ---------------------------------------------------------------------------
# FFmpeg discovery, the terminal fallback decoder. OPTIONAL, tri-state,
# mirroring the RAWFORM_HAVE_COREAUDIO capability shape:
#   RAWFORM_FFMPEG = auto (default) : probe quietly; enable iff all four libs found
#                  = on             : require them, HARD ERROR if missing (so CI
#                                     that expects the fallback fails loudly)
#                  = off            : force native-only, never probe
# Unlike the three native decoders, the libav* libraries ship no CMake config
# package, only pkg-config, so discovery goes straight to pkg-config with no
# CONFIG-mode attempt. The four modules resolve to a single PkgConfig::FFMPEG
# IMPORTED_TARGET. The outcome is published as the PUBLIC RAWFORM_HAVE_FFMPEG
# define on the library (below), exactly like RAWFORM_HAVE_COREAUDIO, so the
# decoder, the factory hook, and the conformance case all compile in or out
# together. macOS: `brew install ffmpeg`. Debian/Ubuntu: `libavformat-dev
# libavcodec-dev libavutil-dev libswresample-dev`.
set(RAWFORM_FFMPEG "auto"
    CACHE STRING "FFmpeg fallback decoder: auto | on | off")
set_property(CACHE RAWFORM_FFMPEG PROPERTY STRINGS auto on off)

set(_rawform_have_ffmpeg 0)
if(NOT RAWFORM_FFMPEG STREQUAL "off")
    find_package(PkgConfig REQUIRED)
    if(RAWFORM_FFMPEG STREQUAL "on")
        pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
            libavformat libavcodec libavutil libswresample)
    else()
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavformat libavcodec libavutil libswresample)
    endif()
    if(FFMPEG_FOUND)
        set(_rawform_have_ffmpeg 1)
        message(STATUS "rawform_audio: FFmpeg fallback ENABLED (${FFMPEG_libavformat_VERSION})")
    else()
        message(STATUS "rawform_audio: FFmpeg fallback disabled (libav* not found)")
    endif()
endif()

# ---------------------------------------------------------------------------
# libebur128 discovery (ReplayGain scanning). The scanner measures BS.1770
# integrated loudness and sample peak through this small C library; it is a HARD
# dependency of rawform_audio (no build guard, unlike the optional FFmpeg
# fallback), normalized to an Ebur128::ebur128 target so the link line below does
# not care which path found it. Like the libav* libraries it ships no widely
# installed CMake config package, so discovery goes through pkg-config. The .pc
# module name is libebur128 on most installs (Homebrew's libebur128, Debian's
# libebur128-dev) and ebur128 on a few, so both are probed before giving up.
#   macOS: `brew install libebur128`.  Debian/Ubuntu: `apt install libebur128-dev`.
if(NOT TARGET Ebur128::ebur128)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EBUR128 QUIET IMPORTED_TARGET libebur128)
    if(NOT EBUR128_FOUND)
        pkg_check_modules(EBUR128 QUIET IMPORTED_TARGET ebur128)
    endif()
    if(EBUR128_FOUND)
        add_library(Ebur128::ebur128 ALIAS PkgConfig::EBUR128)
        message(STATUS "rawform_audio: libebur128 found (${EBUR128_VERSION})")
    else()
        message(FATAL_ERROR
            "rawform_audio: libebur128 not found (tried pkg-config modules "
            "'libebur128' and 'ebur128'). Install it with 'brew install "
            "libebur128' on macOS or 'apt install libebur128-dev' on "
            "Debian/Ubuntu.")
    endif()
endif()

# ---------------------------------------------------------------------------
# The library: RingBuffer, SndfileDecoder, Engine + EngineRingSource +
# NullSink, FlacDecoder + Mpg123Decoder + DecoderFactory. All of these are
# fully portable (zero CoreAudio), so they are always built; CoreAudioSink is
# macOS-only and is added to the target under if(APPLE) below. IDecoderFactory is
# header-only, but DecoderFactory is a real source (the Engine's default
# opener). Private library headers live under src/, so src/ is a PRIVATE include
# dir for the library's own sources, and the CLI and tests add it themselves to
# reach those headers.
add_library(rawform_audio STATIC
    src/RingBuffer.cpp
    src/decoders/SndfileDecoder.cpp
    src/decoders/FlacDecoder.cpp
    src/decoders/Mpg123Decoder.cpp
    src/DecoderFactory.cpp
    src/Engine.cpp
    src/EngineRingSource.cpp
    src/RateManager.cpp
    src/sinks/NullSink.cpp
    # Read-side FFmpeg metadata fallback. Compiled UNCONDITIONALLY (unlike
    # FfmpegDecoder.cpp): its body is guarded on RAWFORM_HAVE_FFMPEG and returns
    # std::nullopt on a native-only build, so the probeAudioMetadata symbol always
    # exists and the UI links it without an #if. When FFmpeg is present it reuses
    # the four libav* targets the decoder already links PRIVATE, adding no new
    # dependency and threading onto consumers' link lines for free (static lib).
    src/MetadataProbe.cpp

    # ReplayGain scanning: the zero-Qt loudness scanner. Decodes through the same
    # DecoderFactory the Engine uses and measures with libebur128; the library
    # header is private to the .cpp (pimpl), so this adds no public include.
    src/ReplayGainScanner.cpp

    # Spectrum analyzer: the zero-Qt DSP transform behind the player's spectrum
    # view. A hand-rolled radix-2 FFT plus log banding, so it pulls in NO new
    # dependency (no FFTW, no kissfft); its public header lets the Qt UI drive it on
    # a timer, and SpectrumAnalyzerTest exercises it through that same header. The
    # real-time tap that feeds it (ScopeTap.h) is header-only and lives beside
    # EngineRingSource, so it needs no entry here.
    src/SpectrumAnalyzer.cpp
)
target_include_directories(rawform_audio
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(rawform_audio PUBLIC cxx_std_20)
target_compile_options(rawform_audio PRIVATE ${_rawform_warnings})

# The codec libraries are implementation details: PRIVATE keeps them off the
# public include interface (the headers are codec-free). Threads is PUBLIC because
# the lib spawns threads and consumers must link the threads runtime to resolve
# them.
target_link_libraries(rawform_audio PRIVATE
    SndFile::sndfile
    FLAC::FLAC
    MPG123::libmpg123
    Ebur128::ebur128)
target_link_libraries(rawform_audio PUBLIC  Threads::Threads)

# ---------------------------------------------------------------------------
# Platform sink. On macOS, compile CoreAudioSink into the library, link the
# CoreAudio family of frameworks PRIVATE (their symbols still thread onto the
# CLI's link line for a static lib), and define RAWFORM_HAVE_COREAUDIO=1 PUBLIC
# so the CLI compiles its play/interactive paths. Off macOS, the macro is 0 and
# those paths become stubs; the library and CLI still build (PipeWireSink covers Linux).
if(APPLE)
    target_sources(rawform_audio PRIVATE src/sinks/CoreAudioSink.cpp)
    target_link_libraries(rawform_audio PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreFoundation")
    target_compile_definitions(rawform_audio PUBLIC RAWFORM_HAVE_COREAUDIO=1)
else()
    target_compile_definitions(rawform_audio PUBLIC RAWFORM_HAVE_COREAUDIO=0)
endif()

# ---------------------------------------------------------------------------
# Platform sink, Linux leg. On non-Apple UNIX, probe pkg-config for
# libpipewire-0.3 (the ABI name PipeWire has kept through its 1.x releases;
# Fedora ships it in pipewire-devel). Found: compile PipeWireSink into the
# library, link PRIVATE (symbols still thread onto consumers' link lines for a
# static lib, and the pimpl keeps the headers out of our public interface), and
# define RAWFORM_HAVE_PIPEWIRE=1 PUBLIC, exactly the RAWFORM_HAVE_COREAUDIO
# shape, so the CLI's transport paths and the UI's sink construction compile
# against it. Not found on a UNIX box: warn loudly (Linux is a production
# target; a silent NullSink UI would read as a rawform bug rather than a
# missing -devel package) but keep building, so non-audio work and CI stay
# green. Everywhere else the macro is simply 0.
set(_rawform_have_pipewire 0)
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    if(PIPEWIRE_FOUND)
        set(_rawform_have_pipewire 1)
    endif()
endif()
if(_rawform_have_pipewire)
    target_sources(rawform_audio PRIVATE src/sinks/PipeWireSink.cpp)
    target_link_libraries(rawform_audio PRIVATE PkgConfig::PIPEWIRE)
    target_compile_definitions(rawform_audio PUBLIC RAWFORM_HAVE_PIPEWIRE=1)
    message(STATUS
        "rawform_audio: PipeWire sink enabled (libpipewire-0.3 ${PIPEWIRE_VERSION})")
else()
    target_compile_definitions(rawform_audio PUBLIC RAWFORM_HAVE_PIPEWIRE=0)
    if(UNIX AND NOT APPLE)
        message(WARNING
            "rawform_audio: libpipewire-0.3 not found; building without an "
            "audio output on this platform (install pipewire-devel or your "
            "distribution's equivalent).")
    endif()
endif()

# ---------------------------------------------------------------------------
# FFmpeg fallback decoder. Same shape as the platform sink: when enabled,
# compile FfmpegDecoder into the library, link the four libav* targets PRIVATE
# (their symbols thread onto consumers' link lines for a static lib, and the
# pimpl keeps the headers out of our public interface), and define
# RAWFORM_HAVE_FFMPEG=1 PUBLIC so DecoderFactory registers the terminal candidate
# and the conformance suite compiles its FFmpeg case. When disabled the macro is
# 0, FfmpegDecoder.cpp is not built, and the library is native-only.
if(_rawform_have_ffmpeg)
    target_sources(rawform_audio PRIVATE src/decoders/FfmpegDecoder.cpp)
    target_link_libraries(rawform_audio PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(rawform_audio PUBLIC RAWFORM_HAVE_FFMPEG=1)
else()
    target_compile_definitions(rawform_audio PUBLIC RAWFORM_HAVE_FFMPEG=0)
endif()

# ---------------------------------------------------------------------------
# The CLI. The info/hash/--dump modes plus the "play <file>" and
# "interactive" transport paths, all driving the Engine. It reaches the Engine
# through its public header and SndfileDecoder / CoreAudioSink through their
# headers under src/, so it adds src/ to its include path and links only the
# library; the RAWFORM_HAVE_COREAUDIO define arrives through the library's
# PUBLIC usage requirements and the sanitizer flags through the directory-scoped
# profile block at the top of this file.
add_executable(rawform_audio_cli cli/main.cpp)
target_link_libraries(rawform_audio_cli PRIVATE rawform_audio)
target_include_directories(rawform_audio_cli
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(rawform_audio_cli PRIVATE ${_rawform_warnings})

# ---------------------------------------------------------------------------
# Tests, registered with CTest.
enable_testing()

# RingBuffer concurrency and accounting.
add_executable(RingBufferTest tests/RingBufferTest.cpp)
target_link_libraries(RingBufferTest PRIVATE rawform_audio Threads::Threads)
target_compile_options(RingBufferTest PRIVATE ${_rawform_warnings})
add_test(NAME RingBufferTest COMMAND RingBufferTest)

# SndfileDecoder round-trips. This test WRITES its fixtures with libsndfile
# directly, so unlike the CLI it links SndFile::sndfile explicitly and includes
# <sndfile.h> in its own translation unit. It also needs src/ on its include path
# to reach the decoder header.
add_executable(SndfileDecoderTest tests/SndfileDecoderTest.cpp)
target_link_libraries(SndfileDecoderTest PRIVATE rawform_audio SndFile::sndfile)
target_include_directories(SndfileDecoderTest
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(SndfileDecoderTest PRIVATE ${_rawform_warnings})
add_test(NAME SndfileDecoderTest COMMAND SndfileDecoderTest)

# The whole Engine, its command queue, state machine, and play queue, driven
# concurrently with the real producer (the engine thread) and a real consumer
# (NullSink's pull thread). No CoreAudio, no libsndfile: an in-memory ramp
# decoder is injected through an IDecoderFactory. This is the ThreadSanitizer
# target for the transport path. It reaches NullSink.h under src/.
add_executable(EngineTest tests/EngineTest.cpp)
target_link_libraries(EngineTest PRIVATE rawform_audio Threads::Threads)
target_include_directories(EngineTest
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(EngineTest PRIVATE ${_rawform_warnings})
add_test(NAME EngineTest COMMAND EngineTest)

# The rate policy truth table. Pure and portable: it exercises
# RateManager::decide() across modes, capability sets, and the gapless gate with no
# sink, no threads, and no audio dependency, so it links only the library (for
# RateManager.cpp) and reaches the policy through the PUBLIC header alone, needing
# no src/ include. The matching engine-level wiring (capabilities -> decide ->
# open and the cross-track gate) is covered inside EngineTest.
add_executable(RateManagerTest tests/RateManagerTest.cpp)
target_link_libraries(RateManagerTest PRIVATE rawform_audio)
target_compile_options(RateManagerTest PRIVATE ${_rawform_warnings})
add_test(NAME RateManagerTest COMMAND RateManagerTest)

# FlacDecoder round-trips. Like SndfileDecoderTest it owns both ends, ENCODING
# its fixtures with libFLAC's stream encoder, so it links FLAC::FLAC explicitly
# and includes <FLAC/stream_encoder.h> in its own translation unit. It needs src/
# on its include path to reach the decoder header.
add_executable(FlacDecoderTest tests/FlacDecoderTest.cpp)
target_link_libraries(FlacDecoderTest PRIVATE rawform_audio FLAC::FLAC)
target_include_directories(FlacDecoderTest
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(FlacDecoderTest PRIVATE ${_rawform_warnings})
add_test(NAME FlacDecoderTest COMMAND FlacDecoderTest)

# Mpg123Decoder. MP3 has no encoder in our dependency set, so this test does
# not round-trip; it embeds two real MPEG streams as byte arrays and asserts
# structure and decode determinism. It still links MPG123::libmpg123 (it could
# decode through the library alone, but linking it explicitly keeps the target
# self-describing and matches the other decoder tests). Needs src/ for the header.
add_executable(Mp3DecoderTest tests/Mp3DecoderTest.cpp)
target_link_libraries(Mp3DecoderTest PRIVATE rawform_audio MPG123::libmpg123)
target_include_directories(Mp3DecoderTest
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(Mp3DecoderTest PRIVATE ${_rawform_warnings})
add_test(NAME Mp3DecoderTest COMMAND Mp3DecoderTest)

# The cross-decoder conformance suite. One identical boundary-contract
# battery (seek-to-end, past-end, EOS reads, short final read, post-seek landing)
# run against EVERY real decoder through the real DecoderFactory, so it doubles as
# a dispatch test and asserts each fixture routes to the decoder it should. It
# CREATES its WAV and FLAC fixtures (hence it links SndFile::sndfile and FLAC::FLAC
# like those decoders' own tests) and writes its MP3 and AAC fixtures from embedded
# byte blobs (no encoder needed, matching Mp3DecoderTest). The AAC case compiles in
# only when the library was built with FFmpeg (the PUBLIC RAWFORM_HAVE_FFMPEG define
# propagates here), and the AAC decode runs inside the library, so this target
# links NO libav* directly. Needs src/ on its include path for DecoderFactory.h.
add_executable(ConformanceTest tests/ConformanceTest.cpp)
target_link_libraries(ConformanceTest PRIVATE
    rawform_audio SndFile::sndfile FLAC::FLAC)
target_include_directories(ConformanceTest
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ConformanceTest PRIVATE ${_rawform_warnings})
add_test(NAME ConformanceTest COMMAND ConformanceTest)

# ReplayGain scanning: the ReplayGainScanner measurement test. Like EngineTest it
# is codec-free, injecting a synthetic IDecoderFactory that emits a known sine or
# silence, so it links only the library (the libebur128 measurement runs inside
# rawform_audio) and reaches the scanner through its PUBLIC header, needing no src/
# include. Assertions are STRUCTURAL (bounded deltas) because loudness is a DSP
# result.
add_executable(ReplayGainScannerTest tests/ReplayGainScannerTest.cpp)
target_link_libraries(ReplayGainScannerTest PRIVATE rawform_audio)
target_compile_options(ReplayGainScannerTest PRIVATE ${_rawform_warnings})
add_test(NAME ReplayGainScannerTest COMMAND ReplayGainScannerTest)

# Spectrum analyzer: the spectrum transform's structural test. Like EngineTest and
# ReplayGainScannerTest it is codec-free and hardware-free, synthesizing a known
# sine or silence in-process and asserting the bar shape (a tone lights its log
# band, silence is flat). It reaches the analyzer through its PUBLIC header alone,
# so it links only the library and needs no src/ include and no codec dependency.
# The hand-rolled FFT means it pulls in nothing new.
add_executable(SpectrumAnalyzerTest tests/SpectrumAnalyzerTest.cpp)
target_link_libraries(SpectrumAnalyzerTest PRIVATE rawform_audio)
target_compile_options(SpectrumAnalyzerTest PRIVATE ${_rawform_warnings})
add_test(NAME SpectrumAnalyzerTest COMMAND SpectrumAnalyzerTest)
