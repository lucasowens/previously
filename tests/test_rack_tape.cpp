// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the VCV Rack adapter's tape, allocated once for every rate (tier T4).

#include "previously/engine.hpp"

#include "block_adapter.hpp"
#include "capture_fixtures.hpp"
#include "known.hpp"
#include "realtime_guard.hpp"
#include "tape.hpp"
#include "tape_fixtures.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>

namespace previously::vcv {
namespace {

using test::engine_feed;
using test::known;
using test::same_bits;

constexpr std::size_t block = block_adapter::block_frames;

// 210 pages of 2^17 frames, 8 bytes each: 210 MiB.
static_assert(tape::full_frames == std::size_t{210} * 131'072);
static_assert(tape::full_frames * sizeof(stereo_frame) == std::size_t{210} * 1024 * 1024);

// An engine holds a span into a tape, so a tape stays where it is.
static_assert(!std::is_copy_constructible_v<tape>);
static_assert(!std::is_copy_assignable_v<tape>);
static_assert(!std::is_move_constructible_v<tape>);
static_assert(!std::is_move_assignable_v<tape>);

/// How many whole pages the full tape holds at `rate`.
constexpr std::size_t full_pages_at(sample_rate rate)
{
    return tape::full_frames / static_cast<std::size_t>(page_frames_for(rate).value());
}

// Above 192 kHz the same memory holds fewer, longer pages: still the history's 46 up to 768 kHz,
// and not beyond, where a page is 2^20 frames (DESIGN.md §2.3).
static_assert(full_pages_at(sample_rate{192'000U}) == 210);
static_assert(full_pages_at(sample_rate{352'800U}) == 105);
static_assert(full_pages_at(sample_rate{384'000U}) == 105);
static_assert(full_pages_at(sample_rate{705'600U}) == 52);
static_assert(full_pages_at(sample_rate{768'000U}) == 52);
static_assert(full_pages_at(sample_rate{768'001U}) == 26);

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

TEST_CASE("a tape whose allocation failed is empty, and an engine given it records nothing")
{
    // The failure covers the tape's one allocation and nothing else.
    std::optional<tape> none;
    {
        const test::allocation_failure no_memory;
        none.emplace();
    }

    const std::span<stereo_frame> nothing = none->for_rate(sample_rate{48'000U});

    REQUIRE(nothing.empty());

    engine      audio_engine{engine_config{.rate = sample_rate{48'000U}, .max_block_frames = block},
                             nothing};
    engine_feed feed{1'000, sample_rate{48'000U}};

    feed.process(audio_engine, 0, 1'000, block);

    CHECK(audio_engine.history().end() == samples{1'000});
    CHECK_FALSE(audio_engine.history().frame_at(samples{0}).has_value());
    CHECK_FALSE(audio_engine.history().frame_at(samples{999}).has_value());
}

TEST_CASE("an engine is rebuilt over the same tape when the rate changes, and records at once")
{
    tape                          memory;
    std::optional<engine>         audio_engine;
    engine_feed                   at_48k{4'800, sample_rate{48'000U}};
    engine_feed                   at_96k{9'600, sample_rate{96'000U}};
    const std::span<stereo_frame> first = memory.for_rate(sample_rate{48'000U});

    REQUIRE(first.size() == tape::full_frames);

    audio_engine.emplace(engine_config{.rate = sample_rate{48'000U}, .max_block_frames = block},
                         first);
    at_48k.process(*audio_engine, 0, 4'800, block);

    CHECK(same_bits(known(audio_engine->history().frame_at(samples{4'799})),
                    at_48k.input.dc_blocked[4'799]));

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

    at_96k.process(*audio_engine, 0, 9'600, block);

    CHECK(audio_engine->history().end() == samples{9'600});
    CHECK(
        same_bits(known(audio_engine->history().frame_at(samples{0})), at_96k.input.dc_blocked[0]));
    CHECK(same_bits(known(audio_engine->history().frame_at(samples{9'599})),
                    at_96k.input.dc_blocked[9'599]));
}

TEST_CASE("at 768 kHz an engine takes the full tape, with fewer and longer pages")
{
    // 52 pages of 2^19 frames (the static_assert above): the history's 46 and six more.
    tape                          memory;
    const std::span<stereo_frame> whole = memory.for_rate(sample_rate{768'000U});

    REQUIRE(whole.size() == tape::full_frames);

    engine audio_engine{engine_config{.rate = sample_rate{768'000U}, .max_block_frames = block},
                        whole};

    engine_feed feed{524'289, sample_rate{768'000U}};
    feed.process(audio_engine, 0, 524'289, block);

    // The first frame of the second page: a page is 2^19 frames at this rate.
    CHECK(audio_engine.history().page_at(samples{524'287}) ==
          audio_engine.history().page_at(samples{0}));
    CHECK(audio_engine.history().page_at(samples{524'288}) !=
          audio_engine.history().page_at(samples{0}));
    CHECK(same_bits(known(audio_engine.history().frame_at(samples{524'288})),
                    feed.input.dc_blocked[524'288]));
}

/// Whether every page from the history's oldest to its newest was recorded: no hole.
[[nodiscard]] bool history_is_whole(const recorder& history, samples page_frames)
{
    const std::int64_t oldest = history.begin().value() / page_frames.value();
    const std::int64_t newest = (history.end().value() - 1) / page_frames.value();

    for (std::int64_t number = oldest; number <= newest; ++number) {
        if (!history.page_at(samples{number * page_frames.value()})) {
            return false;
        }
    }

    return true;
}

TEST_CASE("at 768 kHz a capture needing 7 pages is refused, one needing 6 is held, and the history "
          "stays whole")
{
    // 52 pages: the history's 46 and 6 more (DESIGN.md §2.3). A page is 2^19 frames. Each press
    // is 0.3 beat after its line, so the run is the window, 50 ms each side, and half a pulse.
    constexpr sample_rate   rate_768k{768'000U};
    constexpr samples       page_frames{524'288};
    constexpr engine_config config{.rate = rate_768k, .max_block_frames = 49'152};
    constexpr std::int64_t  pulses_per_bar = 16;

    tape                          memory;
    const std::span<stereo_frame> whole = memory.for_rate(rate_768k);

    REQUIRE(whole.size() == tape::full_frames);

    SUBCASE("7 pages: refused")
    {
        // 120 BPM, a bar 1,536,000 frames. Bars 2 and 3 are [3,072,000, 6,144,000): with the
        // margins and the reserve, [3,033,600, 6,230,400), pages 5 to 11.
        test::clocked_engine rig{config, whole};
        const std::int64_t   pulse = 96'000;

        rig.steady_clock(test::at_sample(0), test::at_sample(pulse), 40 * pulses_per_bar);
        rig.press(test::at_sample((4 * pulses_per_bar * pulse) + 28'800), capture_bars::two);
        rig.run_until(samples{(4 * pulses_per_bar * pulse) + 28'801});

        CHECK(rig.target().loop().outcome.result == capture_result::pool_full);
        CHECK(rig.target().slot().empty());

        // Had it been held, then once page 11 left the history, as page 57 started, the history's
        // 45 whole pages and the loop's 7 would have been all 52, and page 57 a hole.
        rig.run_until(samples{(57 * page_frames.value()) + 1});

        CHECK(history_is_whole(rig.target().history(), page_frames));
        test::check_holders(rig.target());
    }

    SUBCASE("6 pages: held, and the history laps with every page in use")
    {
        // 80 BPM, a bar 2,304,000 frames. Bar 2 is [4,608,000, 6,912,000): with the margins and
        // the reserve, [4,569,600, 7,022,400), pages 8 to 13.
        test::clocked_engine rig{config, whole};
        const std::int64_t   pulse = 144'000;

        rig.steady_clock(test::at_sample(0), test::at_sample(pulse), 30 * pulses_per_bar);
        rig.press(test::at_sample((3 * pulses_per_bar * pulse) + 43'200), capture_bars::one);
        rig.run_until(samples{(3 * pulses_per_bar * pulse) + 43'201});

        REQUIRE(rig.target().loop().outcome.result == capture_result::committed);
        CHECK(rig.target().slot().first_page() == 8);
        CHECK(rig.target().slot().page_count() == 6U);

        // Once its pages have left the history, the pool's 52 are the history's 46 and its 6.
        rig.run_until(samples{60 * page_frames.value()});

        CHECK(rig.target().loop().slot == slot_state::final);
        CHECK(rig.target().slot().pages().size() == 6U);
        CHECK(rig.target().pages().free_count() == 0U);
        CHECK(history_is_whole(rig.target().history(), page_frames));
        test::check_holders(rig.target());
    }
}

}  // namespace
}  // namespace previously::vcv
