// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the tempos below which a capture is shortened (DESIGN.md §3.4).
//
// At 48 kHz, 4 PPQN in 4/4: a window, with its 50 ms pre-roll, must be in the history at its press,
// and its frames, with the read margin and the reserve, must span at most 46 pages. The history at
// a press reaches back 45 pages from the start of the press's page, so between 45 pages and 46 less
// a sample. Each column of §3.4's table is the tempo at which one press stops fitting:
//
//   - any press: the latest late press, a quarter beat and a tick before the next line, made at the
//     start of a page, where the history is shortest;
//   - every early press: a tick before its line, at the start of a page;
//   - no press: an early press a quarter beat before its line, at the end of a page, where the
//     history is longest. The 47-page limit binds there first, for every bar count.
//
// Each floor is checked a hundredth of a BPM either side: N bars fit just above it, and just below
// it the capture is shortened for the reason the table gives.

#include "previously/capture.hpp"
#include "previously/engine.hpp"

#include "capture_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace previously {
namespace {

using test::at_sample;
using test::clocked_engine;
using test::tape_of;

constexpr sample_rate  rate_48k{48'000U};
constexpr std::int64_t page = 32'768;

enum class column : std::uint8_t {
    any_press,
    every_early_press,
    no_press,
};

struct shortening_floor {
    capture_bars  bars;
    column        press;
    double        bpm;
    capture_limit limit;  // what shortens the capture below it
};

// §3.4's table, a row for each floor.
namespace table {

using enum capture_bars;
using enum column;
using enum capture_limit;

constexpr std::array<shortening_floor, 12> floors{
    shortening_floor{.bars = one, .press = any_press, .bpm = 15.16, .limit = history},
    shortening_floor{.bars = two, .press = any_press, .bpm = 22.99, .limit = history},
    shortening_floor{.bars = four, .press = any_press, .bpm = 38.64, .limit = history},
    shortening_floor{.bars = eight, .press = any_press, .bpm = 69.94, .limit = history},
    shortening_floor{.bars = one, .press = every_early_press, .bpm = 7.91, .limit = max_pages},
    shortening_floor{.bars = two, .press = every_early_press, .bpm = 15.65, .limit = history},
    shortening_floor{.bars = four, .press = every_early_press, .bpm = 31.30, .limit = history},
    shortening_floor{.bars = eight, .press = every_early_press, .bpm = 62.60, .limit = history},
    shortening_floor{.bars = one, .press = no_press, .bpm = 7.91, .limit = max_pages},
    shortening_floor{.bars = two, .press = no_press, .bpm = 15.57, .limit = max_pages},
    shortening_floor{.bars = four, .press = no_press, .bpm = 30.91, .limit = max_pages},
    shortening_floor{.bars = eight, .press = no_press, .bpm = 61.58, .limit = max_pages},
};

}  // namespace table

/// The loop report once a press for `bars` at `bpm`, made as `press` says, has seen its end line
/// arrive, its pulse `late` samples after the grid predicts it. Every page's holders are checked
/// then too.
[[nodiscard]] loop_report
report_at(double bpm, capture_bars bars, column press, std::int64_t late = 0)
{
    // A pulse is a quarter beat at 4 PPQN: 720,000 / BPM samples at 48 kHz.
    const auto        period = sample_time::from_ticks(static_cast<std::int64_t>(
        std::llround(720'000.0 / bpm * static_cast<double>(sample_time::ticks_per_sample))));
    const sample_time bar    = period * 16;
    const sample_time tick   = sample_time::from_ticks(1);

    sample_time pressed;
    sample_time end_line;

    switch (press) {
        case column::any_press:
            pressed  = at_sample(46 * page);
            end_line = pressed + period + tick - bar;
            break;

        case column::every_early_press:
            pressed  = at_sample(46 * page);
            end_line = pressed + tick;
            break;

        case column::no_press:
            pressed  = at_sample((47 * page) - 1);
            end_line = pressed + period;
            break;
    }

    // The clock from the earliest bar line at least half a second in, so that its lines fall there.
    const std::int64_t bars_before = (end_line - at_sample(24'000)).ticks() / bar.ticks();

    std::vector<stereo_frame> tape = tape_of(46 + 47, rate_48k);
    clocked_engine            rig{engine_config{.rate = rate_48k, .max_block_frames = 3'072}, tape};

    const sample_time  first     = end_line - (bar * bars_before);
    const std::int64_t end_pulse = bars_before * 16;
    const sample_time  late_by   = at_sample(late);

    for (std::int64_t i = 0; i < end_pulse + 20; ++i) {
        rig.pulse(first + (period * i) + (i == end_pulse ? late_by : sample_time{}));
    }

    rig.press(pressed, bars);
    rig.run_through(std::max(pressed, end_line + late_by) + period);

    test::check_holders(rig.target());

    return rig.target().loop();
}

/// What becomes of the press.
[[nodiscard]] capture_outcome
outcome_at(double bpm, capture_bars bars, column press, std::int64_t late = 0)
{
    return report_at(bpm, bars, press, late).outcome;
}

TEST_CASE("each shortening floor holds to a hundredth of a BPM")
{
    for (const shortening_floor& floor : table::floors) {
        CAPTURE(bar_count(floor.bars));
        CAPTURE(static_cast<int>(floor.press));
        CAPTURE(floor.bpm);

        const capture_outcome above = outcome_at(floor.bpm + 0.01, floor.bars, floor.press);

        CHECK(above.result == capture_result::committed);
        CHECK(above.kept == floor.bars);

        const capture_outcome below = outcome_at(floor.bpm - 0.01, floor.bars, floor.press);

        if (floor.bars == capture_bars::one) {
            CHECK(below.result == capture_result::nothing_fits);
        }
        else {
            CHECK(below.result == capture_result::shortened);
            CHECK(bar_count(below.kept) * 2 == bar_count(floor.bars));
        }

        CHECK(below.limit == floor.limit);
    }
}

TEST_CASE("an end line late by more than the room under the 47-page limit shortens its capture at "
          "the line, or refuses it")
{
    // A hundredth above the no-press floors, an early press's run is 602 samples short of 46
    // pages for 2 bars, and 2,528 for 1. The end line's pulse 3,000 samples late makes each too
    // long once the capture is fitted again at the line. Pressed a quarter beat early, each has
    // begun its entry into its first downbeat by then (DESIGN.md §5.7): it has started, so it is
    // refused rather than shortened. Pressed a tick before its line, 2 bars at 15.66 BPM begin no
    // entry, and are shortened at the line when it comes 12,000 samples late.
    SUBCASE("2 bars, no entry begun: shortened to 1 at the line")
    {
        const loop_report report =
            report_at(15.66, capture_bars::two, column::every_early_press, 12'000);

        CHECK(report.outcome.result == capture_result::shortened);
        CHECK(report.outcome.asked == capture_bars::two);
        CHECK(report.outcome.kept == capture_bars::one);
        CHECK(report.outcome.limit == capture_limit::max_pages);
        CHECK(report.outcome_serial == 2U);
        CHECK(report.slot == slot_state::provisional);
    }

    SUBCASE("2 bars, its entry begun: refused at the line, and no longer waiting")
    {
        const loop_report report = report_at(15.58, capture_bars::two, column::no_press, 3'000);

        CHECK(report.outcome.result == capture_result::nothing_fits);
        CHECK(report.outcome.limit == capture_limit::max_pages);
        CHECK(report.outcome_serial == 2U);
        CHECK_FALSE(report.waiting);
        CHECK(report.slot == slot_state::empty);
    }

    SUBCASE("1 bar: refused at the line, and no longer waiting")
    {
        const loop_report report = report_at(7.92, capture_bars::one, column::no_press, 3'000);

        CHECK(report.outcome.result == capture_result::nothing_fits);
        CHECK(report.outcome.limit == capture_limit::max_pages);
        CHECK(report.outcome_serial == 2U);
        CHECK_FALSE(report.waiting);
        CHECK(report.slot == slot_state::empty);
    }

    SUBCASE("on time, both fit")
    {
        CHECK(outcome_at(15.58, capture_bars::two, column::no_press).result ==
              capture_result::committed);
        CHECK(outcome_at(7.92, capture_bars::one, column::no_press).result ==
              capture_result::committed);
    }
}

}  // namespace
}  // namespace previously
