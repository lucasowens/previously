// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: what the tape's tests share.

#include "tape_fixtures.hpp"

#include "previously/dc_blocker.hpp"

#include <doctest/doctest.h>

#include <bit>
#include <cstdint>
#include <exception>
#include <random>

namespace previously::test {

bool same_bits(const stereo_frame& a, const stereo_frame& b) noexcept
{
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

stereo_noise::stereo_noise(std::size_t frames, sample_rate rate)
    : left(frames),
      right(frames)
{
    std::mt19937                          generator{1234U};
    std::uniform_real_distribution<float> noise{-1.0F, 1.0F};

    for (std::size_t i = 0; i < frames; ++i) {
        left[i]  = noise(generator);
        right[i] = noise(generator) + 0.5F;
    }

    dc_blocker left_blocker{rate};
    dc_blocker right_blocker{rate};

    dc_blocked.reserve(frames);

    for (std::size_t i = 0; i < frames; ++i) {
        dc_blocked.push_back(stereo_frame{
            .left  = left_blocker.process(left[i]),
            .right = right_blocker.process(right[i]),
        });
    }
}

page_audit::page_audit(page_pool& pool) noexcept
    : pool_{&pool},
      exceptions_at_start_{std::uncaught_exceptions()}
{
}

page_audit::~page_audit()
{
    // A failed REQUIRE has reported itself already; only a test that ran to its end is audited.
    if (std::uncaught_exceptions() == exceptions_at_start_) {
        CHECK(pool_->free_count() == pool_->page_count());
    }

    // Bounded: a page has at most 255 holders.
    for (std::size_t i = 0; i < pool_->page_count(); ++i) {
        const auto page = static_cast<page_pool::page_id>(i);

        while (pool_->holders(page) > 0) {
            pool_->release(page);
        }
    }
}

small_pool::small_pool(std::size_t pages, std::size_t extra_frames)
    : memory((pages * static_cast<std::size_t>(page_frames.value())) + extra_frames),
      pool{memory, page_frames},
      audit{pool}
{
}

}  // namespace previously::test
