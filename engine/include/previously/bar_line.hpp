// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: where a past bar line was, from the clock edges around it (DESIGN.md §3.4).
//
// The tracker's filtered times serve prediction. A capture asks about the past, which the raw
// edges on both sides of a line describe better: a hiccup on the line's own pulse is plain once
// the pulses after it have arrived. The rule, step by step, as DESIGN.md §3.4 gives it:
//
//   1. Fit each side. A repeated-median line through the 8 edges on each side of the line, which
//      ignores up to 3 bad edges of 8. The jitter estimate σ̂ comes from the third differences of
//      the 64 pulses before the line and the 8 after it, leaving out any that touch the line's own
//      edge or a filled-in pulse.
//   2. Keep an edge that lies on either side's straight line: a steady tempo, or the first pulse
//      of a phase shift or tempo change, which the side after it confirms.
//   3. Correct both sides for curvature, when the slope difference between them and the second
//      differences within them agree that there is curvature.
//   4. Repair an edge that is clearly off, when the corrected sides agree with each other: the
//      line lies at their mean. This repairs hiccups and bursts.
//   5. Otherwise keep the edge.
//
// Residuals are taken in ticks, exactly, from the straight line through the line's edge at the
// tracker's period. The third and second differences are taken in ticks too, and only then made
// floats, so σ̂ and the curvature within the sides are exact at any tempo. The side fits run in
// float on the residuals, and the result is rounded to the nearest tick.

#pragma once

#include "previously/clock_tracker.hpp"
#include "previously/sample_time.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace previously {

/// Which step of the rule decided an estimate.
enum class line_basis : std::uint8_t {
    short_history,      // fewer than 8 edges before the line: the edge is kept
    on_a_line,          // step 2: the edge lies on a side's straight line, and is kept
    few_jitter_values,  // fewer than 16 usable third differences, so no repair: the edge is kept
    repaired,           // step 4: the edge was clearly off, and the line is the sides' mean
    kept,               // step 5: the edge is kept
};

struct line_estimate {
    sample_time time;
    line_basis  basis{line_basis::kept};
    float       jitter{0.0F};            // σ̂, in samples
    std::size_t jitter_values{0};        // the third differences σ̂ came from
    float       curvature_within{0.0F};  // c_within, in samples per pulse², if step 3 ran
};

/// The edges the rule reads: up to 64 pulses before the line, and exactly 8 after it.
inline constexpr std::size_t line_pulses_before = 64;
inline constexpr std::size_t line_pulses_after  = 8;

/// Where the bar line at pulse `pulse` was. Empty until the line's edge and the 8 after it are
/// recorded: until then the line is not final, and a capture uses the raw edge. Also empty
/// before the tracker knows its period. Edges older than the history shorten the window from the
/// left. Audio thread: bounded time, no allocation.
[[nodiscard]] std::optional<line_estimate> estimate_line(const clock_tracker& grid,
                                                         std::int64_t         pulse) noexcept;

/// The rule on its own. `edges` are consecutive pulses, the line's at `line`, with at least 8
/// after it; edges more than 64 before the line or 8 after it are not read. `period` is the
/// pulse period the residuals are taken from, and must be positive.
[[nodiscard]] line_estimate
estimate_line(std::span<const pulse_edge> edges, std::size_t line, sample_time period) noexcept;

}  // namespace previously
