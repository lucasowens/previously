// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the recorder and its history.
//
// Pages of 16 frames and a history of 4 pages, so every boundary is a few frames away. The
// engine's 2^15-frame pages and 46-page history are tested through the engine.

#include "previously/dc_blocker.hpp"
#include "previously/page_pool.hpp"
#include "previously/recorder.hpp"

#include "known.hpp"
#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace previously {
namespace {

using test::known;

constexpr sample_rate rate_48k{48'000U};
constexpr samples     page_frames{16};
constexpr std::size_t history_pages = 4;

/// Position `frame` of page `page`.
constexpr samples at(std::int64_t page, std::int64_t frame = 0)
{
    return samples{(page * page_frames.value()) + frame};
}

/// A stereo noise signal, and what the recorder should make of it.
struct recording {
    std::vector<float>        left;
    std::vector<float>        right;
    std::vector<stereo_frame> expected;

    explicit recording(std::size_t frames)
        : left(frames),
          right(frames)
    {
        std::mt19937                          generator{1234U};
        std::uniform_real_distribution<float> noise{-1.0F, 1.0F};

        for (std::size_t i = 0; i < frames; ++i) {
            left[i]  = noise(generator);
            right[i] = noise(generator) + 0.5F;  // an offset for the blocker to remove
        }

        dc_blocker left_blocker{rate_48k};
        dc_blocker right_blocker{rate_48k};

        for (std::size_t i = 0; i < frames; ++i) {
            expected.push_back(stereo_frame{
                .left  = left_blocker.process(left[i]),
                .right = right_blocker.process(right[i]),
            });
        }
    }

    /// Records the frames from the recorder's end() up to `until`, in blocks of the given sizes,
    /// over and over.
    void record_until(recorder&                          target,
                      samples                            until,
                      std::initializer_list<std::size_t> block_sizes = {7}) const
    {
        const std::span sizes{block_sizes};
        std::size_t     next_size = 0;

        while (target.end() < until) {
            const auto start = static_cast<std::size_t>(target.end().value());
            const auto count =
                std::min(sizes[next_size], static_cast<std::size_t>(until.value()) - start);

            target.record(std::span{left}.subspan(start, count),
                          std::span{right}.subspan(start, count));

            next_size = (next_size + 1) % sizes.size();
        }
    }

    [[nodiscard]] bool recorded_exactly(const recorder& target, samples position) const
    {
        const auto frame = target.frame_at(position);

        if (!frame) {
            return false;
        }

        const auto index = static_cast<std::size_t>(position.value());

        return std::bit_cast<std::uint64_t>(*frame) ==
               std::bit_cast<std::uint64_t>(expected[index]);
    }
};

/// A pool of small pages and the memory under it.
struct small_pool {
    std::vector<stereo_frame> memory;
    page_pool                 pool;

    explicit small_pool(std::size_t pages)
        : memory(pages * static_cast<std::size_t>(page_frames.value())),
          pool{memory, page_frames}
    {
    }
};

TEST_CASE("every frame is recorded at its position, DC-blocked, whatever the blocks")
{
    using block_sizes = std::initializer_list<std::size_t>;

    const recording signal{static_cast<std::size_t>(at(4).value())};

    // One frame at a time, blocks within a page, a block crossing one boundary or four, and a mix.
    for (const block_sizes sizes : {
             block_sizes{1},
             block_sizes{5},
             block_sizes{16},
             block_sizes{17},
             block_sizes{64},
             block_sizes{3, 29, 1, 15},
         }) {
        CAPTURE(*sizes.begin());
        CAPTURE(sizes.size());

        small_pool pages{6};
        recorder   target{pages.pool, rate_48k, history_pages};

        signal.record_until(target, at(4), sizes);

        CHECK(target.end() == at(4));

        for (std::int64_t position = 0; position < at(4).value(); ++position) {
            CAPTURE(position);
            CHECK(signal.recorded_exactly(target, samples{position}));
        }

        CHECK_FALSE(target.frame_at(samples{-1}).has_value());
        CHECK_FALSE(target.frame_at(at(4)).has_value());
    }
}

TEST_CASE("the history is the newest pages: its oldest frame is kept, the one before is not")
{
    const recording signal{static_cast<std::size_t>(at(11).value())};
    small_pool      pages{6};
    recorder        target{pages.pool, rate_48k, history_pages};

    // Pages 7 to 10, the last one partly recorded.
    signal.record_until(target, at(10, 7));

    CHECK(signal.recorded_exactly(target, at(7)));
    CHECK(signal.recorded_exactly(target, at(10, 6)));

    CHECK_FALSE(target.frame_at(at(7) - samples{1}).has_value());
    CHECK_FALSE(target.frame_at(at(10, 7)).has_value());
    CHECK_FALSE(target.page_at(at(7) - samples{1}).has_value());
}

TEST_CASE("a page leaves the history as the page four after it starts, and not before")
{
    const recording signal{static_cast<std::size_t>(at(5).value())};
    small_pool      pages{6};
    recorder        target{pages.pool, rate_48k, history_pages};

    // The last frame of page 3: four pages held, page 0 still in the history.
    signal.record_until(target, at(4), {1});

    CHECK(signal.recorded_exactly(target, at(0)));
    CHECK(pages.pool.free_count() == 2U);

    // The first frame of page 4. Page 0 leaves, and the three pages after it stay complete.
    signal.record_until(target, at(4, 1), {1});

    CHECK_FALSE(target.frame_at(at(1) - samples{1}).has_value());

    for (std::int64_t position = at(1).value(); position <= at(4).value(); ++position) {
        CAPTURE(position);
        CHECK(signal.recorded_exactly(target, samples{position}));
    }

    CHECK(pages.pool.free_count() == 2U);
}

TEST_CASE("the recorder never holds more pages than its history")
{
    // The pool has no page to spare, so taking a page before giving the oldest back would fail
    // and leave a hole.
    const recording signal{static_cast<std::size_t>(at(21).value())};
    small_pool      pages{history_pages};
    recorder        target{pages.pool, rate_48k, history_pages};

    for (std::int64_t page = 0; page <= 20; ++page) {
        signal.record_until(target, at(page, 1), {1});

        // Just after a page starts, the pages started so far are held, up to four: once full, the
        // history is three whole pages and one frame.
        const auto held = std::min(static_cast<std::size_t>(page) + 1U, history_pages);

        CHECK(pages.pool.free_count() == history_pages - held);

        for (std::int64_t position = std::max(at(page - 3).value(), std::int64_t{0});
             position <= at(page).value();
             ++position) {
            CAPTURE(position);
            REQUIRE(signal.recorded_exactly(target, samples{position}));
        }
    }
}

TEST_CASE("a page someone else holds outlives the history, and is not reused")
{
    const recording signal{static_cast<std::size_t>(at(11).value())};
    small_pool      pages{6};

    {
        recorder target{pages.pool, rate_48k, history_pages};

        signal.record_until(target, at(1));

        const page_pool::page_id kept = known(target.page_at(at(0)));
        pages.pool.retain(kept);

        signal.record_until(target, at(11));

        CHECK_FALSE(target.frame_at(at(0)).has_value());
        CHECK(pages.pool.holders(kept) == 1U);
        CHECK(pages.pool.free_count() == 1U);

        for (std::int64_t position = at(7).value(); position < at(11).value(); ++position) {
            CHECK(target.page_at(samples{position}) != kept);
        }

        // The audio is still there, as it was recorded.
        const auto frames = std::as_const(pages.pool).frames(kept);

        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK(std::bit_cast<std::uint64_t>(frames[i]) ==
                  std::bit_cast<std::uint64_t>(signal.expected[i]));
        }

        pages.pool.release(kept);
        CHECK(pages.pool.free_count() == 2U);
    }

    CHECK(pages.pool.free_count() == 6U);
}

TEST_CASE("with no page free, a page goes unrecorded and recording resumes on the next one")
{
    const recording signal{static_cast<std::size_t>(at(6).value())};
    small_pool      pages{5};

    {
        recorder target{pages.pool, rate_48k, history_pages};

        signal.record_until(target, at(4));

        // Something else holds page 0's audio, and the last free page.
        const page_pool::page_id held  = known(target.page_at(at(0)));
        const page_pool::page_id spare = known(pages.pool.acquire());
        pages.pool.retain(held);

        signal.record_until(target, at(5));

        CHECK(target.end() == at(5));
        CHECK_FALSE(target.page_at(at(4)).has_value());
        CHECK_FALSE(target.frame_at(at(4)).has_value());
        CHECK_FALSE(target.frame_at(at(4, 15)).has_value());
        CHECK(signal.recorded_exactly(target, at(1)));
        CHECK(signal.recorded_exactly(target, at(3, 15)));

        // A page comes free, and the next page is recorded. The DC blocker ran on through the
        // hole, so the new page is exactly what it would have been.
        pages.pool.release(spare);
        signal.record_until(target, at(6));

        for (std::int64_t position = at(5).value(); position < at(6).value(); ++position) {
            CAPTURE(position);
            CHECK(signal.recorded_exactly(target, samples{position}));
        }

        CHECK_FALSE(target.frame_at(at(4)).has_value());

        pages.pool.release(held);
    }

    // The hole held no page, and every page is back.
    CHECK(pages.pool.free_count() == 5U);
}

TEST_CASE("an empty block records nothing and takes no page")
{
    const recording signal{1};
    small_pool      pages{6};
    recorder        target{pages.pool, rate_48k, history_pages};

    target.record({}, {});

    CHECK(target.end() == samples{0});
    CHECK(pages.pool.free_count() == 6U);

    signal.record_until(target, samples{1});

    CHECK(pages.pool.free_count() == 5U);
    CHECK(signal.recorded_exactly(target, samples{0}));
}

TEST_CASE("without pages, nothing is recorded and time still passes")
{
    const recording signal{100};
    page_pool       pool{{}, page_frames};
    recorder        target{pool, rate_48k, history_pages};

    signal.record_until(target, samples{100});

    CHECK(target.end() == samples{100});

    for (std::int64_t position = 0; position < 100; ++position) {
        CHECK_FALSE(target.frame_at(samples{position}).has_value());
    }
}

TEST_CASE("the recorder gives every page back when it goes")
{
    const recording signal{static_cast<std::size_t>(at(9).value())};
    small_pool      pages{6};

    for (const samples until : {samples{0}, at(0, 1), at(2), at(8, 3)}) {
        CAPTURE(until.value());

        {
            recorder target{pages.pool, rate_48k, history_pages};
            signal.record_until(target, until);
        }

        CHECK(pages.pool.free_count() == 6U);
    }
}

TEST_CASE("recording allocates nothing, across page boundaries")
{
    const recording signal{static_cast<std::size_t>(at(10).value())};
    small_pool      pages{6};
    recorder        target{pages.pool, rate_48k, history_pages};

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        signal.record_until(target, at(10), {1, 31, 16});
    }

    CHECK(test::realtime_guard::violations() == 0U);
}

}  // namespace
}  // namespace previously
