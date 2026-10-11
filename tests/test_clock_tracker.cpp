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
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <vector>

namespace previously {
namespace {

constexpr sample_rate rate_48k{48'000U};
constexpr double      samples_per_beat_120_bpm = 24'000.0;                         // 48 kHz
constexpr double      gate_period              = samples_per_beat_120_bpm / 4.0;   // 4 PPQN
constexpr double      midi_period              = samples_per_beat_120_bpm / 24.0;  // 24 PPQN

/// A tracker together with the history storage its owner provides. Not copyable: the tracker
/// refers to this object's own history.
struct tracked_clock {
    explicit tracked_clock(const clock_tracker_config& config)
        : tracker{config, history}
    {
    }

    tracked_clock(const tracked_clock&)            = delete;
    tracked_clock& operator=(const tracked_clock&) = delete;
    tracked_clock(tracked_clock&&)                 = delete;
    tracked_clock& operator=(tracked_clock&&)      = delete;
    ~tracked_clock()                               = default;

    std::array<sample_time, 2'048> history{};
    clock_tracker                  tracker;  // declared after history, which it refers to
};

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

/// True pulse times of a steady clock.
[[nodiscard]] std::vector<double> steady_clock(double start, double period, std::size_t count)
{
    std::vector<double> times;
    times.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        times.push_back(start + (period * static_cast<double>(i)));
    }

    return times;
}

/// True pulse times of a clock that ramps linearly from one period to another over
/// `ramp_pulses`, then holds the end period for `tail_pulses`.
[[nodiscard]] std::vector<double>
tempo_ramp(double start_period, double end_period, int ramp_pulses, int tail_pulses)
{
    std::vector<double> times{0.0};
    times.reserve(static_cast<std::size_t>(ramp_pulses) + static_cast<std::size_t>(tail_pulses));

    for (int i = 1; i < ramp_pulses + tail_pulses; ++i) {
        const double progress = std::min(1.0, static_cast<double>(i) / ramp_pulses);
        times.push_back(times.back() + start_period + ((end_period - start_period) * progress));
    }

    return times;
}

/// Adds uniform jitter of ±`amount` samples to every time.
[[nodiscard]] std::vector<double>
with_jitter(const std::vector<double>& times, double amount, std::uint32_t seed)
{
    std::mt19937                           generator{seed};
    std::uniform_real_distribution<double> jitter{-amount, amount};

    std::vector<double> measured;
    measured.reserve(times.size());

    for (const double time : times) {
        measured.push_back(time + jitter(generator));
    }

    return measured;
}

void feed(clock_tracker& tracker, std::span<const double> times)
{
    for (const double time : times) {
        tracker.pulse(at(time));
    }
}

/// Largest error of the tracker's recorded pulse times, from pulse `first` to the end.
[[nodiscard]] double
max_error(const clock_tracker& tracker, std::span<const double> truth, std::size_t first)
{
    double worst = 0.0;

    for (std::size_t i = first; i < truth.size(); ++i) {
        worst = std::max(
            worst,
            std::abs(error_in_samples(known(tracker.pulse_time(static_cast<std::int64_t>(i))),
                                      truth[i])));
    }

    return worst;
}

/// RMS error of the tracker's pulse times against the truth, from pulse `first` on.
[[nodiscard]] double
rms_error(const clock_tracker& tracker, std::span<const double> truth, std::size_t first)
{
    double squares = 0.0;

    for (std::size_t i = first; i < truth.size(); ++i) {
        const double error =
            error_in_samples(known(tracker.pulse_time(static_cast<std::int64_t>(i))), truth[i]);
        squares += error * error;
    }

    return std::sqrt(squares / static_cast<double>(truth.size() - first));
}

/// RMS difference between measured and true times: the raw jitter.
[[nodiscard]] double rms_difference(std::span<const double> measured, std::span<const double> truth)
{
    double squares = 0.0;

    for (std::size_t i = 0; i < truth.size(); ++i) {
        squares += (measured[i] - truth[i]) * (measured[i] - truth[i]);
    }

    return std::sqrt(squares / static_cast<double>(truth.size()));
}

/// Feeds a steady clock, then `count` pulses at `new_period`, calling advance() every 256
/// samples as an audio engine would. Returns the time of the last pulse.
double change_tempo(clock_tracker& tracker, std::size_t before, double new_period, int count)
{
    const auto steady = steady_clock(0.0, gate_period, before);
    feed(tracker, steady);

    double time = steady.back();

    for (int i = 0; i < count; ++i) {
        const double next = time + new_period;

        // One advance() per 256-sample audio block between the pulses.
        for (int block = 1; time + (256.0 * block) < next; ++block) {
            tracker.advance(at(time + (256.0 * block)));
        }

        tracker.pulse(at(next));
        time = next;
    }

    return time;
}

// ---- Acquisition ------------------------------------------------------------------------------

TEST_CASE("before any pulse there is no clock and no grid")
{
    const tracked_clock clock{clock_tracker_config::gate(rate_48k)};

    CHECK(clock.tracker.state() == clock_state::no_clock);
    CHECK_FALSE(clock.tracker.pulse_period().has_value());
    CHECK_FALSE(clock.tracker.latest_pulse().has_value());
    CHECK_FALSE(clock.tracker.pulse_time(0).has_value());
}

TEST_CASE("one pulse starts acquisition; the second gives an exact tempo")
{
    tracked_clock clock{clock_tracker_config::gate(rate_48k)};

    clock.tracker.pulse(at(1'000.0));
    CHECK(clock.tracker.state() == clock_state::acquiring);
    CHECK(clock.tracker.latest_pulse() == 0);
    CHECK_FALSE(clock.tracker.pulse_period().has_value());

    clock.tracker.pulse(at(1'000.0 + gate_period));
    REQUIRE(clock.tracker.state() == clock_state::locked);
    CHECK(clock.tracker.pulse_period() == at(gate_period));
    CHECK(clock.tracker.beat_period() == at(samples_per_beat_120_bpm));
}

TEST_CASE("a stray edge before the clock starts does not deafen the tracker")
{
    // One edge (a cable being patched), then a steady clock some seconds later. Every real pulse
    // must be counted, and the tempo must be right after two real pulses.
    for (const double gap_seconds : {2.0, 5.0, 60.0}) {
        CAPTURE(gap_seconds);

        tracked_clock clock{clock_tracker_config::gate(rate_48k)};
        clock.tracker.pulse(at(0.0));

        const auto real = steady_clock(gap_seconds * 48'000.0, gate_period, 400);
        feed(clock.tracker, std::span{real}.first(2));
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()), gate_period)) < 0.01);

        feed(clock.tracker, std::span{real}.subspan(2));
        const std::int64_t stray_counted = gap_seconds > 4.0 ? 0 : 1;  // over 4 s it is replaced
        CHECK(clock.tracker.latest_pulse() == 399 + stray_counted);
    }
}

TEST_CASE("MIDI tempo is accurate within the first beat")
{
    // While the fit has only a few points, least-squares gains average them optimally. Steady
    // gains alone leave the first, noisy interval in the tempo for much of the beat. The
    // difference is largest early on (measured over 200 seeds: after 9 pulses the worst error is
    // 8.4 samples with least squares, against a mean of 18 without), so check there as well as
    // at the end of the beat.
    double worst_after_9  = 0.0;
    double worst_after_24 = 0.0;

    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
        const auto measured = with_jitter(steady_clock(0.0, midi_period, 24), 48.0, seed);

        tracked_clock clock{clock_tracker_config::midi(rate_48k)};

        feed(clock.tracker, std::span{measured}.first(9));
        worst_after_9 =
            std::max(worst_after_9,
                     std::abs(error_in_samples(known(clock.tracker.pulse_period()), midi_period)));

        feed(clock.tracker, std::span{measured}.subspan(9));
        worst_after_24 =
            std::max(worst_after_24,
                     std::abs(error_in_samples(known(clock.tracker.pulse_period()), midi_period)));
    }

    // One standard deviation of a least-squares slope with this jitter: about 3.6 samples for 9
    // points, 0.8 for 24.
    CHECK(worst_after_9 < 12.0);
    CHECK(worst_after_24 < 4.0);
}

// ---- Steady tracking and noise ------------------------------------------------------------------

TEST_CASE("a steady clock is tracked to within a hundredth of a sample")
{
    // A fractional period exercises the sub-sample path.
    const auto truth = steady_clock(1'000.25, 6'000.37, 1'000);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, truth);

    CHECK(max_error(clock.tracker, truth, 0) < 0.01);

    // Extrapolation one bar ahead is just as accurate.
    const double next_bar = truth.back() + (6'000.37 * 16.0);
    CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_time(999 + 16)), next_bar)) < 0.01);
}

TEST_CASE("accuracy does not degrade ten days into a performance")
{
    const auto truth = steady_clock(41'472'000'000.5, 6'000.37, 500);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, truth);

    CHECK(max_error(clock.tracker, truth, 0) < 0.01);
}

TEST_CASE("gate jitter is not amplified")
{
    // A filtered edge is a weighted mix of several recent noisy edges. In the RMS sense it is
    // quieter than the raw edges: the alpha-beta noise gain for alpha = 0.9 is about 0.92. Its
    // peaks can still exceed a single edge's worst case when several unlucky edges line up, so
    // the property under test is RMS, with a looser bound on the peak.
    const auto truth    = steady_clock(0.0, gate_period, 2'000);
    const auto measured = with_jitter(truth, 0.05, 42U);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, measured);

    CHECK(rms_error(clock.tracker, truth, 10) <= rms_difference(measured, truth));
    CHECK(max_error(clock.tracker, truth, 10) < 0.1);
}

TEST_CASE("the MIDI preset halves jitter on a jittery tempo ramp")
{
    // 120 -> 132 BPM over 8 bars at 24 PPQN, with up to a millisecond of jitter (±48 samples).
    const auto truth    = tempo_ramp(midi_period, midi_period / 1.1, 8 * 4 * 24, 300);
    const auto measured = with_jitter(truth, 48.0, 7U);

    tracked_clock clock{clock_tracker_config::midi(rate_48k)};
    feed(clock.tracker, measured);

    CHECK(rms_error(clock.tracker, truth, 100) < rms_difference(measured, truth) / 2.0);
}

// ---- Tempo changes
// --------------------------------------------------------------------------------

TEST_CASE("a 10% tempo ramp over 8 bars is followed to within a sample")
{
    const auto truth = tempo_ramp(gate_period, gate_period / 1.1, 128, 64);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, truth);

    // DESIGN.md P1 requires < 5 samples; the gate preset does far better.
    CHECK(max_error(clock.tracker, truth, 0) < 1.0);
}

TEST_CASE("the MIDI preset follows a clean tempo ramp to within 5 samples")
{
    const auto truth = tempo_ramp(midi_period, midi_period / 1.1, 8 * 4 * 24, 300);

    tracked_clock clock{clock_tracker_config::midi(rate_48k)};
    feed(clock.tracker, truth);

    CHECK(max_error(clock.tracker, truth, 0) < 5.0);
}

TEST_CASE("a sudden tempo change re-acquires within two pulses and keeps counting")
{
    constexpr double slower = gate_period * (120.0 / 90.0);  // 90 BPM

    std::vector<double> truth = steady_clock(0.0, gate_period, 100);
    for (int i = 1; i <= 50; ++i) {
        truth.push_back(truth[99] + (slower * i));
    }

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, truth);

    CHECK(clock.tracker.latest_pulse() == 149);
    CHECK(max_error(clock.tracker, truth, 102) < 0.01);
    CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()), slower)) < 0.01);
}

TEST_CASE("slower clocks are counted pulse for pulse, however much slower")
{
    // Dividers and big tempo drops. Every real pulse is counted once (no phantom fill-ins
    // survive), and the tempo is measured from real edges. Exact halving is tested with jitter on
    // both sides of 2P, where a stop-detection threshold at 2P used to decide the outcome.
    for (const double factor : {2.0 - 0.000'005, 2.0 + 0.000'005, 2.6, 3.0, 4.0}) {
        CAPTURE(factor);

        tracked_clock clock{clock_tracker_config::gate(rate_48k)};
        change_tempo(clock.tracker, 100, gate_period * factor, 12);

        CHECK(clock.tracker.latest_pulse() == 99 + 12);
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()),
                                        gate_period * factor)) < 0.01);
    }
}

TEST_CASE("a tempo halving is counted exactly and confirmed by its second pulse")
{
    // The first pulse at the new tempo arrives about two periods after the last, on either side
    // of exactly 2P. It must never read as a stopped clock with a filled-in phantom pulse. One
    // pulse can't distinguish a slowdown from a hiccup, so the tempo is kept until the second
    // pulse agrees (issue #3); then the tempo changes.
    for (const double factor : {2.0 - 0.000'005, 2.0 + 0.000'005, 2.2}) {
        CAPTURE(factor);

        tracked_clock clock{clock_tracker_config::gate(rate_48k)};
        const double  first = change_tempo(clock.tracker, 100, gate_period * factor, 1);

        CHECK(clock.tracker.latest_pulse() == 100);
        CHECK(clock.tracker.state() == clock_state::locked);
        CHECK(clock.tracker.pulse_period() == at(gate_period));
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_time(100)), first)) < 0.01);

        clock.tracker.pulse(at(first + (gate_period * factor)));

        CHECK(clock.tracker.latest_pulse() == 101);
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()),
                                        gate_period * factor)) < 0.01);
    }
}

TEST_CASE("a single late or early pulse in a steady clock changes nothing (issue #3)")
{
    // One pulse off by 0.3 of a period (a jitter spike, a MIDI-to-CV hiccup), then back on the
    // grid. The tempo must not change, the odd pulse is recorded where it really happened, and
    // the next pulse is tracked exactly.
    for (const double offset : {0.3, -0.3}) {
        CAPTURE(offset);

        std::vector<double> times = steady_clock(0.0, gate_period, 60);
        times[40] += gate_period * offset;

        tracked_clock clock{clock_tracker_config::gate(rate_48k)};
        feed(clock.tracker, std::span{times}.first(41));

        CHECK(clock.tracker.pulse_period() == at(gate_period));
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_time(40)), times[40])) < 0.01);

        // A time just after the odd pulse belongs to it, even though the next prediction is more
        // than a period away after an early pulse.
        CHECK(clock.tracker.pulse_at_or_before(at(times[40] + 1.0)) == 40);

        // The grid keeps its phase through the hiccup: pulse 41 is still predicted on the old
        // grid (which matters if the clock stops right here and the grid freewheels).
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_time(41)), gate_period * 41.0)) <
              0.01);

        feed(clock.tracker, std::span{times}.subspan(41));

        CHECK(clock.tracker.latest_pulse() == 59);
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()), gate_period)) < 0.01);
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_time(41)), times[41])) < 0.01);
        CHECK(max_error(clock.tracker, times, 41) < 0.01);
    }
}

TEST_CASE("a hiccup one pulse after a confirmed tempo change is still a hiccup")
{
    // A halving is confirmed at its second slow pulse. Three real edges then agree on the new
    // tempo, so the very next pulse already has hiccup immunity.
    // Pulses 0-19 at P, the first slow pulse at 21P, confirmed at 23P; 25.6P is a 0.3 hiccup at
    // the new tempo.
    std::vector<double> times = steady_clock(0.0, gate_period, 20);
    for (const double at_periods : {21.0, 23.0, 25.6, 27.0, 29.0, 31.0}) {
        times.push_back(gate_period * at_periods);
    }

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, std::span{times}.first(22));  // through the confirming pulse at 23P
    REQUIRE(std::abs(error_in_samples(known(clock.tracker.pulse_period()), gate_period * 2.0)) <
            0.01);

    for (std::size_t i = 22; i < times.size(); ++i) {
        clock.tracker.pulse(at(times[i]));

        CAPTURE(i);
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()), gate_period * 2.0)) <
              0.01);
    }

    CHECK(clock.tracker.latest_pulse() == static_cast<std::int64_t>(times.size()) - 1);
}

TEST_CASE("a phase shift at constant tempo re-anchors within two pulses")
{
    // From pulse 20 on, every pulse is shifted by d: the clock carried on at the same tempo from
    // a new phase. Pulse 20 is an outlier; pulse 21's interval matches the current period, so
    // the phase re-anchors. The tempo never changes, and the grid is right from pulse 21.
    for (const double shift : {0.3, 0.4, 0.6, -0.3, -0.4, -0.6}) {
        CAPTURE(shift);

        std::vector<double> times = steady_clock(0.0, gate_period, 40);
        for (std::size_t i = 20; i < times.size(); ++i) {
            times[i] += gate_period * shift;
        }

        tracked_clock clock{clock_tracker_config::gate(rate_48k)};

        for (std::size_t i = 0; i < times.size(); ++i) {
            clock.tracker.pulse(at(times[i]));

            if (i >= 2) {
                CAPTURE(i);
                CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()), gate_period)) <
                      0.01);
            }

            // From the second shifted pulse on, the next pulse is predicted on the new phase:
            // clock out follows the shift after two pulses, not three.
            if (i >= 21 && i + 1 < times.size()) {
                CAPTURE(i);
                const auto next = static_cast<std::int64_t>(i) + 1;
                CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_time(next)),
                                                times[i + 1])) < 0.01);
            }
        }

        CHECK(clock.tracker.latest_pulse() == 39);
        CHECK(max_error(clock.tracker, times, 21) < 0.01);
    }
}

/// Feeds pulses with advance() every 256 samples between them, as an audio engine would.
void feed_with_blocks(clock_tracker& tracker, std::span<const double> times)
{
    std::int64_t block = 0;

    for (const double time : times) {
        for (; static_cast<double>(block + 1) * 256.0 < time; ++block) {
            tracker.advance(at(static_cast<double>(block + 1) * 256.0));
        }
        tracker.pulse(at(time));
    }
}

TEST_CASE("a halving whose confirming pulse is late gains two pulses (pinned limitation)")
{
    // The tempo halves (first slow pulse at 21P), then the confirming pulse arrives 0.6 of a
    // new period late (23.6P instead of 23P). The intervals (2P, 2.6P) don't agree, so neither
    // a slowdown nor a stop can be told apart from the input: the gap reads as a stopped clock,
    // two pulses are filled in, and nothing retires them. The count stays two ahead until the
    // next reset. Pinned as a known limitation: judging "stopped" against the outlier's longer
    // interval would avoid it, but would misnumber the far commoner case of a hiccup just
    // before a short stop (see the next test).
    std::vector<double> times = steady_clock(0.0, gate_period, 20);  // pulses 0-19
    for (const double at_periods : {21.0, 23.6, 25.0, 27.0, 29.0, 31.0, 33.0, 35.0}) {
        times.push_back(gate_period * at_periods);
    }

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed_with_blocks(clock.tracker, times);

    CHECK(clock.tracker.latest_pulse() == static_cast<std::int64_t>(times.size()) - 1 + 2);
    CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()), gate_period * 2.0)) <
          0.01);
}

TEST_CASE("a hiccup just before a short stop does not change how the return is numbered")
{
    // Pulse 20 arrives late, then the clock stops and returns on the grid. The returning pulse
    // must get the same number as it would without the hiccup: the stop is filled in exactly as
    // usual.
    for (const double pulse_20 : {20.3, 20.6}) {
        for (const double back_at : {23.4, 24.0, 25.0, 26.0, 27.0}) {
            CAPTURE(pulse_20);
            CAPTURE(back_at);

            std::vector<double> times = steady_clock(0.0, gate_period, 20);
            times.push_back(gate_period * pulse_20);
            times.push_back(gate_period * back_at);

            std::vector<double> clean = steady_clock(0.0, gate_period, 21);
            clean.push_back(gate_period * back_at);

            tracked_clock with_hiccup{clock_tracker_config::gate(rate_48k)};
            tracked_clock without{clock_tracker_config::gate(rate_48k)};
            feed_with_blocks(with_hiccup.tracker, times);
            feed_with_blocks(without.tracker, clean);

            CHECK(with_hiccup.tracker.latest_pulse() == without.tracker.latest_pulse());
        }
    }
}

TEST_CASE("extrapolating far ahead never overflows")
{
    // The safe distance depends on the period: about 2^34 pulses at 4 PPQN, 120 BPM. Built with
    // UBSan, an overflow here would abort the test.
    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, steady_clock(0.0, gate_period, 10));

    constexpr std::int64_t two_to_the_20 = 1'048'576;
    constexpr std::int64_t two_to_the_35 = 34'359'738'368;
    constexpr std::int64_t two_to_the_39 = 549'755'813'888;

    CHECK_FALSE(clock.tracker.pulse_time(9 + two_to_the_35).has_value());
    CHECK_FALSE(clock.tracker.pulse_time(9 + two_to_the_39).has_value());
    CHECK(clock.tracker.pulse_time(9 + two_to_the_20).has_value());
}

TEST_CASE("two identical hiccups far apart are still hiccups")
{
    // Each hiccup is judged on its own: the interval remembered from the first must be forgotten
    // once the clock is back on the grid, or the second would "agree" with it and look like a
    // tempo change.
    std::vector<double> times = steady_clock(0.0, gate_period, 80);
    times[30] += gate_period * 0.3;
    times[60] += gate_period * 0.3;

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};

    // The tempo must hold at every pulse, not just come back by the end.
    for (std::size_t i = 0; i < times.size(); ++i) {
        clock.tracker.pulse(at(times[i]));

        if (i >= 2) {
            CAPTURE(i);
            CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()), gate_period)) <
                  0.01);
        }
    }

    CHECK(clock.tracker.latest_pulse() == 79);
    CHECK(max_error(clock.tracker, times, 61) < 0.01);
}

TEST_CASE("faster clocks are counted pulse for pulse, including exact doubling")
{
    // A doubled-tempo pulse that lands a hair early used to be swallowed as a double trigger.
    for (const double factor : {0.5 - 0.000'005, 0.5 + 0.000'005, 0.4}) {
        CAPTURE(factor);

        tracked_clock clock{clock_tracker_config::gate(rate_48k)};
        change_tempo(clock.tracker, 100, gate_period * factor, 12);

        CHECK(clock.tracker.latest_pulse() == 99 + 12);
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()),
                                        gate_period * factor)) < 0.01);
    }
}

TEST_CASE("a double trigger is ignored")
{
    const auto truth = steady_clock(0.0, gate_period, 100);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};

    for (std::size_t i = 0; i < truth.size(); ++i) {
        clock.tracker.pulse(at(truth[i]));

        if (i == 50) {
            clock.tracker.pulse(at(truth[i] + 96.0));  // a bounce 2 ms after the real edge
        }
    }

    CHECK(clock.tracker.latest_pulse() == 99);
    CHECK(max_error(clock.tracker, truth, 0) < 0.01);
}

// ---- Clock stopping and returning
// ----------------------------------------------------------------

TEST_CASE("when the clock stops the grid freewheels; when it returns the tempo stays exact")
{
    const auto truth = steady_clock(0.0, gate_period, 300);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, std::span{truth}.first(100));

    // Silence through pulse 199, with advance() once per pulse slot.
    clock.tracker.advance(at(truth[100] + (gate_period * 0.5)));
    CHECK(clock.tracker.state() == clock_state::locked);  // one late pulse is not a stopped clock

    for (std::size_t i = 101; i < 200; ++i) {
        clock.tracker.advance(at(truth[i] + 1.0));
    }
    CHECK(clock.tracker.state() == clock_state::freewheeling);
    CHECK(clock.tracker.latest_pulse() == 198);  // slot 199 stays open until half a period after it

    SUBCASE("returning on the grid")
    {
        feed(clock.tracker, std::span{truth}.subspan(200));

        CHECK(clock.tracker.state() == clock_state::locked);
        CHECK(clock.tracker.latest_pulse() == 299);
        CHECK(max_error(clock.tracker, truth, 0) < 0.01);
    }

    SUBCASE("returning a few samples late keeps the real timing")
    {
        clock.tracker.advance(at(truth[200] + 1.0));
        clock.tracker.pulse(at(truth[200] + 3.0));

        CHECK(clock.tracker.state() == clock_state::locked);
        CHECK(clock.tracker.latest_pulse() == 200);
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_time(200)), truth[200] + 3.0)) <
              0.5);
    }

    SUBCASE("returning at a new phase does not disturb the tempo")
    {
        // A restarted clock arrives anywhere in its slot. That offset is phase, not tempo: the
        // period must stay exact, and the following pulses must be tracked exactly.
        for (const double phase : {0.1, 0.3, 0.4, -0.4}) {
            CAPTURE(phase);

            tracked_clock restarted{clock_tracker_config::gate(rate_48k)};
            feed(restarted.tracker, std::span{truth}.first(100));

            const auto returning =
                steady_clock(truth[200] + (gate_period * phase), gate_period, 20);

            for (std::size_t i = 101; i < 200; ++i) {
                restarted.tracker.advance(at(truth[i] + 1.0));
            }

            // The first returning pulse re-anchors the phase; the tempo must not move at all.
            feed(restarted.tracker, std::span{returning}.first(1));
            CHECK(std::abs(error_in_samples(known(restarted.tracker.pulse_period()), gate_period)) <
                  0.01);

            // The second measures the tempo from two real edges: still exact.
            feed(restarted.tracker, std::span{returning}.subspan(1, 1));
            CHECK(std::abs(error_in_samples(known(restarted.tracker.pulse_period()), gate_period)) <
                  0.01);

            feed(restarted.tracker, std::span{returning}.subspan(2));

            CHECK(std::abs(error_in_samples(known(restarted.tracker.pulse_period()), gate_period)) <
                  0.01);
            CHECK(std::abs(error_in_samples(known(restarted.tracker.pulse_time(219)),
                                            returning.back())) < 0.01);
        }
    }
}

TEST_CASE("a long silence with no advance() calls is handled in bounded work")
{
    // The clock stops for longer than the history holds, and nothing calls advance() meanwhile
    // (a paused engine, a time skip). The returning clock must lock straight back on.
    for (const std::size_t silent_periods : {100U, 5'000U}) {
        CAPTURE(silent_periods);

        const auto truth = steady_clock(0.0, gate_period, 100 + silent_periods + 60);

        tracked_clock clock{clock_tracker_config::gate(rate_48k)};
        feed(clock.tracker, std::span{truth}.first(100));
        feed(clock.tracker, std::span{truth}.subspan(100 + silent_periods));

        CHECK(clock.tracker.latest_pulse() == static_cast<std::int64_t>(truth.size()) - 1);
        CHECK(std::abs(error_in_samples(known(clock.tracker.pulse_period()), gate_period)) < 0.01);
    }
}

TEST_CASE("a block's pulses are delivered before advance() for the end of the block")
{
    // The timing contract in the header. With an exact tempo halving and 32-sample blocks, the
    // halving is acquired with no phantom pulses.
    tracked_clock clock{clock_tracker_config::gate(rate_48k)};

    const std::vector<double>
                pulses{0.0, 6'000.0, 12'000.0, 24'000.0, 36'000.0, 48'000.0, 60'000.0, 72'000.0};
    std::size_t next = 0;

    for (int block = 1; block <= 2'251; ++block) {
        const double block_end = 32.0 * block;

        while (next < pulses.size() && pulses[next] < block_end) {
            clock.tracker.pulse(at(pulses[next]));
            ++next;
        }
        clock.tracker.advance(at(block_end));
    }

    CHECK(clock.tracker.latest_pulse() == 7);
    CHECK(clock.tracker.pulse_period() == at(12'000.0));
}

// ---- Reset
// ------------------------------------------------------------------------------------------

TEST_CASE("a reset makes the simultaneous pulse bar 0, in either order, with real-world edges")
{
    const auto truth = steady_clock(0.0, gate_period, 100);

    SUBCASE("gate, pulse first")
    {
        tracked_clock clock{clock_tracker_config::gate(rate_48k)};
        feed(clock.tracker, std::span{truth}.first(38));
        clock.tracker.reset(at(truth[37]));

        CHECK(clock.tracker.latest_pulse() == 0);
        CHECK(std::abs(error_in_samples(known(clock.tracker.bar_start(0)), truth[37])) < 0.01);
    }

    SUBCASE("gate, reset first, edge slightly late")
    {
        // The filtered time of a late edge sits before the raw edge; the reset must still match.
        for (const double late : {0.005, 0.05, 0.5}) {
            CAPTURE(late);

            tracked_clock clock{clock_tracker_config::gate(rate_48k)};
            feed(clock.tracker, std::span{truth}.first(37));
            clock.tracker.reset(at(truth[37] + late));
            clock.tracker.pulse(at(truth[37] + late));

            CHECK(clock.tracker.latest_pulse() == 0);
        }
    }

    SUBCASE("MIDI, reset first, edge 40 samples late")
    {
        // A Start message 15 samples before a Clock whose edge is 40 samples late (inside the
        // ±48 samples MIDI jitter).
        const auto midi_truth = steady_clock(0.0, midi_period, 60);

        tracked_clock clock{clock_tracker_config::midi(rate_48k)};
        feed(clock.tracker, std::span{midi_truth}.first(50));
        clock.tracker.reset(at(midi_truth[50] + 25.0));
        clock.tracker.pulse(at(midi_truth[50] + 40.0));

        CHECK(clock.tracker.latest_pulse() == 0);
    }

    SUBCASE("MIDI, pulse first, filtered time far from the raw edge")
    {
        std::vector<double> times = steady_clock(0.0, midi_period, 100);
        times.back() += 80.0;  // the last edge arrives 80 samples (1.7 ms) late

        tracked_clock clock{clock_tracker_config::midi(rate_48k)};
        feed(clock.tracker, times);

        const double filtered_gap =
            error_in_samples(known(clock.tracker.pulse_time(99)), times.back());
        REQUIRE(std::abs(filtered_gap) > 50.0);  // filtered and raw really are far apart

        clock.tracker.reset(at(times.back() + 60.0));
        CHECK(clock.tracker.latest_pulse() == 0);
    }

    SUBCASE("delivered late, after later pulses")
    {
        // A reset passed through a queue can arrive after the pulses around its timestamp. It
        // still labels the pulse at (or just after) its own time, found in the history.
        tracked_clock clock{clock_tracker_config::gate(rate_48k)};
        feed(clock.tracker, std::span{truth}.first(40));

        // Stamped 3 ms before pulse 37: pulse 37 is the first at or after it.
        clock.tracker.reset(at(truth[37] - 144.0));
        CHECK(clock.tracker.pulse_at_or_before(at(truth[37] + 1.0)) == 0);

        // Stamped 3 ms after pulse 37: pulse 38 is the first at or after it.
        clock.tracker.reset(at(truth[37] + 144.0));
        CHECK(clock.tracker.pulse_at_or_before(at(truth[37] + 1.0)) == -1);
        CHECK(clock.tracker.pulse_at_or_before(at(truth[38] + 1.0)) == 0);
    }
}

TEST_CASE("a reset on a stopped clock restarts the running grid")
{
    const auto truth = steady_clock(0.0, gate_period, 20);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, truth);
    clock.tracker.advance(at(truth.back() + (gate_period * 3.6)));
    REQUIRE(clock.tracker.state() == clock_state::freewheeling);

    const double reset_time = truth.back() + (gate_period * 3.6);
    clock.tracker.reset(at(reset_time));
    clock.tracker.advance(at(truth.back() + (gate_period * 5.6)));

    // The next filled-in slot after the reset is bar 0.
    CHECK(std::abs(error_in_samples(known(clock.tracker.bar_start(0)),
                                    truth.back() + (gate_period * 4.0))) < 0.01);
}

TEST_CASE("time windows follow the sample rate")
{
    // At 96 kHz the 2 ms reset window is 192 samples. A reset 150 samples (1.6 ms) after a pulse
    // is the same moment; with a window fixed in samples for 48 kHz it would not be.
    constexpr double period_96k = 12'000.0;  // 120 BPM, 4 PPQN, 96 kHz

    tracked_clock clock{clock_tracker_config::gate(sample_rate{96'000U})};
    feed(clock.tracker, steady_clock(0.0, period_96k, 21));
    clock.tracker.reset(at((period_96k * 20.0) + 150.0));

    CHECK(clock.tracker.latest_pulse() == 0);
}

// ---- The grid
// ---------------------------------------------------------------------------------------

TEST_CASE("past and future times map to the right pulse and bar")
{
    const auto truth = steady_clock(0.0, gate_period, 64);  // 4 bars at 4 PPQN in 4/4

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, truth);

    // In the history.
    CHECK(clock.tracker.pulse_at_or_before(at(truth[20])) == 20);
    CHECK(clock.tracker.pulse_at_or_before(at(truth[20] + 1.0)) == 20);
    CHECK(clock.tracker.pulse_at_or_before(at(truth[20] - 1.0)) == 19);
    CHECK(clock.tracker.bar_at_or_before(at(truth[31] + 1.0)) == 1);
    CHECK(clock.tracker.bar_at_or_before(at(truth[32] + 1.0)) == 2);

    // Before the first pulse there is no answer.
    CHECK_FALSE(clock.tracker.pulse_at_or_before(at(-1.0)).has_value());

    // In the future, extrapolated.
    CHECK(clock.tracker.pulse_at_or_before(at(truth[63] + (gate_period * 10.5))) == 73);
    CHECK(clock.tracker.bar_at_or_before(at(truth[63] + (gate_period * 10.5))) == 4);
    CHECK(std::abs(error_in_samples(known(clock.tracker.bar_start(5)), gate_period * 80.0)) < 0.01);
}

TEST_CASE("pulses before a reset belong to negative bars")
{
    // A reset partway through labels earlier pulses with negative indices. Bars must round
    // toward minus infinity: pulse -1 is the last pulse of bar -1, not part of bar 0.
    const auto truth = steady_clock(0.0, gate_period, 40);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, std::span{truth}.first(21));
    clock.tracker.reset(at(truth[20]));

    CHECK(clock.tracker.pulse_at_or_before(at(truth[19])) == -1);
    CHECK(clock.tracker.bar_at_or_before(at(truth[19])) == -1);
    CHECK(clock.tracker.bar_at_or_before(at(truth[4])) == -1);
    CHECK(clock.tracker.bar_at_or_before(at(truth[3])) == -2);
    CHECK(std::abs(error_in_samples(known(clock.tracker.bar_start(-1)), truth[4])) < 0.01);
}

TEST_CASE("pulses older than the history are no longer answerable")
{
    const auto truth = steady_clock(0.0, gate_period, 3'000);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, truth);

    const std::int64_t oldest_kept =
        3'000 - static_cast<std::int64_t>(clock.tracker.history_capacity());

    CHECK(clock.tracker.pulse_time(oldest_kept).has_value());
    CHECK_FALSE(clock.tracker.pulse_time(oldest_kept - 1).has_value());
}

TEST_CASE("the history size is chosen by the owner")
{
    // 16 bars at 4 PPQN needs only 256 entries.
    std::array<sample_time, 256> history{};
    clock_tracker                tracker{clock_tracker_config::gate(rate_48k), history};

    feed(tracker, steady_clock(0.0, gate_period, 1'000));

    CHECK(tracker.history_capacity() == 256);
    CHECK(tracker.pulse_time(1'000 - 256).has_value());
    CHECK_FALSE(tracker.pulse_time(1'000 - 257).has_value());
}

// ---- Robustness
// -------------------------------------------------------------------------------------

TEST_CASE("absurd indices are refused rather than overflowing")
{
    tracked_clock clock{clock_tracker_config::gate(rate_48k)};
    feed(clock.tracker, steady_clock(0.0, gate_period, 10));

    CHECK_FALSE(clock.tracker.pulse_time(30'000'000'000'000).has_value());
    CHECK_FALSE(clock.tracker.bar_start(INT64_MAX / 16 + 1).has_value());
    CHECK_FALSE(clock.tracker.bar_start(INT64_MIN / 16).has_value());
}

TEST_CASE("the period never collapses to zero")
{
    // At 100 Hz the 4 ms double-trigger window is 0.4 samples, which would round to zero. The
    // window is clamped to one sample, so identical timestamps are rejected as a double trigger
    // and can never lock a zero period.
    tracked_clock clock{clock_tracker_config::gate(sample_rate{100U})};

    clock.tracker.pulse(at(100.0));
    clock.tracker.pulse(at(100.0));
    CHECK(clock.tracker.state() == clock_state::acquiring);

    clock.tracker.pulse(at(101.0));
    CHECK(clock.tracker.pulse_period() == at(1.0));

    clock.tracker.advance(at(10'000.0));
    CHECK(clock.tracker.pulse_at_or_before(at(10'000.0)).has_value());
}

TEST_CASE("tracking never allocates")
{
    const auto truth = steady_clock(0.0, gate_period, 500);

    tracked_clock clock{clock_tracker_config::gate(rate_48k)};

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;

        for (const double time : truth) {
            clock.tracker.pulse(at(time));
            clock.tracker.advance(at(time + 1.0));
            static_cast<void>(clock.tracker.bar_at_or_before(at(time)));
        }
    }

    CHECK(test::realtime_guard::violations() == 0);
}

}  // namespace
}  // namespace previously
