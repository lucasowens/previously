// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the page pool and the page size.

#include "previously/page_pool.hpp"

#include "known.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace previously {
namespace {

using page_id = page_pool::page_id;
using test::known;

constexpr samples small_page{16};

// The page size: the smallest power of two with P × 48,000 ≥ 2^15 × rate (DESIGN.md §2.3).
static_assert(page_frames_for(sample_rate{44'100U}) == samples{32'768});
static_assert(page_frames_for(sample_rate{48'000U}) == samples{32'768});
static_assert(page_frames_for(sample_rate{88'200U}) == samples{65'536});
static_assert(page_frames_for(sample_rate{96'000U}) == samples{65'536});
static_assert(page_frames_for(sample_rate{176'400U}) == samples{131'072});
static_assert(page_frames_for(sample_rate{192'000U}) == samples{131'072});

// Either side of each boundary. At exactly 48 kHz, 2^15 frames last 0.68267 s: a rule written as
// "at least 0.6827 s" would double the page there.
static_assert(page_frames_for(sample_rate{48'001U}) == samples{65'536});
static_assert(page_frames_for(sample_rate{24'000U}) == samples{16'384});
static_assert(page_frames_for(sample_rate{24'001U}) == samples{32'768});
static_assert(page_frames_for(sample_rate{96'001U}) == samples{131'072});

// Rack's lowest and highest rates.
static_assert(page_frames_for(sample_rate{11'025U}) == samples{8'192});
static_assert(page_frames_for(sample_rate{768'000U}) == samples{524'288});

// The largest rate there is still has a page size: 2^32 frames.
static_assert(page_frames_for(sample_rate{0xFFFF'FFFFU}) == samples{4'294'967'296});

static_assert(sizeof(stereo_frame) == 2 * sizeof(float));

/// Memory for a number of small pages, plus any extra frames.
std::vector<stereo_frame> memory_for(std::size_t pages, std::size_t extra_frames = 0)
{
    return std::vector<stereo_frame>((pages * static_cast<std::size_t>(small_page.value())) +
                                     extra_frames);
}

TEST_CASE("a pool is made of whole pages, at most max_pages of them")
{
    SUBCASE("frames after the last whole page are not a page")
    {
        std::vector<stereo_frame> memory = memory_for(3, 15);
        const page_pool           pool{memory, small_page};

        CHECK(pool.page_count() == 3U);
        CHECK(pool.free_count() == 3U);
        CHECK(pool.page_frames() == small_page);
    }

    SUBCASE("memory beyond max_pages is not used")
    {
        std::vector<stereo_frame> memory = memory_for(page_pool::max_pages + 1);
        page_pool                 pool{memory, small_page};

        CHECK(pool.page_count() == page_pool::max_pages);

        std::vector<page_id> taken;

        while (const auto page = pool.acquire()) {
            taken.push_back(*page);
        }

        // The last page ends where max_pages pages end, one page short of the memory's end.
        CHECK(taken.size() == page_pool::max_pages);
        CHECK(pool.frames(taken.back()).data() == &memory[(page_pool::max_pages - 1) * 16]);

        for (const page_id page : taken) {
            pool.release(page);
        }
    }

    SUBCASE("no memory, no pages")
    {
        page_pool pool{{}, small_page};

        CHECK(pool.page_count() == 0U);
        CHECK_FALSE(pool.acquire().has_value());
    }
}

TEST_CASE("pages are handed out once each, in order, until none is free")
{
    std::vector<stereo_frame> memory = memory_for(4);
    page_pool                 pool{memory, small_page};

    for (page_id expected = 0; expected < 4; ++expected) {
        const page_id page = known(pool.acquire());

        CHECK(page == expected);
        CHECK(pool.holders(page) == 1U);
        CHECK(pool.free_count() == 3U - expected);
    }

    CHECK_FALSE(pool.acquire().has_value());

    for (page_id page = 0; page < 4; ++page) {
        pool.release(page);
    }

    CHECK(pool.free_count() == 4U);
}

TEST_CASE("a page goes back only when its last holder lets go")
{
    std::vector<stereo_frame> memory = memory_for(2);
    page_pool                 pool{memory, small_page};

    const page_id page = known(pool.acquire());

    pool.retain(page);
    CHECK(pool.holders(page) == 2U);

    pool.release(page);
    CHECK(pool.holders(page) == 1U);
    CHECK(pool.free_count() == 1U);

    // Still held, so it is not handed out again.
    const page_id other = known(pool.acquire());
    CHECK(other != page);
    CHECK_FALSE(pool.acquire().has_value());

    pool.release(page);
    CHECK(pool.holders(page) == 0U);
    CHECK(pool.free_count() == 1U);

    pool.release(other);
}

TEST_CASE("the page given back last is the next one taken")
{
    std::vector<stereo_frame> memory = memory_for(3);
    page_pool                 pool{memory, small_page};

    const page_id first  = known(pool.acquire());
    const page_id second = known(pool.acquire());

    pool.release(first);

    const page_id again = known(pool.acquire());
    CHECK(again == first);

    pool.release(again);
    pool.release(second);
}

TEST_CASE("pages lie side by side in the memory, without overlapping")
{
    std::vector<stereo_frame> memory = memory_for(3, 5);
    page_pool                 pool{memory, small_page};

    std::vector<page_id> pages;

    while (const auto page = pool.acquire()) {
        pages.push_back(*page);
    }

    REQUIRE(pages.size() == 3U);

    // Every frame of every page gets its own value, written through the pool.
    for (const page_id page : pages) {
        const auto frames = pool.frames(page);

        REQUIRE(frames.size() == 16U);

        for (std::size_t i = 0; i < frames.size(); ++i) {
            frames[i] = stereo_frame{
                .left  = static_cast<float>((std::size_t{page} * 100) + i),
                .right = -static_cast<float>((std::size_t{page} * 100) + i),
            };
        }
    }

    // Read back from the memory: page p is frames 16p to 16p + 15, both ends included, and the
    // five frames after the last page are untouched.
    for (std::size_t i = 0; i < memory.size(); ++i) {
        const std::size_t page   = i / 16;
        const std::size_t offset = i % 16;
        const float       left   = page < 3 ? static_cast<float>((page * 100) + offset) : 0.0F;

        CAPTURE(i);
        CHECK(memory[i].left == left);
        CHECK(memory[i].right == -left);
    }

    for (const page_id page : pages) {
        pool.release(page);
    }
}

TEST_CASE("the pool never reads or writes its memory")
{
    // On the Daisy the memory is SDRAM, which is not set up when the engine is constructed.
    const stereo_frame        sentinel{.left = 1.5F, .right = -2.5F};
    std::vector<stereo_frame> memory(std::size_t{4} * 16, sentinel);

    {
        page_pool pool{memory, small_page};

        const page_id first  = known(pool.acquire());
        const page_id second = known(pool.acquire());

        pool.retain(first);
        pool.release(first);
        pool.release(first);
        pool.release(second);
    }

    CHECK(std::ranges::all_of(memory, [&](const stereo_frame& frame) {
        return std::bit_cast<std::uint64_t>(frame) == std::bit_cast<std::uint64_t>(sentinel);
    }));
}

}  // namespace
}  // namespace previously
