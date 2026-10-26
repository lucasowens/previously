// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — the engine: the one entry point every target drives.
//
// The same engine runs in host tests, the render tool, the VCV Rack plugin and the firmware.
// It knows nothing about hardware: it is handed audio blocks and commands, and it fills the
// output. See DESIGN.md §10.

#pragma once

#include "previously/audio_block.hpp"
#include "previously/capture.hpp"
#include "previously/captured_loop.hpp"
#include "previously/clock_events.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/command.hpp"
#include "previously/loop_player.hpp"
#include "previously/page_pool.hpp"
#include "previously/recorder.hpp"
#include "previously/spsc_queue.hpp"
#include "previously/ui_model.hpp"
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

    /// L_in: how much later the input reaches the engine than the jack (DESIGN.md §3.5). Capture
    /// cuts each bar line this much later, so a window lines up with the clock edges as they
    /// arrived at the jacks. Not negative; 0 in Rack, whose blocks bring each block's audio with
    /// its own edges.
    sample_time input_latency{sample_time::from_samples(samples{0})};

    /// L_out: how much later the output reaches the jack than the engine makes it (DESIGN.md
    /// §3.5). Playback reads this much ahead, so a loop's downbeat leaves the jack on its line. Not
    /// negative; 0 in Rack.
    sample_time output_latency{sample_time::from_samples(samples{0})};
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
    static constexpr std::size_t max_capture_pages = captured_loop::max_pages;

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

    /// Whether a tape of `tape_frames` holds the history at `rate`: the least a tape with any
    /// frames may hold. An empty tape is allowed too, and records nothing.
    [[nodiscard]] static bool holds_history(std::size_t tape_frames, sample_rate rate) noexcept;

    /// The largest block whose grid pulses always fit in one clock_report: reports are at least
    /// half of clock_tracker::double_trigger_spacing apart. 3,072 frames at 48 kHz.
    [[nodiscard]] static std::size_t max_block_frames(sample_rate rate) noexcept;

    /// Audio thread. Applies pending commands, feeds the block's clock events to the grid and its
    /// capture presses to capture, then processes the audio. Events must be in time order with
    /// offsets inside the block. Capture is checked at every moment its state can change: after
    /// the clock edges at each event's moment, and as each fill-in commits. A press goes after the
    /// edges at its moment, wherever it is among them, so that it sees them; and a fill-in due at
    /// the block's end commits in the next block, after the edges at its first frame (DESIGN.md
    /// §10.1). A block of no frames changes nothing. Realtime-safe: no allocation, no locks,
    /// bounded time.
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

    /// What the display shows, as of the last block that completed a bucket of the history: 46.9
    /// times a second at 48 kHz (DESIGN.md §9.3). Until the first, nothing yet, at the engine's
    /// rate, with a serial of 0. Audio thread only: an adapter copies it for the thread that draws
    /// it.
    [[nodiscard]] const ui_model& ui() const noexcept;

    /// The loop report as of the end of the last block (DESIGN.md §3.4): the slot's state,
    /// whether a capture is waiting for its line, and what became of the latest press. Audio
    /// thread only.
    [[nodiscard]] const loop_report& loop() const noexcept;

    /// The loop in the slot: its window, its bar lines and its pages. Audio thread only.
    [[nodiscard]] const captured_loop& slot() const noexcept;

    /// A capture waiting for its end line, or an empty loop. Audio thread only.
    [[nodiscard]] const captured_loop& waiting() const noexcept;

    /// The loop that left the slot while the player fades it out, or an empty loop. Audio thread
    /// only.
    [[nodiscard]] const captured_loop& leaving() const noexcept;

    /// The pool every page is held through, to see who holds what. Audio thread only.
    [[nodiscard]] const page_pool& pages() const noexcept;

    [[nodiscard]] const engine_config& config() const noexcept;

private:
    /// Applies the queued commands at the block's start: each reset as an edge at that moment.
    /// A press waits for the moment's edges, fill-in and check: its bars go to `presses`, and the
    /// count is returned.
    [[nodiscard]] std::size_t
    apply_pending_commands(sample_time                                     block_start,
                           std::span<capture_bars, command_queue_capacity> presses) noexcept;

    /// The block's moments in time order (DESIGN.md §10.1): its start, with the commands; each
    /// event's; each fill-in's commit; and each of the player's. The output between them is
    /// rendered as it goes. Its own function, so that the frames of report_clock() and
    /// update_ui_model() stay out of the deepest call chain.
    void visit_moments(sample_time                  block_start,
                       sample_time                  block_end,
                       const audio_block&           block,
                       std::span<const clock_event> events) noexcept;

    /// The soonest moment still to come in the block, after the last visited: the next of
    /// `events`, the tracker's next fill-in, or the player's next moment.
    [[nodiscard]] std::optional<sample_time>
    soonest_moment(sample_time block_start, std::span<const clock_event> events) const noexcept;

    /// Renders and records the block's samples from `rendered` up to the sample `until`, and
    /// returns how many of the block's samples are rendered then.
    std::size_t render_segment(const audio_block& block,
                               std::int64_t       first_sample,
                               std::size_t        rendered,
                               std::int64_t       until) noexcept;

    /// Gives the tracker the edges at `when`, from `events[first]` on, and returns the index of the
    /// first event after them.
    [[nodiscard]] std::size_t apply_edges(sample_time                  when,
                                          sample_time                  block_start,
                                          sample_time                  block_end,
                                          std::span<const clock_event> events,
                                          std::size_t                  first) noexcept;

    /// A press at `when`, applied now, or held for the player's crossfade to end.
    void press_or_hold(sample_time when, capture_bars bars) noexcept;

    /// Updates the player at `when`, and applies a held press once it no longer holds presses.
    void update_player(sample_time when) noexcept;

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
    page_pool   pool_;
    recorder    recorder_;
    capture     capture_;
    loop_player player_;
    loop_report loop_report_;

    // A press made during a crossfade, waiting for it to end (DESIGN.md §5.7).
    struct held_press {
        sample_time  when;
        capture_bars bars{capture_bars::one};
    };

    std::optional<held_press> held_press_;

    ui_model ui_;
};

}  // namespace previously
