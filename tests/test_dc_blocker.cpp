// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the DC blocker, the audio floor, and floating point that is the same
// everywhere (ADR 0005).

#include "previously/audio_floor.hpp"
#include "previously/dc_blocker.hpp"

#include "flush_to_zero_mode.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <random>
#include <vector>

namespace previously {
namespace {

constexpr sample_rate rate_48k{48'000U};

/// The next float towards zero.
constexpr float next_towards_zero(float value) noexcept
{
    return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) - 1U);
}

/// The peak output for a sine at `frequency_hz`, once the filter has settled: over the last half
/// of eight seconds.
double settled_peak(double frequency_hz)
{
    constexpr std::size_t frames = std::size_t{8} * 48'000;

    dc_blocker blocker{rate_48k};
    float      peak = 0.0F;

    for (std::size_t i = 0; i < frames; ++i) {
        const double phase =
            2.0 * std::numbers::pi * frequency_hz * static_cast<double>(i) / 48'000.0;
        const float output = blocker.process(static_cast<float>(std::sin(phase)));

        if (i >= frames / 2) {
            peak = std::max(peak, std::abs(output));
        }
    }

    return static_cast<double>(peak);
}

bool is_subnormal(float value)
{
    return std::fpclassify(value) == FP_SUBNORMAL;
}

/// Full-scale noise, a minute of silence, subnormal and near-floor inputs, a tiny sine and its
/// decay: everything that leads a recursive filter towards the subnormal range.
std::vector<float> subnormal_bait()
{
    std::vector<float>                    input(24'000);
    std::mt19937                          generator{1234U};
    std::uniform_real_distribution<float> noise{-1.0F, 1.0F};

    std::ranges::generate(input, [&] {
        return noise(generator);
    });

    input.resize(input.size() + (std::size_t{60} * 48'000), 0.0F);

    for (const float value : {
             std::numeric_limits<float>::denorm_min(),
             -std::numeric_limits<float>::denorm_min(),
             1.0e-40F,
             std::numeric_limits<float>::min() / 2.0F,
             audio_floor,
             next_towards_zero(audio_floor),
             audio_floor / 2.0F,
             -audio_floor,
         }) {
        input.push_back(value);
        input.push_back(0.0F);
    }

    // 1e-12 (about 2^-40) lies above the floor, and decays through it after about 11,000 samples.
    for (std::size_t i = 0; i < 4'800; ++i) {
        const double phase = 2.0 * std::numbers::pi * 1'000.0 * static_cast<double>(i) / 48'000.0;
        input.push_back(1.0e-12F * static_cast<float>(std::sin(phase)));
    }

    input.resize(input.size() + (std::size_t{3} * 48'000), 0.0F);

    return input;
}

std::vector<std::uint32_t> filtered_bits(const std::vector<float>& input)
{
    dc_blocker                 blocker{rate_48k};
    std::vector<std::uint32_t> bits;

    bits.reserve(input.size());

    for (const float sample : input) {
        bits.push_back(std::bit_cast<std::uint32_t>(blocker.process(sample)));
    }

    return bits;
}

TEST_CASE("the DC blocker's corner is at 5 Hz, and the audio band passes")
{
    // Measured: -3.01 dB at 5 Hz, -0.26 dB at 20 Hz, +0.003 dB at 1 kHz.
    CHECK(settled_peak(5.0) == doctest::Approx(0.7072).epsilon(0.001));
    CHECK(settled_peak(20.0) == doctest::Approx(0.9704).epsilon(0.001));
    CHECK(settled_peak(1'000.0) == doctest::Approx(1.0003).epsilon(0.0001));
}

TEST_CASE("a constant offset is removed completely, 1.1 s after it starts")
{
    dc_blocker blocker{rate_48k};

    // y[n] = r^n, which falls below the floor and becomes exactly zero at sample 52,934. Without
    // the floor it would only approach zero, and go subnormal at 2.78 s.
    std::size_t first_zero = 0;

    while (first_zero < 60'000 && blocker.process(1.0F) != 0.0F) {
        ++first_zero;
    }

    CHECK(first_zero == 52'934U);

    std::size_t nonzero_after = 0;

    for (std::size_t i = 0; i < std::size_t{4} * 48'000; ++i) {
        nonzero_after += blocker.process(1.0F) != 0.0F ? 1U : 0U;
    }

    CHECK(nonzero_after == 0U);
}

TEST_CASE("a minute of silence after full scale leaves no subnormal, and settles to zero")
{
    dc_blocker                            blocker{rate_48k};
    std::mt19937                          generator{1234U};
    std::uniform_real_distribution<float> noise{-1.0F, 1.0F};

    std::size_t subnormals = 0;

    for (std::size_t i = 0; i < 48'000; ++i) {
        subnormals += is_subnormal(blocker.process(noise(generator))) ? 1U : 0U;
    }

    // Measured: this seed settles 0.95 s into the silence, the worst of seeds 1 to 2,000 at 1.0 s.
    // Any input within ±1 settles by about 1.13 s: the output then stays within ±2 (the impulse
    // response sums to 2 in magnitude), and 2r^n falls below 2^-50 at n = 53,992.
    constexpr std::size_t settled = 57'600;  // 1.2 s

    std::size_t nonzero_after_settled = 0;

    for (std::size_t i = 0; i < std::size_t{60} * 48'000; ++i) {
        const float output = blocker.process(0.0F);

        subnormals += is_subnormal(output) ? 1U : 0U;
        nonzero_after_settled += i >= settled && output != 0.0F ? 1U : 0U;
    }

    CHECK(subnormals == 0U);
    CHECK(nonzero_after_settled == 0U);
}

TEST_CASE("the audio floor keeps 2^-50 and flushes the next float below it")
{
    static_assert(flush_non_audio(audio_floor) == audio_floor);
    static_assert(flush_non_audio(-audio_floor) == -audio_floor);
    static_assert(flush_non_audio(next_towards_zero(audio_floor)) == 0.0F);
    static_assert(flush_non_audio(-next_towards_zero(audio_floor)) == 0.0F);
    static_assert(flush_non_audio(std::numeric_limits<float>::denorm_min()) == 0.0F);
    static_assert(flush_non_audio(1.0F) == 1.0F);

    SUBCASE("an input below the floor is silence")
    {
        // Unflushed, 2^-51 would stay behind as the previous input, and the next output would be
        // 2^-50 - 2^-51, below the floor.
        dc_blocker blocker{rate_48k};

        CHECK(blocker.process(0x1p-51F) == 0.0F);
        CHECK(blocker.process(audio_floor) == audio_floor);
    }

    SUBCASE("state below the floor is zero")
    {
        // The second output would be -(1 - r) × 2^-50, about -5.8e-19: normal, but below the floor.
        dc_blocker blocker{rate_48k};

        CHECK(blocker.process(audio_floor) == audio_floor);
        CHECK(blocker.process(0.0F) == 0.0F);
    }
}

TEST_CASE("a value that is not finite is silence, and leaves nothing behind")
{
    constexpr float nan     = std::numeric_limits<float>::quiet_NaN();
    constexpr float inf     = std::numeric_limits<float>::infinity();
    constexpr float largest = std::numeric_limits<float>::max();

    static_assert(flush_non_audio(nan) == 0.0F);
    static_assert(flush_non_audio(inf) == 0.0F);
    static_assert(flush_non_audio(-inf) == 0.0F);
    static_assert(flush_non_audio(largest) == largest);
    static_assert(flush_non_audio(-largest) == -largest);

    SUBCASE("in the input")
    {
        // A Rack module upstream can send one. Unflushed, a NaN stays in the state for ever, and
        // an infinity becomes a NaN at the next sample (inf - inf). Either way the blocker gives
        // exactly what it gives for a zero in its place.
        for (const float bad : {nan, inf, -inf}) {
            CAPTURE(bad);

            dc_blocker given_bad{rate_48k};
            dc_blocker given_zero{rate_48k};

            std::size_t differences = 0;

            const auto both = [&](float input, float in_place_of_it) {
                const float output = given_bad.process(input);
                const float wanted = given_zero.process(in_place_of_it);

                differences +=
                    std::bit_cast<std::uint32_t>(output) == std::bit_cast<std::uint32_t>(wanted)
                        ? 0U
                        : 1U;
            };

            for (std::size_t i = 0; i < 100; ++i) {
                both(0.5F, 0.5F);
            }

            both(bad, 0.0F);

            for (std::size_t i = 0; i < 48'000; ++i) {
                both(0.25F, 0.25F);
            }

            CHECK(differences == 0U);
        }
    }

    SUBCASE("in the state")
    {
        // Finite input can overflow: the largest float minus its negative is infinite. That
        // output is zero, and the blocker carries on.
        dc_blocker blocker{rate_48k};
        const auto coefficient = std::bit_cast<float>(0x3F7F'D51BU);  // r at 48 kHz

        CHECK(blocker.process(largest) == largest);
        CHECK(blocker.process(-largest) == 0.0F);
        CHECK(blocker.process(0.0F) == largest);
        CHECK(blocker.process(0.0F) == coefficient * largest);
    }
}

TEST_CASE("every multiply and add rounds on its own: one output, to the bit")
{
    // ADR 0005: a build that let the compiler fuse r × y[n-1] + (x[n] - x[n-1]) into one
    // multiply-add would fail here on arm64. Baseline x86-64 has no FMA, so it passes there either
    // way. The impulse's second output cancels almost exactly, so the fused result differs by 205
    // ulps, not one.
    dc_blocker blocker{rate_48k};

    const float first  = blocker.process(0.1F);
    const float second = blocker.process(0.0F);

    CHECK(std::bit_cast<std::uint32_t>(first) == 0x3DCC'CCCDU);  // 0.1F itself
    CHECK(std::bit_cast<std::uint32_t>(second) == 0xB889'4400U);

    // The same arithmetic fused, to show the check above can tell the difference. r at 48 kHz
    // is 0x3F7FD51B.
    const auto  coefficient = std::bit_cast<float>(0x3F7F'D51BU);
    const float fused       = std::fma(coefficient, 0.1F, 0.0F - 0.1F);

    CHECK(std::bit_cast<std::uint32_t>(fused) == 0xB889'4333U);
}

TEST_CASE("the DC blocker gives the same bits with subnormals flushed to zero as without")
{
    // ADR 0005: Rack's engine thread flushes subnormals and the host does not. The engine never
    // makes one, so the mode cannot matter.
    const std::vector<float> input = subnormal_bait();

    const std::vector<std::uint32_t> kept = filtered_bits(input);

    std::vector<std::uint32_t> flushed;
    {
        const test::flush_to_zero_mode mode;

        // The mode really is on: the smallest normal float halved is subnormal, and so becomes
        // zero. volatile keeps the compiler from working it out in advance.
        const volatile float smallest = std::numeric_limits<float>::min();
        REQUIRE(smallest / 2.0F == 0.0F);

        flushed = filtered_bits(input);
    }

    const volatile float smallest = std::numeric_limits<float>::min();
    REQUIRE(is_subnormal(smallest / 2.0F));

    CHECK(kept == flushed);
    CHECK(std::ranges::none_of(kept, [](std::uint32_t bits) {
        return is_subnormal(std::bit_cast<float>(bits));
    }));
}

}  // namespace
}  // namespace previously
