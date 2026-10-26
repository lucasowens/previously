// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the loop player, which plays the slot's loop bar-locked (DESIGN.md §3.6,
// §5.7).
//
// The loop plays at native speed from the pages it holds. Each of its bars is due at the grid line
// whose bar number matches its phrase position: at grid bar g, loop bar (g − first bar) mod N. At
// every line:
//
//   - the natural downbeat is when the playhead would reach the end of the bar it plays;
//   - the due downbeat plays on the line's nearest sample if the natural one is a whole sample or
//     more from the line, and at the natural one otherwise;
//   - if the due bar follows the playing one on the tape and plays at the natural downbeat, the
//     playhead plays on. Otherwise the downbeat is entered: over the F samples before whichever
//     comes first, the playhead crossfades into the audio before the due downbeat, timed so the
//     downbeat lands on its sample (§5.2). The wrap is always entered.
//
// Everything else that moves the playhead is a seek: a 3 ms crossfade from where it is to where it
// should be (§5.4). A loop starts with an entry into its first downbeat when there is time for one,
// and with a seek when there is not: a late press, or a line under 2 ms away.
//
// Every crossfade chooses its shape from the correlation of its two streams (crossfade_gain()).
//
// The player decides at its own moments: as each crossfade begins and ends, at each downbeat, and
// as a waiting capture's entry comes due. The engine visits them in time order with the rest, and
// renders the audio between two moments with the state the first one left, so the output is the
// same to the bit at any block size (§10.1).

#pragma once

#include "previously/capture.hpp"
#include "previously/captured_loop.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/page_pool.hpp"
#include "previously/sample_time.hpp"
#include "previously/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace previously {

/// The incoming stream's gain at `x`, from 0 to 1 through a crossfade, for two streams whose
/// correlation is `correlation`, from 0 to 1: ρ·x + (1 − ρ)·√x. Equal gain when they are
/// correlated, equal power when they are not, and a blend between, within 0.41 dB of unity power.
/// The outgoing stream's gain is the same at 1 − x. No libm curve: `std::sqrt` is correctly
/// rounded everywhere (ADR 0005).
[[nodiscard]] float crossfade_gain(float correlation, float x) noexcept;

class loop_player {
public:
    /// The pool must outlive the player. `output_latency` is L_out (DESIGN.md §3.5), not negative:
    /// each downbeat plays that much before its line, so that it leaves the jack on the line.
    loop_player(const page_pool& pool, sample_rate rate, sample_time output_latency) noexcept;

    /// At the moment `when`, once the samples before it are rendered: takes in what capture and the
    /// grid say, and decides what is due by then. Begins the entry of a waiting capture (telling
    /// capture so), seeks into a loop that took the slot without one, and gives back a leaving loop
    /// it no longer reads. Audio thread. Bounded time, no allocation.
    void update(sample_time when, capture& captured, const clock_tracker& grid) noexcept;

    /// The next sample, after the last update()'s moment, at which the player must update again.
    /// Audio thread.
    [[nodiscard]] std::optional<std::int64_t> next_moment(const capture&       captured,
                                                          const clock_tracker& grid) const noexcept;

    /// Whether presses must wait (DESIGN.md §5.7): a crossfade runs, or a capture's entry has
    /// begun and it has not taken the slot.
    [[nodiscard]] bool holds_presses(std::int64_t now) const noexcept;

    /// Writes the live input plus the loop into `out`, for the samples from `first` on. With no
    /// loop, the input passes through to the bit. Audio thread. O(frames).
    void render(std::int64_t           first,
                std::span<const float> in_left,
                std::span<const float> in_right,
                std::span<float>       out_left,
                std::span<float>       out_right) noexcept;

    /// Starts a block at `block_start`: the downbeats reported are the block's from now on.
    void begin_block(std::int64_t block_start) noexcept;

    /// Whether a loop is heard.
    [[nodiscard]] bool playing() const noexcept;

    /// The loop bar playing, from 0.
    [[nodiscard]] std::size_t bar() const noexcept;

    /// The downbeats played since begin_block(), at most loop_report::max_downbeats.
    [[nodiscard]] std::span<const loop_downbeat> downbeats() const noexcept;

private:
    /// A read head on a loop, or silence.
    struct stream {
        const captured_loop* loop{nullptr};
        std::int64_t         position{0};  // the tape position of the next frame read
    };

    /// A downbeat decided and still to come.
    struct downbeat_plan {
        std::int64_t at{0};             // the sample it plays at
        std::size_t  bar{0};            // the loop bar it starts
        std::int64_t line_sequence{0};  // the pulse that starts the line it is due at
        bool         seek{false};       // joined with a seek at the line: under 2 ms were left
    };

    /// A crossfade in progress, from `outgoing` into the playing stream.
    struct crossfade {
        stream       outgoing;
        std::int64_t start{0};
        std::int64_t length{0};
        float        correlation{1.0F};
    };

    /// When the playing loop's next downbeat is to be decided, and how.
    struct line_plan {
        std::int64_t decide_at{0};  // F before the entry's end
        std::int64_t entry_end{0};  // the earlier of the natural and the due downbeat, or the due
        std::int64_t natural{0};
        std::int64_t due{0};
        std::size_t  bar{0};
        std::int64_t line_sequence{0};
        bool         plays_on{false};
    };

    [[nodiscard]] std::int64_t                downbeat_of(sample_time line) const noexcept;
    [[nodiscard]] std::optional<line_plan>    plan_line(std::int64_t         now,
                                                        const clock_tracker& grid) const noexcept;
    [[nodiscard]] std::optional<std::int64_t> entry_due(const capture&       captured,
                                                        const clock_tracker& grid) const noexcept;
    [[nodiscard]] std::optional<std::int64_t>
    end_downbeat(const captured_loop& loop, const clock_tracker& grid) const noexcept;

    [[nodiscard]] std::int64_t next_downbeat_position() const noexcept;

    void follow_capture(std::int64_t now, capture& captured, const clock_tracker& grid) noexcept;
    void enter(std::int64_t now, capture& captured, const clock_tracker& grid) noexcept;
    void return_to_slot(std::int64_t now, const capture& captured) noexcept;
    void decide_line(sample_time when, const clock_tracker& grid) noexcept;
    void play_from(const captured_loop& loop,
                   std::int64_t         now,
                   std::int64_t         downbeat,
                   std::size_t          due_bar,
                   std::int64_t         line_sequence) noexcept;
    void begin_crossfade(std::int64_t now,
                         std::int64_t length,
                         stream       incoming,
                         float        correlation) noexcept;
    void reach_downbeat(std::int64_t now, const downbeat_plan& plan) noexcept;
    void record_downbeat(std::int64_t at, std::size_t loop_bar) noexcept;

    /// The frames a stream reads next, to the end of their page and at most `most`: a run of the
    /// loop's own, or, outside its pages, `count` frames of silence, with no frames.
    struct held_run {
        std::span<const stereo_frame> frames;
        std::int64_t                  count{0};
    };

    [[nodiscard]] held_run     run_from(const stream& from, std::int64_t most) const noexcept;
    [[nodiscard]] stereo_frame frame_at(const stream& from) const noexcept;
    [[nodiscard]] stereo_frame read(stream& from) const noexcept;
    [[nodiscard]] float        window_correlation(stream a, stream b) const noexcept;
    [[nodiscard]] float
    entry_correlation(stream outgoing, stream incoming, std::int64_t length) const noexcept;
    [[nodiscard]] bool fading(std::int64_t at) const noexcept;

    const page_pool* pool_;  // never null
    sample_time      output_latency_;
    std::int64_t     entry_frames_;   // F: 8 ms
    std::int64_t     seek_frames_;    // 3 ms
    std::int64_t     least_entry_;    // 2 ms: with less time left, a downbeat is joined by a seek
    std::int64_t     window_frames_;  // 50 ms, over which a crossfade's correlation is measured

    std::int64_t                 now_{0};      // the next sample to render
    sample_time                  updated_at_;  // the last update()'s moment
    stream                       playing_;
    std::size_t                  bar_{0};
    std::int64_t                 line_sequence_{0};  // the pulse starting the playing bar's line
    std::optional<downbeat_plan> next_;
    std::optional<crossfade>     fade_;
    bool                         entering_{false};    // playing_ is a waiting capture's loop
    std::int64_t                 entry_downbeat_{0};  // where an entered capture's first plays

    std::int64_t                                          block_start_{0};
    std::array<loop_downbeat, loop_report::max_downbeats> downbeats_{};
    std::size_t                                           downbeat_count_{0};
};

}  // namespace previously
