// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack adapter: feeds Rack's one-sample-at-a-time calls to the engine in blocks.
//
// Rack calls a module once per sample. The firmware's codec delivers blocks of 32 frames. To make
// the plugin behave exactly like the hardware, including where block boundaries fall, this
// adapter collects one block of input, runs the engine on it, then plays the result back during
// the next block. Like the device, that adds exactly one block of latency.

#pragma once

#include "previously/engine.hpp"

#include <array>
#include <cstddef>

namespace previously::vcv {

/// A stereo frame, in engine units (full scale = ±1.0).
struct stereo_frame {
    float left{0.0F};
    float right{0.0F};
};

class block_adapter {
public:
    static constexpr std::size_t block_frames = 32;

    /// Feeds one input frame and returns the output frame for this tick.
    [[nodiscard]] stereo_frame tick(previously::engine& target, stereo_frame input) noexcept
    {
        in_left_[position_]  = input.left;
        in_right_[position_] = input.right;

        const stereo_frame output{.left = out_left_[position_], .right = out_right_[position_]};

        ++position_;

        if (position_ == block_frames) {
            target.process(audio_block{
                .in_left   = in_left_,
                .in_right  = in_right_,
                .out_left  = out_left_,
                .out_right = out_right_,
            });

            position_ = 0;
        }

        return output;
    }

private:
    std::array<float, block_frames> in_left_{};
    std::array<float, block_frames> in_right_{};
    std::array<float, block_frames> out_left_{};
    std::array<float, block_frames> out_right_{};
    std::size_t                     position_{0};
};

}  // namespace previously::vcv
