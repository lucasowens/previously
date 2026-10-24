// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: what the tape's tests share.

#include "tape_fixtures.hpp"

#include "previously/dc_blocker.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <exception>
#include <random>
#include <span>

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

    dc_blocked = dc_block(left, right, rate);
}

std::vector<stereo_frame>
dc_block(std::span<const float> left, std::span<const float> right, sample_rate rate)
{
    REQUIRE(left.size() == right.size());

    dc_blocker                left_blocker{rate};
    dc_blocker                right_blocker{rate};
    std::vector<stereo_frame> blocked;

    blocked.reserve(left.size());

    for (std::size_t i = 0; i < left.size(); ++i) {
        blocked.push_back(stereo_frame{
            .left  = left_blocker.process(left[i]),
            .right = right_blocker.process(right[i]),
        });
    }

    return blocked;
}

engine_feed::engine_feed(std::size_t frames, sample_rate rate)
    : input{frames, rate},
      out_left(frames),
      out_right(frames)
{
}

void engine_feed::process(engine&     target,
                          std::size_t from,
                          std::size_t to,
                          std::size_t block_frames)
{
    for (std::size_t start = from; start < to; start += block_frames) {
        const std::size_t count = std::min(block_frames, to - start);

        target.process(audio_block{
            .in_left   = std::span{input.left}.subspan(start, count),
            .in_right  = std::span{input.right}.subspan(start, count),
            .out_left  = std::span{out_left}.subspan(start, count),
            .out_right = std::span{out_right}.subspan(start, count),
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

}  // namespace previously::test
