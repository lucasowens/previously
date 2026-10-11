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
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
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
/// The bar reported at the end of one block.
struct block_bar {
    double                      block_start{0.0};
    std::optional<std::int64_t> bar;
};

[[nodiscard]] std::vector<reported_pulse> run(engine&                 target,
                                              std::span<const edge>   edges,
                                              std::size_t             total_frames,
                                              std::size_t             block_frames,
                                              std::vector<double>*    bar_restarts = nullptr,
                                              std::vector<block_bar>* bars         = nullptr)
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

        // The bar number and the pulse labels describe the same grid: the last pulse reported in
        // a block is in the bar that contains the block's end.
        if (report.pulse_count > 0 && report.bar) {
            const grid_pulse& last = report.pulses[report.pulse_count - 1];
            CHECK(*report.bar == floor_divide(last.index, 16));
        }

        if (bars != nullptr) {
            bars->push_back(block_bar{.block_start = block_start, .bar = report.bar});
        }

        if (report.bar_restarted && bar_restarts != nullptr) {
            bar_restarts->push_back(block_start);
        }
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

TEST_CASE("a steady clock is reported pulse for pulse at every block size from 1 to 48")
{
    const auto times = steady(100.25, gate_period, 24);
    const auto edges = pulses_at(times);

    for (std::size_t block = 1; block <= 48; ++block) {
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
}

TEST_CASE("a sudden slowdown reports every real pulse; the transition is pinned")
{
    // The grid predicts pulses at the old tempo and reports them before a slowdown can be known.
    // Every real pulse must still be reported, at its time or covered by a prediction less than
    // half an old period before it. Reports are never closer than half an old period. Once the
    // slowdown is confirmed, reports follow the real pulses one for one. The number of extra,
    // predicted reports during the transition is pinned so it cannot change silently.
    //
    // Below 2.5x the tracker confirms a tempo change only at the second slow pulse (one pulse
    // can't distinguish it from a hiccup, issue #3), so one more old-grid prediction goes out
    // than with immediate re-locking: 1 -> 2 extras at 1.5x to 2.2x, 2 -> 4 at 2.5x. From 3x
    // the clock looks stopped first and the freewheel path decides, unchanged.
    struct slowdown {
        double      factor;
        std::size_t extra_reports;
    };

    const std::array<slowdown, 6> cases{
        slowdown{.factor = 1.5, .extra_reports = 2},
        slowdown{.factor = 2.0, .extra_reports = 2},
        slowdown{.factor = 2.2, .extra_reports = 2},
        slowdown{.factor = 2.5, .extra_reports = 4},
        slowdown{.factor = 3.0, .extra_reports = 4},
        slowdown{.factor = 4.0, .extra_reports = 6},
    };

    for (const slowdown& c : cases) {
        CAPTURE(c.factor);

        std::vector<double> times = steady(0.0, gate_period, 20);
        const auto          slower =
            steady(times.back() + (gate_period * c.factor), gate_period * c.factor, 8);
        times.insert(times.end(), slower.begin(), slower.end());

        auto       audio_engine = make_engine(32);
        const auto reported =
            run(*audio_engine, pulses_at(times), static_cast<std::size_t>(times.back()) + 10, 32);

        // Pulses 1-19 are reported as usual (19 reports); count what follows.
        const auto after_change = std::span{reported}.subspan(19);
        CHECK(after_change.size() == slower.size() + c.extra_reports);

        for (const double real : slower) {
            const bool covered =
                std::ranges::any_of(after_change, [&](const reported_pulse& pulse) {
                    return pulse.time <= real + 0.01 && real - pulse.time < gate_period / 2.0;
                });
            CAPTURE(real);
            CHECK(covered);
        }

        for (std::size_t i = 1; i < reported.size(); ++i) {
            CHECK(reported[i].time - reported[i - 1].time >= gate_period / 2.0);
        }

        const auto confirmed = std::span{reported}.last(5);
        const auto expected  = std::span{slower}.last(5);
        for (std::size_t i = 0; i < confirmed.size(); ++i) {
            CHECK(std::abs(confirmed[i].time - expected[i]) < 0.01);
        }
    }
}

TEST_CASE("a single late pulse is reported once, with no extras (issue #3)")
{
    // Pulse 20 arrives a third of a period late, then the clock is back on the grid. Before
    // issue #3 was fixed the tracker treated that as two tempo changes and the reporter emitted
    // one extra pulse; now the hiccup changes nothing and every pulse is reported exactly once.
    std::vector<double> times = steady(0.0, gate_period, 40);
    times[20] += gate_period / 3.0;

    auto       audio_engine = make_engine(32);
    const auto reported =
        run(*audio_engine, pulses_at(times), static_cast<std::size_t>(times.back()) + 10, 32);

    CHECK(reported.size() == times.size() - 1);

    for (std::size_t i = 21; i < times.size(); ++i) {
        const bool covered = std::ranges::any_of(reported, [&](const reported_pulse& pulse) {
            return std::abs(pulse.time - times[i]) < gate_period / 2.0;
        });
        CAPTURE(i);
        CHECK(covered);
    }
}

TEST_CASE("a reset while the clock is stopped reports the new bar 0 as it happens")
{
    // The clock stops after pulse 19; a reset lands between grid slots 27 and 28. The next slot,
    // 28, must be reported as index 0 starting a bar, by the reset input and by the command.
    const auto   times      = steady(0.0, gate_period, 20);
    const double reset_time = gate_period * 27.3;

    const auto check = [&](const std::vector<reported_pulse>& reported,
                           const std::vector<double>&         restarts) {
        const auto slot_28 = std::ranges::find_if(reported, [&](const reported_pulse& pulse) {
            return std::abs(pulse.time - (gate_period * 28.0)) < 0.01;
        });
        REQUIRE(slot_28 != reported.end());
        CHECK(slot_28->index == 0);
        CHECK(slot_28->starts_bar);
        CHECK(std::next(slot_28)->index == 1);
        CHECK(restarts.empty());  // shown as bar 0 when reported: no second bar signal
    };

    SUBCASE("reset input")
    {
        std::vector<edge> edges = pulses_at(times);
        edges.push_back(edge{.time = reset_time, .type = clock_event_type::reset});

        auto                audio_engine = make_engine(32);
        std::vector<double> restarts;
        const auto          reported =
            run(*audio_engine, edges, static_cast<std::size_t>(gate_period * 32.0), 32, &restarts);
        check(reported, restarts);
    }

    SUBCASE("reset_grid command")
    {
        auto                audio_engine = make_engine(32);
        std::vector<double> restarts;
        const auto          edges = pulses_at(times);

        auto reported = run(*audio_engine,
                            edges,
                            static_cast<std::size_t>(reset_time) / 32 * 32,
                            32,
                            &restarts);
        REQUIRE(audio_engine->post(command{.type = command_type::reset_grid}));
        const auto rest =
            run(*audio_engine, {}, static_cast<std::size_t>(gate_period * 5.0), 32, &restarts);
        reported.insert(reported.end(), rest.begin(), rest.end());

        check(reported, restarts);
    }
}

TEST_CASE("while a reset is pending the bar reads -1, then 0 from the claimed pulse")
{
    // The clock stopped after pulse 19 (bar 1 runs from pulse 16 to 31). A reset at 27.3
    // periods claims slot 28. Until the reset, the bar is 1; from the reset to slot 28, the
    // pulses before the new bar 0 are numbered -1 and so is their bar; from slot 28 on, bar 0.
    std::vector<edge> edges = pulses_at(steady(0.0, gate_period, 20));
    edges.push_back(edge{.time = gate_period * 27.3, .type = clock_event_type::reset});

    auto                   audio_engine = make_engine(32);
    std::vector<block_bar> bars;
    static_cast<void>(run(*audio_engine,
                          edges,
                          static_cast<std::size_t>(gate_period * 30.0),
                          32,
                          nullptr,
                          &bars));

    for (const block_bar& block : bars) {
        const double block_end = block.block_start + 32.0;

        CAPTURE(block.block_start);
        if (block_end > gate_period * 16.0 && block_end <= gate_period * 27.3) {
            CHECK(block.bar == 1);
        }
        else if (block.block_start >= gate_period * 27.3 && block_end <= gate_period * 28.0) {
            CHECK(block.bar == -1);
        }
        else if (block.block_start >= gate_period * 28.0) {
            CHECK(block.bar == 0);
        }
    }
}

TEST_CASE("a reset on a late edge whose prediction was already reported signals a bar restart")
{
    // Pulse 30 is predicted exactly at a block start and reported there. Its real edge arrives
    // 40 samples late, in the next block, together with a reset. The pulse cannot be reported
    // again, so the report must say the bar restarted, and numbering continues from 1.
    std::vector<double> times = steady(0.0, gate_period, 40);
    times[30] += 40.0;

    std::vector<edge> edges = pulses_at(times);
    edges.insert(edges.begin() + 31, edge{.time = times[30], .type = clock_event_type::reset});

    auto                audio_engine = make_engine(32);
    std::vector<double> restarts;
    const auto          reported =
        run(*audio_engine, edges, static_cast<std::size_t>(times.back()) + 10, 32, &restarts);

    CHECK(restarts.size() == 1);

    // The report nearest pulse 31's edge (its filtered time sits a few samples from the edge
    // after the 40-sample excursion before it) carries index 1.
    const auto pulse_31 = std::ranges::min_element(reported, {}, [&](const reported_pulse& pulse) {
        return std::abs(pulse.time - times[31]);
    });
    REQUIRE(std::abs(pulse_31->time - times[31]) < gate_period / 4.0);
    CHECK(pulse_31->index == 1);
}

TEST_CASE("a pulse exactly half a period after the last report is reported")
{
    // The clock stops after pulse 39 and returns exactly halfway between two grid slots. The
    // returning pulse sits exactly on the edge of the reporting window, and must not be lost.
    std::vector<double> times = steady(0.0, gate_period, 40);
    const auto          later = steady(gate_period * 59.5, gate_period, 10);
    times.insert(times.end(), later.begin(), later.end());

    auto       audio_engine = make_engine(32);
    const auto reported =
        run(*audio_engine, pulses_at(times), static_cast<std::size_t>(times.back()) + 10, 32);

    CHECK(std::ranges::any_of(reported, [&](const reported_pulse& pulse) {
        return std::abs(pulse.time - (gate_period * 59.5)) < 0.01;
    }));
}

TEST_CASE("a large first block reports every pulse")
{
    // A render-sized block holds the acquiring pulse and several more. Once the tempo is known,
    // every pulse in the block is reported, the acquiring pulse included, rather than only the
    // latest. Predictions follow the last real pulse until the final block ends.
    const auto times = steady(100.0, 1'000.0, 12);  // 1,000-sample pulses: three per block

    auto       audio_engine = make_engine(3'072);
    const auto reported =
        run(*audio_engine, pulses_at(times), static_cast<std::size_t>(times.back()) + 10, 3'072);

    REQUIRE(reported.size() >= times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        CAPTURE(i);
        CHECK(std::abs(reported[i].time - times[i]) < 0.01);
        CHECK(reported[i].index == static_cast<std::int64_t>(i));
    }
    for (std::size_t i = times.size(); i < reported.size(); ++i) {
        CHECK(reported[i].time > times.back());
    }
}

TEST_CASE("the block size limit keeps every block's pulses within one report")
{
    // Real pulses are at least 4 ms apart, but during a tempo transition reports can be half a
    // period apart: at worst 2 ms. A report holds 32.
    CHECK(engine::max_block_frames(sample_rate{48'000U}) == 3'072);
    CHECK(engine::max_block_frames(sample_rate{96'000U}) == 6'144);
    CHECK(engine::max_block_frames(sample_rate{8'000U}) == 512);
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
