// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: where a past bar line was (DESIGN.md §3.4, "Where a bar line is").
//
// The 132 scenarios of §3.4's deterministic table, as PR #6's probe built them, each run through
// the real tracker as the engine runs it: the estimate reads the tracker's edges and period.
// Errors are the worst over the line and the 3 pulses either side. A category the table says is
// repaired is held to its row's bound; one it says keeps its edges is held to its raw edges
// exactly. Then each mechanism of the rule is pinned on its own, and the tracker's side: when a
// line is final, a short history, fill-ins, and no allocation.

#include "previously/bar_line.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/sample_time.hpp"
#include "previously/units.hpp"

#include "known.hpp"
#include "planted_clock.hpp"
#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace previously {
namespace {

using test::planted_clock;
using test::planted_line;
using test::to_samples;
using test::to_time;

constexpr sample_rate rate_48k{48'000U};

/// A planted clock fed to the real tracker as the engine feeds it: each pulse, then the block's
/// end, which here is the pulse itself.
class tracked_clock {
public:
    explicit tracked_clock(const planted_clock& clock)
    {
        for (const double edge : clock.raw) {
            tracker_.pulse(to_time(edge));
            tracker_.advance(to_time(edge));
        }
    }

    [[nodiscard]] const clock_tracker& grid() const noexcept
    {
        return tracker_;
    }

private:
    clock_history<256> history_{};
    clock_tracker      tracker_{clock_tracker_config::gate(rate_48k), history_};
};

/// What the estimate made of one pulse.
struct pulse_outcome {
    double     error{0.0};  // the estimate's, in samples
    bool       kept{true};  // the estimate is the raw edge, to the tick
    line_basis basis{line_basis::kept};
};

/// The estimate of the line and of the 3 pulses either side, through the real tracker.
[[nodiscard]] std::vector<pulse_outcome> estimate_around_line(const planted_clock& clock)
{
    const auto                 tracked = std::make_unique<tracked_clock>(clock);
    std::vector<pulse_outcome> outcomes;

    for (int pulse = planted_line - 3; pulse <= planted_line + 3; ++pulse) {
        const auto          index    = static_cast<std::size_t>(pulse);
        const line_estimate estimate = test::known(estimate_line(tracked->grid(), pulse));

        outcomes.push_back(pulse_outcome{
            .error = std::abs(to_samples(estimate.time) - clock.truth[index]),
            .kept  = estimate.time == to_time(clock.raw[index]),
            .basis = estimate.basis,
        });
    }

    return outcomes;
}

/// The table's rows that keep every edge: the edges are where the pulses belong, or the case is a
/// stated limit, kept at its own error.
void check_kept(const planted_clock& clock)
{
    CAPTURE(clock.name);

    for (const pulse_outcome& outcome : estimate_around_line(clock)) {
        CHECK(outcome.kept);
    }
}

/// The gate clock's jitter never strays beyond 6 × 0.005 samples (test_noise::gaussian), so a pulse
/// on the grid is never further than this from where it belongs.
constexpr double on_grid_error = 0.03;

/// The table's rows that repair: every pulse around the line is repaired within `bound` samples
/// of where it belongs, or kept at its raw edge because that edge is on the grid. The table's
/// worst includes the kept edges' gate jitter, 0.005 samples rms, which another draw of it can
/// put above `bound`. A displaced pulse left at its raw edge fails.
void check_repaired(const planted_clock& clock, double bound)
{
    CAPTURE(clock.name);

    for (const pulse_outcome& outcome : estimate_around_line(clock)) {
        if (outcome.basis == line_basis::repaired) {
            CHECK(outcome.error <= bound);
        }
        else {
            CHECK(outcome.kept);
            CHECK(outcome.error <= on_grid_error);
        }
    }
}

/// As check_repaired, and the line's own pulse is one of those repaired.
void check_line_repaired(const planted_clock& clock, double bound)
{
    check_repaired(clock, bound);

    const std::vector<pulse_outcome> outcomes = estimate_around_line(clock);

    CHECK(outcomes[3].basis == line_basis::repaired);
}

// The deterministic table: 132 scenarios.

TEST_CASE("tempo changes keep every edge, and so do slipped edges at a step: 72 scenarios")
{
    // ×0.5 to ×2, from 2 pulses before the line, at it, or 1 after. A slipped edge at a step is a
    // stated limit: the sides disagree by the step, so it is kept at its slip.
    for (const double ratio : {1.01, 1.1, 0.9, 1.5, 2.0, 0.5}) {
        for (const int from : {-2, 0, 1}) {
            for (const double slip : {0.0, 1.01, 2.0, 30.0}) {
                check_kept(test::step_with_slip(ratio, from, slip));
            }
        }
    }
}

TEST_CASE("a steady gate clock keeps every edge")
{
    check_kept(test::steady(5));
}

TEST_CASE("one pulse off by 2 or 30 samples, or by up to 0.45 of a period, is repaired within 0.01")
{
    for (const double fraction : {0.1, 0.2, 0.3, 0.45, -0.1, -0.2, -0.3, -0.45}) {
        check_line_repaired(test::hiccups({{0, fraction}}), 0.01);
    }

    check_line_repaired(test::small_hiccup(2.0), 0.01);
    check_line_repaired(test::small_hiccup(30.0), 0.01);
}

TEST_CASE("bursts of 2 to 4 off-grid pulses at or around the line are repaired within 0.05")
{
    for (const auto& burst : std::vector<std::vector<std::pair<int, double>>>{
             {{0, 0.3}, {1, -0.3}},
             {{-1, 0.2}, {0, 0.3}, {1, -0.3}},
             {{0, 0.3}, {1, -0.3}, {2, 0.3}, {3, -0.2}},
             {{-1, 0.3}, {0, 0.3}, {1, 0.3}, {2, 0.3}},
         }) {
        check_line_repaired(test::hiccups(burst), 0.05);
    }
}

TEST_CASE("bursts of 3 or 4 off-grid pulses inside a side are repaired within 0.05, around a line "
          "kept on the grid")
{
    // The line's own edge is on the grid, where step 2 keeps it. The displaced pulses within 3 of
    // it are repaired; each kept pulse must be one that was on the grid.
    for (const auto& burst : std::vector<std::vector<std::pair<int, double>>>{
             {{1, 0.3}, {2, -0.3}, {3, 0.3}},
             {{-3, 0.3}, {-2, -0.3}, {-1, 0.3}},
             {{-4, 0.3}, {-3, -0.3}, {-2, 0.3}},
             {{2, 0.3}, {3, -0.3}, {4, 0.3}, {5, -0.3}},
         }) {
        check_repaired(test::hiccups(burst), 0.05);
    }
}

TEST_CASE("phase shifts keep every edge: 15 scenarios")
{
    for (const double fraction : {0.002, 0.1, 0.2, 0.3, -0.2}) {
        for (const int from : {0, -2, 1}) {
            check_kept(test::phase_shift(from, fraction));
        }
    }
}

TEST_CASE("ramps keep every edge, linear or exponential, centred on the line, starting or ending "
          "at it: 13 scenarios")
{
    check_kept(test::gentle_ramp());
    check_kept(test::linear_ramp(0.1, 2));
    check_kept(test::linear_ramp(0.2, 2));
    check_kept(test::linear_ramp(-0.2, 1));
    check_kept(test::linear_ramp(0.5, 4));
    check_kept(test::exponential_ramp(0.1, 8));
    check_kept(test::exponential_ramp(0.2, 2));
    check_kept(test::exponential_ramp(-0.3, 1));
    check_kept(test::exponential_ramp(0.5, 4));
    check_kept(test::ramp_at_line(0.1, 8, false));
    check_kept(test::ramp_at_line(0.2, 2, false));
    check_kept(test::ramp_at_line(0.1, 8, true));
    check_kept(test::ramp_at_line(0.2, 2, true));
}

TEST_CASE("an 1,800-sample hiccup on a ramp is repaired within 0.30: 4 scenarios")
{
    check_line_repaired(test::on_ramp(test::linear_ramp(0.2, 2), 0, 1'800.0, false), 0.30);
    check_line_repaired(test::on_ramp(test::linear_ramp(-0.2, 1), 0, 1'800.0, false), 0.30);
    check_line_repaired(test::on_ramp(test::exponential_ramp(0.2, 2), 0, 1'800.0, false), 0.30);
    check_line_repaired(test::on_ramp(test::exponential_ramp(0.5, 4), 0, 1'800.0, false), 0.30);
}

TEST_CASE("the stated limits keep the edge at its own error: 6 scenarios")
{
    // A ramp that bends hard within 8 pulses: the sides end up 378 samples apart.
    check_kept(test::on_ramp(test::exponential_ramp(-0.3, 1), 0, 1'800.0, false));

    // A hiccup on the very pulse where a ramp starts or ends: the sides are 16·c apart.
    check_kept(test::on_ramp(test::ramp_at_line(0.2, 2, false), 0, 1'800.0, false));
    check_kept(test::on_ramp(test::ramp_at_line(0.2, 2, true), 0, 1'800.0, false));

    // A hiccup on a ramp smaller than twice its curvature bias.
    check_kept(test::on_ramp(test::linear_ramp(0.2, 2), 0, 2.0, false));
    check_kept(test::on_ramp(test::exponential_ramp(0.2, 2), 0, 30.0, false));
    check_kept(test::on_ramp(test::ramp_at_line(0.2, 2, false), 0, 2.0, false));
}

TEST_CASE("a phase shift on a ramp keeps every edge: 3 scenarios")
{
    check_kept(test::on_ramp(test::linear_ramp(0.2, 2), 0, 1'200.0, true));
    check_kept(test::on_ramp(test::linear_ramp(0.2, 2), -2, 1'200.0, true));
    check_kept(test::on_ramp(test::exponential_ramp(0.2, 2), 0, 1'200.0, true));
}

// The rule's thresholds, each just either side.

/// A clock exactly on the grid: the edges are the truth.
[[nodiscard]] planted_clock clean_clock()
{
    return test::uniform_jitter(test::steady(1), 0.0, 1);
}

/// The estimate of the line from a clock whose edges begin `before` pulses before it, as though
/// the history held no more, at the base period.
[[nodiscard]] line_estimate estimate_with_history(const planted_clock& clock, int before)
{
    std::vector<pulse_edge> edges;

    for (int j = planted_line - before; j <= planted_line + 8; ++j) {
        edges.push_back(pulse_edge{.time = to_time(clock.raw[static_cast<std::size_t>(j)])});
    }

    return estimate_line(std::span<const pulse_edge>{edges},
                         static_cast<std::size_t>(before),
                         to_time(test::base_period));
}

[[nodiscard]] double error_at_line(const planted_clock& clock, const line_estimate& estimate)
{
    return std::abs(to_samples(estimate.time) -
                    clock.truth[static_cast<std::size_t>(planted_line)]);
}

TEST_CASE("an edge within 1 sample of a straight line is kept, and one just beyond is repaired")
{
    // With no jitter, sigma is 0 and step 2's threshold is its floor, 1 sample, inclusive.
    for (const double off : {0.99, 1.0, 1.01}) {
        CAPTURE(off);

        planted_clock clock = clean_clock();

        clock.raw[static_cast<std::size_t>(planted_line)] += off;

        const line_estimate estimate = test::estimate_planted(clock, planted_line);

        CHECK(estimate.jitter == 0.0F);

        if (off <= 1.0) {
            CHECK(estimate.basis == line_basis::on_a_line);
            CHECK(estimate.time == to_time(clock.raw[static_cast<std::size_t>(planted_line)]));
        }
        else {
            CHECK(estimate.basis == line_basis::repaired);
            CHECK(error_at_line(clock, estimate) < 0.001);
        }
    }
}

TEST_CASE("a pulse up to half a period off is repaired, and one just beyond is kept")
{
    // Half the 6,000-sample period, inclusive: a pulse further off is closer to its neighbour's
    // slot.
    for (const double off : {2'999.0, 3'000.0, 3'001.0}) {
        CAPTURE(off);

        planted_clock clock = clean_clock();

        clock.raw[static_cast<std::size_t>(planted_line)] += off;

        const line_estimate estimate = test::estimate_planted(clock, planted_line);

        CHECK(estimate.basis == (off <= 3'000.0 ? line_basis::repaired : line_basis::kept));
    }
}

TEST_CASE("a repair needs the sides within 64 samples of each other")
{
    // The pulses after the line moved by `apart`, and the line's own 1,000 samples off: the sides
    // agree with each other to `apart`, and the line goes to their mean, halfway between. 64 is
    // inclusive.
    for (const double apart : {63.0, 64.0, 65.0}) {
        CAPTURE(apart);

        planted_clock clock = clean_clock();

        for (int j = planted_line + 1; j < test::clock_pulses; ++j) {
            clock.raw[static_cast<std::size_t>(j)] += apart;
        }

        clock.raw[static_cast<std::size_t>(planted_line)] += 1'000.0;

        const line_estimate estimate = test::estimate_planted(clock, planted_line);

        if (apart <= 64.0) {
            CHECK(estimate.basis == line_basis::repaired);
            CHECK(std::abs(error_at_line(clock, estimate) - (apart / 2.0)) < 0.001);
        }
        else {
            CHECK(estimate.basis == line_basis::kept);
        }
    }
}

TEST_CASE("a repair needs 16 usable third differences: with 15 the edge is kept")
{
    // With h edges before the line, sigma has h - 3 third differences before it and 5 after.
    planted_clock clock = clean_clock();

    clock.raw[static_cast<std::size_t>(planted_line)] += 100.0;

    const line_estimate fifteen = estimate_with_history(clock, 13);
    const line_estimate sixteen = estimate_with_history(clock, 14);

    CHECK(fifteen.jitter_values == 15);
    CHECK(fifteen.basis == line_basis::few_jitter_values);
    CHECK(fifteen.time == to_time(clock.raw[static_cast<std::size_t>(planted_line)]));

    CHECK(sixteen.jitter_values == 16);
    CHECK(sixteen.basis == line_basis::repaired);
    CHECK(error_at_line(clock, sixteen) < 0.001);
}

TEST_CASE("with fewer than 8 edges before the line, the edge is kept")
{
    planted_clock clock = clean_clock();

    clock.raw[static_cast<std::size_t>(planted_line)] += 100.0;

    CHECK(estimate_with_history(clock, 7).basis == line_basis::short_history);
    CHECK(estimate_with_history(clock, 7).time ==
          to_time(clock.raw[static_cast<std::size_t>(planted_line)]));
    CHECK(estimate_with_history(clock, 8).basis == line_basis::few_jitter_values);
}

TEST_CASE(
    "the curvature check: a 1,800-sample hiccup on the pulse of a x1.1 step lands on the line")
{
    // The line is the last pulse before the step: the intervals grow by 10% from the next pulse
    // on. Both sides' straight lines meet at the line, but their slopes differ, which reads as
    // curvature between them and none within them, so the check sets c to 0. Without it, the
    // correction moves both sides 16·c, 533 samples, and the repair lands there (DESIGN.md §3.4).
    const planted_clock clock    = test::step_with_slip(1.1, 1, 1'800.0);
    const line_estimate estimate = test::estimate_planted(clock, planted_line);

    CHECK(estimate.basis == line_basis::repaired);
    CHECK(error_at_line(clock, estimate) < 0.05);
}

TEST_CASE("sigma ignores a ramp of 2, 4 or 8 bars anywhere in its window: at most 0.033 samples "
          "with no jitter")
{
    // Third differences cancel curvature that changes at a constant rate, so a +20% ramp over 2
    // bars starting anywhere from 8 pulses after the line to 72 before leaves sigma near 0: at
    // most 0.0325, in double and in float, where the ramp's start or end sits in the window.
    // Second differences read a ramp as jitter where its curvature fills about half the window:
    // 5.67 samples on 4 bars, 2.84 on 8, and 11.35 on 2 bars in the earlier 44-value window. A
    // 2-bar ramp fills at most 32 of these 66 values, where their median and spread stay 0.
    float worst = 0.0F;

    for (const int bars : {2, 4, 8}) {
        for (int start = -8; start <= 72; ++start) {
            for (const bool exponential : {false, true}) {
                const planted_clock clock = test::ramp_starting(0.2, bars, start, exponential);

                worst = std::max(worst, test::estimate_planted(clock, planted_line).jitter);
            }
        }
    }

    CHECK(worst <= 0.033F);
}

TEST_CASE("sigma leaves out every third difference that touches a fill-in")
{
    // 10 fill-ins, 30 to 21 pulses before the line, touch the third differences centred from 32
    // before to 20 before: 13 of the 66.
    const planted_clock clock = test::steady(1);
    std::vector<bool>   filled_in(clock.raw.size(), false);

    for (int j = planted_line - 30; j <= planted_line - 21; ++j) {
        filled_in[static_cast<std::size_t>(j)] = true;
    }

    CHECK(test::estimate_planted(clock, planted_line).jitter_values == 66);
    CHECK(test::estimate_planted(clock, planted_line, filled_in).jitter_values == 53);
}

TEST_CASE(
    "the first pulse of a phase shift or a tempo change lies on the line after it, and is kept")
{
    // Step 2 checks both sides: the side after the line confirms a change that starts on it.
    for (const planted_clock& clock : {test::phase_shift(0, 0.1), test::tempo_change(0, 1.1)}) {
        CAPTURE(clock.name);
        CHECK(test::estimate_planted(clock, planted_line).basis == line_basis::on_a_line);
    }
}

TEST_CASE(
    "an edge exactly 2 x the sides' spread from their mean is kept, and one beyond is repaired")
{
    // The pulses after the line moved 10 samples: the sides are 10 apart, their mean 5 from the
    // grid. The line's own edge 25 off is 20 from the mean, twice the spread: not more than it.
    for (const double off : {25.0, 25.5}) {
        CAPTURE(off);

        planted_clock clock = clean_clock();

        for (int j = planted_line + 1; j < test::clock_pulses; ++j) {
            clock.raw[static_cast<std::size_t>(j)] += 10.0;
        }

        clock.raw[static_cast<std::size_t>(planted_line)] += off;

        const line_estimate estimate = test::estimate_planted(clock, planted_line);

        CHECK(estimate.basis == (off > 25.0 ? line_basis::repaired : line_basis::kept));
    }
}

TEST_CASE(
    "under jitter, an edge within 4 sigma of a line is on it, and one within 20 sigma is kept")
{
    // Uniform jitter of +-10 samples. The line's own edge is placed a multiple of sigma from where
    // it belongs. Sigma leaves that edge out, but the residuals are measured from it, so moving it
    // changes sigma only in float rounding.
    const planted_clock jittered = test::uniform_jitter(test::steady(1), 10.0, 7);
    const float         sigma    = test::estimate_planted(jittered, planted_line).jitter;

    REQUIRE(sigma > 4.0F);

    struct placed {
        float      sigmas;
        line_basis basis;
    };

    for (const placed& placement : {
             placed{.sigmas = 3.0F, .basis = line_basis::on_a_line},
             placed{.sigmas = 5.0F, .basis = line_basis::kept},
             placed{.sigmas = 15.0F, .basis = line_basis::kept},
             placed{.sigmas = 25.0F, .basis = line_basis::repaired},
         }) {
        CAPTURE(placement.sigmas);

        planted_clock clock = jittered;

        clock.raw[static_cast<std::size_t>(planted_line)] =
            clock.truth[static_cast<std::size_t>(planted_line)] +
            static_cast<double>(placement.sigmas * sigma);

        const line_estimate estimate = test::estimate_planted(clock, planted_line);

        CHECK(estimate.jitter == doctest::Approx(static_cast<double>(sigma)).epsilon(1e-5));
        CHECK(estimate.basis == placement.basis);
    }
}

TEST_CASE(
    "the curvature within the sides leaves out every second difference that touches a fill-in")
{
    // A hiccup reaches step 3. The 12 second differences are centred 7 to 2 pulses before the
    // line and 2 to 7 after; fill-ins 3 and 2 before it touch the 3 centred 4, 3 and 2 before.
    planted_clock clock = test::steady(1);

    clock.raw[static_cast<std::size_t>(planted_line)] += 1'800.0;

    std::vector<bool> filled_in(clock.raw.size(), false);

    filled_in[static_cast<std::size_t>(planted_line - 3)] = true;
    filled_in[static_cast<std::size_t>(planted_line - 2)] = true;

    CHECK(test::estimate_planted(clock, planted_line).curvature_values == 12);
    CHECK(test::estimate_planted(clock, planted_line, filled_in).curvature_values == 9);
}

// Through the tracker.

TEST_CASE("a line is final once its edge and the 8 pulses after it are recorded, and not before")
{
    const planted_clock clock = test::steady(1);

    clock_history<256> history{};
    clock_tracker      tracker{clock_tracker_config::gate(rate_48k), history};

    for (int j = 0; j <= planted_line + 8; ++j) {
        const sample_time edge = to_time(clock.raw[static_cast<std::size_t>(j)]);

        tracker.pulse(edge);
        tracker.advance(edge);

        CAPTURE(j);
        CHECK(estimate_line(tracker, planted_line).has_value() == (j == planted_line + 8));
    }
}

TEST_CASE("a line older than the history, or before the tracker has a period, has no estimate")
{
    const planted_clock clock = test::steady(1);

    clock_history<64> history{};
    clock_tracker     tracker{clock_tracker_config::gate(rate_48k), history};

    tracker.pulse(to_time(clock.raw[0]));
    CHECK_FALSE(estimate_line(tracker, 0).has_value());

    for (std::size_t j = 1; j < clock.raw.size(); ++j) {
        tracker.pulse(to_time(clock.raw[j]));
        tracker.advance(to_time(clock.raw[j]));
    }

    // 160 pulses through a history of 64: the oldest left is 96.
    CHECK_FALSE(estimate_line(tracker, 95).has_value());
    CHECK(estimate_line(tracker, 96 + 8).has_value());
}

TEST_CASE("just after the clock starts, a line keeps its edge")
{
    const planted_clock clock = test::small_hiccup(30.0);

    clock_history<256> history{};
    clock_tracker      tracker{clock_tracker_config::gate(rate_48k), history};

    // The clock starts 5 pulses before the line.
    for (int j = planted_line - 5; j <= planted_line + 8; ++j) {
        const sample_time edge = to_time(clock.raw[static_cast<std::size_t>(j)]);

        tracker.pulse(edge);
        tracker.advance(edge);
    }

    const line_estimate estimate = test::known(estimate_line(tracker, 5));

    CHECK(estimate.basis == line_basis::short_history);
    CHECK(estimate.time == to_time(clock.raw[static_cast<std::size_t>(planted_line)]));
}

TEST_CASE("a stopped clock's fill-ins reach the estimate marked, and sigma leaves them out")
{
    // The clock stops after pulse 59 and returns on the grid at pulse 70. The tracker fills in
    // pulses 60 to 69, on the grid it was keeping.
    const planted_clock clock = test::steady(1);

    clock_history<256> history{};
    clock_tracker      tracker{clock_tracker_config::gate(rate_48k), history};

    for (int j = 0; j < test::clock_pulses; ++j) {
        const sample_time slot = to_time(clock.truth[static_cast<std::size_t>(j)]);

        if (j >= 60 && j < 70) {
            tracker.advance(slot + to_time(1.0));
            continue;
        }

        tracker.pulse(to_time(clock.raw[static_cast<std::size_t>(j)]));
        tracker.advance(slot + to_time(1.0));
    }

    for (int j = 59; j <= 70; ++j) {
        CAPTURE(j);

        CHECK(test::known(tracker.edge(j)).filled_in == (j >= 60 && j < 70));
    }

    // The line at pulse 72 reads pulses 8 to 80. The third differences touching 60 to 69 are
    // those centred from 58 to 69, 12 of the 61 before the line, so 49 and the 5 after remain.
    const line_estimate estimate = test::known(estimate_line(tracker, 72));

    CHECK(estimate.jitter_values == 54);
    CHECK(estimate.basis == line_basis::on_a_line);
}

TEST_CASE("estimating a line allocates nothing")
{
    const auto tracked = std::make_unique<tracked_clock>(test::small_hiccup(30.0));

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;

        static_cast<void>(estimate_line(tracked->grid(), planted_line));
    }

    CHECK(test::realtime_guard::violations() == 0U);
}

}  // namespace
}  // namespace previously
