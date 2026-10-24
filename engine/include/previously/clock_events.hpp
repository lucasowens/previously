// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: clock events going in, and the clock grid's report coming out.
//
// In: each audio block carries the clock, reset and capture edges that happened during it,
// timestamped relative to the block's first frame with sub-sample precision.
//
// Out: after each block the engine reports the grid: tempo, lock state, where we are in the bar,
// and every grid pulse that fell inside the block. Adapters use the pulses for clock out and the
// beat lights, and the same pulses keep running while the clock freewheels.

#pragma once

#include "previously/capture_bars.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/sample_time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace previously {

enum class clock_event_type : std::uint8_t {
    pulse,    // a rising edge on the clock input
    reset,    // a rising edge on the reset input
    capture,  // a CAPTURE press, from the button or the gate input (DESIGN.md §3.4)
};

/// One edge during a block. `offset` is measured from the block's first frame and lies in
/// [0, frame count). Events within a block are delivered in time order.
struct clock_event {
    clock_event_type type{clock_event_type::pulse};
    capture_bars     bars{capture_bars::four};  // what a capture asks for; other edges ignore it
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
///   - reported pulses are never closer than half the period in force at the previous report:
///     no bursts, no merged triggers
///   - every real pulse is covered: reported at its time, or stood in for by a prediction less
///     than half a period before it
///   - with a steady or jittery clock, hiccups and phase shifts of under half a period, resets,
///     tempo increases, a stopped clock (freewheeling) and its return, every grid pulse is
///     reported exactly once, with its final index. A pulse displaced by more than half a
///     period is closer to its neighbour's slot: it is still covered, but an index can be
///     reported twice or skipped (pinned by a test)
///   - when the clock suddenly slows, predictions go out before the slowdown can be known, so a
///     pinned number of extra pulses is reported during the transition
///     (docs/learning/clock-tracking.md §8)
///   - pulse 0 is reported only if the tempo becomes known in the same block, i.e. pulse 1
///     arrives in that block too; otherwise reporting starts at pulse 1
struct clock_report {
    /// At most this many grid pulses fit in one block's report. The engine accepts only block
    /// sizes that cannot exceed it (engine::max_block_frames).
    static constexpr std::size_t max_pulses = 32;

    clock_state                state{clock_state::no_clock};
    std::optional<sample_time> beat_period;
    /// The bar containing the block's last frame. While a reset is pending it is numbered as it
    /// will be once the reset applies, so between the reset and the pulse it claims, the bar
    /// before the new bar 0 reads -1. Those pulses keep bar -1 afterwards.
    std::optional<std::int64_t>        bar;
    std::array<grid_pulse, max_pulses> pulses{};
    std::size_t                        pulse_count{0};

    /// A reset made bar 0 start at a pulse that is not reported here as index 0: it was already
    /// reported under its old number, or it is too close to the last report to be reported again.
    /// The bar indicator should restart now.
    bool bar_restarted{false};
};

}  // namespace previously
