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
// The speaker position of each interleaved channel, by channel count. The
// engine carries no layout: AudioFormat is rate and channels only (a layout
// field would change its 64-bit packing and its equality, the gapless stitch
// gate), and every decoder delivers channels in its container's default order.
// For 1 to 8 channels that order is the same across the formats rawform plays:
// FLAC's channel assignment and WAVE's default order agree, and the FFmpeg
// decoder remixes its sources to this order before they reach the engine. So
// the position of channel i is a pure function of the channel count, and this
// table is where the sinks look it up when they describe the stream to the
// platform.
//
// Why the sinks need it: a graph told "8 unpositioned channels" cannot mix. On
// PipeWire an unpositioned 5.1 stream on a stereo sink keeps FL and FR and drops
// the center, the LFE and the surrounds; with positions declared the channel
// mixer downmixes them. On CoreAudio the AUHAL does not downmix at all, but a
// declared mono stream can at least be mapped onto both device channels.
//
// Private to the audio library: the public headers stay layout-free on
// purpose; this is a sink implementation detail shared by two sinks.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
};

/// The largest channel count with a defined order. Beyond it a sink declares
/// the stream unpositioned, as before.
inline constexpr std::uint16_t kMaxPositionedChannels = 8;

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

}  // namespace detail

/// The positions of an interleaved stream's channels, in order, for a channel
/// count of 1 to kMaxPositionedChannels; empty for 0 or anything larger, which
/// a sink reads as "declare unpositioned".
[[nodiscard]] constexpr std::span<const ChannelPosition>
channelPositionsFor(std::uint16_t channels) noexcept {
    if (channels == 0 || channels > kMaxPositionedChannels) {
        return {};
    }
    const detail::PositionRow& row = detail::kPositionRows[channels - 1u];
    return std::span<const ChannelPosition>(row.data(), channels);
}

}  // namespace rawform::audio
