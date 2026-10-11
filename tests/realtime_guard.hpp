// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: detects heap allocation inside realtime code.
//
// The test executable replaces the global operator new (realtime_guard.cpp). While a
// realtime_guard is alive on a thread, every allocation on that thread is counted as a
// violation. A test wraps engine::process() in a guard and checks that the count stays at zero.

#pragma once

#include <cstddef>

namespace previously::test {

class realtime_guard {
public:
    realtime_guard() noexcept;
    ~realtime_guard();

    realtime_guard(const realtime_guard&)            = delete;
    realtime_guard& operator=(const realtime_guard&) = delete;
    realtime_guard(realtime_guard&&)                 = delete;
    realtime_guard& operator=(realtime_guard&&)      = delete;

    /// Allocations made inside any guard on this thread since the last reset.
    [[nodiscard]] static std::size_t violations() noexcept;

    static void reset_violations() noexcept;
};

}  // namespace previously::test
