// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: building the UI model.

#include "previously/ui_model.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace previously {
namespace {

/// The first frame at or after `time`: frame n sounds at time n.
[[nodiscard]] std::int64_t first_frame_from(sample_time time) noexcept
{
    return -floor_divide(-time.ticks(), sample_time::ticks_per_sample);
}

/// Shows frames [first, last) in `column`: the loudest of the buckets touching them that the
/// history holds. Only the history is searched, so however long the stretch, the work is bounded:
/// at most the history's buckets, and one more.
void show(strip_column&   column,
          const recorder& history,
          std::int64_t    first,
          std::int64_t    last) noexcept
{
    const std::int64_t bucket = history.bucket_frames().value();
    const std::int64_t from   = std::max(first, history.begin().value());
    const std::int64_t to     = std::min(last, history.end().value());

    for (std::int64_t position = floor_divide(from, bucket) * bucket; position < to;
         position += bucket) {
        if (const auto peaks = history.peaks_at(samples{position})) {
            column.recorded    = true;
            column.peaks.left  = std::max(column.peaks.left, peaks->left);
            column.peaks.right = std::max(column.peaks.right, peaks->right);
        }
    }
}

/// The clock, at the block's last frame, as clock_report gives it.
void show_clock(ui_model&            model,
                const clock_tracker& grid,
                sample_time          last_frame,
                sample_rate          rate) noexcept
{
    model.clock         = grid.state();
    model.beat_period   = grid.beat_period();
    model.rate          = rate;
    model.bar           = grid.bar_at_or_before(last_frame);
    model.beats_per_bar = grid.pulses_per_bar() / grid.pulses_per_beat();

    if (const auto pulse = grid.pulse_at_or_before(last_frame)) {
        model.beat = floor_modulo(*pulse, grid.pulses_per_bar()) / grid.pulses_per_beat();
    }
}

/// The bars view, when the grid can place the bar in progress: where it starts, and where it
/// will end. Returns whether it could.
[[nodiscard]] bool
show_bars(ui_model& model, const clock_tracker& grid, const recorder& history) noexcept
{
    if (!model.bar) {
        return false;
    }

    // The slots' bar lines: the whole bars, the bar in progress, and where it will end.
    std::array<std::optional<sample_time>, ui_model::bar_slots + 1> lines{};

    const std::int64_t first_bar = *model.bar - static_cast<std::int64_t>(ui_model::whole_bars);

    for (std::size_t slot = 0; slot < lines.size(); ++slot) {
        lines[slot] = grid.bar_start(first_bar + static_cast<std::int64_t>(slot));
    }

    if (!lines[ui_model::bar_slots - 1] || !lines[ui_model::bar_slots]) {
        return false;
    }

    // Just after the clock starts, the earliest slots are bars from before it, which the grid
    // cannot place. They stay empty.
    std::size_t first_placed = ui_model::bar_slots - 1;

    while (first_placed > 0 && lines[first_placed - 1]) {
        --first_placed;
    }

    model.view           = strip_view::bars;
    model.first_bar_slot = first_placed;

    // Each bar is divided evenly among its slot's columns. A column starts at the first frame at
    // or after its share of the bar, so the frames of a bar are shown once, and in order.
    const std::int64_t end = history.end().value();

    for (std::size_t slot = 0; slot < ui_model::bar_slots; ++slot) {
        const std::optional<sample_time> start_line = lines[slot];
        const std::optional<sample_time> end_line   = lines[slot + 1];

        if (!start_line || !end_line) {
            continue;  // a bar from before the clock: its slot stays empty
        }

        const sample_time start  = *start_line;
        const sample_time span   = *end_line - start;
        const std::size_t offset = ui_model::slot_start(slot);
        const auto width = static_cast<std::int64_t>(ui_model::slot_start(slot + 1) - offset);

        for (std::int64_t index = 0; index < width; ++index) {
            const std::int64_t first  = first_frame_from(start + ((span * index) / width));
            const std::int64_t last   = first_frame_from(start + ((span * (index + 1)) / width));
            const std::size_t  column = offset + static_cast<std::size_t>(index);

            // The rest of the bar in progress is still to come.
            if (first >= end) {
                model.now_column = std::min(model.now_column, column);
                continue;
            }

            show(model.strip[column], history, first, last);
        }
    }

    return true;
}

/// The time view: the newest buckets, 11 to a column, newest at the right. Columns hold whole
/// groups of 11, so the picture moves on a column at a time.
void show_time(ui_model& model, const recorder& history) noexcept
{
    const std::int64_t end = history.end().value();

    if (end == 0) {
        return;
    }

    const std::int64_t group_frames = static_cast<std::int64_t>(ui_model::buckets_per_time_column) *
                                      history.bucket_frames().value();
    const std::int64_t newest_group = (end - 1) / group_frames;

    for (std::size_t column = 0; column < ui_model::strip_columns; ++column) {
        const std::int64_t group =
            newest_group - static_cast<std::int64_t>(ui_model::strip_columns - 1 - column);

        if (group >= 0) {
            show(model.strip[column], history, group * group_frames, (group + 1) * group_frames);
        }
    }
}

}  // namespace

void update_ui_model(ui_model&            model,
                     const clock_tracker& grid,
                     const recorder&      history,
                     sample_rate          rate) noexcept
{
    // Built afresh, so nothing of the last build can linger. Only the count carries on.
    const std::uint32_t serial = model.serial + 1U;

    model        = ui_model{};
    model.serial = serial;

    const sample_time last_frame =
        sample_time::from_samples(history.end()) - sample_time::from_ticks(1);

    show_clock(model, grid, last_frame, rate);

    if (!show_bars(model, grid, history)) {
        show_time(model, history);
    }
}

}  // namespace previously
