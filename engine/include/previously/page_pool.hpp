// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the page pool, the memory the history and every loop are made of.
//
// Loop audio lives in one region of memory, carved into fixed-size pages (DESIGN.md §2.3,
// ADR 0004). The recorder takes a free page whenever its current one fills, and a loop holds the
// pages its window touches. Audio is never copied: holding a page keeps its audio.
//
// Each page counts its holders. Taking a free page makes its caller the first holder, anyone else
// who needs the audio adds themselves, and the page goes back to the pool when the last holder
// lets go. Taking and returning are O(1): free pages wait on a stack, so the page returned last
// is the next one taken.
//
// The pool keeps its bookkeeping in its own members, and never reads or writes the region. On
// the Daisy the region is SDRAM, which is set up after static constructors have run.

#pragma once

#include "previously/units.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace previously {

/// One stereo frame as the tape stores it: both channels side by side, so a frame is one read.
struct stereo_frame {
    float left;
    float right;
};

/// The smallest power of two lasting at least 2^15 frames at 48 kHz (0.68267 s): 2^15 at 44.1 and
/// 48 kHz, 2^16 at 88.2 and 96 kHz, 2^17 at 176.4 and 192 kHz. Every page count is then its
/// 48 kHz value or less, at any rate (DESIGN.md §2.3).
[[nodiscard]] constexpr samples page_frames_for(sample_rate rate) noexcept
{
    // In integers, so no rounding can move a boundary: the smallest power of two P with
    // P × 48,000 ≥ 2^15 × rate.
    constexpr std::uint64_t reference_frames  = std::uint64_t{1} << 15U;
    constexpr std::uint64_t reference_rate_hz = 48'000U;

    const std::uint64_t at_least =
        ((reference_frames * rate.hz()) + reference_rate_hz - 1U) / reference_rate_hz;

    return samples{static_cast<std::int64_t>(std::bit_ceil(at_least))};
}

class page_pool {
public:
    using page_id = std::uint16_t;

    /// The most pages a pool tracks: 52.5 MiB of float32 stereo at 48 kHz (DESIGN.md §2.3).
    static constexpr std::size_t max_pages = 210;

    /// Carves `memory` into pages of `page_frames` frames, a power of two. At most max_pages are
    /// used, and frames after the last whole page are left alone. Empty memory makes a pool with
    /// no pages. The memory must outlive the pool.
    page_pool(std::span<stereo_frame> memory, samples page_frames) noexcept;

    /// Expects every page to have been returned.
    ~page_pool();

    page_pool(const page_pool&)            = delete;
    page_pool& operator=(const page_pool&) = delete;
    page_pool(page_pool&&)                 = delete;
    page_pool& operator=(page_pool&&)      = delete;

    [[nodiscard]] std::size_t page_count() const noexcept;

    [[nodiscard]] std::size_t free_count() const noexcept;

    [[nodiscard]] samples page_frames() const noexcept;

    /// Takes a free page, with the caller as its only holder. Empty when every page is held.
    [[nodiscard]] std::optional<page_id> acquire() noexcept;

    /// Adds a holder to a page that is already held.
    void retain(page_id page) noexcept;

    /// Removes a holder. The last one to go returns the page to the pool.
    void release(page_id page) noexcept;

    /// How many hold the page: zero while it is free.
    [[nodiscard]] std::uint8_t holders(page_id page) const noexcept;

    /// The page's audio. Only a holder should read or write it.
    [[nodiscard]] std::span<stereo_frame> frames(page_id page) noexcept;

    [[nodiscard]] std::span<const stereo_frame> frames(page_id page) const noexcept;

private:
    std::span<stereo_frame> memory_;
    std::size_t             page_frames_;
    std::size_t             page_count_;
    std::size_t             free_count_;

    std::array<page_id, max_pages>      free_pages_{};  // a stack: the top is the next one taken
    std::array<std::uint8_t, max_pages> holders_{};
};

}  // namespace previously
