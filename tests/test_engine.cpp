// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: engine passthrough, sample clock, commands, and realtime safety.

#include "previously/engine.hpp"

#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <random>
#include <span>
#include <vector>

namespace previously {
namespace {

/// A stereo test signal plus output buffers, processed through an engine block by block.
struct stereo_fixture {
    std::vector<float> in_left;
    std::vector<float> in_right;
    std::vector<float> out_left;
    std::vector<float> out_right;

    explicit stereo_fixture(std::size_t frames)
        : in_left(frames),
          in_right(frames),
          out_left(frames),
          out_right(frames)
    {
        std::mt19937                          generator{1234U};
        std::uniform_real_distribution<float> distribution{-1.0F, 1.0F};

        for (std::size_t i = 0; i < frames; ++i) {
            in_left[i]  = distribution(generator);
            in_right[i] = distribution(generator);
        }
    }

    void process_in_blocks(engine& target, std::size_t block_frames)
    {
        const std::size_t frames = in_left.size();

        for (std::size_t start = 0; start < frames; start += block_frames) {
            const std::size_t count = std::min(block_frames, frames - start);

            const audio_block block{
                .in_left   = std::span{in_left}.subspan(start, count),
                .in_right  = std::span{in_right}.subspan(start, count),
                .out_left  = std::span{out_left}.subspan(start, count),
                .out_right = std::span{out_right}.subspan(start, count),
            };

            target.process(block);
        }
    }
};

TEST_CASE("passthrough is bit-exact at every block size")
{
    for (const std::size_t block_frames : {1U, 16U, 32U, 48U}) {
        CAPTURE(block_frames);

        engine audio_engine{engine_config{.rate = sample_rate{48'000U}, .max_block_frames = 48}};
        stereo_fixture fixture{4'800};

        fixture.process_in_blocks(audio_engine, block_frames);

        CHECK(fixture.out_left == fixture.in_left);
        CHECK(fixture.out_right == fixture.in_right);
    }
}

TEST_CASE("the sample clock advances by exactly the frames processed")
{
    engine         audio_engine{engine_config{}};
    stereo_fixture fixture{1'000};

    fixture.process_in_blocks(audio_engine, 32);

    CHECK(audio_engine.sample_clock() == samples{1'000});
}

TEST_CASE("posting fails cleanly when the command queue is full")
{
    engine audio_engine{engine_config{}};

    for (std::size_t i = 0; i < engine::command_queue_capacity; ++i) {
        REQUIRE(audio_engine.post(command{}));
    }

    CHECK_FALSE(audio_engine.post(command{}));
}

TEST_CASE("process never allocates")
{
    engine         audio_engine{engine_config{}};
    stereo_fixture fixture{4'800};

    REQUIRE(audio_engine.post(command{}));

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        fixture.process_in_blocks(audio_engine, 32);
    }

    CHECK(test::realtime_guard::violations() == 0);
}

TEST_CASE("the realtime guard detects an allocation")
{
    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        const std::vector<int>     allocates(16);
        CHECK(allocates.size() == 16);
    }

    CHECK(test::realtime_guard::violations() > 0);
}

TEST_CASE("the realtime guard counts nothrow and over-aligned allocations, once each")
{
    // Called directly, as a new-expression would call them: an allocation a compiler may not
    // leave out. Each is freed by the delete that matches it.
    constexpr auto cache_line = std::align_val_t{64};

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;

        void* const single         = ::operator new(16, std::nothrow);
        void* const array          = ::operator new[](16, std::nothrow);
        void* const aligned        = ::operator new(64, cache_line);
        void* const aligned_array  = ::operator new[](64, cache_line);
        void* const aligned_single = ::operator new(64, cache_line, std::nothrow);
        void* const aligned_many   = ::operator new[](64, cache_line, std::nothrow);

        CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);  // NOLINT(*-reinterpret-cast)

        ::operator delete(single, std::nothrow);
        ::operator delete[](array, std::nothrow);
        ::operator delete(aligned, cache_line);
        ::operator delete[](aligned_array, cache_line);
        ::operator delete(aligned_single, cache_line, std::nothrow);
        ::operator delete[](aligned_many, cache_line, std::nothrow);
    }

    CHECK(test::realtime_guard::violations() == 6U);
}

}  // namespace
}  // namespace previously
