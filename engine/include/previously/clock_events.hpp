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
///
/// Guarantees for `pulses`:
///   - reported pulses are always at least half a period apart: no bursts, no merged triggers
///   - with a steady or jittery clock, resets, tempo increases, a stopped clock (freewheeling)
///     and its return, every grid pulse is reported exactly once, with its final index
///   - when the clock suddenly slows, the grid's predictions are reported before the slowdown
///     can be known, so a few extra pulses may be reported during the transition; every real
///     pulse is still reported (docs/learning/clock-tracking.md §8)
struct clock_report {
    /// At most this many grid pulses fit in one block's report. The engine accepts only block
    /// sizes that cannot exceed it (engine::max_block_frames).
    static constexpr std::size_t max_pulses = 32;

    clock_state                        state{clock_state::no_clock};
    std::optional<sample_time>         beat_period;
    std::optional<std::int64_t>        bar;  // the bar containing the block's last frame
    std::array<grid_pulse, max_pulses> pulses{};
    std::size_t                        pulse_count{0};

    /// A reset made bar 0 start at a pulse that is not reported here as index 0: it was already
    /// reported under its old number, or it is too close to the last report to be reported again.
    /// The bar indicator should restart now.
    bool bar_restarted{false};
};

}  // namespace previously
