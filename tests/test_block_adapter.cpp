// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the VCV Rack adapter: re-blocking, edge timing, and clock out (tier T4).

#include "previously/engine.hpp"

#include "block_adapter.hpp"
#include "edge_detector.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace previously::vcv {
namespace {

constexpr std::size_t block = block_adapter::block_frames;
constexpr sample_rate rate_48k{48'000U};

[[nodiscard]] std::unique_ptr<engine> make_engine()
{
    return std::make_unique<engine>(engine_config{.rate = rate_48k, .max_block_frames = block});
}

// ---- Edge detector ----------------------------------------------------------------------------

TEST_CASE("a digital step is timed at the sample where it is first seen")
{
    edge_detector detector;

    CHECK_FALSE(detector.step(0.0F).has_value());
    CHECK(detector.step(10.0F) == 0.0F);
}

TEST_CASE("an edge caught mid-rise is interpolated to its 1 V crossing")
{
    edge_detector detector;
    static_cast<void>(detector.step(0.0F));

    SUBCASE("half a sample back")
    {
        static_cast<void>(detector.step(0.5F));
        CHECK(detector.step(1.5F) == doctest::Approx(-0.5));
    }

    SUBCASE("a slow ramp")
    {
        static_cast<void>(detector.step(0.2F));
        CHECK(detector.step(2.0F) == doctest::Approx(-1.0 / 1.8));
    }
}

TEST_CASE("a high gate never retriggers until it falls to 0.1 V")
{
    edge_detector detector;

    CHECK(detector.step(10.0F).has_value());
    CHECK_FALSE(detector.step(0.5F).has_value());  // noise dip, not low enough to re-arm
    CHECK_FALSE(detector.step(10.0F).has_value());
    CHECK_FALSE(detector.step(0.1F).has_value());  // re-armed here
    CHECK(detector.step(10.0F).has_value());
}

// ---- Re-blocking ------------------------------------------------------------------------------

TEST_CASE("audio comes out delayed by exactly one block")
{
    auto          audio_engine = make_engine();
    block_adapter adapter{rate_48k};

    for (std::size_t tick = 0; tick < block * 4; ++tick) {
        const auto        value = static_cast<float>(tick + 1);
        const tick_output output =
            adapter.tick(*audio_engine, {.left = value, .right = -value}, {});

        const float expected = tick < block ? 0.0F : static_cast<float>(tick + 1 - block);

        CAPTURE(tick);
        CHECK(output.audio.left == expected);
        CHECK(output.audio.right == -expected);
    }
}

TEST_CASE("the engine runs once per completed block")
{
    auto          audio_engine = make_engine();
    block_adapter adapter{rate_48k};

    std::size_t finished = 0;
    for (std::size_t tick = 0; tick < (block * 3) - 1; ++tick) {
        finished += adapter.tick(*audio_engine, {}, {}).block_finished ? 1U : 0U;
    }

    CHECK(finished == 2);
    CHECK(audio_engine->sample_clock() == samples{static_cast<std::int64_t>(block * 2)});

    CHECK(adapter.tick(*audio_engine, {}, {}).block_finished);
    CHECK(audio_engine->sample_clock() == samples{static_cast<std::int64_t>(block * 3)});
}

// ---- Clock in, clock out ----------------------------------------------------------------------

/// Drives the adapter with a 10 V clock that steps high at each of `edges` (sample indices) for 1
/// ms (48 samples), and an optional reset at `reset_at`. Returns the clock-out voltage per tick.
[[nodiscard]] std::vector<float> drive_clock(engine&                         target,
                                             block_adapter&                  adapter,
                                             const std::vector<std::size_t>& edges,
                                             std::size_t                     ticks,
                                             std::optional<std::size_t> reset_at = std::nullopt)
{
    std::vector<float> clock_out;
    clock_out.reserve(ticks);

    std::size_t next       = 0;
    std::size_t high_until = 0;

    for (std::size_t tick = 0; tick < ticks; ++tick) {
        if (next < edges.size() && edges[next] == tick) {
            high_until = tick + 48;
            ++next;
        }

        const float clock = tick < high_until ? 10.0F : 0.0F;
        const float reset = (reset_at && tick >= *reset_at && tick < *reset_at + 48) ? 10.0F : 0.0F;

        clock_out.push_back(
            adapter.tick(target, {}, {.clock_volts = clock, .reset_volts = reset}).clock_out_volts);
    }

    return clock_out;
}

/// The ticks at which a signal rises from 0 to a positive voltage.
[[nodiscard]] std::vector<std::size_t> rising_edges(const std::vector<float>& signal)
{
    std::vector<std::size_t> edges;

    for (std::size_t i = 0; i < signal.size(); ++i) {
        if (signal[i] > 0.0F && (i == 0 || signal[i - 1] == 0.0F)) {
            edges.push_back(i);
        }
    }

    return edges;
}

TEST_CASE("clock out follows clock in exactly one block later, pulse for pulse")
{
    // A 120 BPM, 4 PPQN clock starting at a block-unaligned sample. Clock out starts once the
    // tempo is known (the second pulse) and then rises exactly 32 samples after every input edge,
    // the same latency as the audio.
    std::vector<std::size_t> edges;
    edges.reserve(40);
    for (std::size_t i = 0; i < 40; ++i) {
        edges.push_back(1'013 + (6'000 * i));
    }

    auto          audio_engine = make_engine();
    block_adapter adapter{rate_48k};
    const auto    clock_out = drive_clock(*audio_engine, adapter, edges, edges.back() + 200);

    const auto out_edges = rising_edges(clock_out);
    REQUIRE(out_edges.size() == edges.size() - 1);

    for (std::size_t i = 0; i < out_edges.size(); ++i) {
        CAPTURE(i);
        CHECK(out_edges[i] == edges[i + 1] + block);
    }

    // Each trigger is 1 ms long.
    std::size_t high = 0;
    for (std::size_t i = out_edges[5]; i < clock_out.size() && clock_out[i] > 0.0F; ++i) {
        ++high;
    }
    CHECK(high == 48);
    CHECK(clock_out[out_edges[5]] == block_adapter::trigger_volts);
}

TEST_CASE("clock edges reach the engine at their exact sample")
{
    std::vector<std::size_t> edges;
    edges.reserve(10);
    for (std::size_t i = 0; i < 10; ++i) {
        edges.push_back(777 + (6'000 * i));
    }

    auto          audio_engine = make_engine();
    block_adapter adapter{rate_48k};
    static_cast<void>(drive_clock(*audio_engine, adapter, edges, edges.back() + 64));

    CHECK(audio_engine->clock().state == clock_state::locked);
    CHECK(audio_engine->clock().beat_period == sample_time::from_samples(samples{24'000}));
}

TEST_CASE("a reset on the reset input makes the simultaneous pulse bar 0")
{
    std::vector<std::size_t> edges;
    edges.reserve(40);
    for (std::size_t i = 0; i < 40; ++i) {
        edges.push_back(500 + (6'000 * i));
    }

    auto          audio_engine = make_engine();
    block_adapter adapter{rate_48k};

    // Reset fires with pulse 21. Bar 0 then starts there, so the bar containing pulse 39 is 1
    // (pulses 21-36 are bar 0, 37-52 bar 1).
    static_cast<void>(drive_clock(*audio_engine, adapter, edges, edges.back() + 64, edges[21]));

    CHECK(audio_engine->clock().bar == 1);
}

TEST_CASE("clock out keeps running while the input clock is stopped")
{
    std::vector<std::size_t> edges;
    edges.reserve(20);
    for (std::size_t i = 0; i < 20; ++i) {
        edges.push_back(300 + (6'000 * i));
    }

    auto          audio_engine = make_engine();
    block_adapter adapter{rate_48k};

    // Run 10 more periods after the last input edge.
    const auto clock_out =
        drive_clock(*audio_engine, adapter, edges, edges.back() + (std::size_t{6'000} * 10) + 100);
    const auto out_edges = rising_edges(clock_out);

    REQUIRE(out_edges.size() == (edges.size() - 1) + 10);
    for (std::size_t i = 1; i < out_edges.size(); ++i) {
        CAPTURE(i);
        CHECK(out_edges[i] - out_edges[i - 1] == 6'000);
    }
    CHECK(audio_engine->clock().state == clock_state::freewheeling);
}

/// Drives one tick of raw voltages (no audio) and returns whether a block finished.
bool tick_volts(engine& target, block_adapter& adapter, float clock, float reset)
{
    return adapter.tick(target, {}, {.clock_volts = clock, .reset_volts = reset}).block_finished;
}

TEST_CASE("an edge interpolated back across a block boundary lands at the block start")
{
    // A slewed gate: 0.5 V on the last sample of a block, 1.5 V on the first of the next. The
    // crossing is half a sample back, inside the previous (already processed) block, so it is
    // placed at the new block's first frame. A negative offset would break the engine contract.
    auto          audio_engine = make_engine();
    block_adapter adapter{rate_48k};

    constexpr std::size_t cycle = block * 200;  // a whole number of blocks

    for (std::size_t tick = 0; tick < (cycle * 3) + block; ++tick) {
        const std::size_t phase = tick % cycle;

        float clock = 0.0F;
        if (phase == cycle - 1) {
            clock = 0.5F;  // the last frame of a block
        }
        else if (tick >= cycle && phase == 0) {
            clock = 1.5F;  // the first frame of the next block
        }
        else if (tick >= cycle && phase == 1) {
            clock = 10.0F;
        }

        static_cast<void>(tick_volts(*audio_engine, adapter, clock, 0.0F));
    }

    // Three edges exactly one cycle apart, each clamped to a block start: the timing is
    // consistent, so the tempo is exact.
    CHECK(audio_engine->clock().state == clock_state::locked);
    CHECK(audio_engine->clock().beat_period ==
          sample_time::from_samples(samples{static_cast<std::int64_t>(cycle * 4)}));
}

TEST_CASE("a reset interpolated just before a clock step in the same sample stays in order")
{
    // The clock steps at this sample (offset p); the reset was caught mid-rise and interpolates
    // to p - 0.5. The engine requires events in time order, so the adapter must reorder them.
    auto          audio_engine = make_engine();
    block_adapter adapter{rate_48k};

    for (int pulse = 0; pulse < 4; ++pulse) {
        for (std::size_t tick = 0; tick < 5'998; ++tick) {
            static_cast<void>(tick_volts(*audio_engine, adapter, 0.0F, 0.0F));
        }
        const float reset_before = pulse == 3 ? 0.5F : 0.0F;
        const float reset_now    = pulse == 3 ? 1.5F : 0.0F;
        static_cast<void>(tick_volts(*audio_engine, adapter, 0.0F, reset_before));
        static_cast<void>(tick_volts(*audio_engine, adapter, 10.0F, reset_now));
    }

    for (std::size_t tick = 0; tick < block; ++tick) {
        static_cast<void>(tick_volts(*audio_engine, adapter, 0.0F, 0.0F));
    }

    // The reset coincided with the fourth pulse, so that pulse started bar 0.
    CHECK(audio_engine->clock().bar == 0);
}

TEST_CASE("clock-out triggers scale with the sample rate")
{
    constexpr sample_rate rate_96k{96'000U};

    auto audio_engine =
        std::make_unique<engine>(engine_config{.rate = rate_96k, .max_block_frames = block});
    block_adapter adapter{rate_96k};

    const std::vector<std::size_t> edges{100, 12'100, 24'100, 36'100};
    const auto                     clock_out = drive_clock(*audio_engine, adapter, edges, 36'300);

    const auto out_edges = rising_edges(clock_out);
    REQUIRE_FALSE(out_edges.empty());

    std::size_t high = 0;
    for (std::size_t i = out_edges.front(); i < clock_out.size() && clock_out[i] > 0.0F; ++i) {
        ++high;
    }
    CHECK(high == 96);  // 1 ms at 96 kHz
}

}  // namespace
}  // namespace previously::vcv
