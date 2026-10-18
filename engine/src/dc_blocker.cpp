// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the DC blocker.

#include "previously/dc_blocker.hpp"

#include "previously/audio_floor.hpp"
#include "previously/contract.hpp"

#include <numbers>

namespace previously {
namespace {

[[nodiscard]] float coefficient_for(sample_rate rate) noexcept
{
    expects(rate.hz() > 0U);

    return 1.0F - ((2.0F * std::numbers::pi_v<float> * dc_blocker::corner_hz) /
                   static_cast<float>(rate.hz()));
}

}  // namespace

dc_blocker::dc_blocker(sample_rate rate) noexcept
    : coefficient_{coefficient_for(rate)}
{
}

float dc_blocker::process(float input) noexcept
{
    const float input_now = flush_below_floor(input);
    const float output =
        flush_below_floor((input_now - previous_input_) + (coefficient_ * previous_output_));

    previous_input_  = input_now;
    previous_output_ = output;

    return output;
}

}  // namespace previously
