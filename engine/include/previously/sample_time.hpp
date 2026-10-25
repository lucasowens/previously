// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: time in samples, with sub-sample precision, as an exact integer.
//
// Clock edges land between samples, so time needs a fractional part. A float would provide one,
// but its 24-bit mantissa runs out after 2^24 samples (about 6 minutes at 48 kHz). After that,
// whole samples blur together. A performance runs for hours.
//
// sample_time is fixed point instead: a 64-bit integer counting "ticks", with 2^16 ticks per
// sample (Q47.16). One tick is 1/65,536 of a sample (about 0.3 ns at 48 kHz), and the range is
// about 93 years. Addition and subtraction are exact integer operations, so error never
// accumulates. C#'s TimeSpan makes the same choice: it counts long ticks, not double seconds.
//
// The same type serves as a point in time ("pulse 12 happened at...") and as a duration ("one
// beat lasts..."), as std::chrono does with its tick counts.
//
// Why not quantity<Tag, Rep> from units.hpp? A quantity is constructed from its value, so
// sample_time{48'000} would read as 48,000 samples but mean 48,000 ticks (under one sample).
// Here the raw constructor is private and every way in is named: from_ticks, from_samples,
// from_samples_float.

#pragma once

#include "previously/units.hpp"

#include <compare>
#include <cstdint>

namespace previously {

/// Integer division rounding toward minus infinity. The built-in operator/ rounds toward zero,
/// which would put time -0.5 pulses into pulse 0 instead of pulse -1.
[[nodiscard]] constexpr std::int64_t floor_divide(std::int64_t numerator,
                                                  std::int64_t denominator) noexcept
{
    const std::int64_t quotient  = numerator / denominator;
    const std::int64_t remainder = numerator % denominator;

    return (remainder != 0 && ((remainder < 0) != (denominator < 0))) ? quotient - 1 : quotient;
}

/// The remainder of floor_divide: always in [0, divisor) for a positive divisor, also for
/// negative values (unlike %). Pulse -1 is the last pulse of its bar, not pulse "-1" of bar 0.
[[nodiscard]] constexpr std::int64_t floor_modulo(std::int64_t value, std::int64_t divisor) noexcept
{
    return value - (floor_divide(value, divisor) * divisor);
}

class sample_time {
public:
    static constexpr int          fraction_bits    = 16;
    static constexpr std::int64_t ticks_per_sample = std::int64_t{65'536};  // 2^fraction_bits

    constexpr sample_time() noexcept = default;

    [[nodiscard]] static constexpr sample_time from_ticks(std::int64_t ticks) noexcept
    {
        return sample_time{ticks};
    }

    /// A whole number of samples, exactly.
    [[nodiscard]] static constexpr sample_time from_samples(samples whole) noexcept
    {
        return sample_time{whole.value() * ticks_per_sample};
    }

    /// A whole sample plus a fraction in [0, 1), such as an edge found between two samples.
    [[nodiscard]] static constexpr sample_time from_samples(samples whole, float fraction) noexcept
    {
        return from_samples(whole) + from_samples_float(fraction);
    }

    /// A short duration given as float samples. Exact only to float precision: use it for
    /// offsets and corrections, never for absolute time.
    [[nodiscard]] static constexpr sample_time from_samples_float(float duration) noexcept
    {
        return sample_time{round_to_int64(duration * static_cast<float>(ticks_per_sample))};
    }

    [[nodiscard]] constexpr std::int64_t ticks() const noexcept
    {
        return ticks_;
    }

    /// The whole samples, rounded down (toward minus infinity, also for negative times).
    [[nodiscard]] constexpr samples whole_samples() const noexcept
    {
        return samples{floor_divide(ticks_, ticks_per_sample)};
    }

    /// The nearest whole sample, halves up (toward plus infinity, also for negative times): where
    /// a capture cuts the tape at a bar line (DESIGN.md §3.4).
    [[nodiscard]] constexpr samples nearest_sample() const noexcept
    {
        return samples{floor_divide(ticks_ + (ticks_per_sample / 2), ticks_per_sample)};
    }

    /// The fractional part, in [0, 1).
    [[nodiscard]] constexpr float fraction() const noexcept
    {
        const std::int64_t fraction_ticks = ticks_ - (whole_samples().value() * ticks_per_sample);

        return static_cast<float>(fraction_ticks) / static_cast<float>(ticks_per_sample);
    }

    /// This value as float samples. Precise for durations (a beat, an error); do not use it
    /// for absolute time.
    [[nodiscard]] constexpr float to_samples_float() const noexcept
    {
        return static_cast<float>(ticks_) / static_cast<float>(ticks_per_sample);
    }

    constexpr sample_time& operator+=(sample_time other) noexcept
    {
        ticks_ += other.ticks_;
        return *this;
    }

    constexpr sample_time& operator-=(sample_time other) noexcept
    {
        ticks_ -= other.ticks_;
        return *this;
    }

    [[nodiscard]] friend constexpr sample_time operator+(sample_time lhs, sample_time rhs) noexcept
    {
        lhs += rhs;
        return lhs;
    }

    [[nodiscard]] friend constexpr sample_time operator-(sample_time lhs, sample_time rhs) noexcept
    {
        lhs -= rhs;
        return lhs;
    }

    [[nodiscard]] friend constexpr sample_time operator-(sample_time value) noexcept
    {
        return sample_time{-value.ticks_};
    }

    /// Repeats a duration: one pulse period times eight pulses is one bar of 2 beats at 4 PPQN.
    [[nodiscard]] friend constexpr sample_time operator*(sample_time  duration,
                                                         std::int64_t count) noexcept
    {
        return sample_time{duration.ticks_ * count};
    }

    /// Divides a duration into equal parts (half a period, a quarter beat), rounding toward zero.
    [[nodiscard]] friend constexpr sample_time operator/(sample_time  duration,
                                                         std::int64_t parts) noexcept
    {
        return sample_time{duration.ticks_ / parts};
    }

    /// The magnitude of a duration: how far apart, ignoring which comes first.
    [[nodiscard]] friend constexpr sample_time abs(sample_time duration) noexcept
    {
        return duration.ticks_ < 0 ? sample_time{-duration.ticks_} : duration;
    }

    [[nodiscard]] friend constexpr bool operator==(const sample_time&,
                                                   const sample_time&) noexcept = default;

    [[nodiscard]] friend constexpr auto operator<=>(const sample_time&,
                                                    const sample_time&) noexcept = default;

private:
    constexpr explicit sample_time(std::int64_t ticks) noexcept
        : ticks_{ticks}
    {
    }

    std::int64_t ticks_{0};
};

}  // namespace previously
