// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: detects heap allocation inside realtime code, and fails allocation on demand.
//
// The test executable replaces the global operator new (realtime_guard.cpp). While a
// realtime_guard is alive on a thread, every allocation on that thread is counted as a
// violation. A test wraps engine::process() in a guard and checks that the count stays at zero.
//
// While an allocation_failure is alive on a thread, every allocation on that thread fails as it
// would with no memory left: the nothrow forms of new return null, and the throwing forms throw
// std::bad_alloc. A test wraps the one allocation it wants to see fail, and nothing else, since
// the test framework allocates too.

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

class allocation_failure {
public:
    allocation_failure() noexcept;
    ~allocation_failure();

    allocation_failure(const allocation_failure&)            = delete;
    allocation_failure& operator=(const allocation_failure&) = delete;
    allocation_failure(allocation_failure&&)                 = delete;
    allocation_failure& operator=(allocation_failure&&)      = delete;
};

}  // namespace previously::test
