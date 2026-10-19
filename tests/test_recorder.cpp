// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the recorder and its history.
//
// Pages of 16 frames and a history of 4 pages, so every boundary is a few frames away. The
// engine's 2^15-frame pages and 46-page history are tested through the engine.

#include "previously/page_pool.hpp"
#include "previously/recorder.hpp"

#include "known.hpp"
#include "realtime_guard.hpp"
#include "tape_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>

namespace previously {
namespace {

using test::known;
using test::same_bits;
using test::small_pool;
using test::stereo_noise;

constexpr sample_rate rate_48k{48'000U};
constexpr samples     page_frames   = small_pool::page_frames;
constexpr std::size_t history_pages = 4;

/// Noise lasting `pages` small pages.
stereo_noise noise_for(std::int64_t pages)
{
    return stereo_noise{static_cast<std::size_t>(pages * page_frames.value()), rate_48k};
}

/// Position `frame` of page `page`.
constexpr samples at(std::int64_t page, std::int64_t frame = 0)
{
    return samples{(page * page_frames.value()) + frame};
}

/// Records `signal` from the recorder's end() up to `until`, in blocks of the given sizes, over
/// and over.
void record_until(recorder&                          target,
                  const stereo_noise&                signal,
                  samples                            until,
                  std::initializer_list<std::size_t> block_sizes = {7})
{
    const std::span sizes{block_sizes};
    std::size_t     next_size = 0;

    while (target.end() < until) {
        const auto start = static_cast<std::size_t>(target.end().value());
        const auto count =
            std::min(sizes[next_size], static_cast<std::size_t>(until.value()) - start);

        target.record(std::span{signal.left}.subspan(start, count),
                      std::span{signal.right}.subspan(start, count));

        next_size = (next_size + 1) % sizes.size();
    }
}

/// True when the frame at `position` is recorded, and is exactly what the DC blockers made.
bool recorded_exactly(const recorder& target, const stereo_noise& signal, samples position)
{
    const auto frame = target.frame_at(position);

    return frame &&
           same_bits(*frame, signal.dc_blocked[static_cast<std::size_t>(position.value())]);
}

TEST_CASE("every frame is recorded at its position, DC-blocked, whatever the blocks")
{
    using block_sizes = std::initializer_list<std::size_t>;

    const stereo_noise signal = noise_for(4);

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

        record_until(target, signal, at(4), sizes);

        CHECK(target.end() == at(4));

        for (std::int64_t position = 0; position < at(4).value(); ++position) {
            CAPTURE(position);
            CHECK(recorded_exactly(target, signal, samples{position}));
        }

        CHECK_FALSE(target.frame_at(samples{-1}).has_value());
        CHECK_FALSE(target.frame_at(at(4)).has_value());
    }
}

TEST_CASE("the history is the newest pages: its oldest frame is kept, the one before is not")
{
    const stereo_noise signal = noise_for(11);
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    // Pages 7 to 10, the last one partly recorded.
    record_until(target, signal, at(10, 7));

    CHECK(recorded_exactly(target, signal, at(7)));
    CHECK(recorded_exactly(target, signal, at(10, 6)));

    CHECK_FALSE(target.frame_at(at(7) - samples{1}).has_value());
    CHECK_FALSE(target.frame_at(at(10, 7)).has_value());
    CHECK_FALSE(target.page_at(at(7) - samples{1}).has_value());
}

TEST_CASE("a page leaves the history as the page four after it starts, and not before")
{
    const stereo_noise signal = noise_for(5);
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    // The last frame of page 3: four pages held, page 0 still in the history.
    record_until(target, signal, at(4), {1});

    CHECK(recorded_exactly(target, signal, at(0)));
    CHECK(pages.pool.free_count() == 2U);

    // The first frame of page 4. Page 0 leaves, and the three pages after it stay complete.
    record_until(target, signal, at(4, 1), {1});

    CHECK_FALSE(target.frame_at(at(1) - samples{1}).has_value());

    for (std::int64_t position = at(1).value(); position <= at(4).value(); ++position) {
        CAPTURE(position);
        CHECK(recorded_exactly(target, signal, samples{position}));
    }

    CHECK(pages.pool.free_count() == 2U);
}

TEST_CASE("the recorder never holds more pages than its history")
{
    // The pool has no page to spare, so taking a page before giving the oldest back would fail
    // and leave a hole.
    const stereo_noise signal = noise_for(21);
    small_pool         pages{history_pages};
    recorder           target{pages.pool, rate_48k, history_pages};

    for (std::int64_t page = 0; page <= 20; ++page) {
        record_until(target, signal, at(page, 1), {1});

        // Just after a page starts, the pages started so far are held, up to four: once full, the
        // history is three whole pages and one frame.
        const auto held = std::min(static_cast<std::size_t>(page) + 1U, history_pages);

        CHECK(pages.pool.free_count() == history_pages - held);

        for (std::int64_t position = std::max(at(page - 3).value(), std::int64_t{0});
             position <= at(page).value();
             ++position) {
            CAPTURE(position);
            REQUIRE(recorded_exactly(target, signal, samples{position}));
        }
    }
}

TEST_CASE("a page someone else holds outlives the history, and is not reused")
{
    const stereo_noise signal = noise_for(11);
    small_pool         pages{6};

    {
        recorder target{pages.pool, rate_48k, history_pages};

        record_until(target, signal, at(1));

        const page_pool::page_id kept = known(target.page_at(at(0)));
        pages.pool.retain(kept);

        record_until(target, signal, at(11));

        CHECK_FALSE(target.frame_at(at(0)).has_value());
        CHECK(pages.pool.holders(kept) == 1U);
        CHECK(pages.pool.free_count() == 1U);

        for (std::int64_t position = at(7).value(); position < at(11).value(); ++position) {
            CHECK(target.page_at(samples{position}) != kept);
        }

        // The audio is still there, as it was recorded.
        const auto frames = std::as_const(pages.pool).frames(kept);

        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK(same_bits(frames[i], signal.dc_blocked[i]));
        }

        pages.pool.release(kept);
        CHECK(pages.pool.free_count() == 2U);
    }

    CHECK(pages.pool.free_count() == 6U);
}

TEST_CASE("with no page free, a page goes unrecorded and recording resumes on the next one")
{
    const stereo_noise signal = noise_for(9);
    small_pool         pages{5};

    {
        recorder target{pages.pool, rate_48k, history_pages};

        record_until(target, signal, at(4));

        // Something else holds page 0's audio, and the last free page.
        const page_pool::page_id held  = known(target.page_at(at(0)));
        const page_pool::page_id spare = known(pages.pool.acquire());
        pages.pool.retain(held);

        record_until(target, signal, at(5));

        CHECK(target.end() == at(5));
        CHECK_FALSE(target.page_at(at(4)).has_value());
        CHECK_FALSE(target.frame_at(at(4)).has_value());
        CHECK_FALSE(target.frame_at(at(4, 15)).has_value());
        CHECK(recorded_exactly(target, signal, at(1)));
        CHECK(recorded_exactly(target, signal, at(3, 15)));

        // A page comes free, and the next page is recorded. The DC blocker ran on through the
        // hole, so the new page is exactly what it would have been.
        pages.pool.release(spare);
        record_until(target, signal, at(6));

        for (std::int64_t position = at(5).value(); position < at(6).value(); ++position) {
            CAPTURE(position);
            CHECK(recorded_exactly(target, signal, samples{position}));
        }

        CHECK_FALSE(target.frame_at(at(4)).has_value());

        // The hole's slot comes round again at page 8, and this time a page is free for it. Pages
        // 5 to 8 are the history, and page 0's audio is still held: all five pages are in use.
        record_until(target, signal, at(9));

        CHECK(pages.pool.free_count() == 0U);
        CHECK_FALSE(target.frame_at(at(4)).has_value());
        CHECK(recorded_exactly(target, signal, at(8)));
        CHECK(recorded_exactly(target, signal, at(8, 15)));

        pages.pool.release(held);
    }

    // The hole held no page, and every page is back.
    CHECK(pages.pool.free_count() == 5U);
}

TEST_CASE("an empty block records nothing and takes no page")
{
    const stereo_noise signal{1, rate_48k};
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    target.record({}, {});

    CHECK(target.end() == samples{0});
    CHECK(pages.pool.free_count() == 6U);

    record_until(target, signal, samples{1});

    CHECK(pages.pool.free_count() == 5U);
    CHECK(recorded_exactly(target, signal, samples{0}));
}

TEST_CASE("without pages, nothing is recorded and time still passes")
{
    const stereo_noise signal{100, rate_48k};
    small_pool         pages{0};
    recorder           target{pages.pool, rate_48k, history_pages};

    record_until(target, signal, samples{100});

    CHECK(target.end() == samples{100});

    for (std::int64_t position = 0; position < 100; ++position) {
        CHECK_FALSE(target.frame_at(samples{position}).has_value());
    }
}

TEST_CASE("the recorder gives every page back when it goes")
{
    const stereo_noise signal = noise_for(9);
    small_pool         pages{6};

    for (const samples until : {samples{0}, at(0, 1), at(2), at(8, 3)}) {
        CAPTURE(until.value());

        {
            recorder target{pages.pool, rate_48k, history_pages};
            record_until(target, signal, until);
        }

        CHECK(pages.pool.free_count() == 6U);
    }
}

TEST_CASE("recording allocates nothing, across page boundaries")
{
    const stereo_noise signal = noise_for(10);
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        record_until(target, signal, at(10), {1, 31, 16});
    }

    CHECK(test::realtime_guard::violations() == 0U);
}

}  // namespace
}  // namespace previously
