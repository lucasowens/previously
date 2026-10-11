// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack adapter: feeds Rack's one-sample-at-a-time calls to the engine in blocks.
//
// Rack calls a module once per sample. The firmware's codec delivers blocks of 32 frames. To make
// the plugin behave exactly like the hardware, including where block boundaries fall, this
// adapter collects one block of input, runs the engine on it, then plays the result back during
// the next block. Like the device, that adds exactly one block of latency.
//
// Clock and reset edges are timed to a fraction of a sample as they arrive (edge_detector.hpp)
// and handed to the engine with the block. Clock out is the engine's grid: each pulse it reports
// for a block becomes a trigger at the same offset in the next block, so clock out keeps the same
// one-block latency as the audio and stays aligned with it.

#pragma once

#include "previously/clock_events.hpp"
#include "previously/engine.hpp"
#include "previously/units.hpp"

#include "edge_detector.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace previously::vcv {

/// A stereo frame, in engine units (full scale = ±1.0).
struct stereo_frame {
    float left{0.0F};
    float right{0.0F};
};

/// The control voltages for one tick.
struct control_inputs {
    float clock_volts{0.0F};
    float reset_volts{0.0F};
};

/// Everything the adapter produces in one tick.
struct tick_output {
    stereo_frame audio;
    float        clock_out_volts{0.0F};
    bool         block_finished{false};  // the engine just processed a block; its report is fresh
};

class block_adapter {
public:
    static constexpr std::size_t block_frames  = 32;
    static constexpr float       trigger_volts = 10.0F;

    /// Clock-out triggers last 1 ms, a common Eurorack trigger length.
    explicit block_adapter(sample_rate rate) noexcept
        : trigger_frames_{std::max(to_samples(seconds{0.001F}, rate).value(), std::int64_t{1})}
    {
    }

    /// Feeds one tick of input and returns this tick's output.
    [[nodiscard]] tick_output
    tick(engine& target, stereo_frame input, control_inputs controls) noexcept
    {
        in_left_[position_]  = input.left;
        in_right_[position_] = input.right;

        add_edge(clock_detector_.step(controls.clock_volts), clock_event_type::pulse);
        add_edge(reset_detector_.step(controls.reset_volts), clock_event_type::reset);

        tick_output output{
            .audio           = {.left = out_left_[position_], .right = out_right_[position_]},
            .clock_out_volts = next_clock_out_volts(),
        };

        ++position_;

        if (position_ == block_frames) {
            finish_block(target);
            output.block_finished = true;
        }

        return output;
    }

private:
    /// Two inputs, and an edge needs at least one low sample before it: at most one edge per
    /// input every two samples.
    static constexpr std::size_t max_events = block_frames;

    void add_edge(std::optional<float> edge, clock_event_type type) noexcept
    {
        if (!edge || event_count_ == max_events) {
            return;
        }

        // An edge interpolated back past the start of the block lands at its first frame: the
        // previous block has already been processed. Only mid-rise edges, which are rare, can do
        // this, and by less than one sample.
        const float offset = std::max(0.0F, static_cast<float>(position_) + *edge);

        // Events across samples arrive in order. Within one sample the clock and reset edges can
        // be out of order, so insert to keep the block sorted (at most one step back).
        std::size_t slot = event_count_;
        const auto  when = sample_time::from_samples_float(offset);

        while (slot > 0 && events_[slot - 1].offset > when) {
            events_[slot] = events_[slot - 1];
            --slot;
        }

        events_[slot] = clock_event{.type = type, .offset = when};
        ++event_count_;
    }

    [[nodiscard]] float next_clock_out_volts() noexcept
    {
        while (next_trigger_ < trigger_count_ && trigger_starts_[next_trigger_] <= position_) {
            trigger_remaining_ = trigger_frames_;
            ++next_trigger_;
        }

        if (trigger_remaining_ == 0) {
            return 0.0F;
        }

        --trigger_remaining_;
        return trigger_volts;
    }

    void finish_block(engine& target) noexcept
    {
        target.process(
            audio_block{
                .in_left   = in_left_,
                .in_right  = in_right_,
                .out_left  = out_left_,
                .out_right = out_right_,
            },
            std::span<const clock_event>{events_.data(), event_count_});

        event_count_ = 0;
        position_    = 0;

        // Schedule this block's grid pulses as triggers in the next block, at the same offsets.
        const clock_report& report = target.clock();

        trigger_count_ = std::min(report.pulse_count, trigger_starts_.size());
        next_trigger_  = 0;

        for (std::size_t i = 0; i < trigger_count_; ++i) {
            trigger_starts_[i] =
                static_cast<std::size_t>(report.pulses[i].offset.whole_samples().value());
        }
    }

    std::array<float, block_frames> in_left_{};
    std::array<float, block_frames> in_right_{};
    std::array<float, block_frames> out_left_{};
    std::array<float, block_frames> out_right_{};
    std::size_t                     position_{0};

    edge_detector                       clock_detector_;
    edge_detector                       reset_detector_;
    std::array<clock_event, max_events> events_{};
    std::size_t                         event_count_{0};

    std::array<std::size_t, clock_report::max_pulses> trigger_starts_{};
    std::size_t                                       trigger_count_{0};
    std::size_t                                       next_trigger_{0};
    std::int64_t                                      trigger_frames_;
    std::int64_t                                      trigger_remaining_{0};
};

}  // namespace previously::vcv
