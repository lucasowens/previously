// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — the engine: commands, the clock grid, and (for now) a sample-exact passthrough.

#include "previously/engine.hpp"

#include "previously/contract.hpp"

#include <algorithm>
#include <cstdint>

namespace previously {

engine::engine(const engine_config& config) noexcept
    : config_{config},
      clock_tracker_{clock_tracker_config::gate(config.rate), clock_history_}
{
    expects(config.rate.hz() > 0U);
    expects(config.max_block_frames > 0U);
    expects(config.max_block_frames <= max_block_frames(config.rate));
}

std::size_t engine::max_block_frames(sample_rate rate) noexcept
{
    const samples spacing = to_samples(clock_tracker::double_trigger_spacing, rate);

    return static_cast<std::size_t>(std::max(spacing.value(), std::int64_t{1})) *
           clock_report::max_pulses;
}

void engine::process(const audio_block& block, std::span<const clock_event> events) noexcept
{
    expects(block.has_matching_lengths());
    expects(block.frame_count() <= config_.max_block_frames);

    const auto        frames      = samples{static_cast<std::int64_t>(block.frame_count())};
    const sample_time block_start = sample_time::from_samples(sample_clock_);
    const sample_time block_end   = block_start + sample_time::from_samples(frames);

    apply_pending_commands(block_start);

    // The block's edges in time order, then the end of the block: the tracker's timing contract.
    std::optional<sample_time> previous_offset;

    for (const clock_event& event : events) {
        expects(event.offset >= sample_time{} && event.offset < sample_time::from_samples(frames));
        expects(!previous_offset || event.offset >= *previous_offset);
        previous_offset = event.offset;

        const sample_time when = block_start + event.offset;

        switch (event.type) {
            case clock_event_type::pulse:
                clock_tracker_.pulse(when);
                break;

            case clock_event_type::reset:
                clock_tracker_.reset(when);
                break;
        }
    }

    clock_tracker_.advance(block_end);

    std::ranges::copy(block.in_left, block.out_left.begin());
    std::ranges::copy(block.in_right, block.out_right.begin());

    sample_clock_ += frames;

    report_clock(block_start, block_end);
}

bool engine::post(const command& request) noexcept
{
    return commands_.try_push(request);
}

samples engine::sample_clock() const noexcept
{
    return sample_clock_;
}

const clock_report& engine::clock() const noexcept
{
    return clock_report_;
}

const engine_config& engine::config() const noexcept
{
    return config_;
}

void engine::apply_pending_commands(sample_time block_start) noexcept
{
    // Bounded: at most one queue's worth per block, so a flood of commands can never stall audio.
    for (std::size_t i = 0; i < command_queue_capacity; ++i) {
        const auto request = commands_.try_pop();

        if (!request.has_value()) {
            return;
        }

        apply(*request, block_start);
    }
}

void engine::apply(const command& request, sample_time block_start) noexcept
{
    // A switch, even with one case: as command_type grows, -Wswitch flags any unhandled command.
    switch (request.type) {  // NOLINT(readability-trivial-switch)
        case command_type::reset_grid:
            clock_tracker_.reset(block_start);
            break;
    }
}

void engine::report_clock(sample_time block_start, sample_time block_end) noexcept
{
    clock_report_.state       = clock_tracker_.state();
    clock_report_.beat_period = clock_tracker_.beat_period();
    clock_report_.bar = clock_tracker_.bar_at_or_before(block_end - sample_time::from_ticks(1));
    clock_report_.pulse_count   = 0;
    clock_report_.bar_restarted = false;

    // The rule: a grid pulse is reported once it lies at least half a period after the last
    // report, using the period in force AT that report. Hysteresis by time, not index, because
    // indices are relabelled by resets; and the period at the last report, because when the clock
    // slows, the new longer period would hide the first real slow pulse inside the window.
    const sample_time window = last_reported_pulse_ ? last_reported_period_ / 2 : sample_time{};
    const sample_time reportable_from =
        last_reported_pulse_ ? *last_reported_pulse_ + window : block_start;

    // A reset that made bar 0 start at a pulse which will not be reported as index 0, because
    // it was reported already under its old number or falls inside the window: say so once.
    if (const std::uint32_t resets = clock_tracker_.resets_applied(); resets != resets_seen_) {
        resets_seen_ = resets;

        const auto bar_zero      = clock_tracker_.pulse_time(0);
        const bool shown_as_zero = bar_zero && last_reported_bar_zero_ &&
                                   abs(*bar_zero - *last_reported_bar_zero_) < window;

        clock_report_.bar_restarted = bar_zero && *bar_zero < reportable_from && !shown_as_zero;
    }

    const auto period = clock_tracker_.pulse_period();

    if (!period) {
        return;
    }

    std::array<grid_point, clock_report::max_pulses> found{};
    const std::size_t                                count =
        clock_tracker_.pulses_between(reportable_from - sample_time::from_ticks(1),
                                      block_end,
                                      found);

    const std::int64_t pulses_per_beat = clock_tracker_.pulses_per_beat();
    const std::int64_t pulses_per_bar  = clock_tracker_.pulses_per_bar();

    for (std::size_t i = 0; i < count; ++i) {
        const grid_point& point = found[i];

        // Pulses are at least half a period apart, so only the first can be inside the window
        // of the last report from an earlier block; later ones are checked against this block's.
        if (last_reported_pulse_ &&
            point.time < *last_reported_pulse_ + (last_reported_period_ / 2)) {
            continue;
        }

        clock_report_.pulses[clock_report_.pulse_count] = grid_pulse{
            // Never negative in practice: a pulse not yet reported lies at or after this block's
            // start (its prediction did), and a real edge correcting it is inside this block.
            // Clamped anyway, because an adapter turns the offset into an unsigned position.
            .offset      = std::max(point.time - block_start, sample_time{}),
            .index       = point.index,
            .starts_beat = floor_modulo(point.index, pulses_per_beat) == 0,
            .starts_bar  = floor_modulo(point.index, pulses_per_bar) == 0,
        };
        ++clock_report_.pulse_count;

        last_reported_pulse_  = point.time;
        last_reported_period_ = *period;

        if (point.index == 0) {
            last_reported_bar_zero_ = point.time;
        }
    }
}

}  // namespace previously
