// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the UI model, what the display shows (DESIGN.md §9.3).
//
// The model is built from a clock tracker and a recorder, driven here as the engine drives them,
// at the engine's own sizes: 48 kHz, pages of 2^15 frames, buckets of 1,024 and a 46-page history.
// Every column is checked against what it should show, worked out in doubles from the grid's bar
// lines and from the signal itself, DC-blocked as the recorder does it.

#include "previously/clock_events.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/engine.hpp"
#include "previously/page_pool.hpp"
#include "previously/peak_level.hpp"
#include "previously/recorder.hpp"
#include "previously/sample_time.hpp"
#include "previously/ui_model.hpp"

#include "known.hpp"
#include "realtime_guard.hpp"
#include "tape_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace previously {
namespace {

using test::known;

constexpr sample_rate  rate_48k{48'000U};
constexpr std::int64_t second        = 48'000;  // in frames
constexpr std::int64_t bucket_frames = 1'024;
constexpr std::size_t  block_frames  = 32;

// 120 BPM at 4 PPQN: a pulse every 6,000 frames, a beat every 24,000, a bar of 4/4 every 96,000.
constexpr std::int64_t pulse_frames = 6'000;
constexpr std::int64_t bar_frames   = 16 * pulse_frames;

/// The columns where the bar slots start, worked out by hand, and 128 past the last.
constexpr std::array<std::size_t, ui_model::bar_slots + 1> slot_starts{0, 25, 51, 76, 102, 128};

static_assert(ui_model::slot_start(0) == 0);
static_assert(ui_model::slot_start(1) == 25);
static_assert(ui_model::slot_start(2) == 51);
static_assert(ui_model::slot_start(3) == 76);
static_assert(ui_model::slot_start(4) == 102);
static_assert(ui_model::slot_start(5) == ui_model::strip_columns);

[[nodiscard]] sample_time at_frame(std::int64_t frame)
{
    return sample_time::from_samples(samples{frame});
}

[[nodiscard]] double in_samples(sample_time time)
{
    return static_cast<double>(time.ticks()) / static_cast<double>(sample_time::ticks_per_sample);
}

/// Pulses every `period` frames from `first`, up to but not including `until`.
[[nodiscard]] std::vector<std::int64_t>
steady_pulses(std::int64_t first, std::int64_t period, std::int64_t until)
{
    std::vector<std::int64_t> pulses;

    for (std::int64_t time = first; time < until; time += period) {
        pulses.push_back(time);
    }

    return pulses;
}

/// A stereo signal, and what the recorder should make of it.
struct test_signal {
    std::vector<float>        left;
    std::vector<float>        right;
    std::vector<stereo_frame> dc_blocked;
};

/// Noise whose loudness steps down 6 dB every 3,000 frames, from full scale to -48 dB and round
/// again, so neighbouring columns differ. The right channel runs four steps behind the left.
[[nodiscard]] test_signal stepped_noise(std::int64_t frames)
{
    const test::stereo_noise noise{static_cast<std::size_t>(frames), rate_48k};
    test_signal              signal{.left = noise.left, .right = noise.right, .dc_blocked = {}};

    for (std::size_t i = 0; i < signal.left.size(); ++i) {
        const auto step = static_cast<int>(i / 3'000U);

        signal.left[i]  = std::ldexp(signal.left[i], -(step % 9));
        signal.right[i] = std::ldexp(signal.right[i], -((step + 4) % 9));
    }

    signal.dc_blocked = test::dc_block(signal.left, signal.right, rate_48k);
    return signal;
}

/// Silence, but for one frame in the left channel at level 14 (-6 dBFS). After silence the DC
/// blocker passes it unchanged, and its tail is far below -45 dBFS.
[[nodiscard]] test_signal one_loud_frame(std::int64_t frames, std::int64_t position)
{
    test_signal signal{
        .left       = std::vector<float>(static_cast<std::size_t>(frames)),
        .right      = std::vector<float>(static_cast<std::size_t>(frames)),
        .dc_blocked = {},
    };

    signal.left[static_cast<std::size_t>(position)] = level_thresholds[13];
    signal.dc_blocked = test::dc_block(signal.left, signal.right, rate_48k);
    return signal;
}

/// The parts of an engine the model is built from, run as the engine runs them: each block's
/// pulses, then the grid advanced to the block's end, then the block recorded onto a tape of 46
/// pages, exactly the history.
class scene {
public:
    /// Stepped noise for `frames`, and a clock edge at each of `pulses`.
    scene(std::int64_t frames, std::vector<std::int64_t> pulses)
        : scene{stepped_noise(frames), std::move(pulses)}
    {
    }

    scene(test_signal signal, std::vector<std::int64_t> pulses)
        : signal_{std::move(signal)},
          pulses_{std::move(pulses)},
          tape_(engine::history_pages *
                static_cast<std::size_t>(page_frames_for(rate_48k).value())),
          pool_{tape_, page_frames_for(rate_48k)},
          audit_{pool_},
          history_{pool_, rate_48k, engine::history_pages}
    {
    }

    /// Plays on until the history ends at `frame`, a multiple of the block.
    void play_until(std::int64_t frame)
    {
        REQUIRE(frame % static_cast<std::int64_t>(block_frames) == 0);
        REQUIRE(frame <= static_cast<std::int64_t>(signal_.left.size()));

        while (history_.end().value() < frame) {
            const std::int64_t start = history_.end().value();
            const std::int64_t end   = start + static_cast<std::int64_t>(block_frames);

            // The block's edges in time order. A pulse and a reset at once: the pulse first.
            while (true) {
                const bool pulse_due = next_pulse_ < pulses_.size() && pulses_[next_pulse_] < end;
                const bool reset_due = next_reset_ < resets_.size() && resets_[next_reset_] < end;

                if (pulse_due && (!reset_due || pulses_[next_pulse_] <= resets_[next_reset_])) {
                    grid_.pulse(at_frame(pulses_[next_pulse_]));
                    ++next_pulse_;
                }
                else if (reset_due) {
                    grid_.reset(at_frame(resets_[next_reset_]));
                    ++next_reset_;
                }
                else {
                    break;
                }
            }

            grid_.advance(at_frame(end));

            const auto offset = static_cast<std::size_t>(start);

            history_.record(std::span{signal_.left}.subspan(offset, block_frames),
                            std::span{signal_.right}.subspan(offset, block_frames));
        }
    }

    /// Adds a reset edge at `frame`, after any added already, and not yet played.
    void reset_at(std::int64_t frame)
    {
        REQUIRE(frame >= history_.end().value());
        REQUIRE((resets_.empty() || frame >= resets_.back()));

        resets_.push_back(frame);
    }

    /// The model, built once now.
    [[nodiscard]] ui_model model() const
    {
        ui_model built;
        update_ui_model(built, grid_, history_, rate_48k);

        return built;
    }

    [[nodiscard]] const clock_tracker& grid() const
    {
        return grid_;
    }

    [[nodiscard]] const recorder& history() const
    {
        return history_;
    }

    [[nodiscard]] page_pool& pool()
    {
        return pool_;
    }

    [[nodiscard]] const test_signal& signal() const
    {
        return signal_;
    }

    [[nodiscard]] std::span<const std::int64_t> pulses() const
    {
        return pulses_;
    }

    /// What a column covering frames [first, last) should show: the loudest of the buckets
    /// touching them that the history holds, each over its frames recorded so far.
    [[nodiscard]] strip_column expected_column(std::int64_t first, std::int64_t last) const
    {
        strip_column column;
        const auto   end = history_.end().value();

        for (std::int64_t bucket = first / bucket_frames; bucket * bucket_frames < last; ++bucket) {
            if (!history_.peaks_at(samples{bucket * bucket_frames})) {
                continue;
            }

            float left  = 0.0F;
            float right = 0.0F;

            for (std::int64_t frame = bucket * bucket_frames;
                 frame < std::min((bucket + 1) * bucket_frames, end);
                 ++frame) {
                const stereo_frame& blocked = signal_.dc_blocked[static_cast<std::size_t>(frame)];

                left  = std::max(left, std::abs(blocked.left));
                right = std::max(right, std::abs(blocked.right));
            }

            column.recorded    = true;
            column.peaks.left  = std::max(column.peaks.left, level_of(left));
            column.peaks.right = std::max(column.peaks.right, level_of(right));
        }

        return column;
    }

private:
    test_signal               signal_;
    std::vector<std::int64_t> pulses_;
    std::size_t               next_pulse_{0};
    std::vector<std::int64_t> resets_;
    std::size_t               next_reset_{0};

    clock_history<engine::clock_history_pulses> pulse_history_{};
    clock_tracker grid_{clock_tracker_config::gate(rate_48k), pulse_history_};

    std::vector<stereo_frame> tape_;
    page_pool                 pool_;
    test::page_audit          audit_;
    recorder                  history_;
};

/// Checks the time view against the newest 1,408 buckets, 11 to a column, newest at the right.
void check_time_view(const ui_model& model, const scene& played)
{
    CHECK(model.view == strip_view::time);
    CHECK(model.now_column == ui_model::strip_columns);

    const std::int64_t end = played.history().end().value();

    if (end == 0) {
        for (const strip_column& column : model.strip) {
            CHECK(column == strip_column{});
        }

        return;
    }

    const std::int64_t newest_group = ((end - 1) / bucket_frames) / 11;
    const std::int64_t group_frames = 11 * bucket_frames;

    for (std::size_t column = 0; column < ui_model::strip_columns; ++column) {
        CAPTURE(column);

        const std::int64_t group =
            newest_group - static_cast<std::int64_t>(ui_model::strip_columns - 1 - column);

        if (group < 0) {
            CHECK(model.strip[column] == strip_column{});
            continue;
        }

        CHECK(model.strip[column] ==
              played.expected_column(group * group_frames, (group + 1) * group_frames));
    }
}

/// Checks the bars view against the grid's bar lines: the bar in progress and the 4 before it, each
/// filling its slot, its columns dividing the bar evenly, from the first frame at or after each
/// division.
void check_bars_view(const ui_model& model, const scene& played)
{
    REQUIRE(model.view == strip_view::bars);

    const std::int64_t end = played.history().end().value();
    const std::int64_t bar = known(model.bar);

    // The earliest slots are bars the grid cannot place yet.
    std::size_t first_placed = ui_model::bar_slots - 1;

    while (first_placed > 0 &&
           played.grid().bar_start(bar - 4 + static_cast<std::int64_t>(first_placed) - 1)) {
        --first_placed;
    }

    CHECK(model.first_bar_slot == first_placed);

    std::size_t still_to_come = ui_model::strip_columns;

    for (std::size_t slot = 0; slot < ui_model::bar_slots; ++slot) {
        CAPTURE(slot);

        const std::int64_t slot_bar = bar - 4 + static_cast<std::int64_t>(slot);
        const auto         width = static_cast<double>(slot_starts[slot + 1] - slot_starts[slot]);

        for (std::size_t column = slot_starts[slot]; column < slot_starts[slot + 1]; ++column) {
            CAPTURE(column);

            if (slot < first_placed) {
                CHECK(model.strip[column] == strip_column{});
                continue;
            }

            const double start = in_samples(known(played.grid().bar_start(slot_bar)));
            const double span  = in_samples(known(played.grid().bar_start(slot_bar + 1))) - start;
            const auto   index = static_cast<double>(column - slot_starts[slot]);

            const auto first =
                static_cast<std::int64_t>(std::ceil(start + ((span * index) / width)));
            const auto last =
                static_cast<std::int64_t>(std::ceil(start + ((span * (index + 1.0)) / width)));

            if (first >= end) {
                still_to_come = std::min(still_to_come, column);
                CHECK(model.strip[column] == strip_column{});
                continue;
            }

            CHECK(model.strip[column] == played.expected_column(first, last));
        }
    }

    CHECK(model.now_column == still_to_come);
}

/// Checks the header against the grid at the block's last frame.
void check_header(const ui_model& model, const scene& played)
{
    const sample_time last_frame =
        sample_time::from_samples(played.history().end()) - sample_time::from_ticks(1);

    CHECK(model.clock == played.grid().state());
    CHECK(model.beat_period == played.grid().beat_period());
    CHECK(model.rate == rate_48k);
    CHECK(model.bar == played.grid().bar_at_or_before(last_frame));
    CHECK(model.beats_per_bar == 4);

    if (const auto pulse = played.grid().pulse_at_or_before(last_frame)) {
        CHECK(model.beat == floor_modulo(*pulse, 16) / 4);
    }
    else {
        CHECK_FALSE(model.beat.has_value());
    }
}

TEST_CASE("without a clock the strip is time: the newest 1,408 buckets, 11 to a column")
{
    scene played{40 * second, {}};

    // Before anything is played, and after 5 s: the columns older than the start are empty.
    check_time_view(played.model(), played);

    played.play_until(5 * second);

    const ui_model early = played.model();
    check_time_view(early, played);
    check_header(early, played);
    CHECK(early.strip.front() == strip_column{});
    CHECK(early.strip.back().recorded);

    // After 40 s the history has filled, and every column holds sound.
    played.play_until(40 * second);

    const ui_model full = played.model();
    check_time_view(full, played);
    CHECK(std::ranges::all_of(full.strip, [](const strip_column& column) {
        return column.recorded;
    }));
}

TEST_CASE("the time view moves on a whole column every 11 buckets, and only then")
{
    scene played{20 * second, {}};

    // The first frame of a group of 11 buckets: its column has just begun.
    const std::int64_t group_frames = 11 * bucket_frames;
    const std::int64_t start        = 10 * group_frames;

    played.play_until(start + 32);
    const ui_model begun = played.model();
    check_time_view(begun, played);

    // The group's last block: nothing has moved, the newest column has filled.
    played.play_until(start + group_frames);
    const ui_model filled = played.model();
    check_time_view(filled, played);

    // One block into the next group: everything has moved one column to the left.
    played.play_until(start + group_frames + 32);
    const ui_model moved = played.model();
    check_time_view(moved, played);

    for (std::size_t column = 0; column + 1 < ui_model::strip_columns; ++column) {
        CAPTURE(column);
        CHECK(filled.strip[column] == begun.strip[column]);
        CHECK(moved.strip[column] == filled.strip[column + 1]);
    }
}

TEST_CASE("with a tempo the strip is bars: the last 4 whole bars and the bar in progress")
{
    scene played{20 * second, steady_pulses(0, pulse_frames, 20 * second)};

    // A quarter of the way through bar 6, and 10 blocks into its second beat.
    played.play_until((6 * bar_frames) + (bar_frames / 4) + 320);

    const ui_model model = played.model();

    CHECK(model.clock == clock_state::locked);
    CHECK(model.bar == 6);
    CHECK(model.beat == 1);
    CHECK(model.first_bar_slot == 0);
    CHECK(model.now_column > slot_starts[4]);
    CHECK(model.now_column < slot_starts[5]);

    check_header(model, played);
    check_bars_view(model, played);
}

TEST_CASE("the bar in progress fills its slot, and when it ends the strip moves on a bar")
{
    scene played{20 * second, steady_pulses(0, pulse_frames, 20 * second)};

    // Bar 6's last block, its very end, and the first block of bar 7. Up to bar 7's line the last
    // frame is bar 6's, so bar 6 is in progress, and by its last block every column of its slot
    // has begun. One block later bar 7 is in progress, with only its first column begun.
    struct moment {
        std::int64_t end;
        std::int64_t bar;
        std::size_t  now_column;
    };

    for (const moment expected : {
             moment{.end = (7 * bar_frames) - 32, .bar = 6, .now_column = ui_model::strip_columns},
             moment{.end = 7 * bar_frames, .bar = 6, .now_column = ui_model::strip_columns},
             moment{.end = (7 * bar_frames) + 32, .bar = 7, .now_column = slot_starts[4] + 1},
         }) {
        CAPTURE(expected.end);

        played.play_until(expected.end);

        const ui_model model = played.model();

        CHECK(model.bar == expected.bar);
        CHECK(model.now_column == expected.now_column);
        check_header(model, played);
        check_bars_view(model, played);
    }
}

TEST_CASE("a column starts at the first frame at or after its share of its bar")
{
    // At 120 BPM a bar is 96,000 frames, 768 past a whole number of buckets, so every fourth bar
    // line falls the same distance into a bucket. Here it is 403 frames in. In a slot of 26
    // columns the second column's share starts 3,692.3 frames into the bar, so its first frame,
    // 3,693 in, starts a bucket. One loud frame just before belongs to the first column alone.
    const std::int64_t first_pulse = (10 * bucket_frames) + 403;
    const std::int64_t bar_4       = first_pulse + (4 * bar_frames);
    const std::int64_t boundary    = bar_4 + 3'693;
    const std::int64_t end         = first_pulse + (7 * bar_frames) + (bar_frames / 2) + 13;

    REQUIRE(boundary % bucket_frames == 0);
    REQUIRE(end % static_cast<std::int64_t>(block_frames) == 0);

    scene played{one_loud_frame(end, boundary - 1), steady_pulses(first_pulse, pulse_frames, end)};

    played.play_until(end);

    // Bars 3 to 7, so bar 4 is in slot 1, 26 columns from column 25.
    const ui_model model = played.model();

    REQUIRE(model.bar == 7);
    CHECK(model.strip[25] == strip_column{.peaks = {.left = 14, .right = 0}, .recorded = true});
    CHECK(model.strip[26] == strip_column{.peaks = {.left = 0, .right = 0}, .recorded = true});
    check_bars_view(model, played);
}

TEST_CASE("each bar fills its slot whatever its length, through a change of tempo")
{
    // 4 bars at 120 BPM, then 80 BPM: a pulse every 9,000 frames, a bar of 144,000.
    constexpr std::int64_t          slow_bar = 144'000;
    std::vector<std::int64_t>       pulses   = steady_pulses(0, pulse_frames, 4 * bar_frames);
    const std::vector<std::int64_t> slower   = steady_pulses(4 * bar_frames, 9'000, 24 * second);

    pulses.insert(pulses.end(), slower.begin(), slower.end());

    scene played{24 * second, pulses};

    // Two bars into the slower tempo, then four: first the strip holds bars of both lengths.
    for (const std::int64_t end :
         {(4 * bar_frames) + (2 * slow_bar) + 64'000, (4 * bar_frames) + (4 * slow_bar) + 64'000}) {
        CAPTURE(end);

        played.play_until(end);

        const ui_model model = played.model();

        check_header(model, played);
        check_bars_view(model, played);
    }
}

TEST_CASE("after a reset the bars follow the new grid, whichever edge comes first")
{
    // Pulse 40 is half way through bar 2. A reset makes it pulse 0, the start of bar 0, so every
    // bar line moves by half a bar. The reset comes 48 frames before the pulse, or 48 after.
    const std::int64_t reset_pulse = 40 * pulse_frames;

    for (const std::int64_t lead : {std::int64_t{48}, std::int64_t{-48}}) {
        CAPTURE(lead);

        scene played{16 * second, steady_pulses(0, pulse_frames, 16 * second)};

        played.reset_at(reset_pulse - lead);

        // The block before the pulse. A reset that came first is pending, and the bars are
        // numbered as they will be once it applies.
        played.play_until(reset_pulse - 32);

        const ui_model pending = played.model();

        CHECK(pending.bar == (lead > 0 ? -1 : 2));
        check_header(pending, played);
        check_bars_view(pending, played);

        // Four and a half bars after the pulse: bar 4 is in progress, on lines from the pulse.
        played.play_until(reset_pulse + (4 * bar_frames) + (bar_frames / 2));

        const ui_model after = played.model();

        CHECK(after.bar == 4);
        CHECK(known(played.grid().bar_start(0)) == at_frame(reset_pulse));
        check_header(after, played);
        check_bars_view(after, played);
    }
}

TEST_CASE("at the fastest tempo, a bucket spans several columns and each of them shows it")
{
    // A pulse every 240 frames, 5 ms: the tracker takes pulses down to 4 ms apart. A bar is 3,840
    // frames, about 150 a column, so about 7 columns share each bucket.
    const std::int64_t fast_pulse = 240;
    const std::int64_t fast_bar   = 16 * fast_pulse;

    scene played{4 * second, steady_pulses(0, fast_pulse, 4 * second)};

    played.play_until((20 * fast_bar) + (fast_bar / 2));

    const ui_model model = played.model();

    CHECK(model.bar == 20);
    check_header(model, played);
    check_bars_view(model, played);
}

TEST_CASE("just after the clock starts, slots for bars the grid cannot place stay empty")
{
    // 10 s of sound before the first pulse.
    const std::int64_t first_pulse = 480'000;
    scene              played{20 * second, steady_pulses(first_pulse, pulse_frames, 20 * second)};

    // One pulse: the tempo is not known, so the strip is time, and the header knows bar 0.
    played.play_until(first_pulse + 3'200);

    const ui_model acquiring = played.model();

    CHECK(acquiring.clock == clock_state::acquiring);
    CHECK_FALSE(acquiring.beat_period.has_value());
    check_header(acquiring, played);
    check_time_view(acquiring, played);

    // Half way through bar 1: bars -3 to -1 were before the clock, and only bars 0 and 1 show.
    played.play_until(first_pulse + bar_frames + (bar_frames / 2));

    const ui_model started = played.model();

    CHECK(started.bar == 1);
    CHECK(started.first_bar_slot == 3);
    check_header(started, played);
    check_bars_view(started, played);

    for (std::size_t column = 0; column < slot_starts[3]; ++column) {
        CHECK(started.strip[column] == strip_column{});
    }
}

TEST_CASE("while the clock is stopped, the bars go on with the freewheeling grid")
{
    scene played{24 * second, steady_pulses(0, pulse_frames, 8 * bar_frames)};

    // A bar and a half after the last pulse.
    played.play_until((9 * bar_frames) + (bar_frames / 2));

    const ui_model model = played.model();

    CHECK(model.clock == clock_state::freewheeling);
    CHECK(model.bar == 9);
    check_header(model, played);
    check_bars_view(model, played);
}

TEST_CASE("at slow tempos the bars reach back past the history, and what is older is empty")
{
    // 20 BPM: a pulse every 36,000 frames, a bar of 576,000 (12 s). Five bars are 60 s, and the
    // history is 31.4 s.
    const std::int64_t slow_pulse = 36'000;
    const std::int64_t slow_bar   = 16 * slow_pulse;
    const std::int64_t end        = (6 * slow_bar) + (slow_bar / 2);

    scene played{end, steady_pulses(0, slow_pulse, end)};

    played.play_until(end);

    const ui_model model = played.model();

    CHECK(model.bar == 6);
    check_header(model, played);
    check_bars_view(model, played);

    // Bar 2 ended before the history begins, so its slot holds nothing. The history begins part
    // of the way through bar 3, so its slot starts empty and then holds sound.
    REQUIRE(played.history().begin().value() > 3 * slow_bar);
    REQUIRE(played.history().begin().value() < 4 * slow_bar);

    for (std::size_t column = 0; column < slot_starts[1]; ++column) {
        CHECK(model.strip[column] == strip_column{});
    }

    CHECK(model.strip[slot_starts[1]] == strip_column{});
    CHECK(model.strip[slot_starts[2] - 1].recorded);
}

TEST_CASE("a page the pool could not supply is an empty stretch of the strip")
{
    scene played{40 * second, {}};

    // Something else holds page 2 when its slot comes round again, so page 48 goes unrecorded.
    const std::int64_t page_frames = page_frames_for(rate_48k).value();

    played.play_until(3 * page_frames);

    const page_pool::page_id kept = known(played.history().page_at(samples{2 * page_frames}));
    played.pool().retain(kept);

    played.play_until(52 * page_frames);

    REQUIRE_FALSE(played.history().page_at(samples{48 * page_frames}).has_value());

    const ui_model model = played.model();
    check_time_view(model, played);

    // Page 48 is 32 buckets, so at least two whole columns of 11 fall inside it.
    const auto empty = std::ranges::count_if(model.strip, [](const strip_column& column) {
        return !column.recorded;
    });

    CHECK(empty >= 2);

    played.pool().release(kept);
}

TEST_CASE("the model counts the times it is built")
{
    scene    played{second, {}};
    ui_model model;

    played.play_until(4'096);

    update_ui_model(model, played.grid(), played.history(), rate_48k);
    CHECK(model.serial == 1U);

    update_ui_model(model, played.grid(), played.history(), rate_48k);
    CHECK(model.serial == 2U);
}

TEST_CASE("building the model allocates nothing, in either view")
{
    scene    played{20 * second, steady_pulses(10 * second, pulse_frames, 20 * second)};
    ui_model model;

    played.play_until(5 * second);

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        update_ui_model(model, played.grid(), played.history(), rate_48k);
    }

    CHECK(model.view == strip_view::time);

    played.play_until(19 * second);

    {
        const test::realtime_guard guard;
        update_ui_model(model, played.grid(), played.history(), rate_48k);
    }

    CHECK(model.view == strip_view::bars);
    CHECK(test::realtime_guard::violations() == 0U);
}

// Through the engine.

/// An engine fed a scene's signal and pulses, block by block.
class engine_run {
public:
    // Room for the most a block can hold, so a run allocates nothing once it is built.
    engine_run(engine& target, const scene& source)
        : target_{&target},
          source_{&source}
    {
        events_.reserve(clock_report::max_pulses);
        out_.reserve(3'072);
    }

    /// Processes the next block of `frames`, with the pulses that fall inside it.
    void step(std::size_t frames)
    {
        const std::int64_t start = target_->sample_clock().value();
        const std::int64_t end   = start + static_cast<std::int64_t>(frames);
        const auto         first = static_cast<std::size_t>(start);

        REQUIRE(end <= static_cast<std::int64_t>(source_->signal().left.size()));

        events_.clear();

        while (next_pulse_ < source_->pulses().size() && source_->pulses()[next_pulse_] < end) {
            events_.push_back(clock_event{
                .type   = clock_event_type::pulse,
                .offset = at_frame(source_->pulses()[next_pulse_] - start),
            });
            ++next_pulse_;
        }

        out_.resize(frames);

        target_->process(
            audio_block{
                .in_left   = std::span{source_->signal().left}.subspan(first, frames),
                .in_right  = std::span{source_->signal().right}.subspan(first, frames),
                .out_left  = out_,
                .out_right = out_,
            },
            events_);
    }

private:
    engine*                  target_;
    const scene*             source_;
    std::size_t              next_pulse_{0};
    std::vector<clock_event> events_;
    std::vector<float>       out_;
};

/// The engine's tape: exactly the history at 48 kHz.
[[nodiscard]] std::vector<stereo_frame> engine_tape()
{
    return std::vector<stereo_frame>(engine::history_pages *
                                     static_cast<std::size_t>(page_frames_for(rate_48k).value()));
}

TEST_CASE("the engine rebuilds its model at the end of each block that completes a bucket")
{
    const scene source{second, {}};

    // A block per bucket boundary crossed, however many it crosses; none for an empty block.
    for (const std::size_t frames : {std::size_t{32}, std::size_t{1'000}, std::size_t{3'000}}) {
        CAPTURE(frames);

        std::vector<stereo_frame> tape = engine_tape();
        engine     audio_engine{engine_config{.rate = rate_48k, .max_block_frames = 3'072}, tape};
        engine_run run{audio_engine, source};

        std::uint32_t expected = 0;

        while (audio_engine.sample_clock().value() + static_cast<std::int64_t>(frames) <= second) {
            const std::int64_t start = audio_engine.sample_clock().value();

            run.step(frames);

            if (start / bucket_frames != audio_engine.sample_clock().value() / bucket_frames) {
                ++expected;
            }

            CHECK(audio_engine.ui().serial == expected);
        }

        run.step(0);
        CHECK(audio_engine.ui().serial == expected);
    }
}

TEST_CASE(
    "at 96 kHz the model states its rate from the start, and is rebuilt as often as at 48 kHz")
{
    constexpr sample_rate rate_96k{96'000U};

    std::vector<stereo_frame> tape(engine::history_pages *
                                   static_cast<std::size_t>(page_frames_for(rate_96k).value()));
    engine audio_engine{engine_config{.rate = rate_96k, .max_block_frames = block_frames}, tape};

    REQUIRE(audio_engine.history().bucket_frames() == samples{2'048});

    // Before the first rebuild the model holds nothing yet, but at the engine's rate.
    CHECK(audio_engine.ui() == ui_model{.rate = rate_96k});

    std::vector<float> silence(block_frames);
    std::vector<float> out(block_frames);

    // One block short of a bucket, then the block that completes it.
    for (std::size_t block = 0; block < 2'048 / block_frames; ++block) {
        CHECK(audio_engine.ui().serial == 0U);
        CHECK(audio_engine.ui().rate == rate_96k);

        audio_engine.process(audio_block{
            .in_left   = silence,
            .in_right  = silence,
            .out_left  = out,
            .out_right = out,
        });
    }

    CHECK(audio_engine.ui().serial == 1U);
    CHECK(audio_engine.ui().rate == rate_96k);
}

TEST_CASE("the engine's model is what its grid and history give, at every rebuild")
{
    // Sound with no clock for 3 s, then 120 BPM: the strip starts as time and turns to bars.
    scene played{8 * second, steady_pulses(3 * second, pulse_frames, 8 * second)};

    std::vector<stereo_frame> tape = engine_tape();
    engine audio_engine{engine_config{.rate = rate_48k, .max_block_frames = block_frames}, tape};
    engine_run run{audio_engine, played};

    std::uint32_t seen     = 0;
    std::size_t   bar_view = 0;

    while (audio_engine.sample_clock().value() < 8 * second) {
        run.step(block_frames);
        played.play_until(audio_engine.sample_clock().value());

        if (audio_engine.ui().serial == seen) {
            continue;
        }

        seen = audio_engine.ui().serial;

        ui_model expected = played.model();
        expected.serial   = seen;

        REQUIRE(audio_engine.ui() == expected);

        if (expected.view == strip_view::bars) {
            ++bar_view;
        }
    }

    // Both views were compared, many times.
    CHECK(seen == (8 * second) / bucket_frames);
    CHECK(bar_view > 100);
    CHECK(bar_view < seen - 100);
}

TEST_CASE("without a tape the strip is empty, and the clock still shows")
{
    const scene source{8 * second, steady_pulses(0, pulse_frames, 8 * second)};
    engine      audio_engine{engine_config{.rate = rate_48k, .max_block_frames = block_frames}};
    engine_run  run{audio_engine, source};

    while (audio_engine.sample_clock().value() < (3 * bar_frames) + 4'096) {
        run.step(block_frames);
    }

    const ui_model& model = audio_engine.ui();

    CHECK(model.clock == clock_state::locked);
    CHECK(model.bar == 3);
    CHECK(model.view == strip_view::bars);
    CHECK(std::ranges::none_of(model.strip, [](const strip_column& column) {
        return column.recorded;
    }));
}

TEST_CASE("processing allocates nothing while the model is rebuilt, in either view")
{
    const scene source{6 * second, steady_pulses(2 * second, pulse_frames, 6 * second)};

    std::vector<stereo_frame> tape = engine_tape();
    engine audio_engine{engine_config{.rate = rate_48k, .max_block_frames = block_frames}, tape};
    engine_run run{audio_engine, source};

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;

        while (audio_engine.sample_clock().value() < 6 * second) {
            run.step(block_frames);
        }
    }

    CHECK(audio_engine.ui().view == strip_view::bars);
    CHECK(test::realtime_guard::violations() == 0U);
}

}  // namespace
}  // namespace previously
