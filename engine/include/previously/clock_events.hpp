// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: clock events going in, and the clock grid's report coming out.
//
// In: each audio block carries the clock and reset edges that happened during it, timestamped
// relative to the block's first frame with sub-sample precision.
//
// Out: after each block the engine reports the grid: tempo, lock state, where we are in the bar,
// and every grid pulse that fell inside the block. Adapters use the pulses for clock out and the
// beat lights, and the same pulses keep running while the clock freewheels.

#pragma once

#include "previously/clock_tracker.hpp"
#include "previously/sample_time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace previously {

enum class clock_event_type : std::uint8_t {
    pulse,  // a rising edge on the clock input
    reset,  // a rising edge on the reset input
};

/// One edge during a block. `offset` is measured from the block's first frame and lies in
/// [0, frame count). Events within a block are delivered in time order.
struct clock_event {
    clock_event_type type{clock_event_type::pulse};
    sample_time      offset;
};

/// A grid pulse that fell inside a block.
struct grid_pulse {
    sample_time  offset;  // from the block's first frame, in [0, frame count)
    std::int64_t index{0};
    bool         starts_beat{false};
    bool         starts_bar{false};
};

/// The clock grid after one block.
struct clock_report {
    /// At most this many grid pulses are reported per block. Pulses are at least 4 ms apart
    /// (the double-trigger window), so a 4,096-frame block at 48 kHz holds at most 22.
    static constexpr std::size_t max_pulses = 32;

    clock_state                        state{clock_state::no_clock};
    std::optional<sample_time>         beat_period;
    std::optional<std::int64_t>        bar;  // the bar containing the block's last frame
    std::array<grid_pulse, max_pulses> pulses{};
    std::size_t                        pulse_count{0};
};

}  // namespace previously
