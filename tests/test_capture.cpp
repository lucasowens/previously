// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: capture, from the press to a held window of the tape (DESIGN.md §3.4).
//
// Every test runs at DESIGN's tempo: 120 BPM in 4/4 at 48 kHz, the clock from sample 0, so a pulse
// is 6,000 samples, a bar 96,000 (2 s), and bar n's line is at n × 96,000. A page is 32,768
// frames. DESIGN's two examples press at bars 21 and 16; the rest press near bar 6, which needs
// less recording.

#include "previously/bar_line.hpp"
#include "previously/capture.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/engine.hpp"
#include "previously/page_pool.hpp"
#include "previously/recorder.hpp"

#include "capture_fixtures.hpp"
#include "known.hpp"
#include "realtime_guard.hpp"
#include "tape_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace previously {
namespace {

using test::at_sample;
using test::check_holders;
using test::clocked_engine;
using test::tape_of;

constexpr sample_rate  rate_48k{48'000U};
constexpr std::int64_t pulses_per_bar = 16;
constexpr std::int64_t pulse_samples  = 6'000;
constexpr std::int64_t bar_samples    = pulses_per_bar * pulse_samples;
constexpr std::int64_t beat_samples   = 4 * pulse_samples;
constexpr std::int64_t margin         = 2'400;  // 50 ms
constexpr std::int64_t reserve        = pulse_samples / 2;

/// P1 at its fullest: the history and three loops of 47 pages (DESIGN.md §2.3).
constexpr std::size_t p1_pages = 187;

constexpr engine_config config_48k{.rate = rate_48k, .max_block_frames = 480};

/// Where bar `bar`'s line is, the clock from sample 0.
[[nodiscard]] constexpr std::int64_t line(std::int64_t bar)
{
    return bar * bar_samples;
}

/// A rig at 120 BPM: the clock from sample 0 for `bars` bars.
struct steady_rig {
    std::vector<stereo_frame> tape;
    clocked_engine            rig;

    explicit steady_rig(std::size_t pages = p1_pages, std::int64_t bars = 30)
        : tape{tape_of(pages, rate_48k)},
          rig{config_48k, tape}
    {
        rig.steady_clock(at_sample(0), at_sample(pulse_samples), bars * pulses_per_bar);
    }
};

TEST_CASE("a late press holds the bars before its line, and the pages around them")
{
    steady_rig      setup;
    clocked_engine& rig = setup.rig;

    // 0.3 beat into bar 21: the end line is bar 21's, just past.
    const std::int64_t pressed = line(21) + (beat_samples * 3 / 10);

    rig.press(at_sample(pressed), capture_bars::four);
    rig.run_through(at_sample(pressed));

    const engine& audio = rig.target();

    CHECK(audio.loop().outcome.result == capture_result::committed);
    CHECK(audio.loop().outcome.asked == capture_bars::four);
    CHECK(audio.loop().outcome.kept == capture_bars::four);
    CHECK(audio.loop().slot == slot_state::provisional);
    CHECK_FALSE(audio.loop().waiting);

    // Bars 17 to 20: [line 17, line 21), 384,000 frames. Lines 17 to 20 are final, with 8 pulses
    // after each; line 21 is its raw edge until its 8th.
    const captured_loop& loop = audio.slot();

    REQUIRE(loop.bars() == 4U);
    CHECK(loop.first_bar() == 17);
    CHECK_FALSE(loop.numbered_by_reset());

    for (std::size_t k = 0; k <= 4; ++k) {
        CAPTURE(k);
        CHECK(loop.lines()[k].position == samples{line(17 + static_cast<std::int64_t>(k))});
        CHECK(loop.lines()[k].status == (k < 4 ? line_status::final : line_status::edge));
    }

    CHECK(loop.lines().back().position - loop.lines().front().position == samples{384'000});

    // Held: line 17 - 50 ms to line 21 + 50 ms + half a pulse, pages 49 to 61, all recorded.
    CHECK(loop.held().first == samples{line(17) - margin});
    CHECK(loop.held().end == samples{line(21) + reserve + margin});
    CHECK(loop.first_page() == 49);
    CHECK(loop.page_count() == 13U);
    CHECK(loop.pages().size() == 13U);
    check_holders(audio);

    // Final 2 beats after line 21, once its 8th pulse has arrived.
    const std::int64_t eighth = line(21) + (8 * pulse_samples);

    rig.run_until(samples{eighth});
    CHECK(audio.loop().slot == slot_state::provisional);

    rig.run_through(at_sample(eighth));
    CHECK(audio.loop().slot == slot_state::final);
    CHECK(loop.lines().back().status == line_status::final);
    CHECK(loop.lines().back().position == samples{line(21)});

    // The reserve goes: line 21 + 50 ms, still in page 61, so every page stays.
    CHECK(loop.held().end == samples{line(21) + margin});
    CHECK(loop.page_count() == 13U);
    CHECK(loop.pages().size() == 13U);
    check_holders(audio);
}

TEST_CASE("an early press waits for its line, holds each page the recorder takes, and lets go of "
          "one held only for the reserve")
{
    // Bar 16's line, at 1,536,000, is 4,096 frames before page 47 starts: the read margin and the
    // reserve reach into it, and the window does not.
    steady_rig      setup;
    clocked_engine& rig   = setup.rig;
    const engine&   audio = rig.target();

    // 0.2 beat before bar 16: the end line is bar 16's, still to come.
    const std::int64_t pressed = line(16) - (beat_samples / 5);

    rig.press(at_sample(pressed), capture_bars::four);
    rig.run_through(at_sample(pressed));

    CHECK(audio.loop().outcome.result == capture_result::committed);
    CHECK(audio.loop().slot == slot_state::empty);
    CHECK(audio.loop().waiting);

    const captured_loop& waiting = audio.waiting();

    REQUIRE(waiting.bars() == 4U);
    CHECK(waiting.first_bar() == 12);
    CHECK(waiting.lines().back().status == line_status::predicted);
    CHECK(waiting.lines().back().position == samples{line(16)});

    // Pages 35 to 47 are its run. Page 47 is not yet recorded.
    CHECK(waiting.held().first == samples{line(12) - margin});
    CHECK(waiting.held().end == samples{line(16) + reserve + margin});
    CHECK(waiting.first_page() == 35);
    CHECK(waiting.page_count() == 13U);
    CHECK(waiting.pages().size() == 12U);
    check_holders(audio);

    // Its line arrives: it takes the slot, provisional.
    rig.run_through(at_sample(line(16)));

    CHECK(audio.loop().slot == slot_state::provisional);
    CHECK_FALSE(audio.loop().waiting);
    CHECK(audio.slot().lines().back().status == line_status::edge);
    CHECK(audio.slot().pages().size() == 12U);
    check_holders(audio);

    // Page 47 is held in the block the recorder takes it.
    constexpr std::int64_t page_47 = std::int64_t{47} * 32'768;

    rig.run_until(samples{page_47});
    CHECK(audio.slot().pages().size() == 12U);

    rig.run_through(at_sample(page_47));
    CHECK(audio.slot().pages().size() == 13U);
    check_holders(audio);

    // Final 8 pulses after its line. Line 16 + 50 ms ends in page 46, so page 47 goes back.
    rig.run_through(at_sample(line(16) + (8 * pulse_samples)));

    CHECK(audio.loop().slot == slot_state::final);
    CHECK(audio.slot().held().end == samples{line(16) + margin});
    CHECK(audio.slot().page_count() == 12U);
    CHECK(audio.slot().pages().size() == 12U);
    check_holders(audio);
}

TEST_CASE("a capture waiting for its line holds each page the recorder takes meanwhile")
{
    // Page 41 starts at 1,343,488, 512 frames before bar 14's line.
    steady_rig      setup;
    clocked_engine& rig   = setup.rig;
    const engine&   audio = rig.target();

    constexpr std::int64_t page_41 = std::int64_t{41} * 32'768;

    rig.press(at_sample(line(14) - 4'800), capture_bars::four);
    rig.run_through(at_sample(line(14) - 4'800));

    REQUIRE(audio.loop().waiting);
    CHECK(audio.waiting().pages().size() == 12U);

    rig.run_through(at_sample(page_41));

    REQUIRE(audio.loop().waiting);
    CHECK(audio.waiting().pages().size() == 13U);
    check_holders(audio);
}

// On a fast clock a loop is final before its read margin has been recorded: 8 pulses of 240
// samples are 1,920, and the margin 2,400. Bar 170's line is at 653,000, 2,360 frames before page
// 20 starts at 655,360, so a 1-bar loop ending there needs page 20 for its margin.
constexpr std::int64_t fast_pulse    = 240;
constexpr std::int64_t fast_end_line = 653'000;

/// The fast clock, from the bar line 170 bars before bar 170's.
void fast_clock(clocked_engine& rig)
{
    constexpr std::int64_t first_pulse = fast_end_line - (170 * pulses_per_bar * fast_pulse);

    rig.steady_clock(at_sample(first_pulse),
                     at_sample(fast_pulse),
                     (170 * pulses_per_bar) + (4 * pulses_per_bar));
}

/// The final loop in the slot holds page 20 once the recorder takes it, and keeps it once the
/// history has moved past it.
void check_holds_its_margin(clocked_engine& rig)
{
    constexpr std::int64_t page_20 = std::int64_t{20} * 32'768;
    const captured_loop&   loop    = rig.target().slot();

    REQUIRE(rig.target().loop().slot == slot_state::final);
    REQUIRE(loop.page_count() == 2U);
    CHECK(loop.pages().size() == 1U);

    rig.run_through(at_sample(page_20));

    CHECK(loop.pages().size() == 2U);
    check_holders(rig.target());

    // Once page 20 has left the history, the loop is its only holder.
    rig.run_until(samples{std::int64_t{67} * 32'768});

    CHECK(loop.pages().size() == 2U);
    check_holders(rig.target());

    if (loop.pages().size() == 2U) {
        CHECK(rig.target().pages().holders(loop.pages()[1]) == 1U);
    }
}

TEST_CASE("a loop final before its read margin is recorded holds the margin's pages as they come")
{
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{config_48k, tape};

    // Final in a block before page 20, 8 pulses after its line.
    fast_clock(rig);
    rig.press(at_sample(fast_end_line + 300), capture_bars::one);
    rig.run_through(at_sample(fast_end_line + (8 * fast_pulse)));

    check_holds_its_margin(rig);
}

TEST_CASE("a loop final at its press holds the margin's pages as they come")
{
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{config_48k, tape};

    // Pressed on its line's 8th pulse, so final at once, with page 20 still to come.
    fast_clock(rig);
    rig.press(at_sample(fast_end_line + (8 * fast_pulse)), capture_bars::one);
    rig.run_through(at_sample(fast_end_line + (8 * fast_pulse)));

    check_holds_its_margin(rig);
}

TEST_CASE("the end line is the next line at most a quarter beat away, and the latest otherwise")
{
    // At 4 PPQN a quarter beat is one pulse: the press lands on the pulse before bar 6's line, or
    // a tick before it.
    const std::int64_t quarter_beat = beat_samples / 4;

    SUBCASE("a quarter beat before the line: early, so bar 6's line, still to come")
    {
        steady_rig  setup;
        const auto  pressed = at_sample(line(6) - quarter_beat);
        const auto& audio   = setup.rig.target();

        setup.rig.press(pressed, capture_bars::four);
        setup.rig.run_through(pressed);

        REQUIRE(audio.loop().waiting);
        CHECK(audio.waiting().first_bar() == 2);
        CHECK(audio.waiting().lines().back().position == samples{line(6)});
    }

    SUBCASE("a tick further: late, so bar 5's line, held at once and already final")
    {
        steady_rig  setup;
        const auto  pressed = at_sample(line(6) - quarter_beat) - sample_time::from_ticks(1);
        const auto& audio   = setup.rig.target();

        setup.rig.press(pressed, capture_bars::four);
        setup.rig.run_through(pressed);

        CHECK_FALSE(audio.loop().waiting);
        CHECK(audio.loop().slot == slot_state::final);
        CHECK(audio.slot().first_bar() == 1);
        CHECK(audio.slot().lines().back().position == samples{line(5)});
    }

    SUBCASE("on the line, after its pulse: that line, and held at once")
    {
        steady_rig  setup;
        const auto  pressed = at_sample(line(6));
        const auto& audio   = setup.rig.target();

        setup.rig.press(pressed, capture_bars::four);
        setup.rig.run_through(pressed);

        CHECK(audio.loop().slot == slot_state::provisional);
        CHECK(audio.slot().first_bar() == 2);
        CHECK(audio.slot().lines().back().position == samples{line(6)});
    }
}

TEST_CASE("a press on a line's pulse finds the same end line whether the pulse comes first or "
          "not")
{
    // Pulse first: the line is recorded, and the press is late by nothing. Press first: the grid
    // predicts the line at that moment, so it is the most recent, and the capture waits the
    // moment it takes for its pulse.
    for (const bool pulse_first : {true, false}) {
        CAPTURE(pulse_first);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{config_48k, tape};
        const auto                pressed = at_sample(line(6));

        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 6 * pulses_per_bar);

        if (!pulse_first) {
            rig.press(pressed, capture_bars::four);
        }

        rig.steady_clock(pressed, at_sample(pulse_samples), 4 * pulses_per_bar);

        if (pulse_first) {
            rig.press(pressed, capture_bars::four);
        }

        rig.run_through(pressed);

        CHECK(rig.target().loop().slot == slot_state::provisional);
        CHECK(rig.target().slot().first_bar() == 2);
        CHECK(rig.target().slot().lines().back().position == samples{line(6)});
    }
}

TEST_CASE("a late press made 8 pulses or more after its line is final at once, with no reserve")
{
    steady_rig      setup;
    clocked_engine& rig = setup.rig;

    // Bar 6's 8th pulse is 2 beats in: a press on it, after it, finds the line final.
    const std::int64_t eighth = line(6) + (8 * pulse_samples);

    rig.press(at_sample(eighth), capture_bars::two);
    rig.run_through(at_sample(eighth));

    const engine& audio = rig.target();

    CHECK(audio.loop().slot == slot_state::final);
    CHECK(audio.slot().held().first == samples{line(4) - margin});
    CHECK(audio.slot().held().end == samples{line(6) + margin});
}

TEST_CASE("a press is refused when the grid has no tempo")
{
    SUBCASE("no clock at all")
    {
        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{config_48k, tape};

        rig.press(at_sample(1'000), capture_bars::one);
        rig.run_through(at_sample(1'000));

        CHECK(rig.target().loop().outcome.result == capture_result::no_tempo);
        CHECK(rig.target().loop().outcome.asked == capture_bars::one);
        CHECK(rig.target().loop().outcome_serial == 1U);
        CHECK(rig.target().loop().slot == slot_state::empty);
    }

    SUBCASE("one pulse: still acquiring")
    {
        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{config_48k, tape};

        rig.pulse(at_sample(1'000));
        rig.press(at_sample(2'000), capture_bars::one);
        rig.run_through(at_sample(2'000));

        CHECK(rig.target().loop().outcome.result == capture_result::no_tempo);
    }
}

TEST_CASE("a press is refused when not even one bar is in the history")
{
    SUBCASE("just after power-up: bar 0's pre-roll was never recorded")
    {
        // Half a bar into bar 1, with the clock from sample 0: bar 0 is [0, 96,000), and the 50 ms
        // before it do not exist.
        steady_rig setup;
        const auto pressed = at_sample(line(1) + (bar_samples / 2));

        setup.rig.press(pressed, capture_bars::one);
        setup.rig.run_through(pressed);

        const loop_report& report = setup.rig.target().loop();

        CHECK(report.outcome.result == capture_result::nothing_fits);
        CHECK(report.outcome.limit == capture_limit::history);
        CHECK(report.slot == slot_state::empty);
        check_holders(setup.rig.target());
    }

    SUBCASE("the clock started recently: no line a bar before the end line")
    {
        // The clock starts 10 s in. Half a bar later the end line is its first pulse, bar 0's.
        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{config_48k, tape};
        const std::int64_t        start = 480'000;

        rig.steady_clock(at_sample(start), at_sample(pulse_samples), 64);

        const auto pressed = at_sample(start + (bar_samples / 2));

        rig.press(pressed, capture_bars::one);
        rig.run_through(pressed);

        CHECK(rig.target().loop().outcome.result == capture_result::nothing_fits);
        CHECK(rig.target().loop().outcome.limit == capture_limit::history);

        // A bar later, bar 0 is there to take.
        const auto again = pressed + at_sample(bar_samples);

        rig.press(again, capture_bars::four);
        rig.run_through(again);

        CHECK(rig.target().loop().outcome.result == capture_result::shortened);
        CHECK(rig.target().loop().outcome.kept == capture_bars::one);
        CHECK(rig.target().loop().outcome.limit == capture_limit::history);
        CHECK(rig.target().slot().first_bar() == 0);
    }

    SUBCASE("without a tape: nothing is recorded")
    {
        clocked_engine rig{config_48k, {}};

        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 64);

        const auto pressed = at_sample(line(3) + 100);

        rig.press(pressed, capture_bars::one);
        rig.run_through(pressed);

        CHECK(rig.target().loop().outcome.result == capture_result::nothing_fits);
        CHECK(rig.target().loop().outcome.limit == capture_limit::history);
    }
}

TEST_CASE("a press is refused when its pages do not fit in the pool beside the history and the "
          "loop it would replace")
{
    // The history's 46 and 13 more: room for one 4-bar loop at 120 BPM, and not for a second.
    steady_rig      setup{46 + 13};
    clocked_engine& rig   = setup.rig;
    const engine&   audio = rig.target();

    const auto first = at_sample(line(6) + 7'200);

    rig.press(first, capture_bars::four);
    rig.run_through(first);

    REQUIRE(audio.loop().outcome.result == capture_result::committed);
    REQUIRE(audio.slot().page_count() == 13U);

    // A press that would replace it is counted beside it, as though no page were shared.
    const auto second = at_sample(line(7) + 7'200);

    rig.press(second, capture_bars::four);
    rig.run_through(second);

    CHECK(audio.loop().outcome.result == capture_result::pool_full);
    CHECK(audio.loop().outcome.asked == capture_bars::four);
    CHECK(audio.slot().first_bar() == 2);
    check_holders(audio);

    // Nor does one bar: 46 + 13 + 4 is too many.
    const auto third = at_sample(line(8) + 7'200);

    rig.press(third, capture_bars::one);
    rig.run_through(third);

    CHECK(audio.loop().outcome.result == capture_result::pool_full);
    CHECK(audio.loop().outcome_serial == 3U);
}

TEST_CASE("a new press supersedes one still waiting for its line, which gives its pages back")
{
    steady_rig      setup;
    clocked_engine& rig   = setup.rig;
    const engine&   audio = rig.target();

    const auto first = at_sample(line(6) - 4'800);

    rig.press(first, capture_bars::four);
    rig.run_through(first);

    REQUIRE(audio.loop().waiting);
    REQUIRE(audio.waiting().first_bar() == 2);

    // Half as early, asking for 2 bars: the same line, and only bars 4 and 5.
    const auto second = at_sample(line(6) - 2'400);

    rig.press(second, capture_bars::two);
    rig.run_through(second);

    CHECK(audio.loop().waiting);
    CHECK(audio.loop().outcome.asked == capture_bars::two);
    CHECK(audio.loop().outcome_serial == 2U);
    CHECK(audio.waiting().first_bar() == 4);
    CHECK(audio.waiting().held().first == samples{line(4) - margin});
    check_holders(audio);

    rig.run_through(at_sample(line(6)));

    CHECK(audio.loop().slot == slot_state::provisional);
    CHECK(audio.slot().first_bar() == 4);
    check_holders(audio);
}

TEST_CASE("a press whose capture takes the slot replaces the loop there, final or not")
{
    steady_rig      setup;
    clocked_engine& rig   = setup.rig;
    const engine&   audio = rig.target();

    SUBCASE("provisional")
    {
        const auto first = at_sample(line(6) + 7'200);

        rig.press(first, capture_bars::four);
        rig.run_through(first);
        REQUIRE(audio.loop().slot == slot_state::provisional);

        const auto second = at_sample(line(6) + 9'000);

        rig.press(second, capture_bars::two);
        rig.run_through(second);

        CHECK(audio.loop().slot == slot_state::provisional);
        CHECK_FALSE(audio.loop().waiting);
        CHECK(audio.waiting().empty());
        CHECK(audio.slot().first_bar() == 4);
        CHECK(audio.slot().held().first == samples{line(4) - margin});
        check_holders(audio);
    }

    SUBCASE("final")
    {
        const auto first = at_sample(line(6) + 7'200);

        rig.press(first, capture_bars::four);
        rig.run_through(at_sample(line(7)));
        REQUIRE(audio.loop().slot == slot_state::final);

        const auto second = at_sample(line(9) + 7'200);

        rig.press(second, capture_bars::one);
        rig.run_through(second);

        CHECK(audio.slot().first_bar() == 8);
        CHECK_FALSE(audio.loop().waiting);
        CHECK(audio.waiting().empty());
        check_holders(audio);
    }
}

TEST_CASE("a late press holds the slot at once, so an early press in the same block waits beside "
          "it")
{
    // A 2-bar loop is in the slot. Then, a tick apart in one block, a late press for 4 bars ending
    // at bar 7's line, and an early press for 1 bar ending at bar 8's. The late capture replaces
    // the loop at its press; the early one waits for its line, and replaces the late one there.
    steady_rig      setup;
    clocked_engine& rig   = setup.rig;
    const engine&   audio = rig.target();

    rig.press(at_sample(line(4) + 7'200), capture_bars::two);
    rig.run_through(at_sample(line(5)));
    REQUIRE(audio.slot().first_bar() == 2);

    const sample_time quarter_before = at_sample(line(8) - (beat_samples / 4));

    rig.press(quarter_before - sample_time::from_ticks(1), capture_bars::four);
    rig.press(quarter_before, capture_bars::one);
    rig.run_through(quarter_before);

    CHECK(audio.loop().outcome_serial == 3U);
    CHECK(audio.slot().first_bar() == 3);
    CHECK(audio.loop().slot == slot_state::final);
    REQUIRE(audio.loop().waiting);
    CHECK(audio.waiting().first_bar() == 7);
    check_holders(audio);

    rig.run_through(at_sample(line(8)));

    CHECK(audio.slot().first_bar() == 7);
    CHECK_FALSE(audio.loop().waiting);
    check_holders(audio);
}

TEST_CASE("a press sees the fill-ins due before it, even within its block")
{
    // A stopped clock at 200 samples a pulse, in blocks of 3,072. Bar 1's line, a fill-in, is due
    // to commit a sample into the press's block. A 1-bar press a pulse before bar 2's line needs
    // it as its start line: recorded, it fits.
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{engine_config{.rate = rate_48k, .max_block_frames = 3'072}, tape};

    constexpr std::int64_t freewheel_pulse = 200;
    constexpr std::int64_t start_line      = (2 * 3'072) - 99;
    constexpr std::int64_t first           = start_line - (pulses_per_bar * freewheel_pulse);

    // Real pulses up to 5 before bar 1's line; then the grid freewheels.
    rig.steady_clock(at_sample(first), at_sample(freewheel_pulse), pulses_per_bar - 4);

    const auto pressed =
        at_sample(start_line + (pulses_per_bar * freewheel_pulse) - (freewheel_pulse / 2));

    rig.press(pressed, capture_bars::one);
    rig.run_through(pressed);

    CHECK(rig.target().loop().outcome.result == capture_result::committed);
    REQUIRE(rig.target().loop().waiting);
    CHECK(rig.target().waiting().lines().front().position == samples{start_line});
}

TEST_CASE("a refused press still supersedes a waiting one, and leaves the slot's loop in place")
{
    // 46 + 13 + 5: a 4-bar loop and a 1-bar capture waiting fit, and a second 4-bar one does not.
    steady_rig      setup{46 + 13 + 5};
    clocked_engine& rig   = setup.rig;
    const engine&   audio = rig.target();

    const auto first = at_sample(line(6) + 7'200);

    rig.press(first, capture_bars::four);
    rig.run_through(at_sample(line(7)));
    REQUIRE(audio.loop().slot == slot_state::final);

    const auto waiting = at_sample(line(8) - 4'800);

    rig.press(waiting, capture_bars::one);
    rig.run_through(waiting);
    REQUIRE(audio.loop().waiting);

    const auto refused = at_sample(line(8) - 2'400);

    rig.press(refused, capture_bars::four);
    rig.run_through(refused);

    CHECK(audio.loop().outcome.result == capture_result::pool_full);
    CHECK_FALSE(audio.loop().waiting);
    CHECK(audio.waiting().empty());
    CHECK(audio.slot().first_bar() == 2);
    check_holders(audio);
}

TEST_CASE("the window is cut L_in after each line, to the nearest sample, halves up")
{
    // 100.5 samples: every line lands on a half, and rounds up.
    std::vector<stereo_frame> tape   = tape_of(p1_pages, rate_48k);
    engine_config             config = config_48k;

    config.input_latency = sample_time::from_ticks((100 * 65'536) + 32'768);

    clocked_engine rig{config, tape};

    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 30 * pulses_per_bar);

    const auto pressed = at_sample(line(6) + 7'200);

    rig.press(pressed, capture_bars::four);
    rig.run_through(pressed);

    const captured_loop& loop = rig.target().slot();

    REQUIRE(loop.bars() == 4U);

    for (std::size_t k = 0; k <= 4; ++k) {
        CAPTURE(k);
        CHECK(loop.lines()[k].position == samples{line(2 + static_cast<std::int64_t>(k)) + 101});
    }

    CHECK(loop.held().first == samples{line(2) + 101 - margin});
    CHECK(loop.held().end == samples{line(6) + reserve + 101 + margin});
}

TEST_CASE("a hiccup on the end line is the loop's end until the line is final, and then repaired")
{
    // Bar 6's pulse comes 1,800 samples late, and the next is back on the grid.
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{config_48k, tape};
    constexpr std::int64_t    hiccup = 1'800;

    for (std::int64_t pulse = 0; pulse < 30 * pulses_per_bar; ++pulse) {
        const std::int64_t late = pulse == 6 * pulses_per_bar ? hiccup : 0;

        rig.pulse(at_sample((pulse * pulse_samples) + late));
    }

    const auto pressed = at_sample(line(6) + 7'200);

    rig.press(pressed, capture_bars::four);
    rig.run_through(pressed);

    const engine&        audio = rig.target();
    const captured_loop& loop  = audio.slot();

    REQUIRE(loop.bars() == 4U);
    CHECK(loop.lines().back().status == line_status::edge);
    CHECK(loop.lines().back().position == samples{line(6) + hiccup});
    CHECK(loop.held().end == samples{line(6) + hiccup + reserve + margin});

    rig.run_through(at_sample(line(6) + (8 * pulse_samples)));

    CHECK(audio.loop().slot == slot_state::final);
    CHECK(loop.lines().back().status == line_status::final);
    CHECK(loop.lines().back().position == samples{line(6)});
    CHECK(loop.held().end == samples{line(6) + margin});
    check_holders(audio);
}

TEST_CASE("an end line whose edge comes late is fitted again when it arrives")
{
    // A 1-bar early press before bar 3, whose pulse comes 2,000 samples late. At the press its
    // run ends at line 3 + half a pulse + 50 ms, 293,400, in page 8; from the edge it ends at
    // 295,400, in page 9, which starts at 294,912.
    constexpr std::int64_t late    = 2'000;
    constexpr std::int64_t pressed = line(3) - 4'800;

    const auto clock_with_late_line = [](clocked_engine& rig) {
        for (std::int64_t pulse = 0; pulse < 12 * pulses_per_bar; ++pulse) {
            rig.pulse(
                at_sample((pulse * pulse_samples) + (pulse == 3 * pulses_per_bar ? late : 0)));
        }
    };

    SUBCASE("with room for the page more, it takes the slot, and lets go of it once repaired")
    {
        steady_rig      setup{46 + 5, 0};
        clocked_engine& rig   = setup.rig;
        const engine&   audio = rig.target();

        clock_with_late_line(rig);
        rig.press(at_sample(pressed), capture_bars::one);
        rig.run_through(at_sample(pressed));

        REQUIRE(audio.loop().waiting);
        CHECK(audio.waiting().page_count() == 4U);

        rig.run_through(at_sample(line(3) + late));

        CHECK(audio.loop().slot == slot_state::provisional);
        CHECK(audio.loop().outcome.result == capture_result::committed);
        CHECK(audio.loop().outcome_serial == 1U);
        CHECK(audio.slot().held().end == samples{line(3) + late + reserve + margin});
        CHECK(audio.slot().page_count() == 5U);
        check_holders(audio);

        rig.run_through(at_sample(line(3) + (8 * pulse_samples)));

        CHECK(audio.loop().slot == slot_state::final);
        CHECK(audio.slot().lines().back().position == samples{line(3)});
        CHECK(audio.slot().page_count() == 4U);
        check_holders(audio);
    }

    SUBCASE("without it, it is refused at its line, and gives its pages back")
    {
        steady_rig      setup{46 + 4, 0};
        clocked_engine& rig   = setup.rig;
        const engine&   audio = rig.target();

        clock_with_late_line(rig);
        rig.press(at_sample(pressed), capture_bars::one);
        rig.run_through(at_sample(pressed));

        REQUIRE(audio.loop().waiting);

        rig.run_through(at_sample(line(3) + late));

        CHECK(audio.loop().outcome.result == capture_result::pool_full);
        CHECK(audio.loop().outcome_serial == 2U);
        CHECK_FALSE(audio.loop().waiting);
        CHECK(audio.loop().slot == slot_state::empty);
        check_holders(audio);
    }
}

TEST_CASE("while the clock is stopped, a capture cuts on the fill-ins, and waits for one to commit")
{
    // The last real pulse is bar 6's 8th, 2 beats in. 2.5 pulses later the grid freewheels, and
    // each fill-in commits half a pulse after its slot.
    constexpr std::int64_t last_pulse = (6 * pulses_per_bar) + 8;
    constexpr std::int64_t commits    = line(7) + reserve;

    const auto stopped_clock = [](clocked_engine& rig) {
        rig.steady_clock(at_sample(0), at_sample(pulse_samples), last_pulse + 1);
    };

    SUBCASE("a late press on a committed fill-in")
    {
        steady_rig setup{p1_pages, 0};

        stopped_clock(setup.rig);

        const auto pressed = at_sample(line(7) + 7'200);

        setup.rig.press(pressed, capture_bars::four);
        setup.rig.run_through(pressed);

        const engine& audio = setup.rig.target();

        CHECK(audio.loop().slot == slot_state::provisional);
        CHECK(audio.slot().first_bar() == 3);
        CHECK(audio.slot().lines().back().position == samples{line(7)});

        setup.rig.run_through(at_sample(line(7) + (8 * pulse_samples) + reserve));

        CHECK(audio.loop().slot == slot_state::final);
        CHECK(audio.slot().lines().back().position == samples{line(7)});
    }

    SUBCASE("an early press waits for its line's fill-in to commit")
    {
        steady_rig setup{p1_pages, 0};

        stopped_clock(setup.rig);

        const auto pressed = at_sample(line(7) - 4'800);

        setup.rig.press(pressed, capture_bars::four);

        const engine& audio = setup.rig.target();

        // The block before the commit, and the block holding it.
        setup.rig.run_until(samples{commits - 480});
        CHECK(audio.loop().waiting);

        setup.rig.run_through(at_sample(commits));
        CHECK(audio.loop().slot == slot_state::provisional);
        CHECK(audio.slot().lines().back().position == samples{line(7)});
    }
}

TEST_CASE("a held capture keeps its pulses when a reset relabels the grid")
{
    // Two pulses after bar 6's line, a reset makes that pulse bar 0: line 6's pulse is now -2.
    steady_rig      setup;
    clocked_engine& rig   = setup.rig;
    const engine&   audio = rig.target();

    const auto pressed = at_sample(line(6) + 7'200);

    rig.press(pressed, capture_bars::four);
    rig.reset(at_sample(line(6) + (2 * pulse_samples)));
    rig.run_through(at_sample(line(6) + (2 * pulse_samples)));

    REQUIRE(audio.loop().slot == slot_state::provisional);

    // Its line is final at its 8th pulse, as without the reset, and the numbers stay the press's.
    rig.run_until(samples{line(6) + (8 * pulse_samples)});
    CHECK(audio.loop().slot == slot_state::provisional);

    rig.run_through(at_sample(line(6) + (8 * pulse_samples)));
    CHECK(audio.loop().slot == slot_state::final);
    CHECK(audio.slot().first_bar() == 2);
    CHECK_FALSE(audio.slot().numbered_by_reset());
    CHECK(audio.slot().lines().back().position == samples{line(6)});
}

TEST_CASE("bars are numbered as the grid numbers them at the press, and say whether a reset did")
{
    // A reset on bar 2's pulse makes it bar 0, so bar 8's line is bar 6's.
    steady_rig      setup;
    clocked_engine& rig = setup.rig;

    rig.reset(at_sample(line(2)));

    const auto pressed = at_sample(line(8) + 7'200);

    rig.press(pressed, capture_bars::four);
    rig.run_through(pressed);

    CHECK(rig.target().slot().first_bar() == 2);
    CHECK(rig.target().slot().numbered_by_reset());
}

TEST_CASE("a capture command is a press at the start of the next block")
{
    steady_rig      setup;
    clocked_engine& rig = setup.rig;

    // Blocks of 480 frames: 583,200 starts one.
    const std::int64_t block_start = line(6) + 7'200;

    rig.run_until(samples{block_start});
    REQUIRE(rig.target().post(command{.type = command_type::capture, .bars = capture_bars::two}));
    rig.run_through(at_sample(block_start));

    const loop_report& report = rig.target().loop();

    CHECK(report.outcome.result == capture_result::committed);
    CHECK(report.outcome.kept == capture_bars::two);
    CHECK(rig.target().slot().first_bar() == 4);
}

TEST_CASE("presses, supersedes, replacements and finality allocate nothing")
{
    steady_rig      setup{46 + 13 + 5};
    clocked_engine& rig = setup.rig;

    rig.press(at_sample(line(6) - 4'800), capture_bars::four);
    rig.press(at_sample(line(6) - 2'400), capture_bars::two);
    rig.press(at_sample(line(8) + 7'200), capture_bars::four);
    rig.press(at_sample(line(8) + 9'000), capture_bars::eight);
    rig.press(at_sample(line(10) - 4'800), capture_bars::one);
    rig.press(at_sample(line(10) - 2'400), capture_bars::four);

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        rig.run_until(samples{line(12)});
    }

    CHECK(test::realtime_guard::violations() == 0U);
    CHECK(rig.target().loop().outcome_serial == 6U);
}

TEST_CASE("an estimate that would move the end line past what the loop holds keeps the edge")
{
    // Bar 6's pulse comes 3,100 samples early, more than the half pulse reserved at the press. The
    // clock then slows to 6,300 samples a pulse, so by the line's 8th pulse half a pulse is 3,150,
    // and the estimate may repair the line by 3,100. It would put the line, and its 50 ms, 100
    // samples past the loop's last frame: the edge is kept.
    constexpr std::int64_t early  = 3'100;
    constexpr std::int64_t slower = 6'300;

    std::vector<sample_time> pulses;

    for (std::int64_t pulse = 0; pulse <= 6 * pulses_per_bar; ++pulse) {
        pulses.push_back(
            at_sample((pulse * pulse_samples) - (pulse == 6 * pulses_per_bar ? early : 0)));
    }

    for (std::int64_t after = 1; after <= 40; ++after) {
        pulses.push_back(at_sample(line(6) + (after * slower)));
    }

    // The estimate does repair the line, given the same edges.
    clock_history<1'536> memory{};
    clock_tracker        grid{clock_tracker_config::gate(rate_48k), memory};

    for (const sample_time pulse : std::span{pulses}.first((6 * pulses_per_bar) + 9)) {
        grid.pulse(pulse);
    }

    const line_estimate repaired = test::known(estimate_line(grid, 6 * pulses_per_bar));

    REQUIRE(repaired.basis == line_basis::repaired);
    REQUIRE(repaired.time.nearest_sample() == samples{line(6)});

    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{config_48k, tape};

    for (const sample_time pulse : pulses) {
        rig.pulse(pulse);
    }

    // Just after the early edge: a late press, with bar 6's line its end.
    rig.press(at_sample(line(6) - early + 100), capture_bars::four);
    rig.run_through(at_sample(line(6) - early + 100));

    const captured_loop& loop = rig.target().slot();

    REQUIRE(loop.bars() == 4U);
    CHECK(loop.held().end == samples{line(6) - early + reserve + margin});

    rig.run_through(at_sample(line(6) + (8 * slower)));

    REQUIRE(rig.target().loop().slot == slot_state::final);
    CHECK(loop.lines().back().position == samples{line(6) - early});
    CHECK(loop.held().end == samples{line(6) - early + margin});
    check_holders(rig.target());
}

TEST_CASE("a window whose pre-roll starts at the history's first frame fits, and one a sample "
          "earlier does not")
{
    // Just after power-up the history begins at sample 0. With the clock from sample 2,400, bar
    // 0's line has exactly its 50 ms before it; from 2,399, one sample short.
    for (const std::int64_t first_pulse : {2'400, 2'399}) {
        CAPTURE(first_pulse);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{config_48k, tape};

        rig.steady_clock(at_sample(first_pulse), at_sample(pulse_samples), 3 * pulses_per_bar);

        const auto pressed = at_sample(first_pulse + line(1) + 7'200);

        rig.press(pressed, capture_bars::one);
        rig.run_through(pressed);

        const capture_outcome outcome = rig.target().loop().outcome;

        if (first_pulse == 2'400) {
            CHECK(outcome.result == capture_result::committed);
            CHECK(rig.target().slot().held().first == samples{0});
        }
        else {
            CHECK(outcome.result == capture_result::nothing_fits);
            CHECK(outcome.limit == capture_limit::history);
        }
    }
}

TEST_CASE("the pre-roll and the read margin are at least 50 ms at any rate")
{
    // 50 ms at 48,001 Hz is 2,400.05 samples: rounded up, 2,401.
    constexpr sample_rate rate{48'001U};

    std::vector<stereo_frame> tape = tape_of(p1_pages, rate);
    clocked_engine            rig{engine_config{.rate = rate, .max_block_frames = 480}, tape};

    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 10 * pulses_per_bar);

    const auto pressed = at_sample(line(6) + (8 * pulse_samples));

    rig.press(pressed, capture_bars::four);
    rig.run_through(pressed);

    REQUIRE(rig.target().loop().slot == slot_state::final);
    CHECK(rig.target().slot().held().first == samples{line(2) - 2'401});
    CHECK(rig.target().slot().held().end == samples{line(6) + 2'401});
}

// ---- Capture on its own, over grids of shorter bars
// -------------------------------------------------------------------------------------------
//
// The engine's grid has 16 pulses a bar, so every line before the end line has its 8 pulses at
// the press. With fewer, a line can still be raw at the press, and capture reserves half a pulse
// before the start line too (DESIGN.md §3.4).

/// Capture with its own pool, recorder and tracker, run as the engine runs it: each block's pulses,
/// then the tracker to the block's end, the recording, and capture's follow().
struct capture_rig {
    static constexpr std::size_t block_frames = 480;

    std::vector<stereo_frame> memory;
    page_pool                 pool;
    test::page_audit          audit;
    recorder                  history;
    clock_history<1'536>      clock_memory{};
    clock_tracker             grid;
    capture                   captured;

    std::vector<sample_time> pulses;
    std::size_t              next_pulse{0};
    std::vector<float>       silence;

    explicit capture_rig(const clock_tracker_config& clock, std::int64_t page_frames = 32'768)
        : memory(std::size_t{140} * static_cast<std::size_t>(page_frames)),
          pool{memory, samples{page_frames}},
          audit{pool},
          history{pool, clock.rate, 46},
          grid{clock, clock_memory},
          captured{pool, history, clock.rate, sample_time{}},
          silence(block_frames)
    {
    }

    void deliver_before(sample_time end)
    {
        while (next_pulse < pulses.size() && pulses[next_pulse] < end) {
            grid.pulse(pulses[next_pulse]);
            ++next_pulse;
        }
    }

    void run_until(samples end)
    {
        while (history.end() < end) {
            const auto count = static_cast<std::size_t>(
                std::min(static_cast<std::int64_t>(block_frames), (end - history.end()).value()));
            const sample_time block_end = sample_time::from_samples(
                history.end() + samples{static_cast<std::int64_t>(count)});

            deliver_before(block_end);
            grid.advance(block_end);
            history.record(std::span{silence}.first(count), std::span{silence}.first(count));
            captured.follow(grid);
        }
    }

    /// Runs the blocks before `when`, then presses at `when` mid-block, as the engine does.
    void press(sample_time when, capture_bars bars)
    {
        run_until(when.whole_samples());
        deliver_before(when + sample_time::from_ticks(1));
        grid.advance(when);
        captured.press(when, bars, grid);
    }
};

/// One beat a bar at 4 PPQN: 4 pulses a bar, 24,000 samples at 120 BPM.
constexpr clock_tracker_config one_beat_bars{.rate = rate_48k, .beats_per_bar = 1};

TEST_CASE("a line before the end line that is not final at the press is held with a reserve, and "
          "settles")
{
    // 2 bars of 4 pulses before bar 10's line, at 240,000. Pressed 0.8 pulse early, the start
    // line, 8 pulses back, has 7 after it, and the middle line 3.
    constexpr std::int64_t short_bar = 4 * pulse_samples;
    constexpr std::int64_t end_line  = 10 * short_bar;
    constexpr std::int64_t start     = end_line - (2 * short_bar);

    capture_rig rig{one_beat_bars};

    for (std::int64_t pulse = 0; pulse < 100; ++pulse) {
        rig.pulses.push_back(at_sample(pulse * pulse_samples));
    }

    rig.press(at_sample(end_line - 4'800), capture_bars::two);

    const captured_loop& waiting = rig.captured.waiting();

    REQUIRE(waiting.bars() == 2U);
    CHECK(waiting.lines()[0].status == line_status::edge);
    CHECK(waiting.lines()[1].status == line_status::edge);
    CHECK(waiting.lines()[2].status == line_status::predicted);
    CHECK(waiting.held().first == samples{start - reserve - margin});

    // The end line's pulse is the start line's 8th: the start is final as the capture takes the
    // slot, and is fitted again without the reserve before it. The middle line is final at the
    // end line's 4th pulse, and the loop at its 8th.
    rig.run_until(samples{end_line + 1});

    const captured_loop& loop = rig.captured.slot();

    REQUIRE(loop.bars() == 2U);
    CHECK(loop.lines()[0].status == line_status::final);
    CHECK(loop.lines()[1].status == line_status::edge);
    CHECK(loop.held().first == samples{start - margin});

    rig.run_until(samples{end_line + (4 * pulse_samples) + 1});
    CHECK(loop.lines()[1].status == line_status::final);
    CHECK_FALSE(loop.is_final());

    rig.run_until(samples{end_line + (8 * pulse_samples) + 1});
    CHECK(loop.is_final());
    CHECK(loop.held().first == samples{start - margin});
    CHECK(loop.held().end == samples{end_line + margin});

    for (std::size_t k = 0; k <= 2; ++k) {
        CHECK(loop.lines()[k].position ==
              samples{start + (static_cast<std::int64_t>(k) * short_bar)});
    }
}

TEST_CASE("a capture that waited is fitted again with the reserve from its press")
{
    // One bar of 4 pulses before bar 10's line, pressed 0.8 pulse early: the start line has 3
    // pulses after it, so half a pulse is reserved before it. The end line's pulse comes 600
    // samples late, and the tracker's pulse grows by about 440 samples with it. Fitted again at
    // the edge, the capture keeps the press's half pulse each side.
    constexpr std::int64_t short_bar = 4 * pulse_samples;
    constexpr std::int64_t end_line  = 10 * short_bar;
    constexpr std::int64_t start     = end_line - short_bar;
    constexpr std::int64_t late      = 600;

    capture_rig rig{one_beat_bars};

    for (std::int64_t pulse = 0; pulse < 60; ++pulse) {
        rig.pulses.push_back(at_sample((pulse * pulse_samples) + (pulse == 40 ? late : 0)));
    }

    rig.press(at_sample(end_line - 4'800), capture_bars::one);

    REQUIRE(rig.captured.waiting().bars() == 1U);
    CHECK(rig.captured.waiting().held().first == samples{start - reserve - margin});

    rig.run_until(samples{end_line + late + 1});

    const captured_loop& loop = rig.captured.slot();

    REQUIRE(loop.bars() == 1U);
    REQUIRE(test::known(rig.grid.pulse_period()) > at_sample(pulse_samples + 400));
    CHECK(loop.lines()[0].status == line_status::edge);
    CHECK(loop.held().first == samples{start - reserve - margin});
    CHECK(loop.held().end == samples{end_line + late + reserve + margin});
}

TEST_CASE("a line before the end line whose pulse has not arrived cannot be held")
{
    // 2 PPQN, one beat a bar: a bar is 2 pulses, and a quarter beat half a pulse. The press is
    // half a pulse before bar 10's line, pulse 20, so that is its end line, and the start line is
    // pulse 18, a bar back. Pulse 18 comes 10,000 samples late, after the press. The press is
    // 2.5 pulses after pulse 17, the last moment before the grid would freewheel and fill pulse
    // 18 in.
    capture_rig rig{
        clock_tracker_config{.rate = rate_48k, .pulses_per_beat = 2, .beats_per_bar = 1}};

    for (std::int64_t pulse = 0; pulse < 18; ++pulse) {
        rig.pulses.push_back(at_sample(pulse * pulse_samples));
    }

    rig.pulses.push_back(at_sample((18 * pulse_samples) + 10'000));
    rig.pulses.push_back(at_sample((19 * pulse_samples) + 10'000));

    rig.press(at_sample((20 * pulse_samples) - reserve), capture_bars::one);

    CHECK(rig.captured.report().outcome.result == capture_result::nothing_fits);
    CHECK(rig.captured.report().outcome.limit == capture_limit::history);
    CHECK(rig.captured.waiting().empty());
}

TEST_CASE("an estimate that would move the start line before what the loop holds keeps the edge")
{
    // A bar of 4 pulses before bar 10's line. The start line's pulse, 36, comes 3,200 samples
    // late, more than the half pulse reserved at the press. Its next 5 pulses are on the grid; from
    // pulse 42, its 6th, the clock slows to 7,000 samples a pulse. By its 8th the tracker's half
    // pulse has grown past 3,200, while the side after the line still fits its first 5 edges, so
    // the estimate repairs the line by 3,200: before what is held.
    constexpr std::int64_t short_bar = 4 * pulse_samples;
    constexpr std::int64_t end_line  = 10 * short_bar;
    constexpr std::int64_t start     = end_line - short_bar;
    constexpr std::int64_t late      = 3'200;
    constexpr std::int64_t slower    = 7'000;

    std::vector<sample_time> pulses;

    for (std::int64_t pulse = 0; pulse <= 41; ++pulse) {
        pulses.push_back(at_sample((pulse * pulse_samples) + (pulse == 36 ? late : 0)));
    }

    for (std::int64_t after = 1; after <= 40; ++after) {
        pulses.push_back(at_sample((41 * pulse_samples) + (after * slower)));
    }

    // The estimate does repair the line, given the same edges.
    clock_history<1'536> memory{};
    clock_tracker        grid{one_beat_bars, memory};

    for (const sample_time pulse : std::span{pulses}.first(36 + 9)) {
        grid.pulse(pulse);
    }

    const line_estimate repaired = test::known(estimate_line(grid, 36));

    REQUIRE(repaired.basis == line_basis::repaired);
    REQUIRE(repaired.time.nearest_sample() == samples{start});

    capture_rig rig{one_beat_bars};

    rig.pulses = pulses;
    rig.press(at_sample(end_line - 4'800), capture_bars::one);

    REQUIRE(rig.captured.waiting().bars() == 1U);
    CHECK(rig.captured.waiting().held().first == samples{start + late - reserve - margin});

    rig.run_until(samples{start + (12 * pulse_samples)});

    const captured_loop& loop = rig.captured.slot();

    REQUIRE(loop.bars() == 1U);
    CHECK(loop.lines()[0].status == line_status::final);
    CHECK(loop.lines()[0].position == samples{start + late});
    CHECK(loop.held().first == samples{start + late - reserve - margin});
}

TEST_CASE("a capture spanning exactly 46 pages fits, and one a sample longer does not")
{
    // Pages of 2,048 frames, so 46 pages are 94,208. At 9 PPQN and 7 beats a bar, a bar is 63
    // pulses: at 1,408 samples a pulse, 88,704. An early press holds the bar, half a pulse of
    // reserve and 50 ms each side: 88,704 + 704 + 4,800 = 94,208. The end line is a half sample
    // and 5 ticks past a whole one, so both ends of the run round up.
    //
    // A period 2 ticks longer moves the start line 126 ticks earlier and the end of the reserve 1
    // tick later: the start then rounds down, and the run is a sample longer.
    constexpr clock_tracker_config meter{
        .rate            = rate_48k,
        .pulses_per_beat = 9,
        .beats_per_bar   = 7,
    };
    constexpr std::int64_t page_frames = 2'048;
    constexpr std::int64_t period      = 1'408;
    constexpr std::int64_t bar_pulses  = 63;

    for (const std::int64_t extra_ticks : {0, 2}) {
        CAPTURE(extra_ticks);

        capture_rig rig{meter, page_frames};

        const sample_time pulse = at_sample(period) + sample_time::from_ticks(extra_ticks);
        const sample_time end_line =
            at_sample(3 * bar_pulses * period) + sample_time::from_ticks(32'768 + 5);
        const sample_time first = end_line - (pulse * (3 * bar_pulses));

        for (std::int64_t i = 0; i < 4 * bar_pulses; ++i) {
            rig.pulses.push_back(first + (pulse * i));
        }

        // An early press, a tenth of a beat before the line.
        rig.press(end_line - (pulse * 9 / 10), capture_bars::one);

        const capture_outcome outcome = rig.captured.report().outcome;

        if (extra_ticks == 0) {
            REQUIRE(outcome.result == capture_result::committed);
            CHECK(rig.captured.waiting().held().end - rig.captured.waiting().held().first ==
                  samples{46 * page_frames});
        }
        else {
            CHECK(outcome.result == capture_result::nothing_fits);
            CHECK(outcome.limit == capture_limit::max_pages);
        }
    }
}

}  // namespace
}  // namespace previously
