// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the recorder and its history.
//
// Pages of 16 frames and a history of 4 pages, so every boundary is a few frames away. The
// engine's 2^15-frame pages and 46-page history are tested through the engine.

#include "previously/page_pool.hpp"
#include "previously/peak_level.hpp"
#include "previously/recorder.hpp"

#include "known.hpp"
#include "realtime_guard.hpp"
#include "tape_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

namespace previously {
namespace {

using test::known;
using test::same_bits;
using test::small_pool;
using test::stereo_noise;

constexpr sample_rate rate_48k{48'000U};
constexpr samples     page_frames   = small_pool::page_frames;
constexpr std::size_t history_pages = 4;

/// Noise lasting `pages` small pages.
stereo_noise noise_for(std::int64_t pages)
{
    return stereo_noise{static_cast<std::size_t>(pages * page_frames.value()), rate_48k};
}

/// Position `frame` of page `page`.
constexpr samples at(std::int64_t page, std::int64_t frame = 0)
{
    return samples{(page * page_frames.value()) + frame};
}

/// Records `signal` (anything with `left` and `right` vectors) from the recorder's end() up to
/// `until`, in blocks of the given sizes, over and over.
template <typename Signal>
void record_until(recorder&                          target,
                  const Signal&                      signal,
                  samples                            until,
                  std::initializer_list<std::size_t> block_sizes = {7})
{
    const std::span sizes{block_sizes};
    std::size_t     next_size = 0;

    while (target.end() < until) {
        const auto start = static_cast<std::size_t>(target.end().value());
        const auto count =
            std::min(sizes[next_size], static_cast<std::size_t>(until.value()) - start);

        target.record(std::span{signal.left}.subspan(start, count),
                      std::span{signal.right}.subspan(start, count));

        next_size = (next_size + 1) % sizes.size();
    }
}

/// True when the frame at `position` is recorded, and is exactly what the DC blockers made.
bool recorded_exactly(const recorder& target, const stereo_noise& signal, samples position)
{
    const auto frame = target.frame_at(position);

    return frame &&
           same_bits(*frame, signal.dc_blocked[static_cast<std::size_t>(position.value())]);
}

TEST_CASE("every frame is recorded at its position, DC-blocked, whatever the blocks")
{
    using block_sizes = std::initializer_list<std::size_t>;

    const stereo_noise signal = noise_for(4);

    // One frame at a time, blocks within a page, a block crossing one boundary or four, and a mix.
    for (const block_sizes sizes : {
             block_sizes{1},
             block_sizes{5},
             block_sizes{16},
             block_sizes{17},
             block_sizes{64},
             block_sizes{3, 29, 1, 15},
         }) {
        CAPTURE(*sizes.begin());
        CAPTURE(sizes.size());

        small_pool pages{6};
        recorder   target{pages.pool, rate_48k, history_pages};

        record_until(target, signal, at(4), sizes);

        CHECK(target.end() == at(4));

        for (std::int64_t position = 0; position < at(4).value(); ++position) {
            CAPTURE(position);
            CHECK(recorded_exactly(target, signal, samples{position}));
        }

        CHECK_FALSE(target.frame_at(samples{-1}).has_value());
        CHECK_FALSE(target.frame_at(at(4)).has_value());
    }
}

TEST_CASE("the history is the newest pages: its oldest frame is kept, the one before is not")
{
    const stereo_noise signal = noise_for(11);
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    // Pages 7 to 10, the last one partly recorded.
    record_until(target, signal, at(10, 7));

    CHECK(recorded_exactly(target, signal, at(7)));
    CHECK(recorded_exactly(target, signal, at(10, 6)));

    CHECK_FALSE(target.frame_at(at(7) - samples{1}).has_value());
    CHECK_FALSE(target.frame_at(at(10, 7)).has_value());
    CHECK_FALSE(target.page_at(at(7) - samples{1}).has_value());
}

TEST_CASE("a page leaves the history as the page four after it starts, and not before")
{
    const stereo_noise signal = noise_for(5);
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    // The last frame of page 3: four pages held, page 0 still in the history.
    record_until(target, signal, at(4), {1});

    CHECK(recorded_exactly(target, signal, at(0)));
    CHECK(pages.pool.free_count() == 2U);

    // The first frame of page 4. Page 0 leaves, and the three pages after it stay complete.
    record_until(target, signal, at(4, 1), {1});

    CHECK_FALSE(target.frame_at(at(1) - samples{1}).has_value());

    for (std::int64_t position = at(1).value(); position <= at(4).value(); ++position) {
        CAPTURE(position);
        CHECK(recorded_exactly(target, signal, samples{position}));
    }

    CHECK(pages.pool.free_count() == 2U);
}

TEST_CASE("the recorder never holds more pages than its history")
{
    // The pool has no page to spare, so taking a page before giving the oldest back would fail
    // and leave a hole.
    const stereo_noise signal = noise_for(21);
    small_pool         pages{history_pages};
    recorder           target{pages.pool, rate_48k, history_pages};

    for (std::int64_t page = 0; page <= 20; ++page) {
        record_until(target, signal, at(page, 1), {1});

        // Just after a page starts, the pages started so far are held, up to four: once full, the
        // history is three whole pages and one frame.
        const auto held = std::min(static_cast<std::size_t>(page) + 1U, history_pages);

        CHECK(pages.pool.free_count() == history_pages - held);

        for (std::int64_t position = std::max(at(page - 3).value(), std::int64_t{0});
             position <= at(page).value();
             ++position) {
            CAPTURE(position);
            REQUIRE(recorded_exactly(target, signal, samples{position}));
        }
    }
}

TEST_CASE("a page someone else holds outlives the history, and is not reused")
{
    const stereo_noise signal = noise_for(11);
    small_pool         pages{6};

    {
        recorder target{pages.pool, rate_48k, history_pages};

        record_until(target, signal, at(1));

        const page_pool::page_id kept = known(target.page_at(at(0)));
        pages.pool.retain(kept);

        record_until(target, signal, at(11));

        CHECK_FALSE(target.frame_at(at(0)).has_value());
        CHECK(pages.pool.holders(kept) == 1U);
        CHECK(pages.pool.free_count() == 1U);

        for (std::int64_t position = at(7).value(); position < at(11).value(); ++position) {
            CHECK(target.page_at(samples{position}) != kept);
        }

        // The audio is still there, as it was recorded.
        const auto frames = std::as_const(pages.pool).frames(kept);

        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK(same_bits(frames[i], signal.dc_blocked[i]));
        }

        pages.pool.release(kept);
        CHECK(pages.pool.free_count() == 2U);
    }

    CHECK(pages.pool.free_count() == 6U);
}

TEST_CASE("with no page free, a page goes unrecorded and recording resumes on the next one")
{
    const stereo_noise signal = noise_for(9);
    small_pool         pages{5};

    {
        recorder target{pages.pool, rate_48k, history_pages};

        record_until(target, signal, at(4));

        // Something else holds page 0's audio, and the last free page.
        const page_pool::page_id held  = known(target.page_at(at(0)));
        const page_pool::page_id spare = known(pages.pool.acquire());
        pages.pool.retain(held);

        record_until(target, signal, at(5));

        CHECK(target.end() == at(5));
        CHECK_FALSE(target.page_at(at(4)).has_value());
        CHECK_FALSE(target.frame_at(at(4)).has_value());
        CHECK_FALSE(target.frame_at(at(4, 15)).has_value());
        CHECK(recorded_exactly(target, signal, at(1)));
        CHECK(recorded_exactly(target, signal, at(3, 15)));

        // A page comes free, and the next page is recorded. The DC blocker ran on through the
        // hole, so the new page is exactly what it would have been.
        pages.pool.release(spare);
        record_until(target, signal, at(6));

        for (std::int64_t position = at(5).value(); position < at(6).value(); ++position) {
            CAPTURE(position);
            CHECK(recorded_exactly(target, signal, samples{position}));
        }

        CHECK_FALSE(target.frame_at(at(4)).has_value());

        // The hole's slot comes round again at page 8, and this time a page is free for it. Pages
        // 5 to 8 are the history, and page 0's audio is still held: all five pages are in use.
        record_until(target, signal, at(9));

        CHECK(pages.pool.free_count() == 0U);
        CHECK_FALSE(target.frame_at(at(4)).has_value());
        CHECK(recorded_exactly(target, signal, at(8)));
        CHECK(recorded_exactly(target, signal, at(8, 15)));

        pages.pool.release(held);
    }

    // The hole held no page, and every page is back.
    CHECK(pages.pool.free_count() == 5U);
}

TEST_CASE("an empty block records nothing and takes no page")
{
    const stereo_noise signal{1, rate_48k};
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    target.record({}, {});

    CHECK(target.end() == samples{0});
    CHECK(pages.pool.free_count() == 6U);

    record_until(target, signal, samples{1});

    CHECK(pages.pool.free_count() == 5U);
    CHECK(recorded_exactly(target, signal, samples{0}));
}

TEST_CASE("without pages, nothing is recorded and time still passes")
{
    const stereo_noise signal{100, rate_48k};
    small_pool         pages{0};
    recorder           target{pages.pool, rate_48k, history_pages};

    record_until(target, signal, samples{100});

    CHECK(target.end() == samples{100});

    for (std::int64_t position = 0; position < 100; ++position) {
        CHECK_FALSE(target.frame_at(samples{position}).has_value());
    }
}

TEST_CASE("the recorder gives every page back when it goes")
{
    const stereo_noise signal = noise_for(9);
    small_pool         pages{6};

    for (const samples until : {samples{0}, at(0, 1), at(2), at(8, 3)}) {
        CAPTURE(until.value());

        {
            recorder target{pages.pool, rate_48k, history_pages};
            record_until(target, signal, until);
        }

        CHECK(pages.pool.free_count() == 6U);
    }
}

TEST_CASE("recording allocates nothing, across page boundaries")
{
    const stereo_noise signal = noise_for(10);
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        record_until(target, signal, at(10), {1, 31, 16});
    }

    CHECK(test::realtime_guard::violations() == 0U);
}

TEST_CASE("the history begins at its oldest page, and without pages it holds nothing")
{
    const stereo_noise signal = noise_for(11);
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    CHECK(target.begin() == samples{0});

    record_until(target, signal, at(3, 5));
    CHECK(target.begin() == samples{0});

    // Page 4's first frame starts page 4, and page 0 leaves.
    record_until(target, signal, at(4, 1));
    CHECK(target.begin() == at(1));

    record_until(target, signal, at(10, 7));
    CHECK(target.begin() == at(7));

    // Without pages nothing is kept, so the history begins where it ends.
    small_pool none{0};
    recorder   empty{none.pool, rate_48k, history_pages};

    record_until(empty, signal, at(2, 3));
    CHECK(empty.begin() == empty.end());
}

// The peaks (DESIGN.md §9.3). Pages of 256 frames, so each of a page's 32 buckets is 8 frames.

using peak_pool = test::pool_of<256>;

constexpr std::int64_t bucket_frames = peak_pool::page_frames.value() / 32;
constexpr std::int64_t page_buckets  = 32;

/// Position `frame` of bucket `bucket`, counting buckets from the first.
constexpr samples in_bucket(std::int64_t bucket, std::int64_t frame = 0)
{
    return samples{(bucket * bucket_frames) + frame};
}

/// A stereo signal and what the recorder should make of it.
struct test_signal {
    std::vector<float>        left;
    std::vector<float>        right;
    std::vector<stereo_frame> dc_blocked;

    explicit test_signal(std::int64_t buckets)
        : left(static_cast<std::size_t>(buckets * bucket_frames)),
          right(left.size())
    {
    }

    void block_dc()
    {
        dc_blocked = test::dc_block(left, right, rate_48k);
    }
};

/// Noise whose loudness steps down 6 dB a bucket, from full scale to -48 dB and round again. The
/// right channel runs four steps behind the left, so the two channels' levels differ.
test_signal stepped_noise(std::int64_t buckets)
{
    const stereo_noise noise{static_cast<std::size_t>(buckets * bucket_frames), rate_48k};
    test_signal        signal{buckets};

    for (std::size_t i = 0; i < signal.left.size(); ++i) {
        const auto bucket = static_cast<int>(i / static_cast<std::size_t>(bucket_frames));

        signal.left[i]  = std::ldexp(noise.left[i], -(bucket % 9));
        signal.right[i] = std::ldexp(noise.right[i], -((bucket + 4) % 9));
    }

    signal.block_dc();
    return signal;
}

/// What the recorder should keep for `bucket`: its loudest DC-blocked frame, in each channel.
peak_level expected_peaks(const test_signal& signal, std::int64_t bucket)
{
    float left  = 0.0F;
    float right = 0.0F;

    for (std::int64_t frame = 0; frame < bucket_frames; ++frame) {
        const stereo_frame& blocked =
            signal.dc_blocked[static_cast<std::size_t>(in_bucket(bucket, frame).value())];

        left  = std::max(left, std::abs(blocked.left));
        right = std::max(right, std::abs(blocked.right));
    }

    return peak_level{.left = level_of(left), .right = level_of(right)};
}

TEST_CASE("each bucket's peaks are its loudest frame in each channel, whatever the blocks")
{
    using block_sizes = std::initializer_list<std::size_t>;

    const std::int64_t buckets = 4 * page_buckets;
    const test_signal  signal  = stepped_noise(buckets);

    // The stimulus is worth testing with: its buckets cover most levels, in both channels.
    std::array<bool, peak_level::max_level + 1> left_levels{};
    std::array<bool, peak_level::max_level + 1> right_levels{};

    for (std::int64_t bucket = 0; bucket < buckets; ++bucket) {
        left_levels[expected_peaks(signal, bucket).left]   = true;
        right_levels[expected_peaks(signal, bucket).right] = true;
    }

    REQUIRE(std::ranges::count(left_levels, true) >= 14);
    REQUIRE(std::ranges::count(right_levels, true) >= 14);

    // One frame at a time; within a bucket; exactly a bucket; across one; a page, and more; a mix.
    for (const block_sizes sizes : {
             block_sizes{1},
             block_sizes{7},
             block_sizes{8},
             block_sizes{9},
             block_sizes{256},
             block_sizes{300},
             block_sizes{3, 29, 1, 15},
         }) {
        CAPTURE(*sizes.begin());
        CAPTURE(sizes.size());

        peak_pool pages{6};
        recorder  target{pages.pool, rate_48k, history_pages};

        REQUIRE(target.bucket_frames() == samples{bucket_frames});

        record_until(target, signal, in_bucket(buckets), sizes);

        for (std::int64_t bucket = 0; bucket < buckets; ++bucket) {
            CAPTURE(bucket);

            // Any frame of a bucket finds the bucket's peaks.
            CHECK(target.peaks_at(in_bucket(bucket)) == expected_peaks(signal, bucket));
            CHECK(target.peaks_at(in_bucket(bucket, bucket_frames - 1)) ==
                  expected_peaks(signal, bucket));
        }
    }
}

TEST_CASE("the loudest frame counts wherever it falls in its bucket")
{
    // Silence, and in each bucket one frame louder than the rest, at every place in turn.
    test_signal signal{page_buckets};

    for (std::int64_t bucket = 0; bucket < page_buckets; ++bucket) {
        const auto place =
            static_cast<std::size_t>(in_bucket(bucket, bucket % bucket_frames).value());
        const auto level = static_cast<std::size_t>((bucket * 5) % peak_level::max_level);

        signal.left[place]  = level_thresholds[level];
        signal.right[place] = -level_thresholds[peak_level::max_level - 1 - level];
    }

    signal.block_dc();

    peak_pool pages{1};
    recorder  target{pages.pool, rate_48k, 1};

    record_until(target, signal, in_bucket(page_buckets), {5});

    for (std::int64_t bucket = 0; bucket < page_buckets; ++bucket) {
        CAPTURE(bucket);
        CHECK(target.peaks_at(in_bucket(bucket)) == expected_peaks(signal, bucket));
    }
}

TEST_CASE("a peak at a level's threshold reaches it, and one float below does not")
{
    for (const std::size_t level : {std::size_t{1}, std::size_t{9}, std::size_t{16}}) {
        CAPTURE(level);

        // After silence the DC blocker's state is exactly zero, so it passes one frame unchanged.
        const float at_threshold = level_thresholds[level - 1];
        const float just_below   = std::nextafter(at_threshold, 0.0F);

        test_signal signal{1};
        signal.left[3]  = at_threshold;
        signal.right[3] = -just_below;
        signal.block_dc();

        REQUIRE(signal.dc_blocked[3].left == at_threshold);
        REQUIRE(signal.dc_blocked[3].right == -just_below);

        peak_pool pages{1};
        recorder  target{pages.pool, rate_48k, 1};

        record_until(target, signal, in_bucket(1), {1});

        CHECK(target.peaks_at(in_bucket(0)) ==
              peak_level{.left  = static_cast<std::uint8_t>(level),
                         .right = static_cast<std::uint8_t>(level - 1)});
    }
}

TEST_CASE("the bucket being recorded shows the frames recorded so far")
{
    test_signal signal{4};
    signal.left[static_cast<std::size_t>(in_bucket(2, 5).value())] = level_thresholds[11];
    signal.block_dc();

    peak_pool pages{1};
    recorder  target{pages.pool, rate_48k, 1};

    // Up to the frame before the loud one: bucket 2 holds silence so far.
    record_until(target, signal, in_bucket(2, 5), {3});

    CHECK(target.peaks_at(in_bucket(2)) == peak_level{});
    CHECK_FALSE(target.peaks_at(in_bucket(2, 5)).has_value());

    // The loud frame arrives.
    record_until(target, signal, in_bucket(2, 6), {1});

    CHECK(target.peaks_at(in_bucket(2)) == peak_level{.left = 12, .right = 0});

    // The bucket ends, and the next one starts afresh: the DC blocker's tail is far below -45 dB.
    record_until(target, signal, in_bucket(3, 1), {1});

    CHECK(target.peaks_at(in_bucket(2)) == peak_level{.left = 12, .right = 0});
    CHECK(target.peaks_at(in_bucket(3)) == peak_level{});
}

TEST_CASE("peaks leave the history with their page, and a page's slot shows its own")
{
    const test_signal signal = stepped_noise(10 * page_buckets);
    peak_pool         pages{6};
    recorder          target{pages.pool, rate_48k, history_pages};

    // Pages 6 to 9. Page 6 shares its slot with page 2, which was recorded first.
    record_until(target, signal, in_bucket(10 * page_buckets));

    CHECK_FALSE(target.peaks_at(in_bucket(6 * page_buckets) - samples{1}).has_value());
    CHECK_FALSE(target.peaks_at(in_bucket(10 * page_buckets)).has_value());
    CHECK_FALSE(target.peaks_at(samples{-1}).has_value());

    for (std::int64_t bucket = 6 * page_buckets; bucket < 10 * page_buckets; ++bucket) {
        CAPTURE(bucket);
        CHECK(target.peaks_at(in_bucket(bucket)) == expected_peaks(signal, bucket));
    }
}

TEST_CASE("a hole has no peaks, and the pages around it keep theirs")
{
    const test_signal signal = stepped_noise(6 * page_buckets);
    peak_pool         pages{5};

    {
        recorder target{pages.pool, rate_48k, history_pages};

        record_until(target, signal, in_bucket(4 * page_buckets));

        // Something else holds page 0's audio, and the last free page: page 4 is a hole.
        const page_pool::page_id held  = known(target.page_at(samples{0}));
        const page_pool::page_id spare = known(pages.pool.acquire());
        pages.pool.retain(held);

        record_until(target, signal, in_bucket(5 * page_buckets));

        for (std::int64_t bucket = 4 * page_buckets; bucket < 5 * page_buckets; ++bucket) {
            CAPTURE(bucket);
            CHECK_FALSE(target.peaks_at(in_bucket(bucket)).has_value());
        }

        const std::int64_t last_before = (4 * page_buckets) - 1;
        CHECK(target.peaks_at(in_bucket(last_before)) == expected_peaks(signal, last_before));

        // A page comes free, and page 5 has its peaks.
        pages.pool.release(spare);
        record_until(target, signal, in_bucket(6 * page_buckets));

        for (std::int64_t bucket = 5 * page_buckets; bucket < 6 * page_buckets; ++bucket) {
            CAPTURE(bucket);
            CHECK(target.peaks_at(in_bucket(bucket)) == expected_peaks(signal, bucket));
        }

        pages.pool.release(held);
    }
}

TEST_CASE("with pages under 32 frames, each bucket is one frame")
{
    const stereo_noise signal = noise_for(6);
    small_pool         pages{6};
    recorder           target{pages.pool, rate_48k, history_pages};

    record_until(target, signal, at(6));

    REQUIRE(target.bucket_frames() == samples{1});

    for (std::int64_t position = at(2).value(); position < at(6).value(); ++position) {
        CAPTURE(position);

        const stereo_frame& blocked = signal.dc_blocked[static_cast<std::size_t>(position)];

        CHECK(target.peaks_at(samples{position}) ==
              peak_level{.left  = level_of(std::abs(blocked.left)),
                         .right = level_of(std::abs(blocked.right))});
    }
}

}  // namespace
}  // namespace previously
