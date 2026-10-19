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
#include "previously/page_pool.hpp"
#include "previously/recorder.hpp"
#include "previously/spsc_queue.hpp"
#include "previously/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
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

    /// Clock history kept for questions about the past: 16 bars of 4/4 at 24 PPQN (24 KB and 192
    /// bytes). The gate clock needs only a sixth of it; MIDI clock needs all of it.
    static constexpr std::size_t clock_history_pulses = std::size_t{16} * 4 * 24;

    /// The history is the newest 46 pages. Once they have filled, 45 are complete: at least
    /// 30.7 s (DESIGN.md §2.3).
    static constexpr std::size_t history_pages = recorder::max_history_pages;

    /// A capture holds at most 47 pages, in flight or final (DESIGN.md §2.3).
    static constexpr std::size_t max_capture_pages = 47;

    // P1 at its fullest: two slots, one of them being replaced, and one capture in flight.
    static_assert((3 * max_capture_pages) + history_pages <= page_pool::max_pages,
                  "the pool must hold P1's 187 pages");

    /// `tape` is the memory loop audio lives in: SDRAM on the Daisy, a heap block on a host. It is
    /// carved into pages of page_frames_for(config.rate) frames, of which it must hold at least
    /// history_pages, and at most page_pool::max_pages are used. It must outlive the engine, and
    /// is not touched before the first process(). Without it, nothing is recorded.
    ///
    /// A tape too small is a programming error, caught in debug builds. A release build keeps as
    /// long a history as the tape's pages allow, and records nothing on a tape under one page.
    explicit engine(const engine_config& config, std::span<stereo_frame> tape = {}) noexcept;

    /// Whether a tape of `tape_frames` holds the history at `rate`: the least a tape may hold.
    [[nodiscard]] static bool holds_history(std::size_t tape_frames, sample_rate rate) noexcept;

    /// The largest block whose grid pulses always fit in one clock_report: reports are at least
    /// half of clock_tracker::double_trigger_spacing apart. 3,072 frames at 48 kHz.
    [[nodiscard]] static std::size_t max_block_frames(sample_rate rate) noexcept;

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

    /// The recorded history: every frame of input so far, DC-blocked, at its sample clock
    /// position, for as long as its page is in the history. Audio thread only.
    [[nodiscard]] const recorder& history() const noexcept;

    [[nodiscard]] const engine_config& config() const noexcept;

private:
    void apply_pending_commands(sample_time block_start) noexcept;
    void apply(const command& request, sample_time block_start) noexcept;
    void report_clock(sample_time block_start, sample_time block_end) noexcept;

    spsc_queue<command, command_queue_capacity> commands_;
    samples                                     sample_clock_{0};
    engine_config                               config_;

    clock_history<clock_history_pulses> clock_history_{};
    clock_tracker                       clock_tracker_;  // declared after the history it uses
    clock_report                        clock_report_;

    // What report_clock() has already said, so it never says it twice (see report_clock()).
    std::optional<sample_time> last_reported_pulse_;
    sample_time                last_reported_period_;
    std::optional<sample_time> last_reported_bar_zero_;
    std::uint32_t              resets_seen_{0};

    // The tape. The pool is declared before everything that holds its pages, so it outlives them.
    page_pool pool_;
    recorder  recorder_;
};

}  // namespace previously
