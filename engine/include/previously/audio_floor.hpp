// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: what counts as audio, and the floor below which it is exactly zero.
//
// Subnormal floats are handled differently by each place the engine runs. Rack's engine thread
// flushes them to zero, while the host tests and the Cortex-M7 keep them, so any arithmetic that
// produced or consumed one would give different results in each (ADR 0005). The engine never lets
// one exist. Audio entering an arithmetic path, every value passed between processing stages and
// every piece of recursive state is flushed to exactly zero below the floor.
//
// The floor is 2^-50 (-301 dBFS):
//   - a power of two, so the comparison is exact everywhere;
//   - far above the subnormal range, which starts at 2^-126;
//   - far below the 24-bit noise floor (-144 dBFS), so no real audio is changed.
// A product of two values at or above it is at least 2^-100, and sums and differences of such
// products are normal or exactly zero.
//
// A value that is not finite is not audio either, and is flushed the same way. One NaN or
// infinity in a recursive filter's state stays there for ever, and a Rack module upstream can send
// one: a filter that blew up, or a division by zero. Finite input can overflow too: the largest
// float minus its negative is infinite. So recursive state is checked as well as input.

#pragma once

#include <limits>

namespace previously {

/// The smallest magnitude audio may have without being zero.
inline constexpr float audio_floor = 0x1p-50F;

/// `value`, or exactly zero when it is not audio: smaller in magnitude than the floor, or not
/// finite. Comparisons only, so no floating-point rule can change its result.
[[nodiscard]] constexpr float flush_non_audio(float value) noexcept
{
    constexpr float largest = std::numeric_limits<float>::max();

    const bool below_floor = value > -audio_floor && value < audio_floor;
    const bool finite      = value >= -largest && value <= largest;  // false for NaN

    return finite && !below_floor ? value : 0.0F;
}

}  // namespace previously
