// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: how loud a stretch of audio was, in the display's steps.
//
// The display shows peaks in steps of 3 dB (DESIGN.md §9.3), so a peak is kept as a level: how
// many steps it reaches above -48 dBFS. Level k means a peak of at least 10^((3k - 48) / 20), so
// level 1 is -45 dBFS, level 16 is 0 dBFS and above, and level 0 is anything quieter than
// -45 dBFS. The steps are a starting scale, to be tried in Rack.
//
// The thresholds are written out, each the float nearest its true value, rather than computed:
// the engine calls no libm function, and a comparison with a constant is exact, so a peak has the
// same level on every target (ADR 0005).

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace previously {

/// The loudest frame of a stretch of audio, per channel, as a level from 0 to max_level.
struct peak_level {
    static constexpr std::uint8_t max_level = 16;

    std::uint8_t left{0};
    std::uint8_t right{0};

    [[nodiscard]] friend constexpr bool operator==(const peak_level&,
                                                   const peak_level&) noexcept = default;
};

/// The least peak at each level from 1 to 16: 10^((3k - 48) / 20), -45 dBFS to 0 dBFS.
inline constexpr std::array<float, peak_level::max_level> level_thresholds{
    0.0056234132F,  // -45 dB
    0.007943282F,   // -42 dB
    0.011220184F,   // -39 dB
    0.015848933F,   // -36 dB
    0.022387212F,   // -33 dB
    0.031622775F,   // -30 dB
    0.044668358F,   // -27 dB
    0.06309573F,    // -24 dB
    0.0891251F,     // -21 dB
    0.12589253F,    // -18 dB
    0.17782794F,    // -15 dB
    0.25118864F,    // -12 dB
    0.3548134F,     //  -9 dB
    0.5011872F,     //  -6 dB
    0.70794576F,    //  -3 dB
    1.0F,           //   0 dB
};

/// The level a peak reaches: how many thresholds are at or below it. `peak` is a magnitude, never
/// negative, and finite: the recorder measures what the DC blocker passes on, which is neither.
[[nodiscard]] constexpr std::uint8_t level_of(float peak) noexcept
{
    const auto reached =
        std::count_if(level_thresholds.begin(), level_thresholds.end(), [peak](float threshold) {
            return peak >= threshold;
        });

    return static_cast<std::uint8_t>(reached);
}

}  // namespace previously
