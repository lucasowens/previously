// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: unwrapping an optional the test expects to hold a value.

#pragma once

#include <doctest/doctest.h>

#include <optional>

namespace previously::test {

/// The value inside an optional the test expects to be present. A missing value fails the test
/// (REQUIRE) instead of being dereferenced.
template <typename T>
[[nodiscard]] T known(const std::optional<T>& maybe)
{
    REQUIRE(maybe.has_value());
    return maybe.value_or(T{});
}

}  // namespace previously::test
