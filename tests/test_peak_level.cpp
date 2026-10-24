// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the display's level scale.

#include "previously/peak_level.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace previously {
namespace {

static_assert(level_of(0.0F) == 0);
static_assert(level_of(1.0F) == peak_level::max_level);
static_assert(level_of(4.0F) == peak_level::max_level, "louder than full scale is the top level");

TEST_CASE("each threshold is the float nearest 10^((3k - 48) / 20)")
{
    for (std::size_t k = 1; k <= peak_level::max_level; ++k) {
        CAPTURE(k);

        const double decibels = (3.0 * static_cast<double>(k)) - 48.0;
        const auto   expected = static_cast<float>(std::pow(10.0, decibels / 20.0));

        CHECK(level_thresholds[k - 1] == expected);
    }
}

TEST_CASE("a peak reaches a level at its threshold, and not one float below it")
{
    for (std::size_t k = 1; k <= peak_level::max_level; ++k) {
        CAPTURE(k);

        const float threshold = level_thresholds[k - 1];
        const float below     = std::nextafter(threshold, 0.0F);

        CHECK(level_of(threshold) == k);
        CHECK(level_of(below) == k - 1);
    }
}

TEST_CASE("anything quieter than -45 dBFS is level 0, down to the smallest float")
{
    CHECK(level_of(std::nextafter(level_thresholds[0], 0.0F)) == 0);
    CHECK(level_of(0x1p-50F) == 0);
    CHECK(level_of(0x1p-149F) == 0);
}

}  // namespace
}  // namespace previously
