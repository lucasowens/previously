// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the bar-line estimate under jitter, at PR #6's seed counts (DESIGN.md §3.4).
//
// §3.4's jitter table, its never-worse property and σ̂'s own measurements, each at the seed count
// the review stated: 50,000 a row, 20,000 a step cell or family. Jitter is uniform, from
// test_noise, so every platform draws the same values. A line is worse than its raw edge when the
// estimate is more than 0.05 samples further from where the pulse belongs, the probe's measure.

#include "previously/bar_line.hpp"

#include "planted_clock.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <ranges>
#include <string>
#include <vector>

namespace previously {
namespace {

using test::planted_clock;
using test::planted_line;
using test::to_samples;

constexpr auto line_index = static_cast<std::size_t>(planted_line);

/// Seeds spread across the generator's state, as the probe spread them.
[[nodiscard]] std::uint64_t seed_of(std::uint64_t seed)
{
    return seed * 2'654'435'761U;
}

/// Lines worse than their raw edge, and the worst estimate, over a family of seeded clocks.
struct family_outcome {
    std::size_t worse{0};
    double      worst_error{0.0};
    std::size_t repaired{0};  // lines at least half a hiccup closer than their raw edge
};

template <typename Make>
[[nodiscard]] family_outcome run_family(std::uint64_t seeds, const Make& make)
{
    family_outcome outcome;

    for (std::uint64_t seed = 1; seed <= seeds; ++seed) {
        const planted_clock clock     = make(seed_of(seed));
        const double        raw_error = std::abs(clock.raw[line_index] - clock.truth[line_index]);
        const double error = std::abs(to_samples(test::estimate_planted(clock, planted_line).time) -
                                      clock.truth[line_index]);

        outcome.worse += error > raw_error + 0.05 ? 1 : 0;
        outcome.worst_error = std::max(outcome.worst_error, error);
        outcome.repaired += error < raw_error / 2.0 ? 1 : 0;
    }

    return outcome;
}

/// No line of `base`, jittered within +-`amplitude` over `seeds` seeds, is worse than its raw
/// edge, and so the estimate's worst is within the jitter.
void check_none_worse(const planted_clock& base,
                      double               amplitude,
                      std::uint64_t        seeds,
                      std::uint64_t        spread = 1)
{
    CAPTURE(base.name);
    CAPTURE(amplitude);

    const family_outcome outcome = run_family(seeds, [&](std::uint64_t seed) {
        return test::uniform_jitter(base, amplitude, seed * spread);
    });

    CHECK(outcome.worse == 0);
    CHECK(outcome.worst_error <= amplitude + 0.05);
}

TEST_CASE("under jitter, no line is worse than its raw edge: §3.4's jitter table, 50,000 seeds a "
          "row")
{
    check_none_worse(test::steady(1), 0.5, 50'000);
    check_none_worse(test::steady(1), 10.0, 50'000);
    check_none_worse(test::steady(1), 20.0, 50'000);
    check_none_worse(test::gentle_ramp(), 0.5, 50'000);
    check_none_worse(test::gentle_ramp(), 1.0, 50'000);
    check_none_worse(test::gentle_ramp(), 2.0, 50'000);
    check_none_worse(test::gentle_ramp(), 5.0, 50'000);
    check_none_worse(test::linear_ramp(0.2, 2), 2.0, 50'000);
    check_none_worse(test::linear_ramp(0.2, 2), 10.0, 50'000);
    check_none_worse(test::linear_ramp(0.2, 2), 20.0, 50'000);
    check_none_worse(test::tempo_change(1, 1.01), 10.0, 50'000);
    check_none_worse(test::tempo_change(1, 1.1), 20.0, 50'000);
}

TEST_CASE("an 1,800-sample hiccup on +10% over 8 bars at +-2 samples is repaired in every one of "
          "50,000 seeds")
{
    const planted_clock  base    = test::gentle_ramp();
    const family_outcome outcome = run_family(50'000, [&](std::uint64_t seed) {
        planted_clock clock = test::uniform_jitter(base, 2.0, seed);

        clock.raw[line_index] += 1'800.0;

        return clock;
    });

    CHECK(outcome.worse == 0);
    CHECK(outcome.repaired == 50'000);
    CHECK(outcome.worst_error < 20.0);
    MESSAGE("worst repaired error " << outcome.worst_error << " samples (the table: 12.1)");
}

TEST_CASE("small tempo steps at the line keep every line no worse than its raw edge: 21 cells of "
          "20,000 seeds")
{
    for (const double amplitude : {0.5, 2.0, 10.0}) {
        for (const double step : {0.002, 0.005, 0.01, 0.02, 0.03, 0.05, 0.08}) {
            CAPTURE(step);
            check_none_worse(test::tempo_change(1, 1.0 + step), amplitude, 20'000);
        }
    }
}

TEST_CASE("no line is worse than its raw edge: 9 ramp and steady families of 20,000 seeds")
{
    // Seeds other than the jitter table's, as the probe drew them.
    check_none_worse(test::gentle_ramp(), 1.0, 20'000, 3);
    check_none_worse(test::gentle_ramp(), 2.0, 20'000, 3);
    check_none_worse(test::gentle_ramp(), 5.0, 20'000, 3);
    check_none_worse(test::linear_ramp(0.2, 2), 2.0, 20'000, 3);
    check_none_worse(test::linear_ramp(0.2, 2), 10.0, 20'000, 3);
    check_none_worse(test::linear_ramp(0.2, 2), 20.0, 20'000, 3);
    check_none_worse(test::steady(1), 0.5, 20'000, 3);
    check_none_worse(test::steady(1), 10.0, 20'000, 3);
    check_none_worse(test::steady(1), 20.0, 20'000, 3);
}

TEST_CASE("sigma falls below half the true jitter in very few of 50,000 seeds")
{
    // Uniform jitter of +-2 samples has a standard deviation of 2 / sqrt(3).
    const double        truth = 2.0 / std::numbers::sqrt3;
    const planted_clock base  = test::steady(1);
    std::size_t         low   = 0;

    for (std::uint64_t seed = 1; seed <= 50'000; ++seed) {
        const planted_clock clock    = test::uniform_jitter(base, 2.0, seed_of(seed));
        const line_estimate estimate = test::estimate_planted(clock, planted_line);

        low += static_cast<double>(estimate.jitter) < truth / 2.0 ? 1 : 0;
    }

    MESSAGE(low << " of 50,000 seeds below half the true jitter (the table: 0.004%, 2)");
    CHECK(low <= 10);
}

TEST_CASE("with 40 fill-ins before the line, sigma still sees the real pulses' jitter, and no line "
          "is worse than its raw edge: 5,000 seeds")
{
    // Fill-ins sit exactly on the grid. Measured at +-10 samples: with them skipped, the median
    // sigma was 6.03 (true 5.77) and no line worse; without, 0.00 and 384 lines worse.
    const planted_clock base = test::steady(1);
    std::vector<bool>   filled_in(base.raw.size(), false);

    for (int j = planted_line - 40; j < planted_line; ++j) {
        filled_in[static_cast<std::size_t>(j)] = true;
    }

    std::vector<double> sigmas;
    std::size_t         worse = 0;

    for (std::uint64_t seed = 1; seed <= 5'000; ++seed) {
        planted_clock clock = test::uniform_jitter(base, 10.0, seed_of(seed));

        for (std::size_t j = 0; j < clock.raw.size(); ++j) {
            if (filled_in[j]) {
                clock.raw[j] = clock.truth[j];
            }
        }

        const line_estimate estimate  = test::estimate_planted(clock, planted_line, filled_in);
        const double        raw_error = std::abs(clock.raw[line_index] - clock.truth[line_index]);
        const double        error = std::abs(to_samples(estimate.time) - clock.truth[line_index]);

        sigmas.push_back(static_cast<double>(estimate.jitter));
        worse += error > raw_error + 0.05 ? 1 : 0;
    }

    std::ranges::sort(sigmas);

    const double median = sigmas[sigmas.size() / 2];

    MESSAGE("median sigma " << median << " samples (true " << 10.0 / std::numbers::sqrt3 << ")");
    CHECK(worse == 0);
    CHECK(median > 5.0);
    CHECK(median < 7.0);
}

TEST_CASE("with only 10 usable third differences, no line is worse than its raw edge: 50,000 seeds")
{
    // Fill-ins from 64 pulses before the line to 9 before leave 10 third differences, below the
    // minimum of 16, so step 4 never repairs. Without the minimum, 1 line in 50,000 came out worse
    // (by 0.5 samples), and sigma fell below half the true jitter in 8.1% of seeds.
    const planted_clock base = test::steady(1);
    std::vector<bool>   filled_in(base.raw.size(), false);

    for (int j = planted_line - 64; j <= planted_line - 9; ++j) {
        filled_in[static_cast<std::size_t>(j)] = true;
    }

    std::size_t worse = 0;

    for (std::uint64_t seed = 1; seed <= 50'000; ++seed) {
        planted_clock clock = test::uniform_jitter(base, 10.0, seed_of(seed));

        for (std::size_t j = 0; j < clock.raw.size(); ++j) {
            if (filled_in[j]) {
                clock.raw[j] = clock.truth[j];
            }
        }

        const line_estimate estimate  = test::estimate_planted(clock, planted_line, filled_in);
        const double        raw_error = std::abs(clock.raw[line_index] - clock.truth[line_index]);
        const double        error = std::abs(to_samples(estimate.time) - clock.truth[line_index]);

        CHECK(estimate.jitter_values == 10);
        worse += error > raw_error + 0.05 ? 1 : 0;
    }

    CHECK(worse == 0);
}

}  // namespace
}  // namespace previously
