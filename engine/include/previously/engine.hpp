// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — the engine: the one entry point every target drives.
//
// The same engine runs in host tests, the render tool, the VCV Rack plugin and the firmware.
// It knows nothing about hardware: it is handed audio blocks and commands, and it fills the
// output. See DESIGN.md §10.

#pragma once

#include "previously/audio_block.hpp"
#include "previously/command.hpp"
#include "previously/spsc_queue.hpp"
#include "previously/units.hpp"

#include <cstddef>

namespace previously {

/// Fixed settings chosen once, before audio starts.
struct engine_config {
    sample_rate rate{48'000U};
    std::size_t max_block_frames{48};
};

class engine {
public:
    static constexpr std::size_t command_queue_capacity = 64;

    explicit engine(const engine_config& config) noexcept;

    /// Audio thread. Applies pending commands, then processes one block.
    /// Realtime-safe: no allocation, no locks, bounded time.
    void process(const audio_block& block) noexcept;

    /// Any single non-audio thread. Queues a command for the start of the next block.
    /// Returns false when the queue is full; the caller decides whether to retry.
    [[nodiscard]] bool post(const command& request) noexcept;

    /// Frames processed since start (or since the last reset). Audio thread only.
    [[nodiscard]] samples sample_clock() const noexcept;

    [[nodiscard]] const engine_config& config() const noexcept;

private:
    void apply_pending_commands() noexcept;
    void apply(const command& request) noexcept;

    spsc_queue<command, command_queue_capacity> commands_;
    samples                                     sample_clock_{0};
    engine_config                               config_;
};

}  // namespace previously
