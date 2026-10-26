// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: playback, the loop bar-locked at native speed (DESIGN.md §3.6, §5.7).
//
// As in test_capture.cpp: 120 BPM in 4/4 at 48 kHz, the clock from sample 0, so a pulse is 6,000
// samples, a bar 96,000, and bar n's line is at n × 96,000. F, the entry into a downbeat, is 384
// samples; a seek, 144; the shortest entry, 96; the correlation window, 2,400. The input is noise
// until the loop is captured, and silence after, so that the output is the loop alone.

#include "previously/audio_floor.hpp"
#include "previously/capture.hpp"
#include "previously/engine.hpp"
#include "previously/loop_player.hpp"

#include "capture_fixtures.hpp"
#include "planted_clock.hpp"
#include "realtime_guard.hpp"
#include "seam_fixtures.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace previously {
namespace {

using test::at_sample;
using test::check_holders;
using test::clocked_engine;
using test::frame_held;
using test::tape_of;

constexpr sample_rate  rate_48k{48'000U};
constexpr std::int64_t pulses_per_bar = 16;
constexpr std::int64_t pulse_samples  = 6'000;
constexpr std::int64_t bar_samples    = pulses_per_bar * pulse_samples;
constexpr std::int64_t entry          = 384;
constexpr std::int64_t seek           = 144;
constexpr std::int64_t window         = 2'400;

constexpr std::size_t p1_pages = 187;

[[nodiscard]] constexpr std::int64_t line(std::int64_t bar)
{
    return bar * bar_samples;
}

[[nodiscard]] engine_config blocks_of(std::size_t frames)
{
    return engine_config{.rate = rate_48k, .max_block_frames = frames};
}

/// Noise until `until`, and silence from then on, the same at any block size.
[[nodiscard]] auto noise_until(std::int64_t until)
{
    return [until](std::int64_t sample) {
        if (sample >= until) {
            return stereo_frame{};
        }

        test::test_noise noise{static_cast<std::uint64_t>(sample) + 1U};

        return stereo_frame{
            .left  = static_cast<float>(noise.uniform(0.5)),
            .right = static_cast<float>(noise.uniform(0.5)),
        };
    };
}

/// The correlation the player measures between two windows of frames, each read from its first
/// frame on (DESIGN.md §5.7): silence on either side counts as correlated.
template <typename First, typename Second>
[[nodiscard]] float correlation_between(const First& a, const Second& b)
{
    float products = 0.0F;
    float energy_a = 0.0F;
    float energy_b = 0.0F;

    for (std::int64_t i = 0; i < window; ++i) {
        const stereo_frame x = a(i);
        const stereo_frame y = b(i);

        products += (x.left * y.left) + (x.right * y.right);
        energy_a += (x.left * x.left) + (x.right * x.right);
        energy_b += (y.left * y.left) + (y.right * y.right);
    }

    if (energy_a == 0.0F || energy_b == 0.0F) {
        return 1.0F;
    }

    return std::clamp(products / (std::sqrt(energy_a) * std::sqrt(energy_b)), 0.0F, 1.0F);
}

/// The same, between two windows of one loop's frames.
[[nodiscard]] float
correlation_of(const engine& audio, const captured_loop& loop, std::int64_t a, std::int64_t b)
{
    return correlation_between(
        [&](std::int64_t i) {
            return frame_held(audio, loop, a + i);
        },
        [&](std::int64_t i) {
            return frame_held(audio, loop, b + i);
        });
}

/// The output of a crossfade's sample `step` of `length`, from frame `a` into frame `b`.
[[nodiscard]] stereo_frame
faded(stereo_frame a, stereo_frame b, float rho, std::int64_t step, std::int64_t length)
{
    const auto  numerator = static_cast<float>((2 * step) + 1);
    const float x         = numerator / static_cast<float>(2 * length);
    const float in        = crossfade_gain(rho, x);
    const float out       = crossfade_gain(rho, 1.0F - x);

    return stereo_frame{
        .left  = flush_non_audio((out * a.left) + (in * b.left)),
        .right = flush_non_audio((out * a.right) + (in * b.right)),
    };
}

/// How many samples of [first, end) differ from `expected`.
template <typename Expected>
[[nodiscard]] std::int64_t mismatches(const clocked_engine& rig,
                                      std::int64_t          first,
                                      std::int64_t          end,
                                      const Expected&       expected)
{
    std::int64_t wrong = 0;

    for (std::int64_t n = first; n < end; ++n) {
        const stereo_frame got  = rig.output_at(n);
        const stereo_frame want = expected(n);

        if (got.left != want.left || got.right != want.right) {
            if (wrong < 3) {
                MESSAGE("sample " << n << ": " << got.left << " against " << want.left);
            }

            ++wrong;
        }
    }

    return wrong;
}

TEST_CASE("a late press starts its loop at once where it would be, and each bar on its line")
{
    // §5.7's example at bar 7: pressed 0.3 beat into bar 7, the capture holds bars 3 to 6. The
    // loop starts at once, 7,200 samples into its first bar, faded in from silence over a seek at
    // equal gain; plays on into bars 8, 9 and 10 with no crossfade; and at bar 11's line wraps,
    // entered from the 384 samples before bar 3's line.
    constexpr std::int64_t pressed = line(7) + 7'200;

    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};
    const engine&             audio = rig.target();

    rig.play(noise_until(pressed));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 13 * pulses_per_bar);
    rig.press(at_sample(pressed), capture_bars::four);
    rig.run_until(samples{pressed - (pressed % 480)});

    CHECK_FALSE(audio.loop().playing);

    rig.keep_output();
    rig.run_until(samples{line(9) + 480});

    // In bar 9, loop bar 2: bar 5.
    CHECK(audio.loop().playing);
    CHECK(audio.loop().bar == 2U);

    rig.run_until(samples{line(12)});

    const captured_loop& loop = audio.slot();

    REQUIRE(loop.bars() == 4U);
    REQUIRE(loop.lines().front().position == samples{line(3)});

    const auto played = [&](std::int64_t downbeat) {
        return [&audio, &loop, downbeat](std::int64_t n) {
            return frame_held(audio, loop, line(3) + (n - downbeat));
        };
    };

    // The seek from silence: the incoming stream alone, at gains rising from 1/288.
    CHECK(mismatches(rig, pressed, pressed + seek, [&](std::int64_t n) {
              return faded(stereo_frame{}, played(line(7))(n), 1.0F, n - pressed, seek);
          }) == 0);

    // Then on, through bars 8, 9 and 10, to the wrap's crossfade.
    CHECK(mismatches(rig, pressed + seek, line(11) - entry, played(line(7))) == 0);

    // The wrap: the end of bar 6 into the 384 before bar 3's line.
    const float rho = correlation_of(audio, loop, line(7) - window, line(3) - window);

    CHECK(mismatches(rig, line(11) - entry, line(11), [&](std::int64_t n) {
              return faded(frame_held(audio, loop, line(7) - (line(11) - n)),
                           frame_held(audio, loop, line(3) - (line(11) - n)),
                           rho,
                           n - (line(11) - entry),
                           entry);
          }) == 0);

    CHECK(mismatches(rig, line(11), line(12), played(line(11))) == 0);

    check_holders(audio);
}

TEST_CASE("an early press enters its first downbeat from its pre-roll, landing on its line")
{
    // Pressed before bar 7's line, the capture holds bars 3 to 6, and its first downbeat plays on
    // bar 7's line. With F or more of lead, the entry begins F before the line; with less, at the
    // press, shortened to the time left; with under 2 ms, there is none, and the loop starts at
    // the line with a seek as its pulse arrives, fading the downbeat in (DESIGN.md §5.7). From
    // silence, every crossfade is at equal gain.
    for (const std::int64_t lead :
         {std::int64_t{4'800}, entry, entry - 1, std::int64_t{96}, std::int64_t{95}}) {
        CAPTURE(lead);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{blocks_of(480), tape};
        const engine&             audio = rig.target();

        rig.play(noise_until(line(6)));
        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
        rig.press(at_sample(line(7) - lead), capture_bars::four);
        rig.keep_output();
        rig.run_until(samples{line(8)});

        const captured_loop& loop = audio.slot();

        REQUIRE(loop.bars() == 4U);
        REQUIRE(loop.lines().front().position == samples{line(3)});

        const auto held = [&](std::int64_t position) {
            return frame_held(audio, loop, position);
        };

        if (lead >= 96) {
            const std::int64_t length = std::min(lead, entry);
            const std::int64_t start  = line(7) - length;

            CHECK(mismatches(rig, start, line(7), [&](std::int64_t n) {
                      return faded({}, held(line(3) - (line(7) - n)), 1.0F, n - start, length);
                  }) == 0);
            CHECK(mismatches(rig, line(7), line(8), [&](std::int64_t n) {
                      return held(line(3) + (n - line(7)));
                  }) == 0);
        }
        else {
            CHECK(mismatches(rig, line(7) - 96, line(7), [](std::int64_t) {
                      return stereo_frame{};
                  }) == 0);
            CHECK(mismatches(rig, line(7), line(7) + seek, [&](std::int64_t n) {
                      return faded({}, held(line(3) + (n - line(7))), 1.0F, n - line(7), seek);
                  }) == 0);
            CHECK(mismatches(rig, line(7) + seek, line(8), [&](std::int64_t n) {
                      return held(line(3) + (n - line(7)));
                  }) == 0);
        }

        REQUIRE_FALSE(rig.downbeats().empty());
        CHECK(rig.downbeats().front().sample == line(7));
        CHECK(rig.downbeats().front().bar == 0U);
        check_holders(audio);
    }
}

TEST_CASE("an 8-bar loop plays each bar on its line, started late or entered early")
{
    // A press 0.3 beat into bar 9, or 0.2 beat before its line, holds bars 1 to 8. Late, the loop
    // starts at once in its first bar; early, its first downbeat is entered on bar 9's line.
    // Either way loop bar 1, the second of the eight, plays on bar 10's line, and so on, and bar
    // 17's line wraps.
    for (const std::int64_t pressed : {line(9) + 7'200, line(9) - 4'800}) {
        CAPTURE(pressed);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{blocks_of(480), tape};

        rig.play(noise_until(line(9) + 7'200));
        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 19 * pulses_per_bar);
        rig.press(at_sample(pressed), capture_bars::eight);
        rig.run_until(samples{line(18)});

        REQUIRE(rig.target().slot().bars() == 8U);

        std::vector<clocked_engine::played_downbeat> from_10;

        from_10.reserve(rig.downbeats().size());

        for (const clocked_engine::played_downbeat& beat : rig.downbeats()) {
            if (beat.sample >= line(10)) {
                from_10.push_back(beat);
            }
        }

        REQUIRE(from_10.size() == 8U);

        for (std::size_t i = 0; i < from_10.size(); ++i) {
            CHECK(from_10[i].sample == line(10 + static_cast<std::int64_t>(i)));
            CHECK(from_10[i].bar == (i + 1) % 8);
        }
    }
}

/// A 1-bar loop of bar 3, from a late press 0.3 beat into bar 4: it plays bar 3 in every bar.
void play_bar_3(clocked_engine& rig)
{
    rig.press(at_sample(line(4) + 7'200), capture_bars::one);
}

TEST_CASE("a loop whose pre-roll starts at the history's first frame wraps from audio it holds")
{
    // §5.3's guarantee, played (PR #21): just after power-up the history begins at sample 0, and
    // with the clock from sample 2,400, bar 0's line has exactly its 50 ms before it. A 1-bar loop
    // of bar 0 wraps at bar 2's line, and its correlation window before its first downbeat is that
    // pre-roll, from sample 0: every frame held, none read as the silence outside its pages.
    constexpr std::int64_t origin = 2'400;

    const auto at_line = [](std::int64_t bar) {
        return origin + line(bar);
    };

    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};
    const engine&             audio = rig.target();

    rig.play(noise_until(at_line(1) + 7'200));
    rig.steady_clock(at_sample(origin), at_sample(pulse_samples), 4 * pulses_per_bar);
    rig.press(at_sample(at_line(1) + 7'200), capture_bars::one);
    rig.keep_output();
    rig.run_until(samples{at_line(3)});

    const captured_loop& loop = audio.slot();

    REQUIRE(loop.held().first == samples{0});
    REQUIRE(at_line(0) - window == 0);

    const stereo_frame first = frame_held(audio, loop, 0);

    CHECK((first.left != 0.0F || first.right != 0.0F));

    const float rho = correlation_of(audio, loop, at_line(1) - window, at_line(0) - window);

    CHECK(mismatches(rig, at_line(2) - entry, at_line(2), [&](std::int64_t n) {
              return faded(frame_held(audio, loop, at_line(1) - (at_line(2) - n)),
                           frame_held(audio, loop, at_line(0) - (at_line(2) - n)),
                           rho,
                           n - (at_line(2) - entry),
                           entry);
          }) == 0);
}

TEST_CASE("an early press replacing a loop enters from that loop's audio, which leaves the slot")
{
    // Bar 3 plays as a 1-bar loop. A 2-bar press 0.2 beat before bar 7 holds bars 5 and 6, and
    // over the F before bar 7's line the loop's own audio crossfades into their pre-roll. The
    // leaving loop keeps its pages through the crossfade, and gives them back once it ends.
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(64), tape};
    const engine&             audio = rig.target();

    rig.play(noise_until(line(6)));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
    play_bar_3(rig);
    rig.press(at_sample(line(7) - 4'800), capture_bars::two);
    rig.run_until(samples{line(6)});

    const captured_loop* old_loop = &audio.slot();

    REQUIRE(old_loop->bars() == 1U);

    // Keep the old loop's frames before it can be given back.
    std::vector<stereo_frame> bar_3;

    bar_3.reserve(static_cast<std::size_t>(bar_samples + (2 * window)));

    for (std::int64_t i = -window; i < bar_samples + window; ++i) {
        bar_3.push_back(frame_held(audio, *old_loop, line(3) + i));
    }

    const auto old_at = [&](std::int64_t position) {
        return bar_3[static_cast<std::size_t>(position - line(3) + window)];
    };

    rig.keep_output();
    rig.run_until(samples{line(7) - 64});

    // In the entry's block: the old loop is still the slot's, and the new one waits.
    rig.run_until(samples{line(7)});
    CHECK(audio.loop().waiting);
    check_holders(audio);

    rig.run_until(samples{line(8)});

    const captured_loop& loop = audio.slot();

    REQUIRE(loop.bars() == 2U);
    CHECK(audio.leaving().empty());
    check_holders(audio);

    // The old loop's audio before its next natural downbeat, bar 4's line on its tape, and the new
    // loop's before bar 5's.
    const float rho = correlation_between(
        [&](std::int64_t i) {
            return old_at(line(4) - window + i);
        },
        [&](std::int64_t i) {
            return frame_held(audio, loop, line(5) - window + i);
        });

    CHECK(mismatches(rig, line(7) - entry - 64, line(7) - entry, [&](std::int64_t n) {
              return old_at(line(3) + (n - line(6)));
          }) == 0);
    CHECK(mismatches(rig, line(7) - entry, line(7), [&](std::int64_t n) {
              return faded(old_at(line(3) + (n - line(6))),
                           frame_held(audio, loop, line(5) - (line(7) - n)),
                           rho,
                           n - (line(7) - entry),
                           entry);
          }) == 0);
    CHECK(mismatches(rig, line(7), line(8), [&](std::int64_t n) {
              return frame_held(audio, loop, line(5) + (n - line(7)));
          }) == 0);
}

TEST_CASE("a late press replacing a loop seeks from that loop's audio, which leaves the slot")
{
    // Bar 3 plays as a 1-bar loop. A 2-bar late press 0.3 beat into bar 7 holds bars 5 and 6, and
    // starts at once: a seek from the old loop where it plays into the new one where it would be,
    // 7,200 samples into bar 5. In blocks of 64, the seek outlasts the press's block, and the old
    // loop keeps its pages until it ends.
    constexpr std::int64_t pressed = line(7) + 7'200;

    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(64), tape};
    const engine&             audio = rig.target();

    rig.play(noise_until(line(6)));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
    play_bar_3(rig);
    rig.press(at_sample(pressed), capture_bars::two);
    rig.run_until(samples{pressed});

    std::vector<stereo_frame> bar_3;

    bar_3.reserve(static_cast<std::size_t>(bar_samples));

    for (std::int64_t i = 0; i < bar_samples; ++i) {
        bar_3.push_back(frame_held(audio, audio.slot(), line(3) + i));
    }

    rig.keep_output();
    rig.run_until(samples{pressed + 64});

    REQUIRE(audio.slot().bars() == 2U);
    CHECK_FALSE(audio.leaving().empty());
    check_holders(audio);

    rig.run_until(samples{pressed + 192});

    CHECK(audio.leaving().empty());
    check_holders(audio);

    const captured_loop& loop = audio.slot();

    // The next 50 ms of each stream from the seek.
    const float rho = correlation_between(
        [&](std::int64_t i) {
            return bar_3[static_cast<std::size_t>(7'200 + i)];
        },
        [&](std::int64_t i) {
            return frame_held(audio, loop, line(5) + 7'200 + i);
        });

    CHECK(mismatches(rig, pressed, pressed + seek, [&](std::int64_t n) {
              return faded(bar_3[static_cast<std::size_t>(n - line(7))],
                           frame_held(audio, loop, line(5) + (n - line(7))),
                           rho,
                           n - pressed,
                           seek);
          }) == 0);
    CHECK(mismatches(rig, pressed + seek, pressed + 192, [&](std::int64_t n) {
              return frame_held(audio, loop, line(5) + (n - line(7)));
          }) == 0);
}

TEST_CASE("every downbeat lands within a sample of its line, and a moved one on its nearest sample")
{
    // Pulses 6,000.35 samples apart, exactly in ticks, so the grid's lines are exact and fall
    // between samples. A 1-bar loop plays for 64 bars: its own bar is a whole number of samples,
    // so its natural downbeat drifts off the line, and each time the drift reaches a sample the
    // downbeat moves back onto the line's nearest sample: within half a sample of the line. A
    // natural downbeat under a sample before its line is that line's, and the next line is the one
    // after it.
    const sample_time period = sample_time::from_ticks(393'239'938);  // 6,000.35 samples

    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};

    rig.play(noise_until(line(5)));
    rig.steady_clock(at_sample(0), period, 72 * pulses_per_bar);
    rig.press((period * (4 * pulses_per_bar)) + at_sample(7'200), capture_bars::one);
    rig.run_until(samples{(period * (70 * pulses_per_bar)).whole_samples()});

    const captured_loop& loop = rig.target().slot();
    const std::int64_t   loop_bar =
        loop.lines()[1].position.value() - loop.lines()[0].position.value();
    const auto& played = rig.downbeats();

    REQUIRE(played.size() >= 64U);

    std::size_t moved = 0;

    for (std::size_t i = 0; i < played.size(); ++i) {
        const clocked_engine::played_downbeat& beat = played[i];

        CAPTURE(beat.sample);

        // The line it plays on: the grid bar whose line is nearest.
        const auto bar = static_cast<std::int64_t>(
            std::llround(static_cast<double>(beat.sample) /
                         (static_cast<double>(period.ticks()) * pulses_per_bar / 65'536.0)));
        const sample_time line_time = period * (bar * pulses_per_bar);

        CHECK(abs(at_sample(beat.sample) - line_time) < at_sample(1));
        CHECK(beat.bar == 0U);

        if (i == 0) {
            continue;
        }

        // One downbeat a line: none again a sample later.
        CHECK(beat.sample - played[i - 1].sample > bar_samples / 2);

        if (beat.sample != played[i - 1].sample + loop_bar) {
            CHECK(abs(at_sample(beat.sample) - line_time) <= at_sample(1) / 2);
            ++moved;
        }
    }

    CHECK(moved >= 12U);
}

TEST_CASE("a natural downbeat a whole sample from its line moves onto the line")
{
    // A 1-bar loop of bar 3, and from bar 5's line pulses 6,000.0625 samples apart, exactly in
    // ticks: grid bars of 96,001 samples, their lines on whole samples. Each natural downbeat is
    // exactly a sample before its line, so not within a sample of it: every downbeat moves onto
    // its line.
    const sample_time period = sample_time::from_ticks(393'220'096);  // 6,000.0625 samples

    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};

    rig.play(noise_until(line(4) + 7'200));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 5 * pulses_per_bar);
    rig.steady_clock(at_sample(line(5)), period, 10 * pulses_per_bar);
    rig.press(at_sample(line(4) + 7'200), capture_bars::one);
    rig.run_until(samples{line(13)});

    const auto& played = rig.downbeats();

    REQUIRE(played.size() >= 8U);

    for (std::size_t i = 1; i < played.size(); ++i) {
        CAPTURE(played[i].sample);
        CHECK(played[i].sample == line(5) + (static_cast<std::int64_t>(i) * (bar_samples + 1)));
    }
}

TEST_CASE("on a slower clock each bar's end is heard again, and on a faster one cut short")
{
    // A 2-bar loop of bars 2 and 3 at 120 BPM, and the clock from bar 5's line at 119.5 or 120.5
    // BPM: each grid bar is then 401.67 samples longer, or 398.34 shorter, than the loop's. Once
    // the tracker has the new tempo, every downbeat is on its line, 401 or 402 samples after its
    // natural downbeat, or 398 or 399 before (§3.6's example). Between a later line's natural
    // downbeat and the line itself, the audio before the due downbeat plays: on the move from the
    // loop's first bar to its second, the end of the first, heard again.
    struct tempo {
        double                      bpm{120.0};
        std::array<std::int64_t, 2> moves{};
    };

    for (const tempo change :
         {tempo{.bpm = 119.5, .moves = {401, 402}}, tempo{.bpm = 120.5, .moves = {-399, -398}}}) {
        CAPTURE(change.bpm);

        const sample_time period =
            sample_time::from_ticks(std::llround(720'000.0 / change.bpm * 65'536.0));

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{blocks_of(480), tape};
        const engine&             audio = rig.target();

        rig.play(noise_until(line(4)));
        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 5 * pulses_per_bar);
        rig.steady_clock(at_sample(line(5)), period, 12 * pulses_per_bar);
        rig.press(at_sample(line(4) + 7'200), capture_bars::two);
        rig.keep_output();
        rig.run_until(samples{line(15)});

        const captured_loop& loop = audio.slot();

        REQUIRE(loop.bars() == 2U);

        const auto& played = rig.downbeats();

        // From bar 9 on, four bars after the change.
        std::size_t checked = 0;

        for (std::size_t i = 1; i < played.size(); ++i) {
            const clocked_engine::played_downbeat& before = played[i - 1];
            const clocked_engine::played_downbeat& beat   = played[i];

            if (before.sample < line(9)) {
                continue;
            }

            CAPTURE(beat.sample);

            const std::int64_t bar_length = loop.lines()[before.bar + 1].position.value() -
                                            loop.lines()[before.bar].position.value();
            const std::int64_t natural    = before.sample + bar_length;
            const std::int64_t moved      = beat.sample - natural;

            CHECK((moved == change.moves[0] || moved == change.moves[1]));

            if (moved > 0 && beat.bar == 1U) {
                const std::int64_t downbeat = loop.lines()[1].position.value();

                CHECK(mismatches(rig, natural, beat.sample, [&](std::int64_t n) {
                          return frame_held(audio, loop, downbeat - (beat.sample - n));
                      }) == 0);
            }

            ++checked;
        }

        CHECK(checked >= 4U);
    }
}

TEST_CASE("a late wrap is entered from its pre-roll as far as that reaches, and beyond from the "
          "bar's continuation")
{
    // A 1-bar loop of bar 3, and from bar 5's line a slower clock: each grid bar 2,010, 2,030 or
    // 3,310 samples longer than the loop's (116 BPM). An entry can reach 2,016 samples back into
    // the pre-roll, 50 ms less F. At 2,010 the wrap is entered over the F before its natural
    // downbeat, and the 2,010 before bar 3's line play until the line. Beyond that, the playhead
    // plays on past the end of bar 3, through what was recorded after it and silence after that,
    // and enters bar 3's pre-roll over the F before the line. One downbeat a line, each on its
    // line (DESIGN.md §5.7).
    constexpr std::int64_t reach = window - entry;

    struct slower {
        sample_time  period;
        std::int64_t late{0};  // how much longer a grid bar is than the loop's, or a sample less
    };

    for (const slower clock : {
             slower{.period = sample_time::from_ticks(401'448'960), .late = 2'010},  // 6,125.625
             slower{.period = sample_time::from_ticks(401'530'880), .late = 2'030},  // 6,126.875
             slower{
                 .period = sample_time::from_ticks(std::llround(720'000.0 / 116.0 * 65'536.0)),
                 .late   = 3'311,
             },
         }) {
        const sample_time period = clock.period;

        CAPTURE(clock.late);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{blocks_of(480), tape};
        const engine&             audio = rig.target();

        rig.play(noise_until(line(4) + 7'200));
        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 5 * pulses_per_bar);
        rig.steady_clock(at_sample(line(5)), period, 12 * pulses_per_bar);
        rig.press(at_sample(line(4) + 7'200), capture_bars::one);
        rig.keep_output();
        rig.run_until(samples{line(15)});

        const captured_loop& loop   = audio.slot();
        const auto&          played = rig.downbeats();
        const auto&          lines  = rig.bar_lines();

        REQUIRE(played.size() >= 9U);

        // From the line after the change on, a downbeat on each line, and none between.
        std::size_t checked = 0;

        for (std::size_t i = 1; i < played.size(); ++i) {
            const std::int64_t due     = played[i].sample;
            const std::int64_t natural = played[i - 1].sample + bar_samples;
            const std::int64_t late    = due - natural;

            CAPTURE(due);
            CHECK(std::ranges::find(lines, due) != lines.end());
            CHECK((late == clock.late || late == clock.late - 1));

            const auto pre_roll = [&](std::int64_t n) {
                return frame_held(audio, loop, line(3) - (due - n));
            };

            const auto continuation = [&](std::int64_t n) {
                return frame_held(audio, loop, line(4) + (n - natural));
            };

            if (late <= reach) {
                // Entered at the natural downbeat, from the end of bar 3.
                const std::int64_t start = natural - entry;
                const float        rho =
                    correlation_of(audio, loop, line(4) - window, line(3) - late - window);

                CHECK(mismatches(rig, start, natural, [&](std::int64_t n) {
                          return faded(continuation(n), pre_roll(n), rho, n - start, entry);
                      }) == 0);
                CHECK(mismatches(rig, natural, due, pre_roll) == 0);
            }
            else {
                // Played on, and entered at the line.
                const std::int64_t start = due - entry;
                const float        rho =
                    correlation_of(audio, loop, line(4) + late - window, line(3) - window);

                CHECK(mismatches(rig, natural, start, continuation) == 0);
                CHECK(mismatches(rig, start, due, [&](std::int64_t n) {
                          return faded(continuation(n), pre_roll(n), rho, n - start, entry);
                      }) == 0);
            }

            ++checked;
        }

        CHECK(checked >= 8U);
    }
}

TEST_CASE("with L_out, each downbeat plays that much before its line")
{
    for (const std::int64_t latency : {std::int64_t{0}, std::int64_t{100}}) {
        CAPTURE(latency);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{
            engine_config{
                .rate             = rate_48k,
                .max_block_frames = 480,
                .output_latency   = at_sample(latency),
            },
            tape,
        };

        rig.play(noise_until(line(5)));
        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
        play_bar_3(rig);
        rig.run_until(samples{line(8) + 480});

        const auto& played = rig.downbeats();

        REQUIRE(played.size() >= 3U);

        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(played[i].sample == line(5 + static_cast<std::int64_t>(i)) - latency);
        }
    }
}

TEST_CASE("a press during a crossfade waits for it to end, and of a double press only the latest")
{
    // Bar 3 plays as a 1-bar loop, and wraps at bar 7's line, entered over the 384 samples before
    // it. Presses 200 and 300 samples before the line fall in that crossfade: they wait, in blocks
    // of 64, until it ends on the line, and only the latest is applied, with its own timestamp. A
    // quarter beat or less before bar 7's line, it is an early press for that line, which has just
    // arrived: it starts at once, with a seek on the downbeat.
    for (const bool twice : {false, true}) {
        CAPTURE(twice);

        std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
        clocked_engine            rig{blocks_of(64), tape};
        const engine&             audio = rig.target();

        rig.play(noise_until(line(6)));
        rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
        play_bar_3(rig);

        if (twice) {
            rig.press(at_sample(line(7) - 300), capture_bars::two);
        }

        rig.press(at_sample(line(7) - 200), capture_bars::one);
        rig.run_until(samples{line(7)});

        CHECK(audio.loop().outcome_serial == 1U);
        CHECK(audio.slot().first_bar() == 3);

        rig.run_until(samples{line(7) + 64});

        CHECK(audio.loop().outcome_serial == 2U);
        CHECK(audio.loop().outcome.kept == capture_bars::one);
        CHECK(audio.slot().first_bar() == 6);
        CHECK(audio.slot().lines().back().position == samples{line(7)});
        check_holders(audio);
    }
}

TEST_CASE("a capture refused at its line after its entry began hands back to the loop in the slot")
{
    // The clock starts at 10,500, so a 1-bar capture of bar 6 runs to 228 samples short of a page,
    // its reserve and read margin included: it fits in 4 pages beside the history's 46 and the
    // slot's loop. Pressed 0.2 beat before bar 7's line, it begins its entry F before. Its pulse
    // comes 600 samples late: fitted again from it, it needs a fifth page, and is refused. The
    // player seeks back from it to the loop in the slot, at the bar due at bar 7's line, where that
    // loop would be had it played on into the line; or, with none, to silence. The refused capture
    // leaves, giving its pages back as the seek ends.
    constexpr std::int64_t origin = 10'500;
    constexpr std::int64_t late   = 600;

    const auto at_line = [](std::int64_t bar) {
        return origin + line(bar);
    };

    // None; bar 3, from a late press 0.3 beat into bar 4; or bars 2 and 3, from the same press.
    // At bar 7's line, either loop's due bar is bar 3.
    struct slot_loop {
        std::optional<capture_bars> bars;
        std::size_t                 pages{0};
    };

    for (const slot_loop in_slot : {
             slot_loop{.bars = std::nullopt, .pages = 0},
             slot_loop{.bars = capture_bars::one, .pages = 4},
             slot_loop{.bars = capture_bars::two, .pages = 7},
         }) {
        CAPTURE(in_slot.pages);

        std::vector<stereo_frame> tape = tape_of(46 + in_slot.pages + 4, rate_48k);
        clocked_engine            rig{blocks_of(480), tape};
        const engine&             audio = rig.target();

        rig.play(noise_until(at_line(7)));
        rig.steady_clock(at_sample(origin), at_sample(pulse_samples), 7 * pulses_per_bar);
        rig.pulse(at_sample(at_line(7) + late));
        rig.steady_clock(at_sample(at_line(7) + pulse_samples), at_sample(pulse_samples), 16);

        if (in_slot.bars) {
            rig.press(at_sample(at_line(4) + 7'200), *in_slot.bars);
        }

        rig.press(at_sample(at_line(7) - 4'800), capture_bars::one);
        rig.run_until(samples{at_line(7)});

        REQUIRE(audio.loop().waiting);
        REQUIRE(audio.loop().outcome.result == capture_result::committed);
        REQUIRE(audio.slot().page_count() == in_slot.pages);

        const captured_loop& old_loop = audio.slot();

        // The refused capture's bar 6, kept before it can leave.
        std::vector<stereo_frame> bar_6;

        bar_6.reserve(static_cast<std::size_t>(bar_samples));

        for (std::int64_t i = 0; i < bar_samples; ++i) {
            bar_6.push_back(frame_held(audio, audio.waiting(), at_line(6) + i));
        }

        rig.keep_output();
        rig.run_until(samples{at_line(7) + late + 480});

        CHECK(audio.loop().outcome.result == capture_result::pool_full);
        CHECK(audio.loop().outcome_serial == (in_slot.bars ? 3U : 2U));
        CHECK_FALSE(audio.loop().waiting);
        CHECK(&audio.slot() == &old_loop);
        CHECK(audio.leaving().empty());
        check_holders(audio);

        const std::int64_t refused  = at_line(7) + late;
        const std::int64_t seek_end = refused + seek;

        const auto back = [&](std::int64_t n) {
            return in_slot.bars ? frame_held(audio, old_loop, at_line(3) + (n - at_line(7)))
                                : stereo_frame{};
        };

        // The seek, from the refused capture where it plays, at equal gain into silence.
        if (!in_slot.bars) {
            CHECK(mismatches(rig, refused, seek_end, [&](std::int64_t n) {
                      return faded(bar_6[static_cast<std::size_t>(n - at_line(7))],
                                   {},
                                   1.0F,
                                   n - refused,
                                   seek);
                  }) == 0);
        }

        CHECK(mismatches(rig, seek_end, seek_end + 192, back) == 0);
    }
}

TEST_CASE("after a refused 8-bar capture, the slot's loop plays on to the line after its end")
{
    // The clock starts at 15,108, so an 8-bar capture of bars 1 to 8 runs to 228 samples short of
    // a page, its reserve and read margin included: it fits in 24 pages beside the history's 46
    // and the 4 of bar 3's loop, playing. Pressed 0.2 beat before bar 9's line, it begins its
    // entry F before. Its pulse comes 600 samples late, it needs a 25th page, and is refused. The
    // player seeks back to bar 3's loop, which wraps on bar 10's line and bar 11's: the lines
    // after the refused capture's end line, though its first line was 8 bars before that.
    constexpr std::int64_t origin = 15'108;
    constexpr std::int64_t late   = 600;

    const auto at_line = [](std::int64_t bar) {
        return origin + line(bar);
    };

    std::vector<stereo_frame> tape = tape_of(46 + 4 + 24, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};
    const engine&             audio = rig.target();

    rig.play(noise_until(at_line(9)));
    rig.steady_clock(at_sample(origin), at_sample(pulse_samples), 9 * pulses_per_bar);
    rig.pulse(at_sample(at_line(9) + late));
    rig.steady_clock(at_sample(at_line(9) + pulse_samples), at_sample(pulse_samples), 48);
    rig.press(at_sample(at_line(4) + 7'200), capture_bars::one);
    rig.press(at_sample(at_line(9) - 4'800), capture_bars::eight);
    rig.run_until(samples{at_line(12)});

    CHECK(audio.loop().outcome.result == capture_result::pool_full);
    CHECK(audio.slot().first_bar() == 3);
    CHECK(audio.leaving().empty());
    check_holders(audio);

    std::vector<std::int64_t> after;

    after.reserve(rig.downbeats().size());

    for (const clocked_engine::played_downbeat& beat : rig.downbeats()) {
        if (beat.sample > at_line(9) + late) {
            after.push_back(beat.sample);
            CHECK(beat.bar == 0U);
        }
    }

    CHECK(after == std::vector<std::int64_t>{at_line(10), at_line(11)});
}

TEST_CASE("a press that waited applies with its own timestamp")
{
    // A 1-bar late press 6,050 samples before bar 7's line holds bar 5 and starts at once, with a
    // seek to 5,906 before the line. A 2-bar press 6,001 before the line waits through the seek.
    // By its own timestamp it is more than a quarter beat before the line, a late press for bar
    // 6's line: bars 4 and 5, at once. As it applies, 5,906 before, it would be an early press
    // for bar 7's line instead, waiting for bars 5 and 6.
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(64), tape};
    const engine&             audio = rig.target();

    rig.play(noise_until(line(7)));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 8 * pulses_per_bar);
    rig.press(at_sample(line(7) - 6'050), capture_bars::one);
    rig.press(at_sample(line(7) - 6'001), capture_bars::two);
    rig.run_until(samples{line(7) - 5'888});

    CHECK(audio.loop().outcome_serial == 2U);
    CHECK(audio.loop().outcome.kept == capture_bars::two);
    CHECK_FALSE(audio.loop().waiting);
    CHECK(audio.slot().first_bar() == 4);
    check_holders(audio);
}

TEST_CASE("a press during a capture's entry waits for the capture to take the slot")
{
    // Bar 3 plays as a 1-bar loop. A 1-bar press 0.2 beat before bar 7's line enters bar 6 over
    // the F before the line, and bar 7's pulse comes 600 samples late. A 2-bar press 300 samples
    // after the predicted line, between the entry's crossfade and the pulse, waits: until the
    // entered capture takes the slot at its pulse, and then applies with its own timestamp.
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(64), tape};
    const engine&             audio = rig.target();

    rig.play(noise_until(line(7)));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 7 * pulses_per_bar);
    rig.pulse(at_sample(line(7) + 600));
    rig.steady_clock(at_sample(line(7) + pulse_samples),
                     at_sample(pulse_samples),
                     2 * pulses_per_bar);
    play_bar_3(rig);
    rig.press(at_sample(line(7) - 4'800), capture_bars::one);
    rig.press(at_sample(line(7) + 300), capture_bars::two);
    rig.run_until(samples{line(7) + 576});

    CHECK(audio.loop().outcome_serial == 2U);
    CHECK(audio.loop().waiting);

    rig.run_until(samples{line(7) + 640});

    CHECK(audio.loop().outcome_serial == 3U);
    CHECK(audio.loop().outcome.kept == capture_bars::two);
    CHECK(audio.slot().first_bar() == 5);
    check_holders(audio);
}

TEST_CASE("playback allocates nothing")
{
    std::vector<stereo_frame> tape = tape_of(p1_pages, rate_48k);
    clocked_engine            rig{blocks_of(480), tape};

    rig.play(noise_until(line(6)));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 12 * pulses_per_bar);
    play_bar_3(rig);
    rig.press(at_sample(line(7) - 4'800), capture_bars::two);
    rig.press(at_sample(line(9) + 7'200), capture_bars::one);
    rig.run_until(samples{line(4)});

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        rig.run_until(samples{line(11)});
    }

    CHECK(test::realtime_guard::violations() == 0U);
    CHECK(rig.target().loop().playing);
}

TEST_CASE("a crossfade's gains are equal gain when correlated, equal power when not, and a blend")
{
    // At ρ = 1, exactly x. At ρ = 0, the quarter sine: from exactly 0 to exactly 1, rising, within
    // 10⁻⁷ of libm's sine in double, and its power within 2 × 10⁻⁷ of 1. The blend's power within
    // 0.39 dB of unity for streams of the correlation it was chosen for (DESIGN.md §5.7).
    constexpr double half_pi = 1.5707963267948966;

    CHECK(crossfade_gain(0.0F, 0.0F) == 0.0F);
    CHECK(crossfade_gain(0.0F, 1.0F) == 1.0F);

    float  before   = 0.0F;
    double furthest = 0.0;
    double unequal  = 0.0;

    for (int step = 0; step <= 100'000; ++step) {
        const float x  = static_cast<float>(step) / 100'000.0F;
        const float in = crossfade_gain(0.0F, x);
        const auto  xd = static_cast<double>(x);

        CHECK(crossfade_gain(1.0F, x) == x);
        CHECK(in >= before);

        const auto out = static_cast<double>(crossfade_gain(0.0F, 1.0F - x));

        furthest = std::max(furthest, std::abs(static_cast<double>(in) - std::sin(half_pi * xd)));
        unequal  = std::max(
            unequal,
            std::abs((static_cast<double>(in) * static_cast<double>(in)) + (out * out) - 1.0));
        before = in;
    }

    CHECK(furthest < 1.0e-7);
    CHECK(unequal < 2.0e-7);

    double worst = 0.0;

    for (int r = 0; r <= 100; ++r) {
        const float rho = static_cast<float>(r) / 100.0F;

        for (int step = 0; step <= 1'000; ++step) {
            const float x    = static_cast<float>(step) / 1'000.0F;
            const auto  in   = static_cast<double>(crossfade_gain(rho, x));
            const auto  out  = static_cast<double>(crossfade_gain(rho, 1.0F - x));
            const auto  rhod = static_cast<double>(rho);

            worst = std::max(
                worst,
                std::abs(10.0 * std::log10((in * in) + (out * out) + (2.0 * rhod * in * out))));
        }
    }

    CHECK(worst < 0.39);
    CHECK(worst > 0.38);
}

TEST_CASE("seams on §5.6's signals: the sine's wrap, the bass's move, and the kick's downbeat")
{
    // Three of the 36 that `make seeds` checks (test_playback_seams.cpp): the wrap that dips most,
    // the half-seam that bumps most, and the transient on the downbeat.
    test::check_seam(test::test_signal::sine, test::seam_kind::wrap);
    test::check_seam(test::test_signal::bass, test::seam_kind::move);
    test::check_seam(test::test_signal::kick, test::seam_kind::early_replacement);
}

}  // namespace
}  // namespace previously
