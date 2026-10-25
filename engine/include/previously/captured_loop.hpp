// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: a captured loop, the window of the tape a capture holds.
//
// A capture copies nothing (DESIGN.md §3.4). It holds the pages under a window of whole bars, and
// the window's bar lines say where on the tape each bar starts. The pages are held through the
// pool, so they outlive the history: the recorder gives a page back when it leaves the history,
// and the pool keeps it while a loop still holds it.
//
// A loop holds a run of consecutive pages: its window, the pre-roll before it and the read margin
// after it, and a reserve around any line at the window's ends that is not yet final. A page the
// recorder has taken is held at once. One still to come is held in the block the recorder takes
// it (catch_up()), before anything could give it back: the newest page leaves the history only
// once 46 more have started.
//
// A run's ends may move either way, but it grows at its start only from pages still in the
// history. So the pages held are always the run's first pages, in order.

#pragma once

#include "previously/page_pool.hpp"
#include "previously/recorder.hpp"
#include "previously/sample_time.hpp"
#include "previously/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace previously {

/// How much is known about a bar line.
enum class line_status : std::uint8_t {
    predicted,  // not yet recorded: where the grid expects its pulse
    edge,       // its raw edge, until the 8 pulses after it have arrived
    final,      // the bar-line estimate (bar_line.hpp), or the edge if that moved too far
};

/// One of a loop's bar lines.
struct loop_line {
    /// Its pulse, by sequence, which no reset changes (clock_tracker.hpp).
    std::int64_t sequence{0};

    /// Where the grid puts it, as far as `status` knows.
    sample_time time{sample_time::from_samples(samples{0})};

    /// Where its bar starts on the tape: `time` plus L_in, to the nearest sample, halves up.
    samples position{0};

    line_status status{line_status::predicted};
};

/// A stretch of the tape, [first, end).
struct tape_range {
    samples first{0};
    samples end{0};
};

class captured_loop {
public:
    /// The most bars a capture asks for.
    static constexpr std::size_t max_bars = 8;

    /// A capture holds at most 47 pages, in flight or final (DESIGN.md §2.3).
    static constexpr std::size_t max_pages = 47;

    /// An empty loop. The pool must outlive it.
    explicit captured_loop(page_pool& pool) noexcept;

    /// Gives back every page it holds.
    ~captured_loop();

    captured_loop(const captured_loop&)            = delete;
    captured_loop& operator=(const captured_loop&) = delete;
    captured_loop(captured_loop&&)                 = delete;
    captured_loop& operator=(captured_loop&&)      = delete;

    /// Whether it holds no window, and so no pages.
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::size_t bars() const noexcept;

    /// Its bars() + 1 lines, oldest first. The window is [front's position, back's position).
    [[nodiscard]] std::span<const loop_line> lines() const noexcept;

    /// The bar number of the first line, under the labels at the press. The rest follow on.
    [[nodiscard]] std::int64_t first_bar() const noexcept;

    /// Whether a reset had set that numbering (DESIGN.md §3.6).
    [[nodiscard]] bool numbered_by_reset() const noexcept;

    /// Whether every line is final.
    [[nodiscard]] bool is_final() const noexcept;

    /// The frames it holds, or will once they are recorded.
    [[nodiscard]] tape_range held() const noexcept;

    /// The page number of the run's first page: the page holding held().first.
    [[nodiscard]] std::int64_t first_page() const noexcept;

    /// The run's pages: those held, and those still to be recorded.
    [[nodiscard]] std::size_t page_count() const noexcept;

    /// The pages held so far, the run's first, in order.
    [[nodiscard]] std::span<const page_pool::page_id> pages() const noexcept;

    /// A new window: its lines, oldest first, from 2 to max_bars + 1 of them; the bar number of
    /// the first; and whether a reset had set the numbering. The pages are unchanged until hold().
    void assign(std::span<const loop_line> window_lines,
                std::int64_t               number_of_first,
                bool                       numbered_by_a_reset) noexcept;

    /// Replaces line `index`, as more becomes known about it.
    void set_line(std::size_t index, const loop_line& line) noexcept;

    /// Holds the pages under `range`, of at most max_pages, and gives back those outside it. Pages
    /// the recorder has taken are held now, and the rest by catch_up(). The run may start before
    /// its held pages only while the history still has the pages there.
    void hold(tape_range range, const recorder& history) noexcept;

    /// Holds the run's pages the recorder has taken since. Call after each block. Bounded: at most
    /// max_pages.
    void catch_up(const recorder& history) noexcept;

    /// Gives back every page, and holds no window.
    void clear() noexcept;

private:
    [[nodiscard]] std::int64_t page_number_of(samples position) const noexcept;

    page_pool* pool_;  // never null

    std::array<loop_line, max_bars + 1> lines_{};
    std::size_t                         bars_{0};
    std::int64_t                        first_bar_{0};
    bool                                numbered_by_reset_{false};

    tape_range                                held_;
    std::array<page_pool::page_id, max_pages> pages_{};
    std::int64_t                              first_page_{0};
    std::size_t                               page_count_{0};
    std::size_t                               pages_held_{0};
};

}  // namespace previously
