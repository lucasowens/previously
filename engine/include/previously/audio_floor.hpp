// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the floor below which audio is exactly zero.
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

#pragma once

namespace previously {

/// The smallest magnitude audio may have without being zero.
inline constexpr float audio_floor = 0x1p-50F;

/// `value`, or exactly zero when its magnitude is below the floor. A comparison and a choice, no
/// arithmetic, so it is safe to inline anywhere.
[[nodiscard]] constexpr float flush_below_floor(float value) noexcept
{
    return value > -audio_floor && value < audio_floor ? 0.0F : value;
}

}  // namespace previously
