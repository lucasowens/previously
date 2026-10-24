// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: clocks with planted anomalies.

#include "planted_clock.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace previously::test {
namespace {

constexpr double gate_jitter_rms = 0.005;  // samples

[[nodiscard]] std::size_t at(int pulse)
{
    return static_cast<std::size_t>(pulse);
}

[[nodiscard]] std::string with_fraction(double value)
{
    return std::to_string(value).substr(0, 5);
}

/// A clock whose every pulse comes `interval(j)` samples after the last, with no jitter: the
/// edges are the truth.
template <typename Interval>
[[nodiscard]] planted_clock smooth(std::string name, const Interval& interval)
{
    planted_clock made{.name = std::move(name), .raw = {}, .truth = {}};
    double        time = clock_origin;

    for (int j = 0; j < clock_pulses; ++j) {
        made.truth.push_back(time);
        made.raw.push_back(time);
        time += interval(j);
    }

    return made;
}

}  // namespace

std::uint64_t test_noise::next() noexcept
{
    state_ += 0x9E37'79B9'7F4A'7C15U;

    std::uint64_t mixed = state_;

    mixed = (mixed ^ (mixed >> 30U)) * 0xBF58'476D'1CE4'E5B9U;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94D0'49BB'1331'11EBU;

    return mixed ^ (mixed >> 31U);
}

double test_noise::uniform() noexcept
{
    // The top 53 bits, scaled exactly into [0, 1).
    return static_cast<double>(next() >> 11U) * 0x1p-53;
}

double test_noise::uniform(double amplitude) noexcept
{
    return amplitude * ((2.0 * uniform()) - 1.0);
}

double test_noise::gaussian() noexcept
{
    double sum = 0.0;

    for (int draw = 0; draw < 12; ++draw) {
        sum += uniform();
    }

    return sum - 6.0;
}

planted_clock steady(std::uint64_t seed)
{
    test_noise    noise{seed};
    planted_clock made{.name = "steady", .raw = {}, .truth = {}};

    for (int j = 0; j < clock_pulses; ++j) {
        const double grid = clock_origin + (j * base_period);

        made.truth.push_back(grid);
        made.raw.push_back(grid + (gate_jitter_rms * noise.gaussian()));
    }

    return made;
}

planted_clock hiccups(const std::vector<std::pair<int, double>>& displaced)
{
    planted_clock made = steady(1);

    made.name = "hiccup";

    for (const auto& [offset, fraction] : displaced) {
        made.name += " " + std::to_string(offset) + ":" + with_fraction(fraction);
        made.raw[at(planted_line + offset)] += fraction * base_period;
    }

    return made;
}

planted_clock small_hiccup(double samples)
{
    planted_clock made = steady(9);

    made.name = "hiccup " + std::to_string(static_cast<int>(samples)) + " samples";
    made.raw[at(planted_line)] += samples;

    return made;
}

planted_clock phase_shift(int from, double fraction)
{
    planted_clock made = steady(2);

    made.name = "phase shift " + with_fraction(fraction) + " from " + std::to_string(from);

    for (int j = planted_line + from; j < clock_pulses; ++j) {
        made.raw[at(j)] += fraction * base_period;
        made.truth[at(j)] += fraction * base_period;
    }

    return made;
}

planted_clock tempo_change(int from, double ratio)
{
    planted_clock made = steady(3);

    made.name = "tempo x" + with_fraction(ratio) + " from " + std::to_string(from);

    const int first = planted_line + from;

    for (int j = first; j < clock_pulses; ++j) {
        const double shift = (j - first + 1) * base_period * (ratio - 1.0);

        made.raw[at(j)] += shift;
        made.truth[at(j)] += shift;
    }

    return made;
}

planted_clock step_with_slip(double ratio, int from, double slip)
{
    planted_clock made = tempo_change(from, ratio);

    made.name += " slip " + with_fraction(slip);
    made.raw[at(planted_line)] += slip;

    return made;
}

planted_clock gentle_ramp()
{
    return smooth("ramp 10% over 8 bars", [](int j) {
        return base_period * (1.0 + (0.1 * j / 128.0));
    });
}

planted_clock linear_ramp(double fraction, int bars)
{
    const int span  = bars * pulses_per_bar;
    const int begin = planted_line - (span / 2);

    return smooth("ramp " + with_fraction(fraction) + " over " + std::to_string(bars) + " bars",
                  [=](int j) {
                      const int into = std::clamp(j - begin, 0, span);

                      return base_period * (1.0 + (fraction * into / span));
                  });
}

planted_clock exponential_ramp(double fraction, int bars)
{
    const int    span  = bars * pulses_per_bar;
    const int    begin = planted_line - (span / 2);
    const double step  = std::pow(1.0 + fraction, 1.0 / span);

    return smooth("exponential ramp " + with_fraction(fraction) + " over " + std::to_string(bars) +
                      " bars",
                  [=](int j) {
                      return base_period * std::pow(step, std::clamp(j - begin, 0, span));
                  });
}

planted_clock ramp_at_line(double fraction, int bars, bool ends_at_line)
{
    const int span  = bars * pulses_per_bar;
    const int begin = ends_at_line ? planted_line - span : planted_line;

    return smooth(
        std::string{ends_at_line ? "ramp ending at the line " : "ramp starting at the line "} +
            with_fraction(fraction) + " over " + std::to_string(bars) + " bars",
        [=](int j) {
            const int into = std::clamp(j - begin, 0, span);

            return base_period * (1.0 + (fraction * into / span));
        });
}

planted_clock ramp_starting(double fraction, int bars, int start, bool exponential)
{
    const int    span  = bars * pulses_per_bar;
    const int    begin = planted_line - start;
    const double step  = std::pow(1.0 + fraction, 1.0 / span);

    return smooth("ramp starting " + std::to_string(start) + " before the line", [=](int j) {
        const int into = std::clamp(j - begin, 0, span);

        return exponential ? base_period * std::pow(step, into)
                           : base_period * (1.0 + (fraction * into / span));
    });
}

planted_clock on_ramp(planted_clock base, int offset, double samples, bool shift)
{
    base.name += (shift ? " + shift " : " + hiccup ") + std::to_string(static_cast<int>(samples));

    for (int j = planted_line + offset; j < clock_pulses; ++j) {
        base.raw[at(j)] += samples;

        if (!shift) {
            break;
        }

        base.truth[at(j)] += samples;
    }

    return base;
}

planted_clock uniform_jitter(planted_clock base, double amplitude, std::uint64_t seed)
{
    test_noise noise{seed};

    for (std::size_t j = 0; j < base.raw.size(); ++j) {
        base.raw[j] = base.truth[j] + noise.uniform(amplitude);
    }

    return base;
}

sample_time to_time(double samples) noexcept
{
    return sample_time::from_ticks(
        std::llround(samples * static_cast<double>(sample_time::ticks_per_sample)));
}

double to_samples(sample_time time) noexcept
{
    return static_cast<double>(time.ticks()) / static_cast<double>(sample_time::ticks_per_sample);
}

line_estimate
estimate_planted(const planted_clock& clock, int pulse, const std::vector<bool>& filled_in)
{
    const int first = std::max(0, pulse - static_cast<int>(line_pulses_before));
    const int last  = pulse + static_cast<int>(line_pulses_after);

    std::vector<pulse_edge> edges;

    for (int j = first; j <= last; ++j) {
        edges.push_back(pulse_edge{
            .time      = to_time(clock.raw[at(j)]),
            .filled_in = !filled_in.empty() && filled_in[at(j)],
        });
    }

    return estimate_line(std::span<const pulse_edge>{edges},
                         at(pulse - first),
                         to_time(base_period));
}

}  // namespace previously::test
