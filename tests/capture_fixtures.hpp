// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: what capture's tests share. An engine fed silence, with clock edges and
// presses at chosen times, and an audit of who holds each page.

#pragma once

#include "previously/capture_bars.hpp"
#include "previously/captured_loop.hpp"
#include "previously/clock_events.hpp"
#include "previously/engine.hpp"
#include "previously/page_pool.hpp"
#include "previously/sample_time.hpp"
#include "previously/units.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace previously::test {

/// A tape of `pages` pages at `rate`: the history's 46, and room for loops beyond it.
[[nodiscard]] std::vector<stereo_frame> tape_of(std::size_t pages, sample_rate rate);

/// A whole number of samples as a time.
[[nodiscard]] constexpr sample_time at_sample(std::int64_t sample) noexcept
{
    return sample_time::from_samples(samples{sample});
}

/// An engine with a tape, fed silence block by block. Clock edges and presses are added with
/// their times, and each is delivered in its block. Events at the same time go in the order they
/// were added: a reset before or after a pulse at its moment, as added. The engine takes a
/// moment's presses after its edges, whatever their order.
class clocked_engine {
public:
    clocked_engine(const engine_config& config, std::span<stereo_frame> tape);

    void pulse(sample_time when);

    /// `count` pulses from `first`, `period` apart.
    void steady_clock(sample_time first, sample_time period, std::int64_t count);

    void reset(sample_time when);

    void press(sample_time when, capture_bars bars);

    /// Processes blocks until the sample clock reaches `end`, each event in its block.
    void run_until(samples end);

    /// Processes the block holding `when`, and every block before it.
    void run_through(sample_time when);

    [[nodiscard]] engine& target() noexcept;

private:
    struct timed_event {
        clock_event_type type{clock_event_type::pulse};
        capture_bars     bars{capture_bars::four};
        sample_time      when;
    };

    void add(const timed_event& event);

    engine                   engine_;
    std::vector<timed_event> events_;
    std::size_t              next_event_{0};
    std::vector<float>       silence_;
    std::vector<float>       out_left_;
    std::vector<float>       out_right_;
};

/// Where a loop's end line is on the tape, if it holds a window. A test reads it this way so that
/// an empty loop fails the check rather than the run.
[[nodiscard]] std::optional<std::int64_t> end_of(const captured_loop& loop);

/// Checks that every page of the engine's pool has exactly its holders: the history for each page
/// in it, and each loop for each page it holds. A page held by nobody is free. And that each loop
/// holds every page of its run the recorder has taken.
void check_holders(const engine& audio_engine);

/// Everything capture reports and holds, as text: the loop report, and each loop's lines, numbering
/// and run. Pages are given by number, not by id: which free page the recorder takes next depends
/// on when loops gave theirs back, which a block's size can change.
[[nodiscard]] std::string describe_capture(const engine& audio_engine);

/// Adds a seeded scenario to `rig`, `length` long: a gate clock that stops, slows, changes tempo
/// and jitters, resets around its pulses, and presses near its pulses and its fill-ins' commits.
/// The same seed adds the same scenario.
void add_random_scenario(clocked_engine& rig, std::uint64_t seed, samples length);

/// Runs one seeded scenario at each of `block_sizes`, and checks that capture reports and holds
/// the same at every multiple of `checkpoint`, which each size must divide (DESIGN.md §10.1).
void check_any_block_size(std::uint64_t                seed,
                          std::span<const std::size_t> block_sizes,
                          samples                      checkpoint,
                          std::int64_t                 checkpoints);

}  // namespace previously::test
