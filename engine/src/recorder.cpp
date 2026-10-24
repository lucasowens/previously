// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the recorder.

#include "previously/recorder.hpp"

#include "previously/contract.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace previously {
namespace {

/// The power of two a page size is.
[[nodiscard]] std::uint32_t log2_of(samples page_frames) noexcept
{
    const auto frames = static_cast<std::uint64_t>(page_frames.value());

    return static_cast<std::uint32_t>(std::countr_zero(frames));
}

/// The power of two a bucket is: a page's, less the 32 buckets in it. A page under 32 frames has a
/// bucket for each frame.
[[nodiscard]] std::uint32_t bucket_shift_for(std::uint32_t page_shift) noexcept
{
    static_assert(std::has_single_bit(recorder::buckets_per_page));

    constexpr auto buckets_shift =
        static_cast<std::uint32_t>(std::countr_zero(recorder::buckets_per_page));

    return page_shift > buckets_shift ? page_shift - buckets_shift : 0U;
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
      offset_mask_{static_cast<std::uint64_t>(pool.page_frames().value()) - 1U},
      bucket_shift_{bucket_shift_for(page_shift_)}
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

    const std::size_t frames            = left.size();
    const std::size_t frames_per_bucket = std::size_t{1} << bucket_shift_;

    // A run of frames per bucket, so the run's peaks are the bucket's. Buckets divide pages, so a
    // run never crosses a page either. Bounded: every run takes at least one frame.
    std::size_t done = 0;

    while (done < frames) {
        const std::int64_t page_number = page_number_of(end_);
        const std::size_t  offset      = offset_of(end_);

        if (page_number != newest_page_) {
            start_page(page_number);
        }

        const std::size_t into_bucket = offset & (frames_per_bucket - 1U);

        if (into_bucket == 0) {
            bucket_peak_left_  = 0.0F;
            bucket_peak_right_ = 0.0F;
        }

        const std::size_t run  = std::min(frames - done, frames_per_bucket - into_bucket);
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

            bucket_peak_left_  = std::max(bucket_peak_left_, std::abs(filtered.left));
            bucket_peak_right_ = std::max(bucket_peak_right_, std::abs(filtered.right));
        }

        // Kept even for a hole, where peaks_at() never reads it, so that every run is the same.
        peaks_[peak_slot_of(end_)] = peak_level{
            .left  = level_of(bucket_peak_left_),
            .right = level_of(bucket_peak_right_),
        };

        done += run;
        end_ += static_cast<std::int64_t>(run);
    }
}

samples recorder::end() const noexcept
{
    return samples{end_};
}

samples recorder::begin() const noexcept
{
    if (pool_->page_count() == 0U) {
        return samples{end_};
    }

    return samples{oldest_page() * static_cast<std::int64_t>(offset_mask_ + 1U)};
}

samples recorder::bucket_frames() const noexcept
{
    return samples{static_cast<std::int64_t>(std::uint64_t{1} << bucket_shift_)};
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

std::optional<peak_level> recorder::peaks_at(samples position) const noexcept
{
    if (!page_at(position)) {
        return std::nullopt;
    }

    return peaks_[peak_slot_of(position.value())];
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

std::size_t recorder::peak_slot_of(std::int64_t position) const noexcept
{
    const std::size_t page_slot = slot_of(page_number_of(position));

    return (page_slot * buckets_per_page) + (offset_of(position) >> bucket_shift_);
}

std::int64_t recorder::oldest_page() const noexcept
{
    return std::max(newest_page_ - static_cast<std::int64_t>(history_pages_) + 1, std::int64_t{0});
}

}  // namespace previously
