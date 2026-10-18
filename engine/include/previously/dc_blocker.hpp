// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: a DC blocker, the one-pole high-pass on the record path.
//
// A DC offset in the input would be recorded into every loop, and each loop point would then cut
// it on and off: a thump at every edge (DESIGN.md §5.1). The blocker removes it before recording:
//
//     y[n] = x[n] - x[n-1] + r * y[n-1]        r = 1 - 2π × 5 Hz / rate
//
// That puts the corner at 5 Hz (measured: -3.01 dB at 5 Hz, -0.26 dB at 20 Hz, +0.003 dB at
// 1 kHz), and a full-scale offset is gone in 1.1 s. The coefficient uses only +, -, × and ÷,
// which round the same way on every target, never a libm function (ADR 0005).
//
// Fed silence after full scale, the state decays towards zero for ever, and would go subnormal
// after 2.78 s. Below the audio floor it is set to exactly zero instead, and the input is flushed
// the same way, so the filter never produces or consumes a subnormal (audio_floor.hpp).

#pragma once

#include "previously/units.hpp"

namespace previously {

class dc_blocker {
public:
    static constexpr float corner_hz = 5.0F;

    explicit dc_blocker(sample_rate rate) noexcept;

    /// Filters the next sample. Realtime-safe.
    [[nodiscard]] float process(float input) noexcept;

private:
    float coefficient_;
    float previous_input_{0.0F};
    float previous_output_{0.0F};
};

}  // namespace previously
