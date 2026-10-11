// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the VCV Rack re-blocking adapter (tier T4).

#include "previously/engine.hpp"

#include "block_adapter.hpp"

#include <doctest/doctest.h>

#include <cstddef>

namespace previously::vcv {
namespace {

constexpr std::size_t block = block_adapter::block_frames;

[[nodiscard]] engine make_engine()
{
    return engine{engine_config{.rate = sample_rate{48'000U}, .max_block_frames = block}};
}

TEST_CASE("output is the input delayed by exactly one block")
{
    engine        audio_engine = make_engine();
    block_adapter adapter;

    for (std::size_t tick = 0; tick < block * 4; ++tick) {
        const auto         value  = static_cast<float>(tick + 1);
        const stereo_frame output = adapter.tick(audio_engine, {.left = value, .right = -value});

        const float expected = tick < block ? 0.0F : static_cast<float>(tick + 1 - block);

        CAPTURE(tick);
        CHECK(output.left == expected);
        CHECK(output.right == -expected);
    }
}

TEST_CASE("the engine runs once per completed block")
{
    engine        audio_engine = make_engine();
    block_adapter adapter;

    for (std::size_t tick = 0; tick < (block * 3) - 1; ++tick) {
        static_cast<void>(adapter.tick(audio_engine, {}));
    }

    CHECK(audio_engine.sample_clock() == samples{static_cast<std::int64_t>(block * 2)});

    static_cast<void>(adapter.tick(audio_engine, {}));

    CHECK(audio_engine.sample_clock() == samples{static_cast<std::int64_t>(block * 3)});
}

}  // namespace
}  // namespace previously::vcv
