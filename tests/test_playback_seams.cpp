// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: every seam against §5.6's bounds, on its six signals (DESIGN.md §5.6, §5.7).
// Too slow for the sanitizers: `make seeds` runs them from an optimized build, and the unit tests
// run a few.

#include "seam_fixtures.hpp"

#include <doctest/doctest.h>

namespace previously {
namespace {

TEST_CASE("every seam, on each of §5.6's six signals, stays within its bounds")
{
    for (const test::seam_kind kind : test::every_seam) {
        for (const test::test_signal signal : test::every_signal) {
            test::check_seam(signal, kind);
        }
    }
}

}  // namespace
}  // namespace previously
