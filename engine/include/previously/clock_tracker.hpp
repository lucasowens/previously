// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: turns clock pulse timestamps into a tempo and a beat grid.
//
// The tracker receives the time of each clock pulse (from the gate input, MIDI clock, or Rack)
// and answers two kinds of question:
//
//   past:   "when did bar 12 start?"         answered from a history of filtered pulse times
//   future: "when does the next bar start?"  answered by extrapolating the current tempo
//
// Each pulse gets an integer index (the pulse count), and bars are index / pulses_per_bar. The
// grid is therefore exact integers; only the times attached to it are estimates.
//
// Filtering: an alpha-beta tracker (the same family as the DLL that JACK uses to smooth driver
// timing). For each pulse:
//
//     error     = actual - predicted        how wrong was the prediction?
//     corrected = predicted + alpha * error  how much to believe the measurement (0..1]
//     period    = period + beta * error      how much to believe a tempo change
//     predicted = corrected + period         prediction for the next pulse
//
// Alpha comes from the config: close to 1 for clean Eurorack gates, low for jittery MIDI.
// Beta follows from alpha via the Benedict-Bordner rule, beta = alpha^2 / (2 - alpha), which
// keeps the loop critically damped: it converges without overshoot or ringing. The first pulses
// use least-squares gains, so a tempo is exact after two pulses and settles quickly.
//
// The governing rule for everything below: tempo is only ever measured between two REAL edges.
// Pulses the tracker fills in while freewheeling keep the grid counting, but never measure tempo.
//
//   double trigger   a pulse within 4 ms of the previous real pulse is ignored (contact bounce)
//   stray edge       while acquiring, an interval over 4 s means the first edge was stray: it is
//                    replaced by this one
//   outlier          a pulse more than a quarter period off its prediction is recorded where it
//                    happened, but the tempo is kept until the next pulse decides:
//   tempo change       its interval agrees with the outlier's: re-acquire from that interval
//   hiccup             it lands back on the old grid: the outlier was a one-off; nothing changes
//                    The pulse count continues either way.
//   clock stopped    2.5 periods without a pulse switches to freewheeling: the tracker fills in
//                    pulses at the last tempo so the grid keeps running. Each fill-in is committed
//                    half a period after its slot, so a late real pulse can still claim the slot.
//   clock returns    the first real pulse re-anchors the phase only (it may arrive anywhere in its
//                    slot); the next real pulse measures the tempo from two real edges
//   clock slowed     two consecutive long intervals of the same length mean the clock slowed (by
//                    2.5x or more) rather than stopped: the fill-ins are retired and the tempo is
//                    re-acquired, so the pulse count matches the real pulses
//
// Under a steady tempo ramp the tracker lags slightly; the gate preset keeps that well below one
// sample (tested). See docs/learning/clock-tracking.md.
//
// Timing contract for callers: deliver pulses in time order, and deliver each audio block's
// pulses BEFORE calling advance() for the end of that block. pulse(when) also advances to `when`
// itself, so the result never depends on how often advance() runs.

#pragma once

#include "previously/sample_time.hpp"
#include "previously/units.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace previously {

/// The defaults are the gate preset.
struct clock_tracker_config {
    sample_rate  rate{48'000U};
    std::int64_t pulses_per_beat{4};
    std::int64_t beats_per_bar{4};
    float        phase_gain{0.9F};  // alpha, in (0, 1]

    // Smoothing noise and following tempo changes pull in opposite directions: a lower phase
    // gain steadies jittery edges but lags behind tempo ramps. The presets are the measured best
    // trade-off for each source (docs/learning/clock-tracking.md has the numbers).

    /// Eurorack gate clock, 4 PPQN. Edges are clean (sub-sample interrupt jitter), so each one
    /// is trusted: lag on a 10% tempo ramp stays under a sample.
    [[nodiscard]] static constexpr clock_tracker_config gate(sample_rate rate) noexcept
    {
        return {.rate = rate};
    }

    /// MIDI clock: 24 PPQN with up to about 1 ms of jitter. 0.2 gives the lowest total error on
    /// a jittery tempo ramp; lower values smooth more but lag audibly.
    [[nodiscard]] static constexpr clock_tracker_config midi(sample_rate rate) noexcept
    {
        return {.rate = rate, .pulses_per_beat = 24, .phase_gain = 0.2F};
    }
};

enum class clock_state : std::uint8_t {
    no_clock,      // no pulse seen yet
    acquiring,     // one pulse seen; tempo unknown
    locked,        // tracking live pulses
    freewheeling,  // pulses stopped; continuing at the last tempo
};

/// A pulse on the grid: when it is (or will be), and its index as currently labelled.
struct grid_point {
    sample_time  time;
    std::int64_t index{0};
};

class clock_tracker {
public:
    /// Pulses closer together than this are double triggers. It also bounds how many pulses can
    /// fall in any stretch of time: at most one per 4 ms.
    static constexpr seconds double_trigger_spacing{0.004F};

    /// `history` holds the filtered times of past pulses. The caller owns it, so firmware can
    /// choose which memory it lives in, and it need not be initialized. Its size is how far back
    /// questions can reach: 16 bars in 4/4 is 256 pulses at 4 PPQN, 1,536 at 24 PPQN.
    clock_tracker(const clock_tracker_config& config, std::span<sample_time> history) noexcept;

    /// A clock edge happened at `when`. Pulses must arrive in time order (see the timing contract
    /// at the top of this file).
    void pulse(sample_time when) noexcept;

    /// The reset input fired at `when`: the pulse at that moment becomes pulse 0, the start of
    /// bar 0. "At that moment" means within 2 ms, delivered before or after the pulse itself. A
    /// reset older than the latest pulse relabels the first pulse at or after it in the history.
    /// While freewheeling, the next filled-in pulse can take the label: a reset on a stopped clock
    /// restarts the running grid.
    void reset(sample_time when) noexcept;

    /// Time has moved on to `now` (call once per audio block, after that block's pulses).
    /// Detects a stopped clock and keeps the grid running while freewheeling.
    void advance(sample_time now) noexcept;

    [[nodiscard]] clock_state state() const noexcept;

    [[nodiscard]] std::int64_t pulses_per_beat() const noexcept;

    [[nodiscard]] std::int64_t pulses_per_bar() const noexcept;

    /// How many past pulses can be looked up.
    [[nodiscard]] std::size_t history_capacity() const noexcept;

    /// Time between pulses, once known.
    [[nodiscard]] std::optional<sample_time> pulse_period() const noexcept;

    /// Time between beats, once known.
    [[nodiscard]] std::optional<sample_time> beat_period() const noexcept;

    /// Index of the most recently committed pulse. While freewheeling a filled-in pulse commits
    /// half a period after its slot, so this can trail the grid; for "which pulse is it now",
    /// use pulse_at_or_before(now).
    [[nodiscard]] std::optional<std::int64_t> latest_pulse() const noexcept;

    /// When pulse `index` happened (from history) or will happen (extrapolated). Empty when the
    /// pulse is older than the history, the tempo is not yet known, or the index is absurdly far
    /// from the present.
    [[nodiscard]] std::optional<sample_time> pulse_time(std::int64_t index) const noexcept;

    /// Index of the last pulse at or before `when`.
    [[nodiscard]] std::optional<std::int64_t> pulse_at_or_before(sample_time when) const noexcept;

    /// Index of the bar containing `when`.
    [[nodiscard]] std::optional<std::int64_t> bar_at_or_before(sample_time when) const noexcept;

    /// When bar `bar` starts (from history or extrapolated).
    [[nodiscard]] std::optional<sample_time> bar_start(std::int64_t bar) const noexcept;

    /// The grid pulses with `after` < time < `before`, oldest first: recorded pulses from the
    /// history, then predicted ones once the tempo is known. Writes at most out.size() pulses and
    /// returns how many it wrote.
    ///
    /// Labelling, for this and every index query: while a reset is pending, indices are as they
    /// will be once it applies. The pulse it will claim is 0 and earlier pulses count back from
    /// -1, so nothing is ever announced under a number it is about to lose.
    [[nodiscard]] std::size_t
    pulses_between(sample_time after, sample_time before, std::span<grid_point> out) const noexcept;

    /// How many times a reset has relabelled the grid. A change tells a reporter that pulses it
    /// already reported may now carry different indices.
    [[nodiscard]] std::uint32_t resets_applied() const noexcept;

private:
    void lock_on_interval(sample_time when, sample_time interval) noexcept;
    void track(sample_time error) noexcept;
    void handle_outlier(sample_time when, sample_time interval) noexcept;
    void return_from_freewheel(sample_time when, sample_time previous_raw) noexcept;
    void fill_in_closed_slots(sample_time now) noexcept;
    void record(sample_time when) noexcept;
    void apply_pending_reset(sample_time when, std::int64_t sequence) noexcept;
    void note_real_pulse(sample_time when) noexcept;
    void relabel(std::int64_t sequence_of_bar_zero) noexcept;

    [[nodiscard]] std::int64_t effective_offset() const noexcept;

    [[nodiscard]] sample_time  latest_time() const noexcept;
    [[nodiscard]] sample_time  predicted_time(std::int64_t sequence) const noexcept;
    [[nodiscard]] sample_time  history_at(std::int64_t sequence) const noexcept;
    [[nodiscard]] std::int64_t oldest_sequence() const noexcept;
    [[nodiscard]] std::optional<std::int64_t>
    sequence_at_or_before(sample_time when) const noexcept;

    // Pulses are stored by sequence number (0, 1, 2, ... in arrival order). The public pulse
    // index is sequence - index_offset_, so a reset relabels the grid without moving any data.
    std::span<sample_time> history_;
    std::int64_t           latest_sequence_{-1};
    std::int64_t           index_offset_{0};

    // The filter.
    sample_time  period_;
    sample_time  predicted_;
    std::int64_t pulses_in_fit_{0};  // real pulses in the current fit (least-squares schedule)

    // The latest real (measured, not filled-in) pulses, for tempo and reset decisions.
    std::optional<sample_time> latest_raw_;
    std::int64_t               latest_raw_sequence_{-1};
    std::int64_t               previous_raw_sequence_{-1};
    std::optional<sample_time> previous_raw_interval_;  // set only if that gap held fill-ins
    std::int64_t               fill_ins_since_raw_{0};

    // The interval before an outlier: a pulse more than a quarter period off its prediction,
    // waiting for the next pulse to tell a hiccup from a tempo change.
    std::optional<sample_time> outlier_interval_;

    std::optional<sample_time> pending_reset_;
    std::uint32_t              resets_applied_{0};

    // Settings, fixed at construction.
    std::int64_t pulses_per_beat_;
    std::int64_t pulses_per_bar_;
    float        steady_alpha_;
    float        steady_beta_;
    sample_time  double_trigger_window_;
    sample_time  max_acquisition_interval_;
    sample_time  reset_window_;

    clock_state state_{clock_state::no_clock};
};

}  // namespace previously
