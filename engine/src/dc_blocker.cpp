// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the DC blocker.

#include "previously/dc_blocker.hpp"

#include "previously/audio_floor.hpp"
#include "previously/contract.hpp"

#include <algorithm>
#include <cstdint>
#include <numbers>

namespace previously {
namespace {

[[nodiscard]] float coefficient_for(sample_rate rate) noexcept
{
    // A rate of zero is a programming error, caught here in debug builds. It is also clamped, so a
    // release build still behaves sanely. So is the coefficient: at rates up to 31 Hz it would be
    // negative, and below 16 Hz the filter would be unstable. Zero keeps it stable.
    expects(rate.hz() > 0U);

    const auto  rate_hz = static_cast<float>(std::max(rate.hz(), std::uint32_t{1}));
    const float coefficient =
        1.0F - ((2.0F * std::numbers::pi_v<float> * dc_blocker::corner_hz) / rate_hz);

    return std::max(coefficient, 0.0F);
}

}  // namespace

dc_blocker::dc_blocker(sample_rate rate) noexcept
    : coefficient_{coefficient_for(rate)}
{
}

float dc_blocker::process(float input) noexcept
{
    const float input_now = flush_non_audio(input);
    const float output =
        flush_non_audio((input_now - previous_input_) + (coefficient_ * previous_output_));

    previous_input_  = input_now;
    previous_output_ = output;

    return output;
}

}  // namespace previously
