// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: a captured loop's run of pages.
//
// Pages of 16 frames and a history of 4, so every boundary is a few frames away. What the loop's
// lines mean is tested through capture (test_capture.cpp).

#include "previously/captured_loop.hpp"
#include "previously/page_pool.hpp"
#include "previously/recorder.hpp"

#include "tape_fixtures.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace previously {
namespace {

using test::small_pool;

constexpr sample_rate rate_48k{48'000U};
constexpr std::size_t history_pages = 4;

/// Position `frame` of page `page`.
constexpr samples at(std::int64_t page, std::int64_t frame = 0)
{
    return samples{(page * small_pool::page_frames.value()) + frame};
}

/// Silence recorded up to `until`.
void record_until(recorder& target, samples until)
{
    const std::vector<float> silence(static_cast<std::size_t>(until.value()));
    const auto               from  = static_cast<std::size_t>(target.end().value());
    const auto               count = static_cast<std::size_t>(until.value()) - from;

    target.record(std::span{silence}.subspan(from, count), std::span{silence}.subspan(from, count));
}

/// Two lines a window apart, enough for a loop to hold pages.
constexpr std::array<loop_line, 2> one_bar{
    loop_line{.sequence = 0, .position = samples{0}},
    loop_line{.sequence = 16, .position = samples{1}},
};

TEST_CASE("a loop holds the recorded pages of its run at once, and the rest as they are taken")
{
    small_pool    pages{8};
    recorder      history{pages.pool, rate_48k, history_pages};
    captured_loop loop{pages.pool};

    record_until(history, at(2, 5));

    // Pages 1 to 3, from mid-page 1 to mid-page 3. Pages 1 and 2 are recorded.
    loop.assign(one_bar, 0, false);
    loop.hold(tape_range{.first = at(1, 3), .end = at(3, 9)}, history);

    CHECK(loop.first_page() == 1);
    CHECK(loop.page_count() == 3U);
    REQUIRE(loop.pages().size() == 2U);
    CHECK(loop.pages()[0] == history.page_at(at(1)));
    CHECK(loop.pages()[1] == history.page_at(at(2)));
    CHECK(pages.pool.holders(loop.pages()[0]) == 2U);

    // Page 3 is held from its first frame, and the run takes no more.
    record_until(history, at(3));
    loop.catch_up(history);
    CHECK(loop.pages().size() == 2U);

    record_until(history, at(3, 1));
    loop.catch_up(history);
    REQUIRE(loop.pages().size() == 3U);
    CHECK(loop.pages()[2] == history.page_at(at(3)));

    record_until(history, at(4, 1));
    loop.catch_up(history);
    CHECK(loop.pages().size() == 3U);
}

TEST_CASE("a held page outlives the history, and goes back with the loop")
{
    small_pool pages{8};
    recorder   history{pages.pool, rate_48k, history_pages};

    record_until(history, at(1, 1));

    page_pool::page_id first{};
    {
        captured_loop loop{pages.pool};

        loop.assign(one_bar, 0, false);
        loop.hold(tape_range{.first = at(0), .end = at(1)}, history);
        REQUIRE(loop.pages().size() == 1U);
        first = loop.pages()[0];

        // Page 0 leaves the history as page 4 starts, and the loop keeps it.
        record_until(history, at(4, 1));
        CHECK_FALSE(history.page_at(at(0)).has_value());
        CHECK(pages.pool.holders(first) == 1U);
    }

    CHECK(pages.pool.holders(first) == 0U);
}

TEST_CASE("a narrower run gives back the pages outside it, from either end, and keeps the rest in "
          "order")
{
    small_pool    pages{8};
    recorder      history{pages.pool, rate_48k, history_pages};
    captured_loop loop{pages.pool};

    record_until(history, at(3, 8));

    loop.assign(one_bar, 0, false);
    loop.hold(tape_range{.first = at(0, 4), .end = at(3, 8)}, history);
    REQUIRE(loop.pages().size() == 4U);

    const std::array<page_pool::page_id, 4> held{
        loop.pages()[0],
        loop.pages()[1],
        loop.pages()[2],
        loop.pages()[3],
    };

    // Pages 1 and 2 stay; 0 and 3 go back to the history alone.
    loop.hold(tape_range{.first = at(1), .end = at(3)}, history);

    CHECK(loop.first_page() == 1);
    CHECK(loop.page_count() == 2U);
    REQUIRE(loop.pages().size() == 2U);
    CHECK(loop.pages()[0] == held[1]);
    CHECK(loop.pages()[1] == held[2]);
    CHECK(pages.pool.holders(held[0]) == 1U);
    CHECK(pages.pool.holders(held[1]) == 2U);
    CHECK(pages.pool.holders(held[2]) == 2U);
    CHECK(pages.pool.holders(held[3]) == 1U);

    // A run ending in page 3 again holds it again.
    loop.hold(tape_range{.first = at(1), .end = at(3, 1)}, history);

    REQUIRE(loop.pages().size() == 3U);
    CHECK(loop.pages()[2] == held[3]);
    CHECK(pages.pool.holders(held[3]) == 2U);
}

TEST_CASE("a run moved past every page it held starts again from its new first page")
{
    small_pool    pages{8};
    recorder      history{pages.pool, rate_48k, history_pages};
    captured_loop loop{pages.pool};

    record_until(history, at(3, 8));

    loop.assign(one_bar, 0, false);
    loop.hold(tape_range{.first = at(0), .end = at(1)}, history);
    REQUIRE(loop.pages().size() == 1U);

    const page_pool::page_id page_0 = loop.pages()[0];

    loop.hold(tape_range{.first = at(2, 3), .end = at(3, 2)}, history);

    CHECK(pages.pool.holders(page_0) == 1U);
    CHECK(loop.first_page() == 2);
    REQUIRE(loop.pages().size() == 2U);
    CHECK(loop.pages()[0] == history.page_at(at(2)));
    CHECK(loop.pages()[1] == history.page_at(at(3)));
}

TEST_CASE("clearing a loop gives back every page and empties it")
{
    small_pool    pages{8};
    recorder      history{pages.pool, rate_48k, history_pages};
    captured_loop loop{pages.pool};

    record_until(history, at(3, 8));

    loop.assign(one_bar, 3, true);
    loop.hold(tape_range{.first = at(0), .end = at(3, 8)}, history);
    REQUIRE(loop.pages().size() == 4U);
    REQUIRE_FALSE(loop.empty());

    loop.clear();

    CHECK(loop.empty());
    CHECK(loop.lines().empty());
    CHECK(loop.pages().empty());
    CHECK(loop.page_count() == 0U);
    CHECK(pages.pool.free_count() == 8U - history_pages);
}

TEST_CASE("a loop is final once every line is")
{
    small_pool    pages{8};
    captured_loop loop{pages.pool};

    CHECK_FALSE(loop.is_final());

    const std::array<loop_line, 3> lines{
        loop_line{.sequence = 0, .position = samples{0}, .status = line_status::final},
        loop_line{.sequence = 16, .position = samples{16}, .status = line_status::final},
        loop_line{.sequence = 32, .position = samples{32}, .status = line_status::edge},
    };

    loop.assign(lines, 7, false);

    CHECK(loop.bars() == 2U);
    CHECK(loop.first_bar() == 7);
    CHECK_FALSE(loop.is_final());

    loop.set_line(2, {.sequence = 32, .position = samples{32}, .status = line_status::final});

    CHECK(loop.is_final());
}

}  // namespace
}  // namespace previously
