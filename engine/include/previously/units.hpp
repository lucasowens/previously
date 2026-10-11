// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — strong unit types.
//
// A looper juggles several kinds of number: sample counts, seconds, and (later) beats. Mixing
// them up is the classic looper bug. Each unit here is a distinct type, so the compiler rejects
// `samples + seconds` and every conversion is explicit and named.
//
// Absolute time is always `samples` (int64). It is never a float, because a float stops
// resolving single samples after 2^24 of them (about 350 s at 48 kHz). See DESIGN.md §2.3.

#pragma once

#include <compare>
#include <cstdint>

namespace previously {

/// A value of one physical unit. `Tag` makes each unit a distinct type; `Rep` stores the value.
/// Same-unit arithmetic is allowed. Dividing two values of one unit gives a plain ratio.
template <typename Tag, typename Rep>
class quantity {
public:
    using rep = Rep;

    constexpr quantity() noexcept = default;

    constexpr explicit quantity(Rep value) noexcept
        : value_{value}
    {
    }

    [[nodiscard]] constexpr Rep value() const noexcept
    {
        return value_;
    }

    constexpr quantity& operator+=(quantity other) noexcept
    {
        value_ += other.value_;
        return *this;
    }

    constexpr quantity& operator-=(quantity other) noexcept
    {
        value_ -= other.value_;
        return *this;
    }

    [[nodiscard]] friend constexpr quantity operator+(quantity lhs, quantity rhs) noexcept
    {
        lhs += rhs;
        return lhs;
    }

    [[nodiscard]] friend constexpr quantity operator-(quantity lhs, quantity rhs) noexcept
    {
        lhs -= rhs;
        return lhs;
    }

    [[nodiscard]] friend constexpr quantity operator*(quantity lhs, Rep scale) noexcept
    {
        return quantity{lhs.value_ * scale};
    }

    [[nodiscard]] friend constexpr quantity operator*(Rep scale, quantity rhs) noexcept
    {
        return quantity{scale * rhs.value_};
    }

    /// The ratio of two values of the same unit is dimensionless.
    [[nodiscard]] friend constexpr Rep operator/(quantity lhs, quantity rhs) noexcept
    {
        return lhs.value_ / rhs.value_;
    }

    [[nodiscard]] friend constexpr bool operator==(const quantity&,
                                                   const quantity&) noexcept = default;

    [[nodiscard]] friend constexpr auto operator<=>(const quantity&,
                                                    const quantity&) noexcept = default;

private:
    Rep value_{};
};

/// A count of sample frames (one frame = one sample per channel). Also used for absolute time.
using samples = quantity<struct samples_tag, std::int64_t>;

/// A duration in seconds. For human-scale durations only, never absolute time.
using seconds = quantity<struct seconds_tag, float>;

/// Audio sample rate in frames per second.
class sample_rate {
public:
    constexpr explicit sample_rate(std::uint32_t hz) noexcept
        : hz_{hz}
    {
    }

    [[nodiscard]] constexpr std::uint32_t hz() const noexcept
    {
        return hz_;
    }

    [[nodiscard]] friend constexpr bool operator==(const sample_rate&,
                                                   const sample_rate&) noexcept = default;

private:
    std::uint32_t hz_;
};

/// Rounds to the nearest integer, halves away from zero. (std::lround is not constexpr in
/// C++20, and on the M7 `long` is 32 bits.)
[[nodiscard]] constexpr std::int64_t round_to_int64(float value) noexcept
{
    return static_cast<std::int64_t>(value >= 0.0F ? value + 0.5F : value - 0.5F);
}

/// Converts a sample count to seconds at the given rate.
[[nodiscard]] constexpr seconds to_seconds(samples count, sample_rate rate) noexcept
{
    return seconds{static_cast<float>(count.value()) / static_cast<float>(rate.hz())};
}

/// Converts a duration to the nearest whole number of samples at the given rate.
[[nodiscard]] constexpr samples to_samples(seconds duration, sample_rate rate) noexcept
{
    return samples{round_to_int64(duration.value() * static_cast<float>(rate.hz()))};
}

}  // namespace previously
