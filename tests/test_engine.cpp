// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: engine passthrough, sample clock, commands, and realtime safety.

#include "previously/engine.hpp"

#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
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

/// Whether `allocate` throws std::bad_alloc. For code under an allocation_failure, where doctest's
/// CHECK_THROWS_AS cannot run: it allocates as it reports.
template <typename Allocate>
[[nodiscard]] bool throws_bad_alloc(Allocate allocate)
{
    try {
        allocate();
    }
    catch (const std::bad_alloc&) {
        return true;
    }

    return false;
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

        CHECK(single != nullptr);
        CHECK(array != nullptr);
        CHECK(std::bit_cast<std::uintptr_t>(aligned) % 64 == 0);
        CHECK(std::bit_cast<std::uintptr_t>(aligned_array) % 64 == 0);
        CHECK(std::bit_cast<std::uintptr_t>(aligned_single) % 64 == 0);
        CHECK(std::bit_cast<std::uintptr_t>(aligned_many) % 64 == 0);
        CHECK(aligned_single != nullptr);
        CHECK(aligned_many != nullptr);

        ::operator delete(single, std::nothrow);
        ::operator delete[](array, std::nothrow);
        ::operator delete(aligned, cache_line);
        ::operator delete[](aligned_array, cache_line);
        ::operator delete(aligned_single, cache_line, std::nothrow);
        ::operator delete[](aligned_many, cache_line, std::nothrow);
    }

    CHECK(test::realtime_guard::violations() == 6U);
}

TEST_CASE("under an allocation_failure every form of new fails, and each attempt is counted")
{
    constexpr auto cache_line = std::align_val_t{64};

    void* single         = nullptr;
    void* array          = nullptr;
    void* aligned_single = nullptr;
    void* aligned_many   = nullptr;

    bool plain_threw         = false;
    bool plain_array_threw   = false;
    bool aligned_threw       = false;
    bool aligned_array_threw = false;

    // Only the allocations under test happen inside the failure: doctest allocates as it checks.
    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard     guard;
        const test::allocation_failure no_memory;

        single         = ::operator new(16, std::nothrow);
        array          = ::operator new[](16, std::nothrow);
        aligned_single = ::operator new(64, cache_line, std::nothrow);
        aligned_many   = ::operator new[](64, cache_line, std::nothrow);

        // Each frees what it got, so a form that fails to fail does not leak as well.
        plain_threw         = throws_bad_alloc([] {
            ::operator delete(::operator new(16));
        });
        plain_array_threw   = throws_bad_alloc([] {
            ::operator delete[](::operator new[](16));
        });
        aligned_threw       = throws_bad_alloc([] {
            ::operator delete(::operator new(64, cache_line), cache_line);
        });
        aligned_array_threw = throws_bad_alloc([] {
            ::operator delete[](::operator new[](64, cache_line), cache_line);
        });
    }

    CHECK(single == nullptr);
    CHECK(array == nullptr);
    CHECK(aligned_single == nullptr);
    CHECK(aligned_many == nullptr);
    CHECK(plain_threw);
    CHECK(plain_array_threw);
    CHECK(aligned_threw);
    CHECK(aligned_array_threw);
    CHECK(test::realtime_guard::violations() == 8U);

    // Freed in case anything came back, as above.
    ::operator delete(single, std::nothrow);
    ::operator delete[](array, std::nothrow);
    ::operator delete(aligned_single, cache_line, std::nothrow);
    ::operator delete[](aligned_many, cache_line, std::nothrow);

    // Allocation works again once the failure has gone.
    void* const after = ::operator new(16, std::nothrow);

    CHECK(after != nullptr);
    ::operator delete(after, std::nothrow);
}

TEST_CASE("the aligned forms of new fail for a size that cannot be rounded up to the alignment")
{
    // What a new-expression passes when its size overflows: clang hands the allocator SIZE_MAX
    // and relies on it to fail. Rounded up to a cache line that wraps to 0, which aligned_alloc
    // would serve. Also the last size that wraps. One less would round without wrapping, and is
    // not tried: ASan aborts on any request that large rather than failing it.
    constexpr auto        cache_line = std::align_val_t{64};
    constexpr std::size_t limit      = std::numeric_limits<std::size_t>::max();

    // In an array rather than as literals, so no compiler can see the size and warn about it.
    const std::array<std::size_t, 2> sizes{limit, limit - 62};

    for (const std::size_t size : sizes) {
        CAPTURE(size);

        void* const single = ::operator new(size, cache_line, std::nothrow);
        void* const array  = ::operator new[](size, cache_line, std::nothrow);

        CHECK(single == nullptr);
        CHECK(array == nullptr);

        // Freed if anything came back, so a form that fails to fail does not leak as well.
        ::operator delete(single, cache_line, std::nothrow);
        ::operator delete[](array, cache_line, std::nothrow);

        CHECK_THROWS_AS(::operator delete(::operator new(size, cache_line), cache_line),
                        std::bad_alloc);

        CHECK_THROWS_AS(::operator delete[](::operator new[](size, cache_line), cache_line),
                        std::bad_alloc);
    }
}

}  // namespace

}  // namespace previously
