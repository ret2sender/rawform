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

// SndfileDecoder.cpp
//
// libsndfile implementation of the IDecoder contract. Everything that touches
// <sndfile.h> is confined to this translation unit: the SNDFILE handle and the
// SF_INFO it was opened with live in Impl, which the header forward-declares
// only. That is the whole point of the pimpl, it lets the static library link
// SndFile::sndfile privately while the header stays free of the dependency.

#include "decoders/SndfileDecoder.h"

#include "ChannelPositions.h"
#include "rawform/audio/Types.h"

#include <sndfile.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>  // SEEK_SET
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// libsndfile declares its SF_FORMAT_* codes as ENUM values, not preprocessor
// macros, so an #ifdef on one is always false and cannot guard a case label. We
// also want to compile against libsndfile versions old enough to predate the
// Opus enum entirely (a distribution's packaged libsndfile may be one). Both
// are solved by defining the name ourselves when no macro exists: the format
// ABI, so SF_FORMAT_OPUS is 0x0064 everywhere. On a current libsndfile the enum
// already provides the value and this macro simply shadows it with the identical
// number; on an old one it supplies a value the headers lack. Either way the
// Opus case below compiles and matches whatever subtype the runtime reports.
#ifndef SF_FORMAT_OPUS
#define SF_FORMAT_OPUS 0x0064
#endif

namespace rawform::audio {

namespace {

// Maps a libsndfile format word to the source container bit depth. We mask off
// everything but the subtype, then translate the linear-PCM and IEEE subtypes.
// Anything else returns 0, meaning no PCM container depth: the compressed
// subtypes (Vorbis, Opus) are still named by codecForSubtype below, they just
// carry no integer depth, while a genuinely unrecognized subtype ends up
// Codec::Unknown. The decode path itself does not depend on this: sf_readf_float
// converts every supported subtype to float32 regardless, so an unclassified
// subtype still decodes correctly, it just does not get a bit depth or a computed
// bitrate on the status line.
std::uint16_t bitsForSubtype(int sfFormat) noexcept {
    switch (sfFormat & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_PCM_S8:
        case SF_FORMAT_PCM_U8:  return 8;
        case SF_FORMAT_PCM_16:  return 16;
        case SF_FORMAT_PCM_24:  return 24;
        case SF_FORMAT_PCM_32:
        case SF_FORMAT_FLOAT:   return 32;
        case SF_FORMAT_DOUBLE:  return 64;
        default:                return 0;
    }
}

// Classifies a libsndfile subtype into a descriptive Codec. PCM and IEEE
// subtypes (depth != 0) are Codec::Pcm. The compressed subtypes libsndfile
// decodes as secondary formats on builds with the matching libraries (Ogg
// Vorbis, Opus) get their own tag so the status line names them instead of
// showing Unknown; they reach this decoder only on a build without FFmpeg,
// since an FFmpeg build declines them in open() below so FfmpegDecoder serves
// them with a live bitrate. There is no container bit depth for these lossy
// formats and libsndfile does not expose an average bitrate for them, so
// bitsPerSample and bitrate stay 0; only the name is recovered. FLAC and MP3
// are intentionally absent: the factory excludes libsndfile for those by
// content, so their reference decoders always win and libsndfile never reports
// them here. Opus relies on the SF_FORMAT_OPUS fallback define above, so it
// classifies even against an older header.
Codec codecForSubtype(int sfFormat, std::uint16_t bits) noexcept {
    switch (sfFormat & SF_FORMAT_SUBMASK) {
        case SF_FORMAT_VORBIS:  return Codec::Vorbis;
        case SF_FORMAT_OPUS:    return Codec::Opus;
        default:                break;
    }
    return bits != 0 ? Codec::Pcm : Codec::Unknown;
}

// The WAVEFORMATEXTENSIBLE bit of one libsndfile channel-map entry, 0 for a
// speaker the position table has no bit for (mono, ambisonic components, any
// other speaker the table does not name), which leaves the whole file without
// a mask.
// Apple's names and Microsoft's for the same speaker give the same bit, since
// which pair libsndfile reports is its own choice. The entries are plain ints
// from an unnamed enum, hence the default label.
std::uint64_t speakerBit(int sfChannel) noexcept {
    switch (sfChannel) {
        case SF_CHANNEL_MAP_LEFT:
        case SF_CHANNEL_MAP_FRONT_LEFT:   return channelBit(ChannelPosition::FrontLeft);
        case SF_CHANNEL_MAP_RIGHT:
        case SF_CHANNEL_MAP_FRONT_RIGHT:  return channelBit(ChannelPosition::FrontRight);
        case SF_CHANNEL_MAP_CENTER:
        case SF_CHANNEL_MAP_FRONT_CENTER: return channelBit(ChannelPosition::FrontCenter);
        case SF_CHANNEL_MAP_LFE:          return channelBit(ChannelPosition::Lfe);
        case SF_CHANNEL_MAP_REAR_LEFT:    return channelBit(ChannelPosition::BackLeft);
        case SF_CHANNEL_MAP_REAR_RIGHT:   return channelBit(ChannelPosition::BackRight);
        case SF_CHANNEL_MAP_REAR_CENTER:  return channelBit(ChannelPosition::BackCenter);
        case SF_CHANNEL_MAP_SIDE_LEFT:    return channelBit(ChannelPosition::SideLeft);
        case SF_CHANNEL_MAP_SIDE_RIGHT:   return channelBit(ChannelPosition::SideRight);
        case SF_CHANNEL_MAP_FRONT_LEFT_OF_CENTER:
            return channelBit(ChannelPosition::FrontLeftOfCenter);
        case SF_CHANNEL_MAP_FRONT_RIGHT_OF_CENTER:
            return channelBit(ChannelPosition::FrontRightOfCenter);
        case SF_CHANNEL_MAP_TOP_CENTER:   return channelBit(ChannelPosition::TopCenter);
        case SF_CHANNEL_MAP_TOP_FRONT_LEFT:
            return channelBit(ChannelPosition::TopFrontLeft);
        case SF_CHANNEL_MAP_TOP_FRONT_CENTER:
            return channelBit(ChannelPosition::TopFrontCenter);
        case SF_CHANNEL_MAP_TOP_FRONT_RIGHT:
            return channelBit(ChannelPosition::TopFrontRight);
        case SF_CHANNEL_MAP_TOP_REAR_LEFT:
            return channelBit(ChannelPosition::TopBackLeft);
        case SF_CHANNEL_MAP_TOP_REAR_CENTER:
            return channelBit(ChannelPosition::TopBackCenter);
        case SF_CHANNEL_MAP_TOP_REAR_RIGHT:
            return channelBit(ChannelPosition::TopBackRight);
        default:                          return 0;
    }
}

using Row = std::array<ChannelPosition, kMaxDefaultOrderChannels>;

// The Vorbis I channel order, which Opus channel mapping 1 shares: row n-2 holds
// the n-channel order, for 2 to 8 channels; unused tail slots are never read.
// libsndfile hands Vorbis and Opus channels back as libvorbis and libopus decode
// them, in this order, and reports no channel map for them, so this table is
// what tells the decoder where they go.
constexpr std::array<Row, kMaxDefaultOrderChannels - 1> kVorbisOrder{{
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontCenter,
     ChannelPosition::FrontRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight,
     ChannelPosition::BackLeft, ChannelPosition::BackRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontCenter,
     ChannelPosition::FrontRight, ChannelPosition::BackLeft,
     ChannelPosition::BackRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontCenter,
     ChannelPosition::FrontRight, ChannelPosition::BackLeft,
     ChannelPosition::BackRight, ChannelPosition::Lfe},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontCenter,
     ChannelPosition::FrontRight, ChannelPosition::SideLeft,
     ChannelPosition::SideRight, ChannelPosition::BackCenter, ChannelPosition::Lfe},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontCenter,
     ChannelPosition::FrontRight, ChannelPosition::SideLeft,
     ChannelPosition::SideRight, ChannelPosition::BackLeft,
     ChannelPosition::BackRight, ChannelPosition::Lfe},
}};

// ---------------------------------------------------------------------------
// Header facts libsndfile does not report, read from the file itself. Each
// reader looks at a bounded number of chunks, reads a payload only as far as it
// needs, and gives up with nothing on anything it does not expect, which
// leaves the file as libsndfile alone describes it.

using FilePtr = std::unique_ptr<std::FILE, int (*)(std::FILE*)>;

FilePtr openForHeader(const std::string& path) {
    return {std::fopen(path.c_str(), "rb"), &std::fclose};
}

bool readExact(std::FILE* f, unsigned char* out, std::size_t n) {
    return std::fread(out, 1, n, f) == n;
}

std::uint32_t be32(const unsigned char* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

std::uint64_t be64(const unsigned char* p) noexcept {
    return (std::uint64_t{be32(p)} << 32) | be32(p + 4);
}

// Enough for any real file's chunks before its channel layout.
constexpr int kMaxHeaderChunks = 64;

// Skip a chunk payload of size bytes, padded to an even size when pad is set.
bool skipChunk(std::FILE* f, std::uint64_t size, bool pad) {
    const std::uint64_t span = size + (pad ? (size & 1u) : 0u);
    if (span > static_cast<std::uint64_t>(std::numeric_limits<long>::max())) {
        return false;
    }
    return std::fseek(f, static_cast<long>(span), SEEK_CUR) == 0;
}

// The first bytes (at most maxBytes) of a chunk payload of size bytes.
std::vector<unsigned char> readPayload(std::FILE* f, std::uint64_t size,
                                       std::size_t maxBytes) {
    std::vector<unsigned char> payload(
        static_cast<std::size_t>(std::min<std::uint64_t>(size, maxBytes)));
    if (!readExact(f, payload.data(), payload.size())) {
        payload.clear();
    }
    return payload;
}

// The payload of a CAF file's channel layout chunk ('chan'): big-endian 64-bit
// chunk sizes, and a data chunk of size -1 runs to the end of the file.
std::vector<unsigned char> cafLayoutChunk(std::FILE* f, std::size_t maxBytes) {
    std::array<unsigned char, 12> head{};
    if (!readExact(f, head.data(), 8) || std::memcmp(head.data(), "caff", 4) != 0) {
        return {};
    }
    for (int i = 0; i < kMaxHeaderChunks && readExact(f, head.data(), 12); ++i) {
        const std::uint64_t size = be64(head.data() + 4);
        if (std::memcmp(head.data(), "chan", 4) == 0) {
            return readPayload(f, size, maxBytes);
        }
        if (!skipChunk(f, size, /*pad=*/false)) {
            break;
        }
    }
    return {};
}

// The payload of an AIFF or AIFF-C file's channel layout chunk ('CHAN', Apple's
// addition): big-endian 32-bit chunk sizes, each payload padded to even.
std::vector<unsigned char> aiffLayoutChunk(std::FILE* f, std::size_t maxBytes) {
    std::array<unsigned char, 12> head{};
    if (!readExact(f, head.data(), 12) || std::memcmp(head.data(), "FORM", 4) != 0 ||
        (std::memcmp(head.data() + 8, "AIFF", 4) != 0 &&
         std::memcmp(head.data() + 8, "AIFC", 4) != 0)) {
        return {};
    }
    for (int i = 0; i < kMaxHeaderChunks && readExact(f, head.data(), 8); ++i) {
        const std::uint64_t size = be32(head.data() + 4);
        if (std::memcmp(head.data(), "CHAN", 4) == 0) {
            return readPayload(f, size, maxBytes);
        }
        if (!skipChunk(f, size, /*pad=*/true)) {
            break;
        }
    }
    return {};
}

// CoreAudio's layout tags for a layout that names its speakers one by one
// (CoreAudioBaseTypes.h): every other tag names a whole layout, which is
// libsndfile's to read.
constexpr std::uint32_t kLayoutTagUseChannelDescriptions = 0;
constexpr std::uint32_t kLayoutTagUseChannelBitmap       = 1u << 16;

// Bytes of a channel layout payload before its descriptions, and of each one.
constexpr std::size_t kLayoutHeaderBytes      = 12;
constexpr std::size_t kLayoutDescriptionBytes = 20;

// The speakers of a CAF or AIFF channel layout given as a channel bitmap or as
// one description per channel, in stream order, from the chunk's payload
// (CoreAudio's AudioChannelLayout, big-endian). libsndfile reads layout tags
// only, so it reports no map for these. A bitmap's bits are the
// WAVEFORMATEXTENSIBLE bits, and it names its channels in ascending bit order.
// Labels 1 to 18 are the same speakers in the same order; any other label names
// a speaker with no such bit. False for any other tag, or for a layout that
// names a different number of channels.
bool appleLayoutSpeakers(std::span<const unsigned char> payload,
                         std::span<std::uint64_t>       speakers) {
    const std::size_t count = speakers.size();
    if (payload.size() < kLayoutHeaderBytes) {
        return false;
    }
    const std::uint32_t tag = be32(payload.data());
    if (tag == kLayoutTagUseChannelBitmap) {
        const std::uint32_t bitmap = be32(payload.data() + 4);
        if (static_cast<std::size_t>(std::popcount(bitmap)) != count) {
            return false;
        }
        std::size_t c = 0;
        for (unsigned bit = 0; bit < 32; ++bit) {
            if ((bitmap & (1u << bit)) != 0) {
                speakers[c++] = std::uint64_t{1} << bit;
            }
        }
        return true;
    }
    if (tag == kLayoutTagUseChannelDescriptions) {
        if (be32(payload.data() + 8) != count ||
            payload.size() < kLayoutHeaderBytes + count * kLayoutDescriptionBytes) {
            return false;
        }
        for (std::size_t c = 0; c < count; ++c) {
            const std::uint32_t label =
                be32(payload.data() + kLayoutHeaderBytes + c * kLayoutDescriptionBytes);
            speakers[c] = label >= 1 && label <= 18 ? std::uint64_t{1} << (label - 1) : 0;
        }
        return true;
    }
    return false;
}

// The speakers a CAF or AIFF file names in a channel layout chunk that
// libsndfile does not read (appleLayoutSpeakers); false when it has none.
bool layoutChunkSpeakers(const std::string& path, int major,
                         std::span<std::uint64_t> speakers) {
    const FilePtr f = openForHeader(path);
    if (!f) {
        return false;
    }
    const std::size_t maxBytes =
        kLayoutHeaderBytes + speakers.size() * kLayoutDescriptionBytes;
    const std::vector<unsigned char> payload = major == SF_FORMAT_CAF
                                                   ? cafLayoutChunk(f.get(), maxBytes)
                                                   : aiffLayoutChunk(f.get(), maxBytes);
    return appleLayoutSpeakers(payload, speakers);
}

// The channel mapping family of an Ogg Opus file: byte 18 of its ID header
// (OpusHead), which RFC 7845 puts alone on the first page. Empty when the first
// page does not hold one.
std::optional<unsigned> opusMappingFamily(const std::string& path) {
    const FilePtr f = openForHeader(path);
    std::array<unsigned char, 27>  page{};
    std::array<unsigned char, 255> lacing{};
    std::array<unsigned char, 19>  head{};
    if (!f || !readExact(f.get(), page.data(), page.size()) ||
        std::memcmp(page.data(), "OggS", 4) != 0) {
        return std::nullopt;
    }
    const std::size_t segments = page[26];
    if (!readExact(f.get(), lacing.data(), segments)) {
        return std::nullopt;
    }
    std::size_t length = 0;
    for (std::size_t i = 0; i < segments; ++i) {
        length += lacing[i];
    }
    if (length < head.size() || !readExact(f.get(), head.data(), head.size()) ||
        std::memcmp(head.data(), "OpusHead", 8) != 0) {
        return std::nullopt;
    }
    return head[18];
}

// Whether an Ogg Opus file's channels come in Vorbis order. Mapping families 0
// (mono or stereo) and 1 define it. Any other family (255, which defines no
// positions; 2 and 3, ambisonics) does not, and its channels keep their stream
// order with no mask, as FFmpeg delivers them. A first page with no ID header is
// taken as family 1, the one family that names the speakers of a layout.
bool opusInVorbisOrder(const std::string& path) {
    const std::optional<unsigned> family = opusMappingFamily(path);
    return !family || *family <= 1;
}

// The speakers a file names, in the order sf_readf_float delivers its channels.
// A header's channel map comes first: WAVE's channel mask (WAVEX, RF64, W64) or
// a CAF or AIFF channel layout chunk, which need not list the channels in
// ascending order. libsndfile reads a layout chunk's tag only, so a layout given
// as a bitmap or as descriptions is read from the file here. Ogg Vorbis and Opus
// carry no map but a fixed order, the Vorbis one, for 2 to 8 channels, unless an
// Opus file's mapping family defines none. No mask and no reorder for any other
// file, and for anything the table cannot declare.
ChannelOrder channelOrderOf(SNDFILE* snd, const SF_INFO& info, const std::string& path) {
    const int channels = info.channels;
    if (channels < 2 || channels > kMaxPositionedChannels) {
        return {};
    }
    const auto count   = static_cast<std::size_t>(channels);
    const int  major   = info.format & SF_FORMAT_TYPEMASK;
    const int  subtype = info.format & SF_FORMAT_SUBMASK;
    std::array<int, kMaxPositionedChannels>           map{};
    std::array<std::uint64_t, kMaxPositionedChannels> speakers{};
    const std::span<std::uint64_t> named = std::span(speakers).first(count);
    if (sf_command(snd, SFC_GET_CHANNEL_MAP_INFO, map.data(),
                   static_cast<int>(sizeof(int)) * channels) == SF_TRUE) {
        std::ranges::transform(std::span(map).first(count), named.begin(), speakerBit);
    } else if (count <= kMaxDefaultOrderChannels &&
               (subtype == SF_FORMAT_VORBIS ||
                (subtype == SF_FORMAT_OPUS && opusInVorbisOrder(path)))) {
        std::ranges::transform(std::span(kVorbisOrder[count - 2]).first(count),
                               named.begin(), channelBit);
    } else if ((major == SF_FORMAT_CAF || major == SF_FORMAT_AIFF) &&
               layoutChunkSpeakers(path, major, named)) {
        // named holds the layout chunk's speakers.
    } else {
        return {};
    }
    return channelOrderFor(named);
}

}  // namespace

// ---------------------------------------------------------------------------
// The pimpl. It owns the SNDFILE handle and closes it in its destructor, so
// SndfileDecoder's own destructor can be the defaulted one (defined below, where
// Impl is a complete type). The descriptive facts are computed once in open()
// and cached here, keeping the accessors branch-free trivial returns.
struct SndfileDecoder::Impl {
    SNDFILE*      snd      = nullptr;
    int           channels = 0;      // cached for the read() interleave math
    ChannelOrder  order{};           // the channel map's reorder, applied in read()
    AudioFormat   fmt{};
    SourceInfo    src{};
    std::uint64_t total    = 0;
    bool          seekable = false;
    std::string   lastError;         // set by a failed read, cleared by a seek

    ~Impl() {
        if (snd) {
            sf_close(snd);
        }
    }
};

// ---------------------------------------------------------------------------
// Factory. Non-throwing: returns a fully-opened decoder or nullptr, never a
// half-open object. On failure error (if provided) carries libsndfile's reason.
std::unique_ptr<IDecoder> SndfileDecoder::open(const std::string& path,
                                               std::string* error) {
    SF_INFO info{};  // zero-initialized; libsndfile fills it on a read open
    SNDFILE* snd = sf_open(path.c_str(), SFM_READ, &info);
    if (!snd) {
        if (error) {
            *error = sf_strerror(nullptr);  // global last-error after open failure
        }
        return nullptr;
    }

    // Defensive: a real WAV/AIFF always reports positive values, but reject a
    // degenerate header rather than carry a zero channel count into read().
    if (info.channels <= 0 || info.samplerate <= 0) {
        if (error) {
            *error = "libsndfile reported a non-positive channel or sample rate";
        }
        sf_close(snd);
        return nullptr;
    }

    // Routing policy: Vorbis and Opus are Ogg-contained codecs that a
    // current libsndfile can decode, but the project routes them to FfmpegDecoder
    // (see the format list in FfmpegDecoder.h). FFmpeg wraps the reference
    // libvorbis/libopus and, through its packet-level metering, reports a live VBR
    // bitrate; libsndfile exposes no compressed-stream bitrate, so served here they
    // would read zero. Decline them so the DecoderFactory cascade falls through to
    // its terminal FFmpeg candidate. Gated on FFmpeg actually being in the build:
    // with no FFmpeg to catch them, declining would leave these files unplayable,
    // so a no-FFmpeg build keeps serving them, at a zero bitrate. Every other
    // subtype, the PCM containers (WAV/AIFF/AU/CAF) among them, is still handled
    // here. Ogg FLAC is not offered to this decoder: the factory sniffs it and
    // routes it to FlacDecoder.
#if RAWFORM_HAVE_FFMPEG
    {
        const int subtype = info.format & SF_FORMAT_SUBMASK;
        if (subtype == SF_FORMAT_VORBIS || subtype == SF_FORMAT_OPUS) {
            sf_close(snd);
            if (error) {
                *error = "Vorbis/Opus deferred to FfmpegDecoder";
            }
            return nullptr;
        }
    }
#endif

    // Force normalized float reads so integer PCM maps to [-1, 1) deterministically
    // regardless of how this libsndfile was built. Without this, the integer to
    // float scale would depend on the library's compile-time default, which is
    // exactly the kind of hidden variable the project's deterministic-verification
    // philosophy rules out. This is what makes the test's one-LSB tolerance hold.
    sf_command(snd, SFC_SET_NORM_FLOAT, nullptr, SF_TRUE);

    auto impl      = std::make_unique<Impl>();
    impl->snd      = snd;
    impl->channels = info.channels;
    impl->order    = channelOrderOf(snd, info, path);
    impl->fmt      = AudioFormat{
        .sampleRate  = static_cast<std::uint32_t>(info.samplerate),
        .channels    = static_cast<std::uint16_t>(info.channels),
        .channelMask = impl->order.mask};
    impl->total    = info.frames > 0 ? static_cast<std::uint64_t>(info.frames) : 0;
    impl->seekable = info.seekable != 0;

    const std::uint16_t bits = bitsForSubtype(info.format);
    impl->src.codec          = codecForSubtype(info.format, bits);
    impl->src.decoder        = DecoderKind::Sndfile;
    impl->src.bitsPerSample  = bits;
    // 64-bit intermediate so the multiply cannot overflow before the divide;
    // for PCM this is the exact data rate (see the SourceInfo comment in Types.h).
    impl->src.bitrateKbps    = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(info.samplerate) *
         static_cast<std::uint64_t>(info.channels)   *
         static_cast<std::uint64_t>(bits)) / 1000u);

    // Private constructor: only this factory can call it, so the object is born
    // valid. make_unique cannot reach a private constructor, hence the raw new
    // wrapped immediately into the owning unique_ptr.
    return std::unique_ptr<IDecoder>(new SndfileDecoder(std::move(impl)));
}

SndfileDecoder::SndfileDecoder(std::unique_ptr<Impl> impl) noexcept
    : m_impl(std::move(impl)) {}

// Defaulted here, where Impl is complete, so ~Impl (which closes the handle) is
// visible to the unique_ptr deleter.
SndfileDecoder::~SndfileDecoder() = default;

// ---------------------------------------------------------------------------
// Accessors: trivial reads of the facts cached at open time.
AudioFormat   SndfileDecoder::format()      const { return m_impl->fmt; }
SourceInfo    SndfileDecoder::sourceInfo()  const { return m_impl->src; }
std::uint64_t SndfileDecoder::totalFrames() const { return m_impl->total; }
bool          SndfileDecoder::seekable()    const { return m_impl->seekable; }
std::string   SndfileDecoder::lastError()   const { return m_impl->lastError; }

// ---------------------------------------------------------------------------
// Decode. Honors the "fill fully, short only at EOS, 0 == EOS" contract by
// accumulating across sf_readf_float calls until the request is satisfied or the
// library returns nothing more. For a regular file libsndfile satisfies the
// request in a single call until the final partial block, but looping costs
// nothing and makes the EOS semantics independent of libsndfile's chunking.
// Each block then moves to ascending speaker order when the file's channels come
// in another order (a channel map that says so, Ogg Vorbis and Opus), so they
// arrive in the order format()'s mask describes.
std::size_t SndfileDecoder::read(float* dst, std::size_t frames) {
    if (frames == 0) {
        return 0;
    }
    const auto ch = static_cast<std::size_t>(m_impl->channels);
    std::size_t done = 0;
    while (done < frames) {
        const auto want = static_cast<sf_count_t>(frames - done);
        const sf_count_t got = sf_readf_float(m_impl->snd, dst + done * ch, want);
        if (got <= 0) {
            // 0 is end of stream. A negative is a read error, also surfaced
            // as a short read so the caller stops cleanly, with libsndfile's
            // message kept for the engine's report.
            if (got < 0) {
                m_impl->lastError = sf_strerror(m_impl->snd);
            }
            break;
        }
        reorderChannels(dst + done * ch, static_cast<std::size_t>(got), m_impl->order);
        done += static_cast<std::size_t>(got);
    }
    return done;
}

// ---------------------------------------------------------------------------
// Seek to an absolute frame. Returns true only when libsndfile lands exactly
// where asked. An unseekable source short-circuits to false up front; sf_seek
// would fail anyway, but the explicit guard documents the contract at the call.
bool SndfileDecoder::seek(std::uint64_t frame) {
    if (!m_impl->seekable) {
        return false;
    }
    const auto target = static_cast<sf_count_t>(frame);
    const sf_count_t result = sf_seek(m_impl->snd, target, SEEK_SET);
    if (result != target) {
        return false;
    }
    m_impl->lastError.clear();  // a successful seek is a fresh start
    return true;
}

}  // namespace rawform::audio
