// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: clocks with planted anomalies, for the bar-line estimate (DESIGN.md §3.4).
//
// These are the builders of PR #6's probe, which measured §3.4's tables: a gate clock at 4 PPQN
// and 120 BPM, a pulse every 6,000 samples, with an anomaly at the bar line at pulse 112, and 48
// pulses after it. Each clock holds its raw edges and where each pulse belongs musically.
//
// The probe drew its jitter from the standard library's distributions, which differ between
// libc++ and libstdc++. These draw from test_noise instead, which gives the same values on every
// platform, so a seeded property is the same test on macOS and on Linux.

#pragma once

#include "previously/bar_line.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace previously::test {

/// A seeded generator, the same on every platform: SplitMix64, and only integer and exact
/// floating-point arithmetic on its output.
class test_noise {
public:
    explicit test_noise(std::uint64_t seed) noexcept
        : state_{seed}
    {
    }

    [[nodiscard]] std::uint64_t next() noexcept;

    /// Uniform in [0, 1).
    [[nodiscard]] double uniform() noexcept;

    /// Uniform in [-amplitude, amplitude).
    [[nodiscard]] double uniform(double amplitude) noexcept;

    /// Close to a standard normal: the sum of 12 uniforms, less 6. Its variance is 1, and it
    /// never strays beyond ±6.
    [[nodiscard]] double gaussian() noexcept;

private:
    std::uint64_t state_;
};

struct planted_clock {
    std::string         name;
    std::vector<double> raw;    // each pulse's edge, in samples
    std::vector<double> truth;  // where it belongs musically
};

inline constexpr double base_period    = 6'000.0;  // samples: 120 BPM at 4 PPQN and 48 kHz
inline constexpr double clock_origin   = 1'000.0;  // the first pulse, in samples
inline constexpr int    pulses_per_bar = 16;
inline constexpr int    planted_line   = 112;  // bar 7's line, where anomalies are planted
inline constexpr int    clock_pulses   = planted_line + 48;

/// The gate clock: on the grid, with Gaussian jitter of 0.005 samples rms.
[[nodiscard]] planted_clock steady(std::uint64_t seed);

/// Pulses displaced from the grid, each by its fraction of a period, at its offset from the line.
/// They belong on the grid.
[[nodiscard]] planted_clock hiccups(const std::vector<std::pair<int, double>>& displaced);

/// The line's own pulse off by `samples`.
[[nodiscard]] planted_clock small_hiccup(double samples);

/// From `from` pulses after the line on, every pulse a fraction of a period later. The new phase
/// is the truth.
[[nodiscard]] planted_clock phase_shift(int from, double fraction);

/// From `from` pulses after the line on, intervals `ratio` times the period. The edges are the
/// truth.
[[nodiscard]] planted_clock tempo_change(int from, double ratio);

/// A tempo change, with the line's edge slipped by `slip` samples.
[[nodiscard]] planted_clock step_with_slip(double ratio, int from, double slip);

/// The period grows 10% over 8 bars, continuously.
[[nodiscard]] planted_clock gentle_ramp();

/// The period changes by `fraction` over `bars`, linearly, centred on the line.
[[nodiscard]] planted_clock linear_ramp(double fraction, int bars);

/// The period changes by `fraction` over `bars`, a constant ratio a pulse, centred on the line.
[[nodiscard]] planted_clock exponential_ramp(double fraction, int bars);

/// A linear ramp that starts at the line, or ends there.
[[nodiscard]] planted_clock ramp_at_line(double fraction, int bars, bool ends_at_line);

/// A linear or exponential ramp over `bars` starting `start` pulses before the line, with no
/// jitter.
[[nodiscard]] planted_clock ramp_starting(double fraction, int bars, int start, bool exponential);

/// `base` with the pulse at `offset` from the line moved by `samples`: as a hiccup, which belongs
/// where it was, or as a shift of it and every pulse after, which moves the truth too.
[[nodiscard]] planted_clock on_ramp(planted_clock base, int offset, double samples, bool shift);

/// `base` with every edge redrawn uniformly within ±`amplitude` of where it belongs.
[[nodiscard]] planted_clock
uniform_jitter(planted_clock base, double amplitude, std::uint64_t seed);

/// A time in samples, to the nearest tick.
[[nodiscard]] sample_time to_time(double samples) noexcept;

[[nodiscard]] double to_samples(sample_time time) noexcept;

/// The engine's estimate of the line at `pulse`, from the clock's edges: the 64 before it and the
/// 8 after it, at the base period. `filled_in` marks pulses as filled in, by index; empty for none.
[[nodiscard]] line_estimate
estimate_planted(const planted_clock& clock, int pulse, const std::vector<bool>& filled_in = {});

}  // namespace previously::test
