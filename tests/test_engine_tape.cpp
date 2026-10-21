// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the engine's tape, at the engine's own page size and history.

#include "previously/engine.hpp"

#include "realtime_guard.hpp"
#include "tape_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace previously {
namespace {

using test::engine_feed;
using test::same_bits;

constexpr sample_rate rate_48k{48'000U};

// P1 at its fullest takes 3 × 47 + 46 = 187 of the 210 pages (DESIGN.md §2.3).
static_assert(engine::history_pages == 46U);
static_assert((3 * engine::max_capture_pages) + engine::history_pages == 187U);
static_assert(page_pool::max_pages == 210U);

/// Memory for the engine's minimum tape: exactly the history.
std::vector<stereo_frame> tape_for(sample_rate rate)
{
    const auto page_frames = static_cast<std::size_t>(page_frames_for(rate).value());

    return std::vector<stereo_frame>(engine::history_pages * page_frames);
}

TEST_CASE("without a tape, the engine records nothing")
{
    engine      audio_engine{engine_config{}};
    engine_feed feed{1'000, rate_48k};

    feed.process(audio_engine, 0, 1'000, 32);

    CHECK(audio_engine.history().end() == samples{1'000});
    CHECK_FALSE(audio_engine.history().frame_at(samples{0}).has_value());
    CHECK_FALSE(audio_engine.history().frame_at(samples{999}).has_value());
}

TEST_CASE("the tape is not touched before the first block")
{
    // On the Daisy the tape is SDRAM, which is set up after static constructors have run.
    const stereo_frame        sentinel{.left = 1.5F, .right = -2.5F};
    std::vector<stereo_frame> tape = tape_for(rate_48k);
    std::ranges::fill(tape, sentinel);

    engine audio_engine{engine_config{.rate = rate_48k, .max_block_frames = 48}, tape};

    CHECK(std::ranges::all_of(tape, [&](const stereo_frame& frame) {
        return same_bits(frame, sentinel);
    }));

    // The first block writes the first 32 frames of the first page, and nothing else.
    engine_feed feed{32, rate_48k};
    feed.process(audio_engine, 0, 32, 32);

    for (std::size_t i = 0; i < 32; ++i) {
        CHECK(same_bits(tape[i], feed.input.dc_blocked[i]));
    }

    CHECK(std::all_of(tape.begin() + 32, tape.end(), [&](const stereo_frame& frame) {
        return same_bits(frame, sentinel);
    }));
}

TEST_CASE("the engine records the DC-blocked input at its sample clock, at every block size")
{
    // Two and a half pages: two page boundaries inside the run.
    constexpr std::size_t frames = (2 * 32'768) + 16'384;

    engine_feed feed{frames, rate_48k};

    for (const std::size_t block_frames : {1U, 16U, 32U, 48U}) {
        CAPTURE(block_frames);

        std::vector<stereo_frame> tape = tape_for(rate_48k);
        engine audio_engine{engine_config{.rate = rate_48k, .max_block_frames = 48}, tape};

        feed.process(audio_engine, 0, frames, block_frames);

        CHECK(audio_engine.history().end() == audio_engine.sample_clock());

        std::size_t mismatches = 0;

        for (std::size_t i = 0; i < frames; ++i) {
            const auto frame =
                audio_engine.history().frame_at(samples{static_cast<std::int64_t>(i)});

            if (!frame || !same_bits(*frame, feed.input.dc_blocked[i])) {
                ++mismatches;
            }
        }

        CHECK(mismatches == 0U);

        // The live path is untouched: still the input, exactly.
        CHECK(feed.out_left == feed.input.left);
        CHECK(feed.out_right == feed.input.right);
    }
}

TEST_CASE("once full, the history at 48 kHz reaches back 30.72 s, never less")
{
    // Just after the 47th page starts, the history is pages 1 to 46: 45 whole pages and one
    // frame. That is the shortest it ever is.
    constexpr std::size_t page   = 32'768;
    constexpr std::size_t frames = (46 * page) + 1;

    std::vector<stereo_frame> tape = tape_for(rate_48k);
    engine      audio_engine{engine_config{.rate = rate_48k, .max_block_frames = 48}, tape};
    engine_feed feed{frames, rate_48k};

    feed.process(audio_engine, 0, frames, 48);

    const recorder& history = audio_engine.history();
    const samples   oldest{static_cast<std::int64_t>(page)};

    CHECK(history.frame_at(oldest).has_value());
    CHECK_FALSE(history.frame_at(oldest - samples{1}).has_value());
    CHECK(history.frame_at(history.end() - samples{1}).has_value());

    // 45 × 32,768 + 1 frames.
    CHECK(to_seconds(history.end() - oldest, rate_48k).value() ==
          doctest::Approx(30.72).epsilon(1e-4));
}

TEST_CASE("at 96 kHz a page is 2^16 frames")
{
    constexpr sample_rate rate_96k{96'000U};

    std::vector<stereo_frame> tape = tape_for(rate_96k);
    engine      audio_engine{engine_config{.rate = rate_96k, .max_block_frames = 48}, tape};
    engine_feed feed{65'537, rate_96k};

    feed.process(audio_engine, 0, 65'537, 48);

    const recorder& history = audio_engine.history();

    CHECK(history.page_at(samples{65'535}) == history.page_at(samples{0}));
    CHECK(history.page_at(samples{65'536}).has_value());
    CHECK(history.page_at(samples{65'536}) != history.page_at(samples{0}));
}

TEST_CASE("processing with a tape allocates nothing, across a page boundary")
{
    std::vector<stereo_frame> tape = tape_for(rate_48k);
    engine      audio_engine{engine_config{.rate = rate_48k, .max_block_frames = 48}, tape};
    engine_feed feed{32'768 + 4'800, rate_48k};

    feed.process(audio_engine, 0, 32'768 - 1'000, 48);

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        feed.process(audio_engine, 32'768 - 1'000, 32'768 + 4'800, 48);
    }

    CHECK(test::realtime_guard::violations() == 0U);
    CHECK(audio_engine.history().page_at(samples{32'768}).has_value());
}

}  // namespace
}  // namespace previously
