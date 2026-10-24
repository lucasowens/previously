// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: what capture's tests share.

#include "capture_fixtures.hpp"

#include "previously/audio_block.hpp"
#include "previously/captured_loop.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace previously::test {

std::vector<stereo_frame> tape_of(std::size_t pages, sample_rate rate)
{
    return std::vector<stereo_frame>(pages *
                                     static_cast<std::size_t>(page_frames_for(rate).value()));
}

clocked_engine::clocked_engine(const engine_config& config, std::span<stereo_frame> tape)
    : engine_{config, tape},
      silence_(config.max_block_frames),
      out_left_(config.max_block_frames),
      out_right_(config.max_block_frames)
{
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

        engine_.process(
            audio_block{
                .in_left   = std::span{silence_}.first(count),
                .in_right  = std::span{silence_}.first(count),
                .out_left  = std::span{out_left_}.first(count),
                .out_right = std::span{out_right_}.first(count),
            },
            std::span{block_events}.first(delivered));
    }
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

    for (const captured_loop* loop : {&audio_engine.slot(), &audio_engine.waiting()}) {
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
}

}  // namespace previously::test
