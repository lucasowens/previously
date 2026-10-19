// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the page pool.

#include "previously/page_pool.hpp"

#include "previously/contract.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace previously {
namespace {

/// The page size as an index, checked before anything divides by it.
[[nodiscard]] std::size_t checked_page_frames(samples page_frames) noexcept
{
    const auto frames = static_cast<std::uint64_t>(page_frames.value());

    expects(page_frames.value() > 0);
    expects(std::has_single_bit(frames));
    expects(std::in_range<std::size_t>(frames));

    return static_cast<std::size_t>(frames);
}

}  // namespace

page_pool::page_pool(std::span<stereo_frame> memory, samples page_frames) noexcept
    : memory_{memory},
      page_frames_{checked_page_frames(page_frames)},
      page_count_{std::min(memory.size() / page_frames_, max_pages)},
      free_count_{page_count_}
{
    // Stacked so that page 0 is taken first: a fresh pool hands out pages in order.
    for (std::size_t i = 0; i < page_count_; ++i) {
        free_pages_[i] = static_cast<page_id>(page_count_ - 1U - i);
    }
}

page_pool::~page_pool()
{
    expects(free_count_ == page_count_);
}

std::size_t page_pool::page_count() const noexcept
{
    return page_count_;
}

std::size_t page_pool::free_count() const noexcept
{
    return free_count_;
}

samples page_pool::page_frames() const noexcept
{
    return samples{static_cast<std::int64_t>(page_frames_)};
}

std::optional<page_pool::page_id> page_pool::acquire() noexcept
{
    if (free_count_ == 0) {
        return std::nullopt;
    }

    --free_count_;

    const page_id page = free_pages_[free_count_];

    holders_[page] = 1;

    return page;
}

void page_pool::retain(page_id page) noexcept
{
    expects(std::size_t{page} < page_count_);
    expects(holders_[page] > 0);
    expects(holders_[page] < std::numeric_limits<std::uint8_t>::max());

    ++holders_[page];
}

void page_pool::release(page_id page) noexcept
{
    expects(std::size_t{page} < page_count_);
    expects(holders_[page] > 0);

    --holders_[page];

    if (holders_[page] == 0) {
        free_pages_[free_count_] = page;
        ++free_count_;
    }
}

std::uint8_t page_pool::holders(page_id page) const noexcept
{
    expects(std::size_t{page} < page_count_);

    return holders_[page];
}

std::span<stereo_frame> page_pool::frames(page_id page) noexcept
{
    expects(std::size_t{page} < page_count_);

    return memory_.subspan(std::size_t{page} * page_frames_, page_frames_);
}

std::span<const stereo_frame> page_pool::frames(page_id page) const noexcept
{
    expects(std::size_t{page} < page_count_);

    return memory_.subspan(std::size_t{page} * page_frames_, page_frames_);
}

}  // namespace previously
