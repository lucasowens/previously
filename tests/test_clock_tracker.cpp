// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: clock tracker.
//
// Each test builds a "true" clock in double precision (tests may use double as a reference; the
// engine may not), feeds the tracker the timestamps it would really receive, and measures the
// tracker's estimates against the truth.

#include "previously/clock_tracker.hpp"

#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace previously {
namespace {

constexpr sample_rate rate_48k{48'000U};
constexpr double      samples_per_beat_120_bpm = 24'000.0;                        // 48 kHz
constexpr double      gate_period              = samples_per_beat_120_bpm / 4.0;  // 4 PPQN

/// Converts a reference time in samples to the tracker's fixed-point time.
[[nodiscard]] sample_time at(double time_in_samples)
{
    return sample_time::from_ticks(static_cast<std::int64_t>(
        std::llround(time_in_samples * static_cast<double>(sample_time::ticks_per_sample))));
}

/// Signed error of an estimate against the truth, in samples.
[[nodiscard]] double error_in_samples(sample_time estimate, double truth)
{
    return (static_cast<double>(estimate.ticks()) /
            static_cast<double>(sample_time::ticks_per_sample)) -
           truth;
}

/// The value inside an optional the test expects to be present. A missing value fails the test
/// (REQUIRE) instead of being dereferenced.
template <typename T>
[[nodiscard]] T known(const std::optional<T>& maybe)
{
    REQUIRE(maybe.has_value());
    return maybe.value_or(T{});
}

/// A sequence of true pulse times.
[[nodiscard]] std::vector<double> steady_clock(double start, double period, int count)
{
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(count));

    for (int i = 0; i < count; ++i) {
        times.push_back(start + (period * i));
    }

    return times;
}

void feed(clock_tracker& tracker, const std::vector<double>& times)
{
    for (const double time : times) {
        tracker.pulse(at(time));
    }
}

/// Largest error of the tracker's recorded pulse times, from pulse `first` to the end.
[[nodiscard]] double
max_error(const clock_tracker& tracker, const std::vector<double>& truth, std::size_t first)
{
    double worst = 0.0;

    for (std::size_t i = first; i < truth.size(); ++i) {
        const sample_time estimate = known(tracker.pulse_time(static_cast<std::int64_t>(i)));

        worst = std::max(worst, std::abs(error_in_samples(estimate, truth[i])));
    }

    return worst;
}

TEST_CASE("before any pulse there is no clock and no grid")
{
    const clock_tracker tracker{clock_tracker_config{}};

    CHECK(tracker.state() == clock_state::no_clock);
    CHECK_FALSE(tracker.pulse_period().has_value());
    CHECK_FALSE(tracker.latest_pulse().has_value());
    CHECK_FALSE(tracker.pulse_time(0).has_value());
}

TEST_CASE("one pulse starts acquisition; the second gives an exact tempo")
{
    clock_tracker tracker{clock_tracker_config{}};

    tracker.pulse(at(1'000.0));
    CHECK(tracker.state() == clock_state::acquiring);
    CHECK(tracker.latest_pulse() == 0);
    CHECK_FALSE(tracker.pulse_period().has_value());

    tracker.pulse(at(1'000.0 + gate_period));
    REQUIRE(tracker.state() == clock_state::locked);
    CHECK(tracker.pulse_period() == at(gate_period));
    CHECK(tracker.beat_period() == at(samples_per_beat_120_bpm));
}

TEST_CASE("a steady clock is tracked to within a hundredth of a sample")
{
    // A fractional period exercises the sub-sample path.
    const auto truth = steady_clock(1'000.25, 6'000.37, 1'000);

    clock_tracker tracker{clock_tracker_config{}};
    feed(tracker, truth);

    CHECK(max_error(tracker, truth, 0) < 0.01);

    // Extrapolation one bar ahead is just as accurate.
    const double next_bar = truth.back() + (6'000.37 * 16.0);
    CHECK(std::abs(error_in_samples(known(tracker.pulse_time(999 + 16)), next_bar)) < 0.01);
}

TEST_CASE("accuracy does not degrade ten days into a performance")
{
    const auto truth = steady_clock(41'472'000'000.5, 6'000.37, 500);

    clock_tracker tracker{clock_tracker_config{}};
    feed(tracker, truth);

    CHECK(max_error(tracker, truth, 0) < 0.01);
}

TEST_CASE("gate jitter is not amplified")
{
    // A filtered edge is a weighted mix of several recent noisy edges. In the RMS sense it is
    // quieter than the raw edges: the alpha-beta noise gain for alpha = 0.9 is about 0.92. Its
    // peaks can still exceed a single edge's worst case when several unlucky edges line up, so
    // the property under test is RMS, with a looser bound on the peak.
    std::mt19937                           generator{42U};
    std::uniform_real_distribution<double> jitter{-0.05, 0.05};

    const std::vector<double> truth = steady_clock(0.0, gate_period, 2'000);
    std::vector<double>       measured;
    measured.reserve(truth.size());
    for (const double time : truth) {
        measured.push_back(time + jitter(generator));
    }

    clock_tracker tracker{clock_tracker_config{}};
    feed(tracker, measured);

    double raw_squares      = 0.0;
    double filtered_squares = 0.0;

    for (std::size_t i = 10; i < truth.size(); ++i) {
        const double raw = measured[i] - truth[i];
        const double filtered =
            error_in_samples(known(tracker.pulse_time(static_cast<std::int64_t>(i))), truth[i]);

        raw_squares += raw * raw;
        filtered_squares += filtered * filtered;
    }

    CHECK(std::sqrt(filtered_squares) <= std::sqrt(raw_squares));
    CHECK(max_error(tracker, truth, 10) < 0.1);
}

/// A MIDI-clock tempo ramp, 120 -> 132 BPM over 8 bars at 24 PPQN, then steady.
[[nodiscard]] std::vector<double> midi_ramp()
{
    constexpr double period      = samples_per_beat_120_bpm / 24.0;
    constexpr int    ramp_pulses = 8 * 4 * 24;

    std::vector<double> truth{0.0};
    for (int i = 1; i < ramp_pulses + 300; ++i) {
        const double progress = std::min(1.0, static_cast<double>(i) / ramp_pulses);
        truth.push_back(truth.back() + period + (((period / 1.1) - period) * progress));
    }

    return truth;
}

/// RMS error of the tracker's pulse times against the truth, from pulse `first` on.
[[nodiscard]] double
rms_error(const clock_tracker& tracker, const std::vector<double>& truth, std::size_t first)
{
    double squares = 0.0;

    for (std::size_t i = first; i < truth.size(); ++i) {
        const double error =
            error_in_samples(known(tracker.pulse_time(static_cast<std::int64_t>(i))), truth[i]);
        squares += error * error;
    }

    return std::sqrt(squares / static_cast<double>(truth.size() - first));
}

TEST_CASE("the MIDI preset halves jitter on a jittery tempo ramp")
{
    // MIDI clock arrives with up to about a millisecond of jitter: ±48 samples.
    std::mt19937                           generator{7U};
    std::uniform_real_distribution<double> jitter{-48.0, 48.0};

    const std::vector<double> truth = midi_ramp();
    std::vector<double>       measured;
    measured.reserve(truth.size());
    double raw_squares = 0.0;

    for (const double time : truth) {
        const double offset = jitter(generator);
        measured.push_back(time + offset);
        raw_squares += offset * offset;
    }

    const double raw_rms = std::sqrt(raw_squares / static_cast<double>(truth.size()));

    clock_tracker tracker{clock_tracker_config::midi(rate_48k)};
    feed(tracker, measured);

    CHECK(rms_error(tracker, truth, 100) < raw_rms / 2.0);
}

TEST_CASE("MIDI tempo is accurate within the first beat")
{
    // While the fit has only a few points, least-squares gains average them optimally. Steady
    // gains alone would leave the first, noisy interval in the tempo for many beats.
    constexpr double period = samples_per_beat_120_bpm / 24.0;

    double worst_period_error = 0.0;

    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        std::mt19937                           generator{seed};
        std::uniform_real_distribution<double> jitter{-48.0, 48.0};

        clock_tracker tracker{clock_tracker_config::midi(rate_48k)};

        for (int i = 0; i < 24; ++i) {
            tracker.pulse(at((period * i) + jitter(generator)));
        }

        worst_period_error =
            std::max(worst_period_error,
                     std::abs(error_in_samples(known(tracker.pulse_period()), period)));
    }

    // A least-squares fit of 24 points with this jitter has a slope error of about 0.8 samples
    // (one standard deviation); without it the error is tens of samples.
    CHECK(worst_period_error < 4.0);
}

TEST_CASE("the MIDI preset follows a clean tempo ramp to within 5 samples")
{
    const std::vector<double> truth = midi_ramp();

    clock_tracker tracker{clock_tracker_config::midi(rate_48k)};
    feed(tracker, truth);

    CHECK(max_error(tracker, truth, 0) < 5.0);
}

TEST_CASE("a 10% tempo ramp over 8 bars is followed to within a sample")
{
    // 120 -> 132 BPM over 128 pulses (8 bars at 4 PPQN): the period shrinks a little every pulse.
    constexpr int    ramp_pulses  = 128;
    constexpr double start_period = gate_period;
    constexpr double end_period   = gate_period / 1.1;

    std::vector<double> truth{0.0};
    for (int i = 1; i < ramp_pulses + 64; ++i) {
        const double progress = std::min(1.0, static_cast<double>(i) / ramp_pulses);
        truth.push_back(truth.back() + start_period + ((end_period - start_period) * progress));
    }

    clock_tracker tracker{clock_tracker_config{}};
    feed(tracker, truth);

    // DESIGN.md P1 requires < 5 samples; the gate default does far better.
    CHECK(max_error(tracker, truth, 0) < 1.0);
}

TEST_CASE("a sudden tempo change re-acquires within two pulses and keeps counting")
{
    std::vector<double> truth = steady_clock(0.0, gate_period, 100);

    constexpr double slower = samples_per_beat_120_bpm / 4.0 * (120.0 / 90.0);  // 90 BPM
    for (int i = 1; i <= 50; ++i) {
        truth.push_back(truth[99] + (slower * i));
    }

    clock_tracker tracker{clock_tracker_config{}};
    feed(tracker, truth);

    CHECK(tracker.latest_pulse() == 149);
    CHECK(max_error(tracker, truth, 102) < 0.01);
    CHECK(std::abs(error_in_samples(known(tracker.pulse_period()), slower)) < 0.01);
}

TEST_CASE("a double trigger is ignored")
{
    const auto truth = steady_clock(0.0, gate_period, 100);

    clock_tracker tracker{clock_tracker_config{}};

    for (std::size_t i = 0; i < truth.size(); ++i) {
        tracker.pulse(at(truth[i]));

        if (i == 50) {
            tracker.pulse(at(truth[i] + 96.0));  // a bounce 2 ms after the real edge
        }
    }

    CHECK(tracker.latest_pulse() == 99);
    CHECK(max_error(tracker, truth, 0) < 0.01);
}

TEST_CASE("when the clock stops the grid freewheels, then locks again in phase")
{
    const auto truth = steady_clock(0.0, gate_period, 300);

    clock_tracker tracker{clock_tracker_config{}};

    // Pulses 0-99 arrive, then nothing for 100 pulses, then 200-299 arrive.
    for (int i = 0; i < 100; ++i) {
        tracker.pulse(at(truth[static_cast<std::size_t>(i)]));
    }

    tracker.advance(at(truth[100] + (gate_period * 0.5)));
    CHECK(tracker.state() == clock_state::locked);  // one late pulse is not a stopped clock yet

    for (int i = 101; i < 200; ++i) {
        tracker.advance(at(truth[static_cast<std::size_t>(i)] + 1.0));
    }
    CHECK(tracker.state() == clock_state::freewheeling);
    CHECK(tracker.latest_pulse() == 199);

    for (int i = 200; i < 300; ++i) {
        tracker.pulse(at(truth[static_cast<std::size_t>(i)]));
    }

    CHECK(tracker.state() == clock_state::locked);
    CHECK(tracker.latest_pulse() == 299);
    CHECK(max_error(tracker, truth, 0) < 0.01);
}

TEST_CASE("a reset makes the coincident pulse the start of bar 0, in either order")
{
    const auto truth = steady_clock(0.0, gate_period, 100);

    SUBCASE("reset delivered just after the pulse")
    {
        clock_tracker tracker{clock_tracker_config{}};

        for (int i = 0; i <= 37; ++i) {
            tracker.pulse(at(truth[static_cast<std::size_t>(i)]));
        }
        tracker.reset(at(truth[37]));

        CHECK(tracker.latest_pulse() == 0);
        CHECK(tracker.bar_start(0) == tracker.pulse_time(0));
        CHECK(std::abs(error_in_samples(known(tracker.bar_start(0)), truth[37])) < 0.01);
    }

    SUBCASE("reset delivered just before the pulse")
    {
        clock_tracker tracker{clock_tracker_config{}};

        for (int i = 0; i < 37; ++i) {
            tracker.pulse(at(truth[static_cast<std::size_t>(i)]));
        }
        tracker.reset(at(truth[37] - 0.5));
        tracker.pulse(at(truth[37]));

        CHECK(tracker.latest_pulse() == 0);
        CHECK(std::abs(error_in_samples(known(tracker.bar_start(0)), truth[37])) < 0.01);
    }
}

TEST_CASE("pulses before a reset belong to negative bars")
{
    // A reset partway through labels earlier pulses with negative indices. Bars must round
    // toward minus infinity: pulse -1 is the last pulse of bar -1, not part of bar 0.
    const auto truth = steady_clock(0.0, gate_period, 40);

    clock_tracker tracker{clock_tracker_config::gate(rate_48k)};

    for (int i = 0; i <= 20; ++i) {
        tracker.pulse(at(truth[static_cast<std::size_t>(i)]));
    }
    tracker.reset(at(truth[20]));

    CHECK(tracker.pulse_at_or_before(at(truth[19])) == -1);
    CHECK(tracker.bar_at_or_before(at(truth[19])) == -1);
    CHECK(tracker.bar_at_or_before(at(truth[4])) == -1);
    CHECK(tracker.bar_at_or_before(at(truth[3])) == -2);
    CHECK(std::abs(error_in_samples(known(tracker.bar_start(-1)), truth[4])) < 0.01);
}

TEST_CASE("time windows follow the sample rate")
{
    // At 96 kHz the 2 ms reset window is 192 samples. A reset 150 samples (1.6 ms) after a pulse
    // is the same moment; with a window fixed in samples for 48 kHz it would not be.
    constexpr double period_96k = 12'000.0;  // 120 BPM, 4 PPQN, 96 kHz

    clock_tracker tracker{clock_tracker_config::gate(sample_rate{96'000U})};
    feed(tracker, steady_clock(0.0, period_96k, 21));
    tracker.reset(at((period_96k * 20.0) + 150.0));

    CHECK(tracker.latest_pulse() == 0);
}

TEST_CASE("a reset matches the raw edge, not the filtered time")
{
    // With the MIDI preset a late edge is mostly smoothed away: its filtered time sits well
    // before the raw edge. A reset at the raw edge must still count as the same moment.
    constexpr double period = samples_per_beat_120_bpm / 24.0;

    std::vector<double> times = steady_clock(0.0, period, 100);
    times.back() += 80.0;  // the last edge arrives 80 samples (1.7 ms) late

    clock_tracker tracker{clock_tracker_config::midi(rate_48k)};
    feed(tracker, times);

    const double filtered_gap = error_in_samples(known(tracker.pulse_time(99)), times.back());
    REQUIRE(std::abs(filtered_gap) > 50.0);  // filtered and raw really are far apart

    tracker.reset(at(times.back() + 60.0));  // 60 samples after the raw edge, within 2 ms

    CHECK(tracker.latest_pulse() == 0);
}

TEST_CASE("past and future times map to the right pulse and bar")
{
    const auto truth = steady_clock(0.0, gate_period, 64);  // 4 bars at 4 PPQN in 4/4

    clock_tracker tracker{clock_tracker_config{}};
    feed(tracker, truth);

    // In the history.
    CHECK(tracker.pulse_at_or_before(at(truth[20])) == 20);
    CHECK(tracker.pulse_at_or_before(at(truth[20] + 1.0)) == 20);
    CHECK(tracker.pulse_at_or_before(at(truth[20] - 1.0)) == 19);
    CHECK(tracker.bar_at_or_before(at(truth[31] + 1.0)) == 1);
    CHECK(tracker.bar_at_or_before(at(truth[32] + 1.0)) == 2);

    // Before the first pulse there is no answer.
    CHECK_FALSE(tracker.pulse_at_or_before(at(-1.0)).has_value());

    // In the future, extrapolated.
    CHECK(tracker.pulse_at_or_before(at(truth[63] + (gate_period * 10.5))) == 73);
    CHECK(tracker.bar_at_or_before(at(truth[63] + (gate_period * 10.5))) == 4);
    CHECK(std::abs(error_in_samples(known(tracker.bar_start(5)), gate_period * 80.0)) < 0.01);
}

TEST_CASE("pulses older than the history are no longer answerable")
{
    const auto truth = steady_clock(0.0, gate_period, 3'000);

    clock_tracker tracker{clock_tracker_config{}};
    feed(tracker, truth);

    const std::int64_t oldest_kept =
        3'000 - static_cast<std::int64_t>(clock_tracker::history_capacity);

    CHECK(tracker.pulse_time(oldest_kept).has_value());
    CHECK_FALSE(tracker.pulse_time(oldest_kept - 1).has_value());
}

TEST_CASE("tracking never allocates")
{
    const auto truth = steady_clock(0.0, gate_period, 500);

    clock_tracker tracker{clock_tracker_config{}};

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;

        for (const double time : truth) {
            tracker.pulse(at(time));
            tracker.advance(at(time + 1.0));
            static_cast<void>(tracker.bar_at_or_before(at(time)));
        }
    }

    CHECK(test::realtime_guard::violations() == 0);
}

}  // namespace
}  // namespace previously
