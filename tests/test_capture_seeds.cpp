// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: capture comes out the same at any block size, over many seeded scenarios
// (DESIGN.md §3.4, "Capture under change", and §10.1). Too slow for the sanitizers: `make seeds`
// runs them from an optimized build.

#include "capture_fixtures.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace previously {
namespace {

TEST_CASE("capture reports and holds the same at any block size, over 200 seeded scenarios")
{
    // Each scenario is 13.4 s, compared every 107,520 samples, which every size here divides.
    constexpr std::array<std::size_t, 5> sizes{3'072, 480, 32, 7, 1};

    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        test::check_any_block_size(seed, sizes, samples{107'520}, 6);
    }
}

}  // namespace
}  // namespace previously
