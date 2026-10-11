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

// SinkLog.h
//
// The one diagnostic funnel the platform sinks share: format once, then write
// the line to BOTH stderr (the CLI's byte-for-byte output; the trailing newline
// is added here, so format strings carry none) and the installed ILogOutput
// when present (the engine's seam, which the Qt layer forwards as onInfo).
// Engine-thread only: every log site in a sink is an open/close/reconfigure
// path the engine drives; never the real-time callback, and never a platform
// notification thread (those write stderr directly, since the logger is an
// engine-thread facility).
//
// Private to the sinks directory, like the sinks themselves; NullSink has
// nothing to say and does not include it.

#pragma once

#include "rawform/audio/IAudioSink.h"  // ILogOutput

#include <cstdarg>
#include <cstdio>
#include <string>

namespace rawform::audio {

__attribute__((format(printf, 2, 3)))
inline void sinkLog(ILogOutput* out, const char* fmt, ...) {
    char    buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::fprintf(stderr, "%s\n", buf);
    if (out != nullptr) {
        out->logLine(std::string(buf));
    }
}

}  // namespace rawform::audio
