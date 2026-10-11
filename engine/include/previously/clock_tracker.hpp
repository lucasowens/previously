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
// keeps the loop critically damped: it converges without overshoot or ringing.
//
// Acquisition uses least-squares gains for the first pulses (pulse 2 fits a line through two
// points exactly, pulse 3 averages, and so on) until they fall below the steady gains. A tempo
// is therefore known after two pulses and settles quickly.
//
// Robustness:
//   - while acquiring, a pulse less than 0.5 ms after the first is a glitch
//   - a pulse arriving less than half a period after the previous one is a glitch (double
//     trigger) and is ignored
//   - an error of more than a quarter period is a tempo change: the tracker re-acquires from
//     this pulse, and the pulse count continues
//   - silence for two periods means the clock stopped: the tracker freewheels at the last tempo,
//     adding pulses on schedule so the grid keeps running, and locks again when pulses return
//
// Under a steady tempo ramp the tracker lags slightly; the gate default keeps that well below
// one sample (tested). See docs/learning/clock-tracking.md.

#pragma once

#include "previously/sample_time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace previously {

struct clock_tracker_config {
    sample_rate  rate{48'000U};
    std::int64_t pulses_per_beat{4};
    std::int64_t beats_per_bar{4};
    float        phase_gain{0.9F};  // alpha, in (0, 1]

    // Smoothing noise and following tempo changes pull in opposite directions: a lower phase
    // gain steadies jittery edges but lags behind tempo ramps. The presets are the measured best
    // trade-off for each source (docs/learning/clock-tracking.md has the numbers).

    /// Eurorack gate clock. Edges are clean (sub-sample interrupt jitter), so each one is
    /// trusted: lag on a 10% tempo ramp stays under a sample.
    [[nodiscard]] static constexpr clock_tracker_config
    gate(sample_rate rate, std::int64_t pulses_per_beat = 4) noexcept
    {
        return {
            .rate            = rate,
            .pulses_per_beat = pulses_per_beat,
            .beats_per_bar   = 4,
            .phase_gain      = 0.9F,
        };
    }

    /// MIDI clock: 24 PPQN with up to about 1 ms of jitter. 0.2 gives the lowest total error on
    /// a jittery tempo ramp; lower values smooth more but lag audibly.
    [[nodiscard]] static constexpr clock_tracker_config midi(sample_rate rate) noexcept
    {
        return {.rate = rate, .pulses_per_beat = 24, .beats_per_bar = 4, .phase_gain = 0.2F};
    }
};

enum class clock_state : std::uint8_t {
    no_clock,      // no pulse seen yet
    acquiring,     // one pulse seen; tempo unknown
    locked,        // tracking live pulses
    freewheeling,  // pulses stopped; continuing at the last tempo
};

class clock_tracker {
public:
    /// Pulses remembered for questions about the past: 16 bars at 24 PPQN in 4/4 is 1,536.
    static constexpr std::size_t history_capacity = 2'048;

    explicit clock_tracker(const clock_tracker_config& config) noexcept;

    /// A clock edge happened at `when`. Pulses must arrive in time order.
    void pulse(sample_time when) noexcept;

    /// The reset input fired: the pulse at (or immediately after) `when` becomes pulse 0, which
    /// is the start of bar 0.
    void reset(sample_time when) noexcept;

    /// Time has moved on to `now` (call once per audio block). Detects a stopped clock and keeps
    /// the grid running while freewheeling.
    void advance(sample_time now) noexcept;

    [[nodiscard]] clock_state state() const noexcept;

    [[nodiscard]] std::int64_t pulses_per_bar() const noexcept;

    /// Time between pulses, once known.
    [[nodiscard]] std::optional<sample_time> pulse_period() const noexcept;

    /// Time between beats, once known.
    [[nodiscard]] std::optional<sample_time> beat_period() const noexcept;

    /// Index of the most recent pulse (real or freewheeling), once there is one.
    [[nodiscard]] std::optional<std::int64_t> latest_pulse() const noexcept;

    /// When pulse `index` happened (from history) or will happen (extrapolated). Empty when the
    /// pulse is older than the history or the tempo is not yet known.
    [[nodiscard]] std::optional<sample_time> pulse_time(std::int64_t index) const noexcept;

    /// Index of the last pulse at or before `when`.
    [[nodiscard]] std::optional<std::int64_t> pulse_at_or_before(sample_time when) const noexcept;

    /// Index of the bar containing `when`.
    [[nodiscard]] std::optional<std::int64_t> bar_at_or_before(sample_time when) const noexcept;

    /// When bar `bar` starts (from history or extrapolated).
    [[nodiscard]] std::optional<sample_time> bar_start(std::int64_t bar) const noexcept;

private:
    void start_acquisition(sample_time when) noexcept;
    void reacquire(sample_time when) noexcept;
    void track(sample_time error) noexcept;
    void record(sample_time when) noexcept;

    [[nodiscard]] sample_time  latest_time() const noexcept;
    [[nodiscard]] sample_time  history_at(std::int64_t sequence) const noexcept;
    [[nodiscard]] std::int64_t oldest_sequence() const noexcept;

    // Pulses are stored by sequence number (0, 1, 2, ... in arrival order). The public pulse
    // index is sequence - index_offset_, so a reset relabels the grid without moving any data.
    std::array<sample_time, history_capacity> history_{};
    std::int64_t                              latest_sequence_{-1};
    std::int64_t                              index_offset_{0};

    sample_time latest_raw_;  // the latest pulse exactly as measured (reset matching uses this)

    sample_time  period_;
    sample_time  predicted_;
    std::int64_t pulses_in_fit_{0};  // pulses since acquisition began (least-squares gain schedule)

    sample_time reset_time_;
    bool        reset_pending_{false};

    clock_tracker_config config_;
    sample_time          min_acquisition_interval_;
    sample_time          reset_window_;
    float                steady_alpha_;
    float                steady_beta_;
    clock_state          state_{clock_state::no_clock};
};

}  // namespace previously
