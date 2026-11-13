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

// PositionNotifyGate.h
//
// Which position-tick notifications AudioController owes the interface, and
// when it may send them. The engine reports the position every 100 ms on its
// own phase; a notification sent at that moment changes the seek bar between
// two spectrum frames, so the window draws a frame of its own for it. While
// the spectrum clock runs it holds the gate, and the notifications owed since
// its last tick go out with the next one, in the same frame as the bars. The
// values themselves are never held: only the signals that redraw them.
//
// Plain C++ and header-only so the state machine is testable without a Qt
// event loop; AudioController turns an Emit into the actual signals.

#pragma once

namespace rawform {

class PositionNotifyGate {
public:
    /// The notifications to send now. Both flags false means send nothing.
    struct Emit {
        bool position = false;  ///< positionChanged
        bool bitrate  = false;  ///< formatSummaryChanged, for the live bitrate

        [[nodiscard]] bool any() const { return position || bitrate; }
    };

    /// A position report arrived; @p bitrateMoved says the live bitrate changed
    /// with it. Unheld, the report is sent at once; held, it is owed until the
    /// next flush. Owed flags accumulate, so several reports flush as one.
    [[nodiscard]] Emit report(bool bitrateMoved) {
        if (!m_held) {
            return Emit{.position = true, .bitrate = bitrateMoved};
        }
        m_owed.position = true;
        m_owed.bitrate  = m_owed.bitrate || bitrateMoved;
        return {};
    }

    /// The clock started: hold reports until its ticks flush them.
    void hold() { m_held = true; }

    /// The clock stopped: nothing will flush any more, so what is owed goes now.
    [[nodiscard]] Emit release() {
        m_held = false;
        return take();
    }

    /// A clock tick: what is owed goes out with it. Nothing is owed while
    /// unheld, so a stray flush sends nothing.
    [[nodiscard]] Emit flush() { return take(); }

    [[nodiscard]] bool held() const { return m_held; }

private:
    [[nodiscard]] Emit take() {
        const Emit owed = m_owed;
        m_owed          = {};
        return owed;
    }

    bool m_held = false;
    Emit m_owed;
};

}  // namespace rawform
