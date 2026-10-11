// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: fixed-point sample time.

#include "previously/sample_time.hpp"

#include <doctest/doctest.h>

#include <cstdint>

namespace previously {
namespace {

static_assert(sample_time::fraction_bits == 16);
static_assert(sample_time::ticks_per_sample == 65'536);
static_assert(sample_time::from_samples(samples{2}).ticks() == 131'072);
static_assert(sample_time::from_samples(samples{3}) - sample_time::from_samples(samples{1}) ==
              sample_time::from_samples(samples{2}));

// floor_divide rounds toward minus infinity in every sign combination.
static_assert(floor_divide(7, 2) == 3);
static_assert(floor_divide(-7, 2) == -4);
static_assert(floor_divide(7, -2) == -4);
static_assert(floor_divide(-7, -2) == 3);
static_assert(floor_divide(-8, 2) == -4);
static_assert(floor_divide(0, 5) == 0);

TEST_CASE("a fractional time splits into whole samples and a fraction")
{
    const sample_time time = sample_time::from_samples(samples{1'000}, 0.25F);

    CHECK(time.whole_samples() == samples{1'000});
    CHECK(time.fraction() == doctest::Approx(0.25));
}

TEST_CASE("negative times round their whole part toward minus infinity")
{
    const sample_time time =
        sample_time::from_samples(samples{0}) - sample_time::from_samples_float(0.25F);

    CHECK(time.whole_samples() == samples{-1});
    CHECK(time.fraction() == doctest::Approx(0.75));
}

TEST_CASE("repeated addition is exact: no drift after a million fractional periods")
{
    // A period of 6000.37 samples cannot be represented exactly, but once it is a whole number of
    // ticks, adding it a million times must equal multiplying it by a million, to the tick.
    const sample_time period = sample_time::from_samples_float(6'000.37F);

    sample_time sum;
    for (int i = 0; i < 1'000'000; ++i) {
        sum += period;
    }

    CHECK(sum == period * 1'000'000);
}

TEST_CASE("times far into a performance keep sub-sample resolution")
{
    // Ten days at 48 kHz is about 4.1e10 samples, far past where a float stops resolving samples.
    const sample_time ten_days = sample_time::from_samples(samples{41'472'000'000});
    const sample_time later    = ten_days + sample_time::from_samples_float(0.001F);

    CHECK(later > ten_days);
    CHECK((later - ten_days).to_samples_float() == doctest::Approx(0.001).epsilon(0.01));
}

}  // namespace
}  // namespace previously
