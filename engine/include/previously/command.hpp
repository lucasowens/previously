// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — commands sent to the engine from outside the audio thread.

#pragma once

#include <cstdint>

namespace previously {

/// Every action the engine can be asked to perform. The list grows with each phase; every entry
/// is reachable over MIDI (DESIGN.md §11).
enum class command_type : std::uint8_t {
    reset_grid,  // like the reset input: the pulse at this moment becomes bar 0
};

/// One request for the engine. Small and trivially copyable so it can go through spsc_queue.
struct command {
    command_type type{command_type::reset_grid};
};

}  // namespace previously
