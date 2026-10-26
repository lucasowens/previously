// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: what capture's tests share.

#include "capture_fixtures.hpp"

#include "previously/audio_block.hpp"
#include "previously/captured_loop.hpp"
#include "previously/clock_tracker.hpp"

#include "planted_clock.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>

namespace previously::test {

std::vector<stereo_frame> tape_of(std::size_t pages, sample_rate rate)
{
    return std::vector<stereo_frame>(pages *
                                     static_cast<std::size_t>(page_frames_for(rate).value()));
}

clocked_engine::clocked_engine(const engine_config& config, std::span<stereo_frame> tape)
    : engine_{config, tape},
      in_left_(config.max_block_frames),
      in_right_(config.max_block_frames),
      out_left_(config.max_block_frames),
      out_right_(config.max_block_frames)
{
    // Room for hours of downbeats and bar lines, so that recording them never allocates while a
    // test guards the engine against allocating.
    constexpr std::size_t room = 4'096;

    downbeats_.reserve(room);
    bar_lines_.reserve(room);
}

void clocked_engine::pulse(sample_time when)
{
    add(timed_event{.type = clock_event_type::pulse, .when = when});
}

void clocked_engine::steady_clock(sample_time first, sample_time period, std::int64_t count)
{
    for (std::int64_t i = 0; i < count; ++i) {
        pulse(first + (period * i));
    }
}

void clocked_engine::reset(sample_time when)
{
    add(timed_event{.type = clock_event_type::reset, .when = when});
}

void clocked_engine::press(sample_time when, capture_bars bars)
{
    add(timed_event{.type = clock_event_type::capture, .bars = bars, .when = when});
}

void clocked_engine::run_until(samples end)
{
    const std::size_t block_frames = engine_.config().max_block_frames;

    // One block's events at most: every edge is 4 ms or more from the next, and presses are few.
    std::array<clock_event, 256> block_events{};

    while (engine_.sample_clock() < end) {
        const samples start = engine_.sample_clock();
        const auto    count = static_cast<std::size_t>(
            std::min(static_cast<std::int64_t>(block_frames), (end - start).value()));
        const sample_time block_start = sample_time::from_samples(start);
        const sample_time block_end =
            block_start + sample_time::from_samples(samples{static_cast<std::int64_t>(count)});

        std::size_t delivered = 0;

        while (next_event_ < events_.size() && events_[next_event_].when < block_end) {
            const timed_event& event = events_[next_event_];

            REQUIRE(event.when >= block_start);
            REQUIRE(delivered < block_events.size());

            block_events[delivered] = clock_event{
                .type   = event.type,
                .bars   = event.bars,
                .offset = event.when - block_start,
            };
            ++delivered;
            ++next_event_;
        }

        for (std::size_t i = 0; i < count; ++i) {
            const stereo_frame frame =
                input_ ? input_(start.value() + static_cast<std::int64_t>(i)) : stereo_frame{};

            in_left_[i]  = frame.left;
            in_right_[i] = frame.right;
        }

        engine_.process(
            audio_block{
                .in_left   = std::span{in_left_}.first(count),
                .in_right  = std::span{in_right_}.first(count),
                .out_left  = std::span{out_left_}.first(count),
                .out_right = std::span{out_right_}.first(count),
            },
            std::span{block_events}.first(delivered));

        if (kept_from_) {
            for (std::size_t i = 0; i < count; ++i) {
                kept_.push_back(stereo_frame{.left = out_left_[i], .right = out_right_[i]});
            }
        }

        // FNV-1a over each sample's bits.
        for (std::size_t i = 0; i < count; ++i) {
            for (const float value : {out_left_[i], out_right_[i]}) {
                output_hash_ ^= std::bit_cast<std::uint32_t>(value);
                output_hash_ *= 1'099'511'628'211U;
            }
        }

        const loop_report& report = engine_.loop();

        for (const loop_downbeat& beat : std::span{report.downbeats}.first(report.downbeat_count)) {
            downbeats_.push_back(
                played_downbeat{.sample = start.value() + beat.offset.value(), .bar = beat.bar});
        }

        const clock_report& clock = engine_.clock();

        for (const grid_pulse& pulse : std::span{clock.pulses}.first(clock.pulse_count)) {
            if (pulse.starts_bar) {
                bar_lines_.push_back((block_start + pulse.offset).nearest_sample().value());
            }
        }
    }
}

const std::vector<clocked_engine::played_downbeat>& clocked_engine::downbeats() const noexcept
{
    return downbeats_;
}

const std::vector<std::int64_t>& clocked_engine::bar_lines() const noexcept
{
    return bar_lines_;
}

std::uint64_t clocked_engine::output_hash() const noexcept
{
    return output_hash_;
}

void clocked_engine::play(std::function<stereo_frame(std::int64_t)> input)
{
    input_ = std::move(input);
}

void clocked_engine::keep_output()
{
    kept_from_ = engine_.sample_clock().value();
    kept_.clear();
}

stereo_frame clocked_engine::output_at(std::int64_t sample) const
{
    // Checked only when it fails: a test reads many thousands of samples.
    if (kept_from_ && sample >= *kept_from_ && std::cmp_less(sample - *kept_from_, kept_.size())) {
        return kept_[static_cast<std::size_t>(sample - *kept_from_)];
    }

    FAIL("no output kept at sample " << sample);
    return {};
}

stereo_frame
frame_held(const engine& audio_engine, const captured_loop& loop, std::int64_t position)
{
    const std::int64_t page_frames = audio_engine.pages().page_frames().value();
    const std::int64_t page        = floor_divide(position, page_frames);
    const std::int64_t held        = page - loop.first_page();

    if (position < 0 || held < 0 || std::cmp_greater_equal(held, loop.pages().size())) {
        return {};
    }

    const auto frames = audio_engine.pages().frames(loop.pages()[static_cast<std::size_t>(held)]);

    return frames[static_cast<std::size_t>(position - (page * page_frames))];
}

void clocked_engine::run_through(sample_time when)
{
    const auto         block_frames = static_cast<std::int64_t>(engine_.config().max_block_frames);
    const std::int64_t block        = when.whole_samples().value() / block_frames;

    run_until(samples{(block + 1) * block_frames});
}

engine& clocked_engine::target() noexcept
{
    return engine_;
}

void clocked_engine::add(const timed_event& event)
{
    REQUIRE(event.when >= sample_time::from_samples(engine_.sample_clock()));

    // After every event at the same time or earlier: same-time events keep the order added.
    const auto later = std::upper_bound(events_.begin() + static_cast<std::ptrdiff_t>(next_event_),
                                        events_.end(),
                                        event.when,
                                        [](sample_time when, const timed_event& other) {
                                            return when < other.when;
                                        });

    events_.insert(later, event);
}

std::optional<std::int64_t> end_of(const captured_loop& loop)
{
    if (loop.empty()) {
        return std::nullopt;
    }

    return loop.lines().back().position.value();
}

void check_holders(const engine& audio_engine)
{
    const page_pool&   pool        = audio_engine.pages();
    const recorder&    history     = audio_engine.history();
    const std::int64_t page_frames = pool.page_frames().value();

    std::vector<int> expected(pool.page_count(), 0);

    // The history holds each page from its oldest to its newest.
    if (history.end() > history.begin()) {
        const std::int64_t oldest = history.begin().value() / page_frames;
        const std::int64_t newest = (history.end().value() - 1) / page_frames;

        for (std::int64_t number = oldest; number <= newest; ++number) {
            if (const auto page = history.page_at(samples{number * page_frames})) {
                ++expected[*page];
            }
        }
    }

    for (const captured_loop* loop :
         {&audio_engine.slot(), &audio_engine.waiting(), &audio_engine.leaving()}) {
        for (const page_pool::page_id page : loop->pages()) {
            ++expected[page];
        }
    }

    std::size_t wrong = 0;

    for (std::size_t i = 0; i < pool.page_count(); ++i) {
        wrong +=
            std::cmp_equal(pool.holders(static_cast<page_pool::page_id>(i)), expected[i]) ? 0U : 1U;
    }

    CHECK(wrong == 0U);

    // And each loop holds every page of its run the recorder has taken, final or not.
    for (const captured_loop* loop :
         {&audio_engine.slot(), &audio_engine.waiting(), &audio_engine.leaving()}) {
        std::size_t taken = 0;

        for (std::size_t i = 0; i < loop->page_count(); ++i) {
            const std::int64_t number = loop->first_page() + static_cast<std::int64_t>(i);

            taken += number * page_frames < history.end().value() ? 1U : 0U;
        }

        CHECK(loop->pages().size() == taken);
    }
}

namespace {

void describe_loop(std::ostringstream& text, const char* name, const captured_loop& loop)
{
    text << name << ": " << loop.bars() << " bars from bar " << loop.first_bar()
         << (loop.numbered_by_reset() ? " by a reset" : "") << ", lines";

    for (const loop_line& each : loop.lines()) {
        text << " [" << each.sequence << " " << each.time.ticks() << " " << each.position.value()
             << " " << static_cast<int>(each.status) << "]";
    }

    text << ", holding " << loop.held().first.value() << " to " << loop.held().end.value()
         << " in pages " << loop.first_page() << " + " << loop.page_count() << ", "
         << loop.pages().size() << " held\n";
}

[[nodiscard]] sample_time at_time(double samples_in)
{
    return sample_time::from_ticks(
        std::llround(samples_in * static_cast<double>(sample_time::ticks_per_sample)));
}

}  // namespace

std::string describe_capture(const engine& audio_engine)
{
    const loop_report& report = audio_engine.loop();

    std::ostringstream text;

    text << "slot " << static_cast<int>(report.slot) << (report.waiting ? ", waiting" : "")
         << ", outcome " << static_cast<int>(report.outcome.result) << " of "
         << bar_count(report.outcome.asked) << " keeping " << bar_count(report.outcome.kept)
         << " limit " << static_cast<int>(report.outcome.limit) << ", serial "
         << report.outcome_serial << "\n";

    describe_loop(text, "slot", audio_engine.slot());
    describe_loop(text, "waiting", audio_engine.waiting());

    return text.str();
}

void add_random_scenario(clocked_engine& rig, std::uint64_t seed, samples length)
{
    test_noise noise{seed};

    // Whole samples or not; any period from 80 to 180 BPM at 4 PPQN. A quarter of the clocks are
    // as fast as the tracker takes, or nearly, in whole samples: at 192 samples half a pulse is the
    // 2 ms in which a reset claims a pulse, so a reset at a fill-in's commit claims that fill-in.
    const std::array<double, 4> fast{192.0, 192.0, 193.0, 256.0};
    const bool                  quick  = noise.uniform() < 0.25;
    const bool                  whole  = quick || noise.uniform() < 0.5;
    const double                jitter = quick || noise.uniform() < 0.5 ? 0.0 : 3.0;
    double period = quick ? fast[static_cast<std::size_t>(noise.uniform() * 4.0)]
                          : 4'000.0 + (5'000.0 * noise.uniform());

    if (whole) {
        period = std::round(period);
    }

    const auto end  = static_cast<double>(length.value());
    double     time = 1'000.0 + std::round(period * noise.uniform());

    // The clock's edges in delivery order: a reset at the same moment as a pulse goes before or
    // after it, as drawn.
    struct edge_event {
        clock_event_type type;
        sample_time      when;
    };

    std::vector<edge_event> edges;

    while (time < end) {
        const double edge = time + noise.uniform(jitter);

        // A reset around a pulse, within the 2 ms that make them one moment, or exactly at it:
        // around one pulse in a hundred, and on a fast clock one in a thousand, so that a reset at
        // a commit (below) is still what numbers the bars when a press comes.
        const double reset = noise.uniform() * (quick ? 10.0 : 1.0);
        const double near  = noise.uniform() < 0.3 ? 0.0 : noise.uniform(96.0);

        if (reset < 0.01) {
            edges.push_back({.type = clock_event_type::reset, .when = at_time(edge + near)});
        }

        edges.push_back({.type = clock_event_type::pulse, .when = at_time(edge)});

        if (reset >= 0.01 && reset < 0.02) {
            edges.push_back({.type = clock_event_type::reset, .when = at_time(edge + near)});
        }

        // Mostly steady. Now and then the clock stops and restarts at a new phase. Unless it is
        // fast it may also slow by 2.5x or more, so that its fill-ins are retired, speed back up,
        // or drift: a fast clock keeps its period, so that its fill-ins commit half a pulse after
        // their slots to the tick, where resets tie with them.
        const double drawn  = noise.uniform();
        const double change = quick && drawn >= 0.015 ? 1.0 : drawn;

        if (change < 0.015) {
            time += period * (2.6 + (4.0 * noise.uniform()));
        }
        else if (change < 0.03 && period < 12'000.0) {
            const std::array<double, 4> slowdowns{3.5, 4.5, 2.6 + noise.uniform(), 5.0};

            period *= slowdowns[static_cast<std::size_t>(noise.uniform() * 4.0)];
            time += period;
        }
        else if (change < 0.04 && period > 12'000.0) {
            period = std::round(period / 4.0);
            time += period;
        }
        else if (change < 0.05) {
            period *= 0.95 + (0.1 * noise.uniform());
            time += period;
        }
        else {
            time += period;
        }

        if (whole) {
            time = std::round(time);
        }
    }

    std::ranges::stable_sort(edges, {}, &edge_event::when);

    // Where the engine's grid will put each bar line, and when it arrives: as its pulse, or as its
    // fill-in commits. Found by a tracker fed as the engine feeds its own.
    struct arrival {
        sample_time when;
        sample_time line;
        sample_time period;
    };

    clock_history<1'536> memory{};
    clock_tracker        grid{clock_tracker_config::gate(sample_rate{48'000U}), memory};

    std::vector<arrival>     arrivals;
    std::vector<sample_time> commits;
    std::int64_t             recorded = -1;

    const auto note_arrivals = [&](sample_time now) {
        const std::optional<std::int64_t> latest = grid.latest_pulse();
        const std::optional<sample_time>  each   = grid.pulse_period();

        if (!latest || !each) {
            return;
        }

        for (std::int64_t index = std::max(recorded + 1, *latest - 3); index <= *latest; ++index) {
            const std::optional<sample_time> line = grid.pulse_time(index);

            if (line && index % grid.pulses_per_bar() == 0) {
                arrivals.push_back({.when = now, .line = *line, .period = *each});
            }
        }

        recorded = *latest;
    };

    for (const edge_event& edge : edges) {
        for (auto due = grid.next_fill_in(); due && *due < edge.when;) {
            grid.advance(*due);
            commits.push_back(*due);
            note_arrivals(*due);

            // Each commit comes after the last, or this would loop for ever.
            const std::optional<sample_time> next = grid.next_fill_in();

            REQUIRE((!next || *next > *due));
            due = next;
        }

        if (edge.type == clock_event_type::pulse) {
            grid.pulse(edge.when);
            note_arrivals(edge.when);
            rig.pulse(edge.when);
        }
        else {
            grid.reset(edge.when);
            recorded = grid.latest_pulse().value_or(-1);
            rig.reset(edge.when);
        }
    }

    // Presses after the first 3 s. Most come in pairs: an early press within a quarter beat of a
    // line, waiting for it, and a second as the line arrives, a tick either side of it, or in the
    // same 3,072 frames. Some lines get a reset too, just before their pulse or just after, while
    // the first press waits or once it has started. The rest are a tick either side of a fill-in's
    // commit, or anywhere; and a few resets are at a commit.
    constexpr std::array<capture_bars, 4> counts{
        capture_bars::one,
        capture_bars::two,
        capture_bars::four,
        capture_bars::eight,
    };

    const auto bars_drawn = [&] {
        return counts[static_cast<std::size_t>(noise.uniform() * 4.0)];
    };
    const sample_time three_seconds = at_time(144'000.0);
    const sample_time one_tick      = sample_time::from_ticks(1);

    std::erase_if(arrivals, [&](const arrival& each) {
        return each.line - (each.period * 2) < three_seconds;
    });
    std::erase_if(commits, [&](sample_time each) {
        return each < three_seconds;
    });

    for (int press = 0; press < 8 && !arrivals.empty(); ++press) {
        const arrival& line = arrivals[static_cast<std::size_t>(
            noise.uniform() * static_cast<double>(arrivals.size()))];

        const auto lead = std::llround(static_cast<double>(line.period.ticks()) * noise.uniform());

        rig.press(line.line - sample_time::from_ticks(lead), bars_drawn());

        const std::array<sample_time, 4> seconds{
            line.when,
            line.when - one_tick,
            line.when + one_tick,
            line.when + at_time(3'072.0 * noise.uniform()),
        };
        const sample_time after = seconds[static_cast<std::size_t>(noise.uniform() * 4.0)];

        if (after >= three_seconds) {
            rig.press(after, bars_drawn());
        }

        const double reset = noise.uniform();

        if (reset < 0.15) {
            rig.reset(line.when - at_time(96.0 * noise.uniform()));
        }
        else if (reset < 0.3) {
            rig.reset(line.when + (reset < 0.2 ? one_tick : at_time(96.0 * noise.uniform())));
        }
    }

    for (int press = 0; press < 4 && !commits.empty(); ++press) {
        const sample_time commit = commits[static_cast<std::size_t>(
            noise.uniform() * static_cast<double>(commits.size()))];

        rig.press(commit + (noise.uniform() < 0.5 ? -one_tick : one_tick), bars_drawn());
    }

    // Resets at a fill-in's commit as well: a tick either side of it, or exactly on it.
    for (int reset = 0; reset < 6 && !commits.empty(); ++reset) {
        const sample_time                commit = commits[static_cast<std::size_t>(
            noise.uniform() * static_cast<double>(commits.size()))];
        const std::array<sample_time, 3> around{commit - one_tick, commit, commit + one_tick};

        rig.reset(around[static_cast<std::size_t>(noise.uniform() * 3.0)]);
    }

    for (int press = 0; press < 4; ++press) {
        rig.press(three_seconds + at_time((end - 144'000.0) * noise.uniform()), bars_drawn());
    }
}

void check_any_block_size(std::uint64_t                seed,
                          std::span<const std::size_t> block_sizes,
                          samples                      checkpoint,
                          std::int64_t                 checkpoints)
{
    CAPTURE(seed);

    std::vector<std::string> reference;

    for (const std::size_t frames : block_sizes) {
        CAPTURE(frames);
        REQUIRE(checkpoint.value() % static_cast<std::int64_t>(frames) == 0);

        // P1's 187 pages, the history and three loops of 47; or fewer, so that a loop in the slot
        // can keep out a press, and whether a press comes after a capture has started shows.
        const std::array<std::size_t, 3> pages{187, 62, 70};
        std::vector<stereo_frame>        tape =
            tape_of(pages[static_cast<std::size_t>(seed % 3)], sample_rate{48'000U});
        clocked_engine rig{
            engine_config{.rate = sample_rate{48'000U}, .max_block_frames = frames},
            tape,
        };

        add_random_scenario(rig, seed, checkpoint * checkpoints);

        // Noise in, so that the loops played carry it, and the output is compared to the bit.
        rig.play([seed](std::int64_t sample) {
            test_noise noise{(seed * 1'000'003U) + static_cast<std::uint64_t>(sample)};

            return stereo_frame{
                .left  = static_cast<float>(noise.uniform(0.5)),
                .right = static_cast<float>(noise.uniform(0.5)),
            };
        });

        for (std::int64_t k = 1; k <= checkpoints; ++k) {
            CAPTURE(k);

            rig.run_until(checkpoint * k);
            check_holders(rig.target());

            const std::string described = describe_capture(rig.target()) + "output " +
                                          std::to_string(rig.output_hash()) + "\n";
            const auto        index     = static_cast<std::size_t>(k - 1);

            if (reference.size() <= index) {
                reference.push_back(described);
            }
            else {
                CHECK(described == reference[index]);
            }
        }
    }
}

}  // namespace previously::test
