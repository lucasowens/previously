// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the recorder.

#include "previously/recorder.hpp"

#include "previously/contract.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace previously {
namespace {

/// The power of two a page size is.
[[nodiscard]] std::uint32_t log2_of(samples page_frames) noexcept
{
    const auto frames = static_cast<std::uint64_t>(page_frames.value());

    return static_cast<std::uint32_t>(std::countr_zero(frames));
}

/// The history the recorder keeps: as asked, if the ring and the pool can hold it.
[[nodiscard]] std::size_t usable_history(std::size_t asked, const page_pool& pool) noexcept
{
    // Bad settings are programming errors, caught here in debug builds. They are also clamped, so
    // a release build still behaves sanely: a history longer than the pool would leave a hole in
    // every lap, and one longer than the ring would write past it.
    expects(asked >= 1U && asked <= recorder::max_history_pages);
    expects(pool.page_count() == 0U || pool.page_count() >= asked);

    const std::size_t within_ring = std::clamp(asked, std::size_t{1}, recorder::max_history_pages);

    return pool.page_count() == 0U ? within_ring : std::min(within_ring, pool.page_count());
}

}  // namespace

recorder::recorder(page_pool& pool, sample_rate rate, std::size_t history_pages) noexcept
    : pool_{&pool},
      left_{rate},
      right_{rate},
      history_pages_{usable_history(history_pages, pool)},
      page_shift_{log2_of(pool.page_frames())},
      offset_mask_{static_cast<std::uint64_t>(pool.page_frames().value()) - 1U}
{
    // Every slot starts empty, so a slot says for itself whether it holds a page.
    pages_.fill(no_page);
}

recorder::~recorder()
{
    for (const page_id page : std::span{pages_}.first(history_pages_)) {
        if (page != no_page) {
            pool_->release(page);
        }
    }
}

void recorder::record(std::span<const float> left, std::span<const float> right) noexcept
{
    expects(left.size() == right.size());

    // Without pages nothing is ever recorded, and the DC blockers' work could not be seen.
    if (pool_->page_count() == 0U) {
        end_ += static_cast<std::int64_t>(left.size());
        return;
    }

    const std::size_t frames      = left.size();
    const auto        page_frames = static_cast<std::size_t>(offset_mask_) + 1U;

    // A run of frames per page. Bounded: every run takes at least one frame.
    std::size_t done = 0;

    while (done < frames) {
        const std::int64_t page_number = page_number_of(end_);
        const std::size_t  offset      = offset_of(end_);

        if (page_number != newest_page_) {
            start_page(page_number);
        }

        const std::size_t run  = std::min(frames - done, page_frames - offset);
        const page_id     page = pages_[slot_of(page_number)];

        const std::span<stereo_frame> destination =
            page == no_page ? std::span<stereo_frame>{} : pool_->frames(page).subspan(offset, run);

        for (std::size_t i = 0; i < run; ++i) {
            const stereo_frame filtered{
                .left  = left_.process(left[done + i]),
                .right = right_.process(right[done + i]),
            };

            if (!destination.empty()) {
                destination[i] = filtered;
            }
        }

        done += run;
        end_ += static_cast<std::int64_t>(run);
    }
}

samples recorder::end() const noexcept
{
    return samples{end_};
}

std::optional<page_pool::page_id> recorder::page_at(samples position) const noexcept
{
    if (position.value() < 0 || position.value() >= end_) {
        return std::nullopt;
    }

    const std::int64_t page_number = page_number_of(position.value());

    if (page_number < oldest_page()) {
        return std::nullopt;
    }

    const page_id page = pages_[slot_of(page_number)];

    if (page == no_page) {
        return std::nullopt;
    }

    return page;
}

std::optional<stereo_frame> recorder::frame_at(samples position) const noexcept
{
    const auto page = page_at(position);

    if (!page) {
        return std::nullopt;
    }

    return std::as_const(*pool_).frames(*page)[offset_of(position.value())];
}

void recorder::start_page(std::int64_t page_number) noexcept
{
    const std::size_t slot = slot_of(page_number);

    // The page history_pages_ older leaves the history now, unless it was a hole. It shares this
    // slot, so it goes back before the new page is taken, and the pool needs one fewer free page.
    if (pages_[slot] != no_page) {
        pool_->release(pages_[slot]);
    }

    pages_[slot] = pool_->acquire().value_or(no_page);
    newest_page_ = page_number;
}

std::int64_t recorder::page_number_of(std::int64_t position) const noexcept
{
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(position) >> page_shift_);
}

std::size_t recorder::offset_of(std::int64_t position) const noexcept
{
    return static_cast<std::size_t>(static_cast<std::uint64_t>(position) & offset_mask_);
}

std::size_t recorder::slot_of(std::int64_t page_number) const noexcept
{
    return static_cast<std::size_t>(page_number % static_cast<std::int64_t>(history_pages_));
}

std::int64_t recorder::oldest_page() const noexcept
{
    return std::max(newest_page_ - static_cast<std::int64_t>(history_pages_) + 1, std::int64_t{0});
}

}  // namespace previously
