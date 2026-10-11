// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the engine's clock wiring. Edges in through process(), the grid report out.
//
// The property that matters most for clock out and the beat lights: every grid pulse is reported
// exactly once, at its time, whatever happens around block boundaries: jitter, freewheeling,
// resets, tempo jumps, and every block size.

#include "previously/engine.hpp"

#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <vector>

namespace previously {
namespace {

constexpr double gate_period = 6'000.0;  // 120 BPM at 4 PPQN, 48 kHz

[[nodiscard]] sample_time at(double time_in_samples)
{
    return sample_time::from_ticks(static_cast<std::int64_t>(
        std::llround(time_in_samples * static_cast<double>(sample_time::ticks_per_sample))));
}

[[nodiscard]] double in_samples(sample_time time)
{
    return static_cast<double>(time.ticks()) / static_cast<double>(sample_time::ticks_per_sample);
}

/// A grid pulse as the test sees it: absolute time in samples.
struct reported_pulse {
    double       time{0.0};
    std::int64_t index{0};
    bool         starts_beat{false};
    bool         starts_bar{false};
};

/// Edges to deliver, at absolute times in samples, in time order.
struct edge {
    double           time{0.0};
    clock_event_type type{clock_event_type::pulse};
};

/// Runs the engine for `total_frames`, in blocks of `block_frames`, delivering each edge in the
/// block that contains it. Returns every reported grid pulse with its absolute time.
[[nodiscard]] std::vector<reported_pulse>
run(engine& target, std::span<const edge> edges, std::size_t total_frames, std::size_t block_frames)
{
    std::vector<reported_pulse> reported;
    std::vector<float>          silence(block_frames, 0.0F);
    std::vector<float>          out_left(block_frames);
    std::vector<float>          out_right(block_frames);
    std::vector<clock_event>    events;
    std::size_t                 next_edge = 0;

    for (std::size_t start = 0; start < total_frames; start += block_frames) {
        const double block_start = in_samples(sample_time::from_samples(target.sample_clock()));
        const double block_end   = block_start + static_cast<double>(block_frames);

        events.clear();
        while (next_edge < edges.size() && edges[next_edge].time < block_end) {
            events.push_back(clock_event{
                .type   = edges[next_edge].type,
                .offset = at(edges[next_edge].time - block_start),
            });
            ++next_edge;
        }

        target.process(
            audio_block{
                .in_left   = silence,
                .in_right  = silence,
                .out_left  = out_left,
                .out_right = out_right,
            },
            events);

        const clock_report& report = target.clock();
        for (std::size_t i = 0; i < report.pulse_count; ++i) {
            const grid_pulse& pulse = report.pulses[i];

            // Offsets must lie inside the block: an adapter turns them into sample positions.
            CHECK(pulse.offset >= sample_time{});
            CHECK(in_samples(pulse.offset) < static_cast<double>(block_frames));

            reported.push_back(reported_pulse{
                .time        = block_start + in_samples(pulse.offset),
                .index       = pulse.index,
                .starts_beat = pulse.starts_beat,
                .starts_bar  = pulse.starts_bar,
            });
        }
    }

    return reported;
}

[[nodiscard]] std::vector<edge> pulses_at(std::span<const double> times)
{
    std::vector<edge> edges;
    edges.reserve(times.size());

    for (const double time : times) {
        edges.push_back(edge{.time = time});
    }

    return edges;
}

[[nodiscard]] std::vector<double> steady(double start, double period, std::size_t count)
{
    std::vector<double> times;
    times.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        times.push_back(start + (period * static_cast<double>(i)));
    }

    return times;
}

[[nodiscard]] std::unique_ptr<engine> make_engine(std::size_t block_frames)
{
    return std::make_unique<engine>(
        engine_config{.rate = sample_rate{48'000U}, .max_block_frames = block_frames});
}

/// Checks that consecutive reported pulses are spaced between 0.5 and 1.5 `period`s apart:
/// no pulse reported twice, none skipped.
void check_spacing(std::span<const reported_pulse> pulses, double period)
{
    for (std::size_t i = 1; i < pulses.size(); ++i) {
        const double spacing = pulses[i].time - pulses[i - 1].time;

        CAPTURE(i);
        CAPTURE(spacing);
        CHECK(spacing > period * 0.5);
        CHECK(spacing < period * 1.5);
    }
}

TEST_CASE("a steady clock is reported pulse for pulse at every block size")
{
    const auto times = steady(100.25, gate_period, 64);
    const auto edges = pulses_at(times);

    for (const std::size_t block : {1U, 16U, 32U, 48U}) {
        CAPTURE(block);

        auto       audio_engine = make_engine(block);
        const auto reported =
            run(*audio_engine, edges, static_cast<std::size_t>(times.back()) + 10, block);

        // Reporting starts when the tempo is known: from the second pulse.
        REQUIRE(reported.size() == times.size() - 1);

        for (std::size_t i = 0; i < reported.size(); ++i) {
            CAPTURE(i);
            CHECK(std::abs(reported[i].time - times[i + 1]) < 0.01);
            CHECK(reported[i].index == static_cast<std::int64_t>(i + 1));
            CHECK(reported[i].starts_beat == ((i + 1) % 4 == 0));
            CHECK(reported[i].starts_bar == ((i + 1) % 16 == 0));
        }

        CHECK(audio_engine->clock().state == clock_state::locked);
        CHECK(audio_engine->clock().beat_period == at(gate_period * 4.0));
    }
}

TEST_CASE("jittered edges are still reported exactly once")
{
    // Jitter moves estimates across block boundaries; each pulse must still appear once.
    std::mt19937                           generator{11U};
    std::uniform_real_distribution<double> jitter{-40.0, 40.0};

    auto times = steady(1'000.0, gate_period, 200);
    for (double& time : times) {
        time += jitter(generator);
    }

    auto       audio_engine = make_engine(32);
    const auto reported =
        run(*audio_engine, pulses_at(times), static_cast<std::size_t>(times.back()) + 10, 32);

    CHECK(reported.size() == times.size() - 1);
    check_spacing(reported, gate_period);

    for (std::size_t i = 1; i < reported.size(); ++i) {
        CHECK(reported[i].index == reported[i - 1].index + 1);
    }
}

TEST_CASE("while the clock is stopped the grid keeps being reported, and returns cleanly")
{
    // 40 pulses, silence for 20 periods, then the clock returns at a new phase.
    for (const double phase : {0.0, 0.4, -0.4}) {
        CAPTURE(phase);

        std::vector<double> times = steady(0.0, gate_period, 40);
        const auto          later = steady(gate_period * (60.0 + phase), gate_period, 20);
        times.insert(times.end(), later.begin(), later.end());

        auto       audio_engine = make_engine(32);
        const auto reported =
            run(*audio_engine, pulses_at(times), static_cast<std::size_t>(times.back()) + 10, 32);

        // Freewheeling reports continue across the silence: nothing skipped, nothing doubled.
        check_spacing(reported, gate_period);

        // After the return, reports follow the real pulses.
        CHECK(std::abs(reported.back().time - times.back()) < 0.01);
    }
}

TEST_CASE("a reset relabels the grid without doubling or dropping a pulse")
{
    const auto        times      = steady(0.0, gate_period, 60);
    std::vector<edge> edges      = pulses_at(times);
    const double      reset_time = times[37];
    edges.insert(edges.begin() + 38, edge{.time = reset_time, .type = clock_event_type::reset});

    auto       audio_engine = make_engine(32);
    const auto reported =
        run(*audio_engine, edges, static_cast<std::size_t>(times.back()) + 10, 32);

    REQUIRE(reported.size() == times.size() - 1);
    check_spacing(reported, gate_period);

    const auto at_reset = std::ranges::find_if(reported, [&](const reported_pulse& pulse) {
        return std::abs(pulse.time - reset_time) < 0.01;
    });
    REQUIRE(at_reset != reported.end());
    CHECK(at_reset->index == 0);
    CHECK(at_reset->starts_bar);
    CHECK(std::next(at_reset)->index == 1);
}

TEST_CASE("the reset_grid command makes the pulse at the start of the next block bar 0")
{
    const auto times = steady(0.0, gate_period, 60);

    auto audio_engine = make_engine(32);

    // Run to just before pulse 20, post the command, then let pulse 20 arrive. The command is
    // applied at the start of the block containing pulse 20, within 2 ms of it.
    const std::size_t before_pulse_20 = static_cast<std::size_t>(times[20]) - 16;
    const auto        first_part      = pulses_at(std::span{times}.first(20));
    static_cast<void>(run(*audio_engine, first_part, before_pulse_20 - (before_pulse_20 % 32), 32));

    REQUIRE(audio_engine->post(command{.type = command_type::reset_grid}));

    const auto rest     = pulses_at(std::span{times}.subspan(20));
    const auto reported = run(*audio_engine,
                              rest,
                              static_cast<std::size_t>(times.back()) + 10 -
                                  static_cast<std::size_t>(audio_engine->sample_clock().value()),
                              32);

    const auto pulse_20 = std::ranges::find_if(reported, [&](const reported_pulse& pulse) {
        return std::abs(pulse.time - times[20]) < 0.01;
    });
    REQUIRE(pulse_20 != reported.end());
    CHECK(pulse_20->index == 0);
    CHECK(pulse_20->starts_bar);
}

TEST_CASE("tempo changes are reported without doubled or lost pulses")
{
    SUBCASE("doubling")
    {
        std::vector<double> times = steady(0.0, gate_period, 40);
        const auto faster = steady(times.back() + (gate_period / 2.0), gate_period / 2.0, 40);
        times.insert(times.end(), faster.begin(), faster.end());

        auto       audio_engine = make_engine(32);
        const auto reported =
            run(*audio_engine, pulses_at(times), static_cast<std::size_t>(times.back()) + 10, 32);

        REQUIRE(reported.size() == times.size() - 1);
        for (std::size_t i = 0; i < reported.size(); ++i) {
            CHECK(std::abs(reported[i].time - times[i + 1]) < 0.01);
        }
    }

    SUBCASE("slowing to a third")
    {
        // During the first two slow gaps the clock looks stopped, so the grid freewheels at the
        // old tempo; once the slowdown is confirmed, reports follow the real pulses exactly.
        std::vector<double> times = steady(0.0, gate_period, 40);
        const auto slower = steady(times.back() + (gate_period * 3.0), gate_period * 3.0, 20);
        times.insert(times.end(), slower.begin(), slower.end());

        auto       audio_engine = make_engine(32);
        const auto reported =
            run(*audio_engine, pulses_at(times), static_cast<std::size_t>(times.back()) + 10, 32);

        const auto confirmed = std::span{reported}.last(15);
        const auto expected  = std::span{times}.last(15);
        for (std::size_t i = 0; i < confirmed.size(); ++i) {
            CHECK(std::abs(confirmed[i].time - expected[i]) < 0.01);
        }

        for (std::size_t i = 1; i < reported.size(); ++i) {
            CHECK(reported[i].time - reported[i - 1].time > gate_period * 0.5);
        }
    }
}

TEST_CASE("the sample clock never goes backwards")
{
    // Every timestamp in the engine is measured against it; commands must never rewind it.
    auto audio_engine = make_engine(32);

    static_cast<void>(run(*audio_engine, {}, 320, 32));
    REQUIRE(audio_engine->post(command{.type = command_type::reset_grid}));
    static_cast<void>(run(*audio_engine, {}, 320, 32));

    CHECK(audio_engine->sample_clock() == samples{640});
}

TEST_CASE("processing clock events never allocates")
{
    const auto times = steady(0.0, gate_period, 40);
    const auto edges = pulses_at(times);

    std::vector<clock_event> events{
        clock_event{.type = clock_event_type::pulse, .offset = at(3.5)},
    };
    std::vector<float> silence(32, 0.0F);
    std::vector<float> out_left(32);
    std::vector<float> out_right(32);
    auto               audio_engine = make_engine(32);

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;

        for (int block = 0; block < 10'000; ++block) {
            const bool has_pulse = block % 188 == 0;
            audio_engine->process(
                audio_block{
                    .in_left   = silence,
                    .in_right  = silence,
                    .out_left  = out_left,
                    .out_right = out_right,
                },
                has_pulse ? std::span<const clock_event>{events} : std::span<const clock_event>{});
        }
    }

    CHECK(test::realtime_guard::violations() == 0);
    CHECK(audio_engine->clock().state == clock_state::locked);
}

}  // namespace
}  // namespace previously
