// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: capture under change (DESIGN.md §3.4, "Capture under change"). A capture is
// checked at each moment its state can change, in time order within the block: after the clock
// edges at a moment, and as each fill-in commits. A capture still waiting for its line is planned
// again when the labels change. A loop in the slot keeps its pulses, and a line of it whose pulse
// was retired keeps its edge.
//
// As in test_capture.cpp: 120 BPM in 4/4 at 48 kHz, so a pulse is 6,000 samples and a bar 96,000.
// Each test runs at block sizes that put its decisive moment mid-block, and at a block's end.

#include "previously/capture.hpp"
#include "previously/engine.hpp"

#include "capture_fixtures.hpp"
#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace previously {
namespace {

using test::at_sample;
using test::check_holders;
using test::clocked_engine;
using test::end_of;
using test::tape_of;

constexpr sample_rate  rate_48k{48'000U};
constexpr std::int64_t pulses_per_bar = 16;
constexpr std::int64_t pulse_samples  = 6'000;
constexpr std::int64_t bar_samples    = pulses_per_bar * pulse_samples;
constexpr std::int64_t reserve        = pulse_samples / 2;
constexpr std::int64_t margin         = 2'400;  // 50 ms

/// P1 at its fullest: the history and three loops of 47 pages (DESIGN.md §2.3).
constexpr std::size_t p1_pages = 187;

/// 46 for the history, 13 for a 4-bar loop and 3 more: a 1-bar capture's 4 do not fit beside it.
constexpr std::size_t tight_pages = 62;

const sample_time one_tick = sample_time::from_ticks(1);

/// Where bar `bar`'s line is, with the clock from sample `origin`.
[[nodiscard]] constexpr std::int64_t line(std::int64_t bar, std::int64_t origin = 0)
{
    return origin + (bar * bar_samples);
}

[[nodiscard]] engine_config blocks_of(std::size_t frames)
{
    return engine_config{.rate = rate_48k, .max_block_frames = frames};
}

/// Where each of a loop's lines is on the tape.
[[nodiscard]] std::vector<std::int64_t> positions_of(const captured_loop& loop)
{
    std::vector<std::int64_t> positions;

    for (const loop_line& each : loop.lines()) {
        positions.push_back(each.position.value());
    }

    return positions;
}

/// Lines `count` bars apart from `first`, as positions.
[[nodiscard]] std::vector<std::int64_t> bars_from(std::int64_t first, std::int64_t count)
{
    std::vector<std::int64_t> positions;

    for (std::int64_t k = 0; k <= count; ++k) {
        positions.push_back(first + (k * bar_samples));
    }

    return positions;
}

// ---- The moments
// ------------------------------------------------------------------------------------------

TEST_CASE("a capture waiting for a fill-in starts as it commits: a press a tick later comes after "
          "it, and a tick sooner supersedes it")
{
    // The last real pulse is bar 6's 8th. Bar 7's line is a fill-in, committed half a pulse after
    // its slot. An early 4-bar press waits for it; then a 1-bar late press. In 62 pages the two
    // do not fit together, so the late press is refused if the 4-bar capture has started, and
    // otherwise supersedes it and waits for the same line.
    constexpr std::int64_t commits = line(7) + reserve;

    for (const std::size_t frames : {std::size_t{480}, std::size_t{600}, std::size_t{3'072}}) {
        CAPTURE(frames);  // 600: the commit is at a block's end

        for (const sample_time offset : {-one_tick, sample_time{}, one_tick}) {
            CAPTURE(offset.ticks());

            std::vector<stereo_frame> tape = tape_of(tight_pages, rate_48k);
            clocked_engine            rig{blocks_of(frames), tape};
            const engine&             audio = rig.target();

            rig.steady_clock(at_sample(0), at_sample(pulse_samples), (6 * pulses_per_bar) + 9);
            rig.press(at_sample(line(7) - 4'800), capture_bars::four);
            rig.press(at_sample(commits) + offset, capture_bars::one);
            rig.run_through(at_sample(commits + 1));

            CHECK(audio.loop().outcome_serial == 2U);
            CHECK_FALSE(audio.loop().waiting);
            CHECK(end_of(audio.slot()) == line(7));

            if (offset < sample_time{}) {
                CHECK(audio.loop().outcome.result == capture_result::committed);
                CHECK(audio.slot().bars() == 1U);
            }
            else {
                // At the commit's own moment the commit goes first, as an edge would.
                CHECK(audio.loop().outcome.result == capture_result::pool_full);
                CHECK(audio.slot().bars() == 4U);
            }

            check_holders(audio);
        }
    }
}

TEST_CASE("a capture waiting for a fill-in starts as it commits, though another commits after the "
          "press in the same block")
{
    // A stopped clock of 200-sample pulses, so 16 fill-ins can commit in one block of 3,072. Bar
    // 10's line is a fill-in at 62,940, mid-block. A 1-bar press 150 samples before it waits; the
    // slot before the line closes 50 samples later, and the line's own 200 samples after that,
    // both before the block ends. The capture has started by the block's end.
    constexpr std::int64_t pulse   = 200;
    constexpr std::int64_t line_10 = (20 * 3'072) + 1'500;
    constexpr std::int64_t origin  = line_10 - (10 * pulses_per_bar * pulse);

    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(3'072), tape};
    const engine&             audio = rig.target();

    // Real pulses up to 5 before the line; then the grid freewheels.
    rig.steady_clock(at_sample(origin), at_sample(pulse), (10 * pulses_per_bar) - 4);
    rig.press(at_sample(line_10 - 150), capture_bars::one);
    rig.run_through(at_sample(line_10 + (pulse / 2)));

    CHECK_FALSE(audio.loop().waiting);
    CHECK(audio.loop().slot == slot_state::provisional);
    CHECK(end_of(audio.slot()) == line_10);
}

TEST_CASE("a capture whose line commits at a block's end starts in the next block, before a "
          "command press there")
{
    // Blocks of 600: bar 7's fill-in commits at 675,000, a block's end. The capture waiting for it
    // is seen to start in the next block, with no event in it. A capture command applied at that
    // block's start comes after the start, so in 62 pages it is refused.
    constexpr std::int64_t commits = line(7) + reserve;

    for (const bool commanded : {false, true}) {
        CAPTURE(commanded);

        std::vector<stereo_frame> tape = tape_of(tight_pages, rate_48k);
        clocked_engine            rig{blocks_of(600), tape};
        engine&                   audio = rig.target();

        rig.steady_clock(at_sample(0), at_sample(pulse_samples), (6 * pulses_per_bar) + 9);
        rig.press(at_sample(line(7) - 4'800), capture_bars::four);
        rig.run_until(samples{commits});

        REQUIRE(audio.loop().waiting);

        if (commanded) {
            REQUIRE(audio.post(command{.type = command_type::capture, .bars = capture_bars::one}));
        }

        rig.run_until(samples{commits + 600});

        CHECK_FALSE(audio.loop().waiting);
        CHECK(audio.slot().bars() == 4U);
        CHECK(audio.loop().outcome.result ==
              (commanded ? capture_result::pool_full : capture_result::committed));
        check_holders(audio);
    }
}

// ---- Resets
// -------------------------------------------------------------------------------------------

TEST_CASE("a reset while a capture waits plans it again, numbered by the reset, unless the "
          "capture has started")
{
    // DESIGN.md §3.4's example, at bar 7: no reset has numbered the bars, a 4-bar capture is
    // pressed 0.2 beat before bar 7's line, and a sequencer restarts on that downbeat. The window
    // is the same however the reset arrives; only its numbering differs. Planned again with the
    // same outcome, the capture keeps its serial.
    struct arrival {
        const char*  name{""};
        sample_time  offset;  // from the line
        bool         before_the_pulse{false};
        std::int64_t first_bar{0};
        bool         by_reset{false};
    };

    const std::array<arrival, 6> arrivals{
        arrival{
            .name             = "1 ms before",
            .offset           = at_sample(-48),
            .before_the_pulse = true,
            .first_bar        = -4,
            .by_reset         = true,
        },
        arrival{
            .name             = "a tick before",
            .offset           = -one_tick,
            .before_the_pulse = true,
            .first_bar        = -4,
            .by_reset         = true,
        },
        arrival{
            .name             = "with the pulse, delivered before it",
            .offset           = sample_time{},
            .before_the_pulse = true,
            .first_bar        = -4,
            .by_reset         = true,
        },
        arrival{
            .name             = "with the pulse, delivered after it",
            .offset           = sample_time{},
            .before_the_pulse = false,
            .first_bar        = -4,
            .by_reset         = true,
        },
        arrival{
            .name             = "a tick after",
            .offset           = one_tick,
            .before_the_pulse = false,
            .first_bar        = 3,
            .by_reset         = false,
        },
        arrival{
            .name             = "1 ms after",
            .offset           = at_sample(48),
            .before_the_pulse = false,
            .first_bar        = 3,
            .by_reset         = false,
        },
    };

    for (const std::size_t frames : {std::size_t{480}, std::size_t{3'072}}) {
        CAPTURE(frames);  // 480: bar 7's line starts a block

        for (const arrival& reset : arrivals) {
            CAPTURE(reset.name);

            std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
            clocked_engine            rig{blocks_of(frames), tape};
            const engine&             audio  = rig.target();
            const sample_time         line_7 = at_sample(line(7));

            rig.steady_clock(at_sample(0), at_sample(pulse_samples), 7 * pulses_per_bar);
            rig.press(line_7 - at_sample(4'800), capture_bars::four);

            if (reset.before_the_pulse) {
                rig.reset(line_7 + reset.offset);
            }

            rig.steady_clock(line_7, at_sample(pulse_samples), pulses_per_bar);

            if (!reset.before_the_pulse) {
                rig.reset(line_7 + reset.offset);
            }

            rig.run_through(line_7 + at_sample(48));

            CHECK(audio.loop().slot == slot_state::provisional);
            CHECK(audio.loop().outcome_serial == 1U);
            CHECK(positions_of(audio.slot()) == bars_from(line(3), 4));
            CHECK(audio.slot().first_bar() == reset.first_bar);
            CHECK(audio.slot().numbered_by_reset() == reset.by_reset);
            check_holders(audio);
        }
    }
}

TEST_CASE("a reset 1 ms before the line plans the waiting capture again at the reset")
{
    // Blocks of 480: the reset is in the block before the line's, and at that block's end the
    // capture still waits, already numbered by the reset.
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};
    const engine&             audio = rig.target();

    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 8 * pulses_per_bar);
    rig.press(at_sample(line(7) - 4'800), capture_bars::four);
    rig.reset(at_sample(line(7) - 48));
    rig.run_until(samples{line(7)});

    REQUIRE(audio.loop().waiting);
    CHECK(audio.waiting().first_bar() == -4);
    CHECK(audio.waiting().numbered_by_reset());
    CHECK(audio.loop().slot == slot_state::empty);
}

TEST_CASE("an early press that a late reset makes late starts at once, a pulse earlier")
{
    // DESIGN.md §3.4's second example. The clock starts at sample 10,000, so bar 7's line is at
    // 682,000. A 4-bar capture is pressed 1 ms after the pulse before it, at 676,048: an early
    // press, waiting. A reset for that pulse arrives 1 ms after the press, within the 2 ms a reset
    // may trail its pulse, and the pulse becomes bar 0, 1 ms before the press. Planned again, the
    // capture is a late press whose line is recorded. Its window starts a pulse earlier, a page
    // before the pages it held: it takes that page from the history.
    constexpr std::int64_t origin  = 10'000;
    constexpr std::int64_t before  = line(7, origin) - pulse_samples;  // the pulse before bar 7
    constexpr std::int64_t pressed = before + 48;
    constexpr std::int64_t page    = 32'768;

    struct arrival {
        const char* name{""};
        sample_time when;
    };

    const std::array<arrival, 3> arrivals{
        arrival{.name = "a tick before the press", .when = at_sample(pressed) - one_tick},
        arrival{.name = "a tick after the press", .when = at_sample(pressed) + one_tick},
        arrival{.name = "1 ms after the press", .when = at_sample(pressed + 48)},
    };

    for (const arrival& reset : arrivals) {
        CAPTURE(reset.name);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{blocks_of(64), tape};
        const engine&             audio = rig.target();

        rig.steady_clock(at_sample(origin), at_sample(pulse_samples), 8 * pulses_per_bar);
        rig.press(at_sample(pressed), capture_bars::four);
        rig.reset(reset.when);

        // Blocks of 64: the press's block ends at 676,096, the third reset's moment.
        rig.run_until(samples{pressed + 48});

        if (reset.when >= at_sample(pressed + 48)) {
            REQUIRE(audio.loop().waiting);
            CHECK(audio.waiting().first_bar() == 3);
            CHECK(audio.waiting().first_page() == (line(3, origin) - 2'400) / page);
        }

        rig.run_through(at_sample(pressed + 48));

        CHECK_FALSE(audio.loop().waiting);
        CHECK(audio.loop().slot == slot_state::provisional);
        CHECK(audio.loop().outcome_serial == 1U);
        CHECK(positions_of(audio.slot()) == bars_from(before - (4 * bar_samples), 4));
        CHECK(audio.slot().first_bar() == -4);
        CHECK(audio.slot().numbered_by_reset());
        CHECK(audio.slot().first_page() == (before - (4 * bar_samples) - 2'400) / page);
        check_holders(audio);
    }
}

TEST_CASE("a late press racing a reset delivered just after its pulse keeps its window")
{
    // The reset race §3.4 decided. Bar 6's 9th pulse, two beats in, is at 624,000; a late 4-bar
    // press 1 ms after it ends at bar 6's line, final already, and starts at once. A reset for that
    // pulse 1 ms later makes it bar 0: the capture has started, and keeps its window and
    // numbering. A reset a tick before the press numbers the press itself: the pulse is then its
    // end line, just arrived.
    constexpr std::int64_t pulse   = line(6) + (8 * pulse_samples);
    constexpr std::int64_t pressed = pulse + 48;

    for (const bool reset_first : {false, true}) {
        CAPTURE(reset_first);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{blocks_of(480), tape};
        const engine&             audio = rig.target();

        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 8 * pulses_per_bar);
        rig.press(at_sample(pressed), capture_bars::four);
        rig.reset(reset_first ? at_sample(pressed) - one_tick : at_sample(pressed + 48));
        rig.run_through(at_sample(pressed + 48));

        CHECK(audio.loop().slot == (reset_first ? slot_state::provisional : slot_state::final));
        CHECK(audio.loop().outcome_serial == 1U);

        if (reset_first) {
            CHECK(positions_of(audio.slot()) == bars_from(pulse - (4 * bar_samples), 4));
            CHECK(audio.slot().first_bar() == -4);
            CHECK(audio.slot().numbered_by_reset());
        }
        else {
            CHECK(positions_of(audio.slot()) == bars_from(line(2), 4));
            CHECK(audio.slot().first_bar() == 2);
            CHECK_FALSE(audio.slot().numbered_by_reset());
        }

        check_holders(audio);
    }
}

// ---- Retirement
// -------------------------------------------------------------------------------------------

/// A clock steady to pulse `last`, then slowed `slowdown` times: each later pulse that much
/// further on. `slowdown` is in thousandths, so every time is a whole sample.
void slowed_clock(clocked_engine& rig, std::int64_t last, std::int64_t slowdown)
{
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), last + 1);

    for (std::int64_t k = 1; k <= 12; ++k) {
        rig.pulse(at_sample((last * pulse_samples) + (k * slowdown * pulse_samples / 1'000)));
    }
}

TEST_CASE("a loop whose end line is the first fill-in is kept through a retirement: at 4.5x its "
          "line keeps its edge, and at 4.51x it is final first")
{
    // §3.4's case. The last real pulse is bar 5's last, at 570,000, and bar 6's line is the first
    // fill-in, at 576,000. An early 4-bar press waits for it, and starts as it commits, 2.5 pulses
    // after the last real pulse. The clock has slowed k times:
    //   - the 8th pulse after the line, the 4th fill-in after the clock returns, commits k + 4.5
    //     pulses after the last real pulse;
    //   - the second slow pulse retires the fill-ins, 2k pulses after it.
    // At 4.5x they are one moment, and the edge goes first: the end line's pulse is retired, and
    // the line keeps its edge, final at the retirement. At 4.51x the line is final 0.01 pulse
    // sooner, and keeps its estimate, which is its edge here too.
    constexpr std::int64_t last = (6 * pulses_per_bar) - 1;

    struct slowdown {
        std::int64_t thousandths{0};
        std::int64_t final_at{0};  // the moment the loop is final
    };

    const std::array<slowdown, 2> slowdowns{
        slowdown{.thousandths = 4'500, .final_at = 624'000},
        slowdown{.thousandths = 4'510, .final_at = 624'060},
    };

    for (const slowdown& slowed : slowdowns) {
        CAPTURE(slowed.thousandths);

        for (const std::size_t frames : {std::size_t{32}, std::size_t{480}, std::size_t{3'072}}) {
            CAPTURE(frames);  // 32 and 480: 624,000 starts a block

            std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
            clocked_engine            rig{blocks_of(frames), tape};
            const engine&             audio = rig.target();

            slowed_clock(rig, last, slowed.thousandths);
            rig.press(at_sample(line(6) - 3'000), capture_bars::four);

            // Started as the first fill-in commits, 2.5 pulses and a tick after the last pulse.
            rig.run_through(at_sample(585'000) + one_tick);
            REQUIRE(audio.loop().slot == slot_state::provisional);

            // Not final before its moment, and final at it, at any block size.
            const auto frames_64 = static_cast<std::int64_t>(frames);

            rig.run_until(samples{(slowed.final_at / frames_64) * frames_64});
            CHECK(audio.loop().slot == slot_state::provisional);

            // Final, it holds only its window and margins: the reserve has gone back.
            rig.run_through(at_sample(slowed.final_at));
            CHECK(audio.loop().slot == slot_state::final);
            CHECK(end_of(audio.slot()) == line(6));
            CHECK(positions_of(audio.slot()) == bars_from(line(2), 4));
            CHECK(audio.slot().held().end == samples{line(6) + margin});
            CHECK(audio.loop().outcome_serial == 1U);
            check_holders(audio);
        }
    }
}

/// The waiting tie: a clock steady to pulse 16m - 7, then a gap of 3.5 pulses, a returning pulse,
/// and the slowed clock's second pulse 3.5 pulses later, plus `late`. After the return the grid
/// runs on from the returning pulse, so bar m's line is a fill-in 3 pulses after it, and commits
/// at the very moment of the second slow pulse: 7 pulses after the last steady one.
void tie_clock(clocked_engine& rig, std::int64_t bar, sample_time late)
{
    const std::int64_t last  = (bar * pulses_per_bar) - 7;
    const sample_time  steps = at_sample(pulse_samples * 7 / 2);

    rig.steady_clock(at_sample(0), at_sample(pulse_samples), last + 1);
    rig.pulse(at_sample(last * pulse_samples) + steps);
    rig.pulse(at_sample(last * pulse_samples) + (steps * 2) + late);
}

TEST_CASE("a capture waiting for a fill-in is planned again by a retirement at the moment it "
          "commits, and kept a tick later")
{
    // The tie that decides between planned again and kept (DESIGN.md §3.4). Bar 6's line is a
    // fill-in at 573,000, committing at 576,000. An early 4-bar press waits for it at 571,000.
    //   - The retiring pulse at 576,000 goes first: the capture is planned again on the slowed
    //     grid, where the press falls in bar 5, far from bar 6's line. A late press then, it ends
    //     at bar 5's line, at 480,000, final already.
    //   - A tick later, the fill-in has committed and the capture has started. Its end line's
    //     pulse is then retired, and the line keeps its edge, final.
    for (const std::size_t frames : {std::size_t{480}, std::size_t{3'072}}) {
        CAPTURE(frames);  // 480: the tie is at a block's start

        for (const bool tie : {true, false}) {
            CAPTURE(tie);

            std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
            clocked_engine            rig{blocks_of(frames), tape};
            const engine&             audio = rig.target();

            tie_clock(rig, 6, tie ? sample_time{} : one_tick);
            rig.press(at_sample(571'000), capture_bars::four);
            rig.run_through(at_sample(576'000) + one_tick);

            CHECK(audio.loop().slot == slot_state::final);
            CHECK(audio.loop().outcome_serial == 1U);
            CHECK_FALSE(audio.loop().waiting);

            if (tie) {
                CHECK(positions_of(audio.slot()) == bars_from(line(1), 4));
                CHECK(audio.slot().first_bar() == 1);
            }
            else {
                CHECK(positions_of(audio.slot()) ==
                      std::vector<std::int64_t>{line(2), line(3), line(4), line(5), 573'000});
                CHECK(audio.slot().held().end == samples{573'000 + margin});
                CHECK(audio.slot().first_bar() == 2);
            }

            check_holders(audio);
        }
    }
}

TEST_CASE("a capture planned again by a retirement can be shortened by it")
{
    // The same tie at bar 5: the fill-in at 477,000 commits at 480,000. Pressed at 475,000, the
    // 4 bars from bar 1's line fit. Planned again, the capture ends at bar 4's line, and 4 bars
    // would start at bar 0's, the clock's first pulse at sample 0, whose pre-roll was never
    // recorded: shortened to 2 bars by the history, and the serial says so.
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};
    const engine&             audio = rig.target();

    tie_clock(rig, 5, sample_time{});
    rig.press(at_sample(475'000), capture_bars::four);
    rig.run_until(samples{480'000});

    REQUIRE(audio.loop().waiting);
    REQUIRE(audio.loop().outcome.result == capture_result::committed);

    rig.run_through(at_sample(480'000));

    CHECK(audio.loop().outcome.result == capture_result::shortened);
    CHECK(audio.loop().outcome.kept == capture_bars::two);
    CHECK(audio.loop().outcome.limit == capture_limit::history);
    CHECK(audio.loop().outcome_serial == 2U);
    CHECK(positions_of(audio.slot()) == bars_from(line(2), 2));
    CHECK(audio.slot().first_bar() == 2);
    check_holders(audio);
}

// ---- Any block size
// -------------------------------------------------------------------------------------------

TEST_CASE("capture reports and holds the same at any block size")
{
    // One seeded scenario of 13.4 s, compared every 107,520 samples, which blocks of 3,072, 480 and
    // 7 frames all divide. `make seeds` runs many more, at more sizes (test_capture_seeds.cpp).
    constexpr std::array<std::size_t, 3> sizes{3'072, 480, 7};

    test::check_any_block_size(1, sizes, samples{107'520}, 6);
}

TEST_CASE("planning again and keeping retired lines allocate nothing")
{
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};

    tie_clock(rig, 6, sample_time{});
    rig.press(at_sample(571'000), capture_bars::four);
    rig.press(at_sample(578'000), capture_bars::two);
    rig.reset(at_sample(590'000));

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        rig.run_until(samples{600'000});
    }

    CHECK(test::realtime_guard::violations() == 0U);
    CHECK(rig.target().loop().slot != slot_state::empty);
}

}  // namespace
}  // namespace previously
