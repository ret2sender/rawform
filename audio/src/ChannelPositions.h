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

// ChannelPositions.h
//
// The speaker position of each interleaved channel, which the sinks declare when
// they describe the stream to the platform. A stream's speakers travel in
// AudioFormat::channelMask: a set of WAVEFORMATEXTENSIBLE speaker bits, with the
// channels interleaved in ascending bit order. A mask of 0 means the default
// order for the channel count, the table below: for 1 to 8 channels FLAC's
// channel assignment and WAVE's default order agree. Beyond 8 channels there is
// no default order, so such a stream is positioned only when its source names
// every speaker, up to the 18 a mask can name. A decoder whose source
// names its speakers (FFmpeg's channel layout, a FLAC file's
// WAVEFORMATEXTENSIBLE_CHANNEL_MASK comment, the channel map libsndfile reads
// from a WAVE, CAF or AIFF header) reports them when they differ from that
// order, through the one rule in sourceChannelMask below. So a 4.0 source (FL FR
// FC BC) is not declared as quad (FL FR BL BR), nor AC3's side surrounds (SL SR)
// as back surrounds (BL BR). A source whose channels come in some other order
// than ascending bit order (a CAF or AIFF layout such as C L R Cs, an FFmpeg
// custom order, Ogg Vorbis or Opus as libsndfile decodes them, in the Vorbis
// order L C R) is moved to it as it is decoded (channelOrderFor,
// reorderChannels): the samples only change places, so a mask always describes
// the order the channels arrive in. A later block that names the same speakers
// in yet another order moves to the declared one the same way
// (channelOrderBetween). The layout rides in AudioFormat rather than beside it
// because every path that must agree on it already carries one: the gapless
// stitch gate, the hold gate, the sink's open and reconfigure.
//
// Why the sinks need it: a graph told "8 unpositioned channels" cannot mix. On
// PipeWire an unpositioned 5.1 stream on a stereo sink keeps FL and FR and drops
// the center, the LFE and the surrounds; with positions declared the channel
// mixer downmixes them. On CoreAudio the AUHAL does not downmix at all, and with
// an explicit channel map it does not route by the declared layout either, so the
// sink builds that map itself from the device's speaker configuration
// (routeToDevice); a declared mono stream is mapped onto both device channels.
//
// Private to the audio library: the public headers carry the mask as a plain
// integer and stay free of this vocabulary, a sink implementation detail shared
// by two sinks.

#pragma once

#include "rawform/audio/Types.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace rawform::audio {

/// Platform-neutral speaker positions. Each sink maps these onto its own
/// enumeration (SPA_AUDIO_CHANNEL_*, kAudioChannelLabel_*).
enum class ChannelPosition : std::uint8_t {
    Mono,
    FrontLeft,
    FrontRight,
    FrontCenter,
    Lfe,
    BackLeft,
    BackRight,
    BackCenter,
    SideLeft,
    SideRight,
    FrontLeftOfCenter,
    FrontRightOfCenter,
    TopCenter,
    TopFrontLeft,
    TopFrontCenter,
    TopFrontRight,
    TopBackLeft,
    TopBackCenter,
    TopBackRight,
};

/// The largest channel count with a default order, the table below. Beyond it a
/// stream is positioned only by a mask.
inline constexpr std::uint16_t kMaxDefaultOrderChannels = 8;

/// The largest channel count a sink declares positions for: the number of
/// distinct speakers a mask can name. Beyond it a sink declares the stream
/// unpositioned.
inline constexpr std::uint16_t kMaxPositionedChannels = 18;

/// The position's WAVEFORMATEXTENSIBLE speaker bit, its bit in
/// AudioFormat::channelMask. Mono has none: it exists only as the one-channel
/// row of the table.
[[nodiscard]] constexpr std::uint32_t channelBit(ChannelPosition p) noexcept {
    switch (p) {
        case ChannelPosition::Mono:        return 0x0000;
        case ChannelPosition::FrontLeft:   return 0x0001;
        case ChannelPosition::FrontRight:  return 0x0002;
        case ChannelPosition::FrontCenter: return 0x0004;
        case ChannelPosition::Lfe:         return 0x0008;
        case ChannelPosition::BackLeft:    return 0x0010;
        case ChannelPosition::BackRight:   return 0x0020;
        case ChannelPosition::BackCenter:  return 0x0100;
        case ChannelPosition::SideLeft:    return 0x0200;
        case ChannelPosition::SideRight:   return 0x0400;
        case ChannelPosition::FrontLeftOfCenter:  return 0x0040;
        case ChannelPosition::FrontRightOfCenter: return 0x0080;
        case ChannelPosition::TopCenter:          return 0x0800;
        case ChannelPosition::TopFrontLeft:       return 0x1000;
        case ChannelPosition::TopFrontCenter:     return 0x2000;
        case ChannelPosition::TopFrontRight:      return 0x4000;
        case ChannelPosition::TopBackLeft:        return 0x8000;
        case ChannelPosition::TopBackCenter:      return 0x10000;
        case ChannelPosition::TopBackRight:       return 0x20000;
    }
    return 0;
}

/// The positions of an interleaved stream's channels, in order. Held by value:
/// a mask's positions are not a row of the table, so there is no static storage
/// to point into. Empty means "declare the stream unpositioned".
class ChannelPositionList {
public:
    [[nodiscard]] constexpr std::size_t size() const noexcept { return m_count; }
    [[nodiscard]] constexpr bool empty() const noexcept { return m_count == 0; }
    [[nodiscard]] constexpr ChannelPosition operator[](std::size_t i) const noexcept {
        return m_slots[i];
    }
    [[nodiscard]] constexpr const ChannelPosition* begin() const noexcept {
        return m_slots.data();
    }
    [[nodiscard]] constexpr const ChannelPosition* end() const noexcept {
        return m_slots.data() + m_count;
    }
    /// Callers stay within kMaxPositionedChannels; channelPositionsFor does.
    constexpr void push_back(ChannelPosition p) noexcept { m_slots[m_count++] = p; }

private:
    std::array<ChannelPosition, kMaxPositionedChannels> m_slots{};
    std::size_t                                         m_count = 0;
};

namespace detail {

using PositionRow = std::array<ChannelPosition, kMaxDefaultOrderChannels>;

// Row n-1 holds the n-channel order; unused tail slots are never read.
inline constexpr std::array<PositionRow, kMaxDefaultOrderChannels> kPositionRows{{
    {ChannelPosition::Mono},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight,
     ChannelPosition::FrontCenter},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight,
     ChannelPosition::BackLeft, ChannelPosition::BackRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight,
     ChannelPosition::FrontCenter, ChannelPosition::BackLeft,
     ChannelPosition::BackRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight,
     ChannelPosition::FrontCenter, ChannelPosition::Lfe,
     ChannelPosition::BackLeft, ChannelPosition::BackRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight,
     ChannelPosition::FrontCenter, ChannelPosition::Lfe,
     ChannelPosition::BackCenter, ChannelPosition::SideLeft,
     ChannelPosition::SideRight},
    {ChannelPosition::FrontLeft, ChannelPosition::FrontRight,
     ChannelPosition::FrontCenter, ChannelPosition::Lfe,
     ChannelPosition::BackLeft, ChannelPosition::BackRight,
     ChannelPosition::SideLeft, ChannelPosition::SideRight},
}};

// The positions a mask can name, in ascending bit order: the order the channels
// of a masked stream are interleaved in.
inline constexpr std::array<ChannelPosition, kMaxPositionedChannels> kMaskOrder{{
    ChannelPosition::FrontLeft,         ChannelPosition::FrontRight,
    ChannelPosition::FrontCenter,       ChannelPosition::Lfe,
    ChannelPosition::BackLeft,          ChannelPosition::BackRight,
    ChannelPosition::FrontLeftOfCenter, ChannelPosition::FrontRightOfCenter,
    ChannelPosition::BackCenter,        ChannelPosition::SideLeft,
    ChannelPosition::SideRight,         ChannelPosition::TopCenter,
    ChannelPosition::TopFrontLeft,      ChannelPosition::TopFrontCenter,
    ChannelPosition::TopFrontRight,     ChannelPosition::TopBackLeft,
    ChannelPosition::TopBackCenter,     ChannelPosition::TopBackRight,
}};

}  // namespace detail

/// Every bit a mask may carry; any other bit is a speaker this table cannot
/// name, and a decoder whose source has one reports no mask.
inline constexpr std::uint32_t kNamedChannelMask = [] {
    std::uint32_t mask = 0;
    for (const ChannelPosition p : detail::kMaskOrder) {
        mask |= channelBit(p);
    }
    return mask;
}();

/// The mask a channel count's default order stands for: the bits of its table
/// row. 0 for mono, for no channels, and beyond the table. A decoder compares a
/// source's mask against it and reports 0 when they agree, so one layout has
/// one representation.
[[nodiscard]] constexpr std::uint32_t
defaultChannelMask(std::uint16_t channels) noexcept {
    if (channels == 0 || channels > kMaxDefaultOrderChannels) {
        return 0;
    }
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < channels; ++i) {
        mask |= channelBit(detail::kPositionRows[channels - 1u][i]);
    }
    return mask;
}

/// The mask a decoder reports for a source that names its speakers as a mask,
/// with its channels in ascending bit order: the source's mask when it differs
/// from the count's default order, and 0 otherwise. 0 also for anything the
/// table cannot declare: fewer than 2 or more than kMaxPositionedChannels
/// channels, a speaker the table cannot name, or a bit count that disagrees
/// with the channel count. Beyond kMaxDefaultOrderChannels there is no default
/// order to agree with, so the mask is always reported. Every decoder goes
/// through this one rule, so two sources with the same speakers carry the same
/// mask and compare equal at the stitch and hold gates.
[[nodiscard]] constexpr std::uint32_t
sourceChannelMask(std::uint64_t speakers, std::uint16_t channels) noexcept {
    if (channels < 2 || channels > kMaxPositionedChannels ||
        (speakers & ~std::uint64_t{kNamedChannelMask}) != 0 ||
        std::popcount(speakers) != channels) {
        return 0;
    }
    const auto mask = static_cast<std::uint32_t>(speakers);
    return mask == defaultChannelMask(channels) ? 0 : mask;
}

/// What a decoder does with a source that names a speaker for each channel.
struct ChannelOrder {
    /// For AudioFormat::channelMask, by sourceChannelMask's rule.
    std::uint32_t mask = 0;
    /// Whether the channels must move to reach ascending bit order.
    bool reorder = false;
    /// The stream's channel count, and the source channel each output channel
    /// takes: output channel i is source channel from[i]. Read only when
    /// reorder is set.
    std::uint16_t                                     channels = 0;
    std::array<std::uint8_t, kMaxPositionedChannels> from{};
};

/// The order for a source that names one speaker per channel, in stream order:
/// each entry is the speaker's WAVEFORMATEXTENSIBLE bit, 0 for a speaker that
/// has none. When every entry is a single named bit and none repeats, the mask
/// is their set by sourceChannelMask's rule, and channels that do not already
/// come in ascending bit order (the order a mask describes) are to be moved
/// there; a set equal to the count's default order then reports mask 0 like any
/// other. Any other list (a speaker the table cannot name, a speaker with no
/// bit, a repeated one) gets mask 0 and keeps its source order, so it keeps the
/// default order's positions.
[[nodiscard]] constexpr ChannelOrder
channelOrderFor(std::span<const std::uint64_t> speakers) noexcept {
    const std::size_t count = speakers.size();
    if (count < 2 || count > kMaxPositionedChannels) {
        return {};
    }
    std::uint64_t set = 0;
    for (const std::uint64_t bit : speakers) {
        if (std::popcount(bit) != 1 || (set & bit) != 0) {
            return {};
        }
        set |= bit;
    }
    if ((set & ~std::uint64_t{kNamedChannelMask}) != 0) {
        return {};
    }
    ChannelOrder order;
    order.mask     = sourceChannelMask(set, static_cast<std::uint16_t>(count));
    order.channels = static_cast<std::uint16_t>(count);
    std::size_t out = 0;
    for (const ChannelPosition p : detail::kMaskOrder) {
        const auto it = std::ranges::find(speakers, std::uint64_t{channelBit(p)});
        if (it != speakers.end()) {
            const auto source = static_cast<std::size_t>(it - speakers.begin());
            order.from[out]   = static_cast<std::uint8_t>(source);
            order.reorder     = order.reorder || source != out;
            ++out;
        }
    }
    return order;
}

/// Moves each of frames interleaved frames to ascending speaker order, in place,
/// as order says; nothing when order needs no reorder. It only copies, so every
/// sample keeps its exact value, and it allocates nothing, so a decoder can run
/// it on every block it delivers.
constexpr void reorderChannels(float* interleaved, std::size_t frames,
                               const ChannelOrder& order) noexcept {
    if (!order.reorder) {
        return;
    }
    const std::size_t                         ch = order.channels;
    std::array<float, kMaxPositionedChannels> frame{};
    for (std::size_t f = 0; f < frames; ++f) {
        float* p = interleaved + f * ch;
        std::ranges::copy_n(p, static_cast<std::ptrdiff_t>(ch), frame.begin());
        for (std::size_t i = 0; i < ch; ++i) {
            p[i] = frame[order.from[i]];
        }
    }
}

/// The order, for reorderChannels, that moves channels whose speakers source
/// lists to the order target lists: output channel i takes the source channel
/// holding target[i]. Speakers compare as plain values, so a decoder can pass its
/// own channel ids, including speakers this table cannot name. nullopt when the
/// lists name different speakers, when either repeats one, or beyond
/// kMaxPositionedChannels, where a ChannelOrder cannot hold the move. The mask is
/// left 0, since the stream keeps the one it declared.
template <typename Speaker>
[[nodiscard]] constexpr std::optional<ChannelOrder>
channelOrderBetween(std::span<const Speaker> source,
                    std::span<const Speaker> target) noexcept {
    const std::size_t count = source.size();
    if (count != target.size() || count > kMaxPositionedChannels) {
        return std::nullopt;
    }
    ChannelOrder order;
    order.channels = static_cast<std::uint16_t>(count);
    std::array<bool, kMaxPositionedChannels> taken{};
    for (std::size_t i = 0; i < count; ++i) {
        std::size_t hits = 0;
        for (std::size_t s = 0; s < count; ++s) {
            if (source[s] == target[i]) {
                ++hits;
                order.from[i] = static_cast<std::uint8_t>(s);
            }
        }
        if (hits != 1 || taken[order.from[i]]) {
            return std::nullopt;
        }
        taken[order.from[i]] = true;
        order.reorder         = order.reorder || order.from[i] != i;
    }
    return order;
}

/// The positions of a stream's channels, in order, for 1 to
/// kMaxPositionedChannels channels; empty for 0 or anything larger, which a sink
/// reads as "declare unpositioned". A nonzero mask is used when every bit is one
/// the table names and it has one bit per channel; any other mask is ignored and
/// the count's default order applies, so beyond kMaxDefaultOrderChannels, where
/// there is none, the list is empty too.
[[nodiscard]] constexpr ChannelPositionList
channelPositionsFor(const AudioFormat& format) noexcept {
    ChannelPositionList list;
    const std::uint16_t channels = format.channels;
    if (channels == 0 || channels > kMaxPositionedChannels) {
        return list;
    }
    const std::uint32_t mask = format.channelMask;
    if (mask != 0 && (mask & ~kNamedChannelMask) == 0 &&
        std::popcount(mask) == channels) {
        for (const ChannelPosition p : detail::kMaskOrder) {
            if ((mask & channelBit(p)) != 0) {
                list.push_back(p);
            }
        }
        return list;
    }
    if (channels > kMaxDefaultOrderChannels) {
        return list;
    }
    for (std::size_t i = 0; i < channels; ++i) {
        list.push_back(detail::kPositionRows[channels - 1u][i]);
    }
    return list;
}

/// Routes a stream's channels onto a device's. device holds, for each device
/// channel, the position the device's speaker configuration names there, or
/// nullopt where it names none of the table's. map gets one entry per device
/// channel: the source channel to play there, or -1 for silence. A source speaker
/// the device names exactly once goes to that channel. Every other speaker keeps
/// its own index while that channel is still free, and is dropped otherwise, so
/// a device that names nothing gets the identity and a source wider than the
/// device keeps its first channels.
constexpr void routeToDevice(const ChannelPositionList&                      source,
                             std::span<const std::optional<ChannelPosition>> device,
                             std::span<std::int32_t> map) noexcept {
    const std::size_t channels = std::min(device.size(), map.size());
    std::ranges::fill(map, -1);
    std::array<bool, kMaxPositionedChannels> placed{};
    for (std::size_t i = 0; i < source.size(); ++i) {
        std::size_t hits = 0;
        std::size_t at   = 0;
        for (std::size_t d = 0; d < channels; ++d) {
            if (device[d] == source[i]) {
                ++hits;
                at = d;
            }
        }
        if (hits == 1) {
            map[at]   = static_cast<std::int32_t>(i);
            placed[i] = true;
        }
    }
    for (std::size_t i = 0; i < source.size() && i < channels; ++i) {
        if (!placed[i] && map[i] == -1) {
            map[i] = static_cast<std::int32_t>(i);
        }
    }
}

}  // namespace rawform::audio
