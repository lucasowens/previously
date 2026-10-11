// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: strong unit types.

#include "previously/units.hpp"

#include <doctest/doctest.h>

#include <functional>
#include <type_traits>

namespace previously {
namespace {

// Compile-time proof that units cannot be mixed by accident.
static_assert(!std::is_invocable_v<std::plus<>, samples, seconds>);
static_assert(!std::is_invocable_v<std::minus<>, seconds, samples>);
static_assert(!std::is_convertible_v<std::int64_t, samples>);
static_assert(!std::is_convertible_v<samples, std::int64_t>);
static_assert(!std::is_convertible_v<float, seconds>);

// Same-unit arithmetic works at compile time.
static_assert(samples{3} + samples{4} == samples{7});
static_assert(samples{10} - samples{4} == samples{6});
static_assert(samples{5} * std::int64_t{3} == samples{15});
static_assert(samples{12} / samples{4} == 3);
static_assert(samples{1} < samples{2});

TEST_CASE("samples accumulate exactly beyond float precision")
{
    // 2^24 + 1 cannot be represented by a float; int64 sample counts must stay exact.
    samples clock{16'777'216};
    clock += samples{1};

    CHECK(clock.value() == 16'777'217);
}

TEST_CASE("to_seconds converts at the given sample rate")
{
    constexpr sample_rate rate{48'000U};

    CHECK(to_seconds(samples{48'000}, rate).value() == doctest::Approx(1.0));
    CHECK(to_seconds(samples{24'000}, rate).value() == doctest::Approx(0.5));
    CHECK(to_seconds(samples{0}, rate).value() == doctest::Approx(0.0));
}

TEST_CASE("to_samples rounds to the nearest whole sample")
{
    constexpr sample_rate rate{48'000U};

    CHECK(to_samples(seconds{1.0F}, rate) == samples{48'000});
    CHECK(to_samples(seconds{0.010F}, rate) == samples{480});
    CHECK(to_samples(seconds{-0.5F}, rate) == samples{-24'000});
}

TEST_CASE("to_samples and to_seconds round-trip short durations exactly")
{
    constexpr sample_rate rate{48'000U};

    for (std::int64_t count = 0; count <= 48'000; count += 37) {
        const samples original{count};

        CHECK(to_samples(to_seconds(original, rate), rate) == original);
    }
}

}  // namespace
}  // namespace previously
