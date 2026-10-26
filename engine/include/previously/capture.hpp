// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: capture, which turns a press into a window of the tape, held.
//
// The input is always being recorded (recorder.hpp). A CAPTURE press asks for the last N bars,
// and capture finds them on the tape and holds their pages: nothing is copied (DESIGN.md §3.4).
// A press at time t, step by step:
//
//   1. The end line is the next bar line if it is at most a quarter beat after t (an early press),
//      and otherwise the most recent one. It is chosen at the press, once.
//   2. The window is the N bars before the end line. Each line is where the bar-line estimate puts
//      it once it is final (bar_line.hpp), at its raw edge before that, and where the grid
//      predicts it before its edge arrives; plus L_in, to the nearest sample.
//   3. Around the window: the pre-roll before it and the read margin after it, 50 ms each; and
//      half a pulse beyond a line at either end that is not yet final, since the estimate moves a
//      line by at most that much.
//   4. The capture is shortened to the most of 8, 4, 2 or 1 bars, up to N, whose window and
//      pre-roll the history held at the press, and whose frames span at most 46 pages, so they
//      touch at most 47. The history at the press is the 45 pages before the press's page, and
//      that page up to the press.
//   5. It is admitted while the history's pages, every loop's, and its own fit in the pool, as
//      though no page were shared. The recorder then always finds a free page.
//   6. It holds its pages: those recorded at once, the rest as the recorder takes them, final or
//      not, since on a fast clock its read margin can end after its 8 pulses.
//   7. It takes the slot once its end line's edge is recorded, and the loop there gives its pages
//      back. A capture that had to wait is fitted again then, from the edge, which can shorten it
//      further or refuse it.
//   8. It is final once every line is: 8 pulses after its end line. The pages it held only for
//      the reserve then go back.
//
// A new press supersedes a capture still waiting for its line, whatever becomes of the new press.
// A press whose capture takes the slot replaces the loop there, final or not.
//
// A capture's state changes only when the grid learns something: at a clock edge, and as a
// fill-in commits. The engine checks the capture at each of those moments, in time order, so a
// capture comes out the same at any block size (DESIGN.md §3.4, "Capture under change"). When the
// labels change, by a reset or by a slowed clock's fill-ins being retired, a capture still waiting
// for its line is planned again, as though pressed again at its own moment. A loop in the slot
// keeps its pulses and its numbering; a line of it whose pulse was retired keeps its edge.

#pragma once

#include "previously/capture_bars.hpp"
#include "previously/captured_loop.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/page_pool.hpp"
#include "previously/recorder.hpp"
#include "previously/sample_time.hpp"
#include "previously/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace previously {

/// What became of the latest press.
enum class capture_result : std::uint8_t {
    none,          // no press yet
    committed,     // the window holds every bar the press asked for
    shortened,     // fewer bars: the most that fit, and `limit` says what kept out more
    no_tempo,      // refused: the grid has no tempo yet
    nothing_fits,  // refused: not even one bar fits, and `limit` says why
    pool_full,     // refused: its pages do not fit in the pool beside the history and every loop
};

/// What kept a capture from more bars.
enum class capture_limit : std::uint8_t {
    none,
    history,    // the history at the press did not hold the bars and their pre-roll
    max_pages,  // the bars, their margins and their reserve would span more than 46 pages
};

struct capture_outcome {
    capture_result result{capture_result::none};
    capture_bars   asked{capture_bars::one};
    capture_bars   kept{capture_bars::one};  // the bars held, when committed or shortened
    capture_limit  limit{capture_limit::none};

    [[nodiscard]] friend constexpr bool operator==(const capture_outcome&,
                                                   const capture_outcome&) noexcept = default;
};

/// The loop in the slot.
enum class slot_state : std::uint8_t {
    empty,        // no loop yet
    provisional,  // its end line is not yet final, so the loop uses the raw end edge
    final,        // every line is final
};

/// A downbeat the loop played: its offset from the block's first frame, and its bar, from 0.
struct loop_downbeat {
    samples     offset{0};
    std::size_t bar{0};
};

/// The loop report (DESIGN.md §3.4, §5.7), as of the end of a block. Capture fills the first
/// four; the engine fills playback's from the player.
struct loop_report {
    /// A block of 3,072 frames holds at most 4 downbeats of bars of 16 ms, the shortest a 4 ms
    /// pulse makes at 4 pulses a bar.
    static constexpr std::size_t max_downbeats = 4;

    slot_state      slot{slot_state::empty};
    bool            waiting{false};     // a capture is waiting for its end line
    capture_outcome outcome{};          // the latest press's, as it stands now
    std::uint32_t   outcome_serial{0};  // changes whenever the outcome does

    bool                                     playing{false};  // a loop is heard
    std::size_t                              bar{0};          // the loop bar playing, from 0
    std::size_t                              downbeat_count{0};
    std::array<loop_downbeat, max_downbeats> downbeats{};
};

class capture {
public:
    /// The pool, the recorder and every page it holds must outlive the capture. `input_latency` is
    /// L_in (DESIGN.md §3.5), and not negative.
    capture(page_pool&      pool,
            const recorder& history,
            sample_rate     rate,
            sample_time     input_latency) noexcept;

    /// A press made at `when`, asking for `bars`, applied at `now`: later than `when` when it
    /// waited for a crossfade (DESIGN.md §5.7). Planned from `grid` as it stands then, with the
    /// history at its press as far as it is still there. It first checks, as check() does, so it
    /// sees everything the grid knows. Not while a capture's entry has begun. Audio thread.
    /// Bounded time, no allocation.
    void
    press(sample_time when, sample_time now, capture_bars bars, const clock_tracker& grid) noexcept;

    /// At `now`, a moment the capture's state can change: after the clock edges at that moment, or
    /// as a fill-in commits, with `grid` as it stands then. Keeps the edges of the slot's lines
    /// whose pulses were retired, plans a waiting capture again if the labels changed, starts it
    /// if its end line has arrived, and settles lines that are final. Audio thread. Bounded time,
    /// no allocation.
    void check(sample_time now, const clock_tracker& grid) noexcept;

    /// After each block, once it is recorded: holds the pages the recorder has taken. Audio
    /// thread. Bounded time, no allocation.
    void follow() noexcept;

    [[nodiscard]] const loop_report& report() const noexcept;

    /// The loop in the slot.
    [[nodiscard]] const captured_loop& slot() const noexcept;

    /// A capture waiting for its end line, or an empty loop.
    [[nodiscard]] const captured_loop& waiting() const noexcept;

    /// The loop that left the slot, until release_leaving(): the player may still be fading it out
    /// (DESIGN.md §5.7). Or an empty loop.
    [[nodiscard]] const captured_loop& leaving() const noexcept;

    /// The player has begun the waiting capture's entry into its first downbeat: it has started.
    /// It is no longer superseded, it takes only new numbering if planned again, and if its line
    /// would shorten or refuse it, it is refused and leaves the slot's way.
    void enter_waiting() noexcept;

    /// Whether the waiting capture's entry has begun.
    [[nodiscard]] bool waiting_entered() const noexcept;

    /// The leaving loop is no longer heard: its pages go back.
    void release_leaving() noexcept;

private:
    /// What is known of a line: where it is, and how sure that is.
    struct line_fix {
        std::int64_t sequence{0};
        sample_time  time;
        line_status  status{line_status::predicted};
    };

    using line_fixes = std::array<std::optional<line_fix>, captured_loop::max_bars + 1>;

    /// A window of some bars, and the frames it would hold.
    struct window_plan {
        std::array<loop_line, captured_loop::max_bars + 1> lines{};
        capture_bars                                       bars{capture_bars::one};
        tape_range                                         held{};
    };

    /// The most bars that fit, or why none did.
    struct fitting {
        std::optional<window_plan> plan{std::nullopt};
        capture_limit              limit{capture_limit::none};  // what kept out more bars, or all
    };

    /// The labels the grid gives its pulses: which pulse is bar 0's, and how many retirements have
    /// rewound the sequences. Whether a reset set them changes only with bar 0's pulse: no capture
    /// is planned before a second pulse gives a tempo, so the first reset moves bar 0 off the
    /// clock's first pulse.
    struct labelling {
        std::int64_t  bar_zero{0};
        std::uint32_t retirements{0};

        [[nodiscard]] friend constexpr bool operator==(const labelling&,
                                                       const labelling&) noexcept = default;
    };

    [[nodiscard]] static labelling labels_of(const clock_tracker& grid) noexcept;

    [[nodiscard]] static std::optional<line_fix> locate(const clock_tracker& grid,
                                                        std::int64_t         sequence) noexcept;

    /// A press's end line: the next bar line at most a quarter beat away, or the latest.
    struct end_line {
        std::int64_t bar{0};
        std::int64_t sequence{0};
    };

    [[nodiscard]] static std::optional<end_line> end_line_of(sample_time          when,
                                                             const clock_tracker& grid) noexcept;

    [[nodiscard]] capture_outcome plan(sample_time          when,
                                       capture_bars         bars,
                                       samples              history_begin,
                                       const clock_tracker& grid) noexcept;

    [[nodiscard]] fitting fit(const line_fixes& fixes,
                              capture_bars      most,
                              samples           history_begin,
                              sample_time       reserve) const noexcept;

    [[nodiscard]] std::optional<window_plan>
    plan_window(const line_fixes& fixes, capture_bars bars, sample_time reserve) const noexcept;

    [[nodiscard]] capture_limit limit_of(const window_plan& window,
                                         samples            history_begin) const noexcept;

    [[nodiscard]] samples history_begin_at(sample_time when) const noexcept;
    [[nodiscard]] bool    fits_the_pool(tape_range range) const noexcept;
    [[nodiscard]] samples position_of(sample_time time) const noexcept;

    void plan_again(sample_time now, const clock_tracker& grid) noexcept;
    void arrive(const clock_tracker& grid) noexcept;
    void keep_retired(captured_loop& loop, const clock_tracker& grid) noexcept;
    void settle(captured_loop& loop, const clock_tracker& grid) noexcept;
    void finish(captured_loop& loop) noexcept;
    void take(captured_loop&     loop,
              const window_plan& window,
              std::int64_t       end_bar,
              bool               numbered_by_reset) noexcept;
    void take_slot() noexcept;
    void leave_waiting() noexcept;
    void conclude(const capture_outcome& outcome) noexcept;
    void report_states() noexcept;

    [[nodiscard]] captured_loop& slot_loop() noexcept;
    [[nodiscard]] captured_loop& waiting_loop() noexcept;
    [[nodiscard]] captured_loop& leaving_loop() noexcept;

    page_pool*      pool_;     // never null
    const recorder* history_;  // never null
    samples         margin_;   // the pre-roll and the read margin: 50 ms, rounded up
    sample_time     input_latency_;

    // The slot's loop, a waiting capture's, and one leaving the slot, one of each: a capture takes
    // the slot by moving the indices round.
    std::array<captured_loop, 3> loops_;
    std::size_t                  slot_index_{0};
    std::size_t                  waiting_index_{1};
    std::size_t                  leaving_index_{2};

    // The waiting capture's press, to plan it again when the labels change and to fit it again
    // when its end line arrives: its moment and what it asked for; and as planned, what it holds,
    // the history it was planned from, its end line's bar, half a pulse then (its reserve), and
    // the labels it was planned under.
    sample_time  waiting_pressed_;
    capture_bars waiting_asked_{capture_bars::one};
    capture_bars waiting_kept_{capture_bars::one};
    samples      waiting_history_begin_{0};
    std::int64_t waiting_end_bar_{0};
    sample_time  waiting_reserve_;
    labelling    waiting_labels_;
    bool         waiting_entered_{false};

    loop_report report_;
};

}  // namespace previously
