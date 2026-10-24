// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the recorder, which keeps the newest audio on the tape at all times.
//
// Retro capture needs whatever was played in the last few bars, at any moment (DESIGN.md §3.4).
// The recorder writes the input to the tape without pause, through the DC blocker, one position
// per frame: the frame the engine received at sample n is at position n. Positions fill pages in
// order, so with P frames per page, position p is frame p mod P of page number p / P. P is a power
// of two, so that is a mask and a shift.
//
// The history is the newest pages, 46 in the engine (DESIGN.md §2.3). The recorder holds each of
// them, and a page leaves the history when the page 46 after it starts. It goes back to the pool
// then, unless a loop holds it too. The two share a slot in the recorder's ring, so the old page
// is always given back before the new one is taken: the recorder never holds more than the
// history.
//
// A page is free whenever one is needed: the pool has at least the history's pages, and today the
// recorder is their only holder. Captures will be admitted only while that stays true (§2.3).
// Should the pool be empty all the same, that page goes unrecorded and recording resumes on the
// next one. The history has a hole, and no loop's audio is touched. The DC blocker runs on through
// the hole, so what is recorded after it is unchanged.
//
// The display shows the history too, but it cannot read the tape: the pages under it are given
// back and taken again as it draws. So the recorder also keeps the peaks of each 1/32 of a page, a
// bucket, as levels (peak_level.hpp, DESIGN.md §9.3). A bucket's peaks live in its page's slot,
// and leave the history with the page.

#pragma once

#include "previously/dc_blocker.hpp"
#include "previously/page_pool.hpp"
#include "previously/peak_level.hpp"
#include "previously/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace previously {

class recorder {
public:
    /// The longest history a recorder keeps: the engine's (DESIGN.md §2.3).
    static constexpr std::size_t max_history_pages = 46;

    /// How many buckets a page's peaks are kept in. A page under 32 frames has one per frame.
    static constexpr std::size_t buckets_per_page = 32;

    /// Records into pages taken from `pool`, keeping the newest `history_pages` of them. A pool
    /// with pages must have at least that many; one without any leaves nothing recorded, and costs
    /// nothing per frame. The pool must outlive the recorder.
    recorder(page_pool& pool, sample_rate rate, std::size_t history_pages) noexcept;

    /// Gives the history's pages back to the pool.
    ~recorder();

    recorder(const recorder&)            = delete;
    recorder& operator=(const recorder&) = delete;
    recorder(recorder&&)                 = delete;
    recorder& operator=(recorder&&)      = delete;

    /// Audio thread. Records the next frames of input. Realtime-safe: O(frames), and one take
    /// and one give-back per page started.
    void record(std::span<const float> left, std::span<const float> right) noexcept;

    /// Where the next frame will be recorded: how many frames have been recorded so far.
    [[nodiscard]] samples end() const noexcept;

    /// Where the history begins: the first position of its oldest page. Nothing before it is kept,
    /// and without pages nothing is, so then it is end().
    [[nodiscard]] samples begin() const noexcept;

    /// How long a bucket is: 1/32 of a page, 1,024 frames (21.3 ms) at 48 kHz.
    [[nodiscard]] samples bucket_frames() const noexcept;

    /// The page holding `position`. Empty when the position is not recorded yet, is older than
    /// the history, or fell on a page the pool could not supply. A capture adds itself as a
    /// holder to keep the audio once the history has moved on.
    [[nodiscard]] std::optional<page_pool::page_id> page_at(samples position) const noexcept;

    /// The frame recorded at `position`, under the same conditions as page_at().
    [[nodiscard]] std::optional<stereo_frame> frame_at(samples position) const noexcept;

    /// The peaks of the bucket holding `position`: its loudest frame in each channel, of those
    /// recorded so far. Empty under the same conditions as page_at().
    [[nodiscard]] std::optional<peak_level> peaks_at(samples position) const noexcept;

private:
    using page_id = page_pool::page_id;

    // Marks a page the pool could not supply.
    static constexpr page_id no_page = std::numeric_limits<page_id>::max();
    static_assert(page_pool::max_pages <= no_page, "page ids must stay clear of no_page");

    void start_page(std::int64_t page_number) noexcept;

    // Positions are never negative, so they are split as unsigned.
    [[nodiscard]] std::int64_t page_number_of(std::int64_t position) const noexcept;
    [[nodiscard]] std::size_t  offset_of(std::int64_t position) const noexcept;

    [[nodiscard]] std::size_t  slot_of(std::int64_t page_number) const noexcept;
    [[nodiscard]] std::size_t  peak_slot_of(std::int64_t position) const noexcept;
    [[nodiscard]] std::int64_t oldest_page() const noexcept;

    page_pool*  pool_;  // never null
    dc_blocker  left_;
    dc_blocker  right_;
    std::size_t history_pages_;

    std::uint32_t page_shift_;    // position >> page_shift_ is the page number
    std::uint64_t offset_mask_;   // position & offset_mask_ is the frame within its page
    std::uint32_t bucket_shift_;  // offset >> bucket_shift_ is the bucket within its page
    std::int64_t  end_{0};

    // The history: page number n's page is in slot n mod history_pages_, or no_page for a hole
    // and before the slot's first page.
    std::array<page_id, max_history_pages> pages_{};
    std::int64_t                           newest_page_{-1};

    // The peaks: a slot's buckets in order, then the next slot's. The bucket being recorded holds
    // the levels of its peaks so far, and these two hold the peaks themselves.
    std::array<peak_level, max_history_pages * buckets_per_page> peaks_{};
    float                                                        bucket_peak_left_{0.0F};
    float                                                        bucket_peak_right_{0.0F};
};

}  // namespace previously
