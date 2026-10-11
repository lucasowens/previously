// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack adapter: finds rising edges in a sampled gate signal, with sub-sample timing.
//
// Detection is a Schmitt trigger using Rack's voltage conventions: an edge fires when the signal
// reaches 1 V, and the detector re-arms only after the signal falls back to 0.1 V, so noise on a
// high gate never retriggers.
//
// Timing. Rack modules emit digital steps: the gate is low on one sample and high on the next.
// Such a step carries no information about where between the two samples it "really" happened,
// and the module that produced it means "this sample". So a step is timed at the sample where it
// is first seen, which also lines Rack clocks up with Rack audio sample for sample.
//
// An edge caught mid-rise (the previous sample was already above 0.1 V, as with a slewed or
// filtered gate) does carry sub-sample information: the time it crossed 1 V is found by linear
// interpolation between the two samples.

#pragma once

#include <optional>

namespace previously::vcv {

class edge_detector {
public:
    static constexpr float high_volts = 1.0F;  // at or above: high (Rack's convention)
    static constexpr float low_volts  = 0.1F;  // at or below: low, and the detector re-arms

    /// Feeds one sample. If a rising edge completed at this sample, returns where it happened,
    /// relative to this sample, in (-1, 0]: 0 is this sample, -0.25 a quarter sample before it.
    [[nodiscard]] std::optional<float> step(float volts) noexcept
    {
        std::optional<float> edge;

        if (armed_ && volts >= high_volts) {
            armed_ = false;
            edge = previous_volts_ <= low_volts ? 0.0F
                                                : -(volts - high_volts) / (volts - previous_volts_);
        }
        else if (!armed_ && volts <= low_volts) {
            armed_ = true;
        }

        previous_volts_ = volts;

        return edge;
    }

private:
    float previous_volts_{0.0F};
    bool  armed_{true};
};

}  // namespace previously::vcv
