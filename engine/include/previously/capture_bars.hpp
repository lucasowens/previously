// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: how many bars a capture asks for.

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace previously {

/// A CAPTURE press asks for the last 1, 2, 4 or 8 bars (DESIGN.md §3.4). Each is stored as its
/// power of two, so that a zeroed value is one bar.
enum class capture_bars : std::uint8_t {
    one   = 0,
    two   = 1,
    four  = 2,
    eight = 3,
};

/// The bars as a count. A value that is not one of the four, an adapter's error, counts as the
/// most there are, 8, rather than shift by more than that.
[[nodiscard]] constexpr std::int64_t bar_count(capture_bars bars) noexcept
{
    constexpr std::uint8_t most = 3;

    return static_cast<std::int64_t>(std::uint64_t{1}
                                     << std::min(static_cast<std::uint8_t>(bars), most));
}

/// Whether `bars` is one of the four counts. Anything else is an adapter's programming error.
[[nodiscard]] constexpr bool is_bar_count(capture_bars bars) noexcept
{
    switch (bars) {
        case capture_bars::one:
        case capture_bars::two:
        case capture_bars::four:
        case capture_bars::eight:
            return true;
    }

    return false;
}

/// The next shorter count a capture is shortened to: half as many bars, or none below one.
[[nodiscard]] constexpr std::optional<capture_bars> fewer(capture_bars bars) noexcept
{
    switch (bars) {
        case capture_bars::eight:
            return capture_bars::four;
        case capture_bars::four:
            return capture_bars::two;
        case capture_bars::two:
            return capture_bars::one;
        case capture_bars::one:
            return std::nullopt;
    }

    return std::nullopt;
}

}  // namespace previously
