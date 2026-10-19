// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the VCV Rack adapter's tape, allocated once for every rate (tier T4).

#include "previously/engine.hpp"

#include "block_adapter.hpp"
#include "known.hpp"
#include "realtime_guard.hpp"
#include "tape.hpp"
#include "tape_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace previously::vcv {
namespace {

using test::known;
using test::same_bits;
using test::stereo_noise;

constexpr std::size_t block = block_adapter::block_frames;

// 210 pages of 2^17 frames, 8 bytes each: 210 MiB.
static_assert(tape::full_frames == std::size_t{210} * 131'072);
static_assert(tape::full_frames * sizeof(stereo_frame) == std::size_t{210} * 1024 * 1024);

/// Every rate Rack's engine menu offers: 44.1 and 48 kHz times 1/4 to 16.
constexpr std::array<std::uint32_t, 14> rack_rates_hz{
    11'025U,
    12'000U,
    22'050U,
    24'000U,
    44'100U,
    48'000U,
    88'200U,
    96'000U,
    176'400U,
    192'000U,
    352'800U,
    384'000U,
    705'600U,
    768'000U,
};

/// Feeds an engine `frames` of the noise, from position `from`, in the adapter's blocks.
void record(engine& target, const stereo_noise& noise, std::size_t from, std::size_t frames)
{
    std::vector<float> out(block);

    for (std::size_t start = from; start < from + frames; start += block) {
        const std::size_t count = std::min(block, from + frames - start);

        target.process(audio_block{
            .in_left   = std::span{noise.left}.subspan(start, count),
            .in_right  = std::span{noise.right}.subspan(start, count),
            .out_left  = std::span{out}.first(count),
            .out_right = std::span{out}.first(count),
        });
    }
}

TEST_CASE("a full tape holds the history at every rate Rack offers, and not beyond")
{
    for (const std::uint32_t rate_hz : rack_rates_hz) {
        CAPTURE(rate_hz);
        CHECK(engine::holds_history(tape::full_frames, sample_rate{rate_hz}));
    }

    // Above 768 kHz a page is 2^20 frames, and 210 MiB holds 26 of them: not the history's 46.
    CHECK_FALSE(engine::holds_history(tape::full_frames, sample_rate{768'001U}));
}

TEST_CASE(
    "a tape is given whole to an engine that it holds the history for, and not at all otherwise")
{
    // Exactly the history at 48 kHz, and one frame short of it.
    const std::size_t history_frames = engine::history_pages * std::size_t{32'768};

    tape exact{history_frames};
    tape short_by_one{history_frames - 1};

    CHECK(exact.for_rate(sample_rate{48'000U}).size() == history_frames);
    CHECK(short_by_one.for_rate(sample_rate{48'000U}).empty());

    // At 96 kHz pages are twice as long, so the same memory holds half the history.
    CHECK(exact.for_rate(sample_rate{96'000U}).empty());
}

TEST_CASE("an engine is rebuilt over the same tape when the rate changes, and records at once")
{
    tape                          memory;
    std::optional<engine>         audio_engine;
    const stereo_noise            at_48k{4'800, sample_rate{48'000U}};
    const stereo_noise            at_96k{9'600, sample_rate{96'000U}};
    const std::span<stereo_frame> first = memory.for_rate(sample_rate{48'000U});

    REQUIRE(first.size() == tape::full_frames);

    audio_engine.emplace(engine_config{.rate = sample_rate{48'000U}, .max_block_frames = block},
                         first);
    record(*audio_engine, at_48k, 0, 4'800);

    CHECK(same_bits(known(audio_engine->history().frame_at(samples{4'799})),
                    at_48k.dc_blocked[4'799]));

    // The rate changes, as Rack changes it on its audio thread: the engine is rebuilt in place
    // over the same memory, allocating nothing. The history starts again, with the noise
    // DC-blocked at the new rate.
    const std::span<stereo_frame> second = memory.for_rate(sample_rate{96'000U});

    CHECK(second.data() == first.data());

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        audio_engine.emplace(engine_config{.rate = sample_rate{96'000U}, .max_block_frames = block},
                             second);
    }

    CHECK(test::realtime_guard::violations() == 0U);

    record(*audio_engine, at_96k, 0, 9'600);

    CHECK(audio_engine->history().end() == samples{9'600});
    CHECK(same_bits(known(audio_engine->history().frame_at(samples{0})), at_96k.dc_blocked[0]));
    CHECK(same_bits(known(audio_engine->history().frame_at(samples{9'599})),
                    at_96k.dc_blocked[9'599]));
}

TEST_CASE("at 768 kHz an engine takes the full tape, with fewer and longer pages")
{
    // 52 pages of 2^19 frames: the history's 46 and six more.
    tape   memory;
    engine audio_engine{engine_config{.rate = sample_rate{768'000U}, .max_block_frames = block},
                        memory.for_rate(sample_rate{768'000U})};

    const stereo_noise noise{524'289, sample_rate{768'000U}};
    record(audio_engine, noise, 0, 524'289);

    // The first frame of the second page: a page is 2^19 frames at this rate.
    CHECK(audio_engine.history().page_at(samples{524'287}) ==
          audio_engine.history().page_at(samples{0}));
    CHECK(audio_engine.history().page_at(samples{524'288}) !=
          audio_engine.history().page_at(samples{0}));
    CHECK(same_bits(known(audio_engine.history().frame_at(samples{524'288})),
                    noise.dc_blocked[524'288]));
}

}  // namespace
}  // namespace previously::vcv
