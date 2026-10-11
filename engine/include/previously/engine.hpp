// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — the engine: the one entry point every target drives.
//
// The same engine runs in host tests, the render tool, the VCV Rack plugin and the firmware.
// It knows nothing about hardware: it is handed audio blocks and commands, and it fills the
// output. See DESIGN.md §10.

#pragma once

#include "previously/audio_block.hpp"
#include "previously/clock_events.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/command.hpp"
#include "previously/spsc_queue.hpp"
#include "previously/units.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace previously {

/// Fixed settings chosen once, before audio starts.
struct engine_config {
    sample_rate rate{48'000U};
    std::size_t max_block_frames{48};
};

class engine {
public:
    static constexpr std::size_t command_queue_capacity = 64;

    /// Clock history kept for questions about the past: 16 bars of 4/4 at 24 PPQN (12 KB). The
    /// gate clock needs only a sixth of it; MIDI clock needs all of it.
    static constexpr std::size_t clock_history_pulses = std::size_t{16} * 4 * 24;

    explicit engine(const engine_config& config) noexcept;

    /// Audio thread. Applies pending commands, feeds the block's clock events to the grid, then
    /// processes the audio. Events must be in time order with offsets inside the block.
    /// Realtime-safe: no allocation, no locks, bounded time.
    void process(const audio_block& block, std::span<const clock_event> events = {}) noexcept;

    /// Any single non-audio thread. Queues a command for the start of the next block.
    /// Returns false when the queue is full; the caller decides whether to retry.
    [[nodiscard]] bool post(const command& request) noexcept;

    /// Frames processed since start. Never goes backwards: every timestamp in the engine is
    /// measured against it. Audio thread only.
    [[nodiscard]] samples sample_clock() const noexcept;

    /// The clock grid as of the end of the last processed block. Audio thread only.
    [[nodiscard]] const clock_report& clock() const noexcept;

    [[nodiscard]] const engine_config& config() const noexcept;

private:
    void apply_pending_commands(sample_time block_start) noexcept;
    void apply(const command& request, sample_time block_start) noexcept;
    void report_clock(sample_time block_start, sample_time block_end) noexcept;

    spsc_queue<command, command_queue_capacity> commands_;
    samples                                     sample_clock_{0};
    engine_config                               config_;

    std::array<sample_time, clock_history_pulses> clock_history_{};
    clock_tracker                                 clock_tracker_;
    clock_report                                  clock_report_;

    // The time of the last grid pulse reported, so each pulse is reported exactly once even when
    // its estimated time moves across a block boundary (see report_clock()).
    std::optional<sample_time> last_reported_pulse_;
};

}  // namespace previously
