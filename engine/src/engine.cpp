// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — the engine. P0: a sample-exact passthrough with a working command path.

#include "previously/engine.hpp"

#include "previously/contract.hpp"

#include <algorithm>
#include <cstdint>

namespace previously {

engine::engine(const engine_config& config) noexcept
    : config_{config}
{
    expects(config.rate.hz() > 0U);
    expects(config.max_block_frames > 0U);
}

void engine::process(const audio_block& block) noexcept
{
    expects(block.has_matching_lengths());
    expects(block.frame_count() <= config_.max_block_frames);

    apply_pending_commands();

    std::ranges::copy(block.in_left, block.out_left.begin());
    std::ranges::copy(block.in_right, block.out_right.begin());

    sample_clock_ += samples{static_cast<std::int64_t>(block.frame_count())};
}

bool engine::post(const command& request) noexcept
{
    return commands_.try_push(request);
}

samples engine::sample_clock() const noexcept
{
    return sample_clock_;
}

const engine_config& engine::config() const noexcept
{
    return config_;
}

void engine::apply_pending_commands() noexcept
{
    // Bounded: at most one queue's worth per block, so a flood of commands can never stall audio.
    for (std::size_t i = 0; i < command_queue_capacity; ++i) {
        const auto request = commands_.try_pop();

        if (!request.has_value()) {
            return;
        }

        apply(*request);
    }
}

void engine::apply(const command& request) noexcept
{
    // A switch, even with one case: as command_type grows, -Wswitch flags any unhandled command.
    switch (request.type) {  // NOLINT(readability-trivial-switch)
        case command_type::reset_sample_clock:
            sample_clock_ = samples{0};
            break;
    }
}

}  // namespace previously
