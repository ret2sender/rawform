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
// channel assignment and WAVE's default order agree. The FFmpeg and FLAC
// decoders report the speakers their source names (FFmpeg's channel layout, a
// FLAC file's WAVEFORMATEXTENSIBLE_CHANNEL_MASK comment) when they differ from
// that order, through the one rule in sourceChannelMask below. So a 4.0 source
// (FL FR FC BC) is not declared as quad (FL FR BL BR), nor AC3's side surrounds
// (SL SR) as back surrounds (BL BR). The layout rides in AudioFormat rather
// than beside it because every path that must agree on it already carries one:
// the gapless stitch gate, the hold gate, the sink's open and reconfigure.
//
// Why the sinks need it: a graph told "8 unpositioned channels" cannot mix. On
// PipeWire an unpositioned 5.1 stream on a stereo sink keeps FL and FR and drops
// the center, the LFE and the surrounds; with positions declared the channel
// mixer downmixes them. On CoreAudio the AUHAL does not downmix at all, but a
// declared mono stream can at least be mapped onto both device channels.
//
// Private to the audio library: the public headers carry the mask as a plain
// integer and stay free of this vocabulary, a sink implementation detail shared
// by two sinks.

#pragma once

#include "rawform/audio/Types.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

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
};

/// The largest channel count with a defined order. Beyond it a sink declares
/// the stream unpositioned.
inline constexpr std::uint16_t kMaxPositionedChannels = 8;

/// The position's WAVEFORMATEXTENSIBLE speaker bit, its bit in
/// AudioFormat::channelMask. Mono has none: it exists only as the one-channel
/// row of the table.
[[nodiscard]] constexpr std::uint16_t channelBit(ChannelPosition p) noexcept {
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

using PositionRow = std::array<ChannelPosition, kMaxPositionedChannels>;

// Row n-1 holds the n-channel order; unused tail slots are never read.
inline constexpr std::array<PositionRow, kMaxPositionedChannels> kPositionRows{{
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
inline constexpr std::array<ChannelPosition, 9> kMaskOrder{{
    ChannelPosition::FrontLeft, ChannelPosition::FrontRight,
    ChannelPosition::FrontCenter, ChannelPosition::Lfe,
    ChannelPosition::BackLeft, ChannelPosition::BackRight,
    ChannelPosition::BackCenter, ChannelPosition::SideLeft,
    ChannelPosition::SideRight,
}};

}  // namespace detail

/// Every bit a mask may carry; any other bit is a speaker this table cannot
/// name, and a decoder whose source has one reports no mask.
inline constexpr std::uint16_t kNamedChannelMask = [] {
    std::uint16_t mask = 0;
    for (const ChannelPosition p : detail::kMaskOrder) {
        mask = static_cast<std::uint16_t>(mask | channelBit(p));
    }
    return mask;
}();

/// The mask a channel count's default order stands for: the bits of its table
/// row. 0 for mono, for no channels, and beyond the table. A decoder compares a
/// source's mask against it and reports 0 when they agree, so one layout has
/// one representation.
[[nodiscard]] constexpr std::uint16_t
defaultChannelMask(std::uint16_t channels) noexcept {
    if (channels == 0 || channels > kMaxPositionedChannels) {
        return 0;
    }
    std::uint16_t mask = 0;
    for (std::size_t i = 0; i < channels; ++i) {
        mask = static_cast<std::uint16_t>(
            mask | channelBit(detail::kPositionRows[channels - 1u][i]));
    }
    return mask;
}

/// The mask a decoder reports for a source that names its speakers as a mask,
/// with its channels in ascending bit order: the source's mask when it differs
/// from the count's default order, and 0 otherwise. 0 also for anything the
/// table cannot declare: fewer than 2 or more than kMaxPositionedChannels
/// channels, a speaker the table cannot name, or a bit count that disagrees
/// with the channel count. Every decoder goes through this one rule, so two
/// sources with the same speakers carry the same mask and compare equal at the
/// stitch and hold gates.
[[nodiscard]] constexpr std::uint16_t
sourceChannelMask(std::uint64_t speakers, std::uint16_t channels) noexcept {
    if (channels < 2 || channels > kMaxPositionedChannels ||
        (speakers & ~std::uint64_t{kNamedChannelMask}) != 0 ||
        std::popcount(speakers) != channels) {
        return 0;
    }
    const auto mask = static_cast<std::uint16_t>(speakers);
    return mask == defaultChannelMask(channels) ? 0 : mask;
}

/// The positions of a stream's channels, in order, for 1 to
/// kMaxPositionedChannels channels; empty for 0 or anything larger, which a sink
/// reads as "declare unpositioned". A nonzero mask is used when every bit is one
/// the table names and it has one bit per channel; any other mask is ignored and
/// the count's default order applies.
[[nodiscard]] constexpr ChannelPositionList
channelPositionsFor(const AudioFormat& format) noexcept {
    ChannelPositionList list;
    const std::uint16_t channels = format.channels;
    if (channels == 0 || channels > kMaxPositionedChannels) {
        return list;
    }
    const std::uint16_t mask = format.channelMask;
    if (mask != 0 && (mask & ~kNamedChannelMask) == 0 &&
        std::popcount(mask) == channels) {
        for (const ChannelPosition p : detail::kMaskOrder) {
            if ((mask & channelBit(p)) != 0) {
                list.push_back(p);
            }
        }
        return list;
    }
    for (std::size_t i = 0; i < channels; ++i) {
        list.push_back(detail::kPositionRows[channels - 1u][i]);
    }
    return list;
}

}  // namespace rawform::audio
