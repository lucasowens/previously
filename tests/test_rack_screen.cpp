// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the VCV Rack adapter's screen feed, and the runs its widget paints (tier T4).

#include "previously/audio_block.hpp"
#include "previously/display.hpp"
#include "previously/engine.hpp"
#include "previously/framebuffer.hpp"
#include "previously/peak_level.hpp"
#include "previously/ui_model.hpp"
#include "previously/units.hpp"

#include "realtime_guard.hpp"
#include "screen.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace previously::vcv {
namespace {

constexpr sample_rate rate_48k{48'000U};
constexpr sample_rate rate_96k{96'000U};
constexpr std::size_t block_frames = 32;

[[nodiscard]] engine_config config_at(sample_rate rate)
{
    return engine_config{.rate = rate, .max_block_frames = block_frames};
}

/// Runs one block of silence through `target`.
void run_block(engine& target)
{
    const std::array<float, block_frames> silence{};
    std::array<float, block_frames>       out{};

    target.process(audio_block{
        .in_left   = silence,
        .in_right  = silence,
        .out_left  = out,
        .out_right = out,
    });
}

// The feed.

TEST_CASE("before the first block, the screen's model is empty")
{
    screen_feed feed;

    CHECK_FALSE(feed.take());
    CHECK(feed.model() == ui_model{});
}

TEST_CASE("the model is published after the first block, and after each that rebuilt it, only")
{
    // Without a tape the engine still rebuilds its model at every bucket of 1,024 frames, so 100
    // blocks of 32 frames rebuild it 3 times: after blocks 32, 64 and 96.
    engine      target{config_at(rate_48k)};
    screen_feed feed;
    std::size_t publishes = 0;

    for (std::size_t block = 1; block <= 100; ++block) {
        CAPTURE(block);

        const std::uint32_t serial_before = target.ui().serial;

        run_block(target);
        feed.after_block(target);

        const bool rebuilt   = target.ui().serial != serial_before;
        const bool published = feed.take();

        CHECK(published == (block == 1 || rebuilt));
        CHECK(feed.model() == target.ui());

        publishes += published ? 1 : 0;
    }

    CHECK(publishes == 4);
}

TEST_CASE("after a restart, a rebuilt engine's first model is published, whatever its serial")
{
    std::optional<engine> target;
    screen_feed           feed;

    target.emplace(config_at(rate_48k));
    run_block(*target);
    feed.after_block(*target);

    REQUIRE(feed.take());
    REQUIRE(feed.model().serial == 0);

    // A new rate rebuilds the engine, whose serials start again at 0. Without the restart, its
    // first model would match the last one published, and the screen would never see it.
    SUBCASE("with the restart, as the module does")
    {
        target.emplace(config_at(rate_96k));
        feed.restart();
        run_block(*target);
        feed.after_block(*target);

        CHECK(feed.take());
        CHECK(feed.model().rate == rate_96k);
    }

    SUBCASE("without it, the model would go unseen")
    {
        target.emplace(config_at(rate_96k));
        run_block(*target);
        feed.after_block(*target);

        CHECK_FALSE(feed.take());
        CHECK(feed.model().rate == rate_48k);
    }
}

TEST_CASE("publishing a model allocates nothing")
{
    engine      target{config_at(rate_48k)};
    screen_feed feed;

    run_block(target);

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;

        feed.after_block(target);
        feed.restart();
        feed.after_block(target);
    }

    CHECK(test::realtime_guard::violations() == 0U);
}

// The runs.

/// Every run on `screen`, in the order for_each_run gives them.
[[nodiscard]] std::vector<pixel_run> runs_of(const framebuffer& screen)
{
    std::vector<pixel_run> runs;

    for_each_run(screen, [&runs](pixel_run run) {
        runs.push_back(run);
    });

    return runs;
}

/// Checks that `runs` paint exactly the lit pixels of `screen`, from the left and from the top,
/// each run as long as it can be.
void check_runs(const framebuffer& screen, const std::vector<pixel_run>& runs)
{
    framebuffer              painted;
    std::optional<pixel_run> previous;
    std::size_t              wrong = 0;

    for (const pixel_run& run : runs) {
        const bool in_order = !previous || run.x > previous->x ||
                              (run.x == previous->x && run.top > previous->bottom);
        const bool sized    = run.top < run.bottom && run.bottom <= framebuffer::height;
        const bool longest  = (run.top == 0 || !screen.lit(run.x, run.top - 1)) &&
                              (run.bottom == framebuffer::height || !screen.lit(run.x, run.bottom));

        wrong += in_order && sized && longest ? 0 : 1;

        for (std::size_t y = run.top; y < run.bottom; ++y) {
            painted.light(run.x, y);
        }

        previous = run;
    }

    CHECK(wrong == 0);
    CHECK(painted == screen);
}

TEST_CASE("the runs paint exactly the lit pixels, each run as long as it can be")
{
    SUBCASE("an empty screen has none")
    {
        CHECK(runs_of(framebuffer{}).empty());
    }

    SUBCASE("a full screen has one a column")
    {
        framebuffer full;

        for (std::size_t y = 0; y < framebuffer::height; ++y) {
            for (std::size_t x = 0; x < framebuffer::width; ++x) {
                full.light(x, y);
            }
        }

        const std::vector<pixel_run> runs = runs_of(full);

        CHECK(runs.size() == framebuffer::width);
        check_runs(full, runs);
    }

    SUBCASE("a checkerboard has one a lit pixel")
    {
        framebuffer checks;

        for (std::size_t y = 0; y < framebuffer::height; ++y) {
            for (std::size_t x = 0; x < framebuffer::width; ++x) {
                if ((x + y) % 2 == 0) {
                    checks.light(x, y);
                }
            }
        }

        const std::vector<pixel_run> runs = runs_of(checks);

        CHECK(runs.size() == framebuffer::width * framebuffer::height / 2);
        check_runs(checks, runs);
    }

    SUBCASE("the corners, at both ends of a column")
    {
        framebuffer corners;

        corners.light(0, 0);
        corners.light(0, 63);
        corners.light(127, 0);
        corners.light(127, 63);

        const std::vector<pixel_run> runs = runs_of(corners);

        CHECK(runs.size() == 4);
        check_runs(corners, runs);
    }

    SUBCASE("a screen drawn from a busy model")
    {
        ui_model model;

        model.clock       = clock_state::locked;
        model.beat_period = sample_time::from_samples(samples{24'000});
        model.bar         = 20;
        model.beat        = 1;
        model.view        = strip_view::bars;
        model.now_column  = 109;

        for (std::size_t x = 0; x < ui_model::strip_columns; ++x) {
            const peak_level peaks{
                .left  = static_cast<std::uint8_t>(x % 17),
                .right = static_cast<std::uint8_t>((x * 7) % 17),
            };

            model.strip[x] = strip_column{.peaks = peaks, .recorded = x % 23 != 5};
        }

        framebuffer screen;

        draw(model, screen);

        check_runs(screen, runs_of(screen));
    }
}

}  // namespace
}  // namespace previously::vcv
