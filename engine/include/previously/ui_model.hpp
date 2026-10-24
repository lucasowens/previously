// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the UI model, what the display shows (DESIGN.md §9.3).
//
// The model says what the screen shows, not how it is drawn: the clock's state, the tempo, where
// the bar is, and the history strip, 128 columns of peaks. A renderer, still to come, will draw it
// the same way for an OLED, Rack's panel and the golden images.
//
// The strip has two views:
//   - bars, once the grid has a tempo: the last 4 whole bars and the bar in progress, in five
//     slots starting at columns 0, 25, 51, 76 and 102. The 4 whole bars are what a 4-bar capture
//     pressed now asks for (§3.4 can shorten it), so the strip answers "if I press now, what do I
//     get?". The bar in progress fills its slot from the left, and when it ends the strip moves
//     on a bar. Its end is a prediction, so a new estimate of the tempo moves its columns, and the
//     fill can step back. Each bar fills its slot whatever its length, so a tempo change squeezes
//     none off the screen.
//   - time, before that: the newest buckets, 11 to a column, newest at the right. The newest
//     column fills as it is recorded, so the strip holds from 1,397 buckets and a frame up to
//     1,408 (29.8 to 30.0 s at 48 kHz). Whole columns move on together, one every 11 buckets.
// A column shows the loudest bucket it touches (recorder.hpp), so its resolution is a bucket.
//
// The engine rebuilds the model at the end of every block that completes a bucket: 46.9 times a
// second at 48 kHz, 43.1 at 44.1 kHz. It reads at most the history's buckets and one more for each
// column, so rebuilding takes bounded time, and it allocates nothing.

#pragma once

#include "previously/clock_tracker.hpp"
#include "previously/peak_level.hpp"
#include "previously/recorder.hpp"
#include "previously/sample_time.hpp"
#include "previously/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace previously {

/// One column of the history strip.
struct strip_column {
    peak_level peaks;
    bool       recorded{false};  // false where the tape holds nothing, or the bar is still to come

    [[nodiscard]] friend constexpr bool operator==(const strip_column&,
                                                   const strip_column&) noexcept = default;
};

enum class strip_view : std::uint8_t {
    time,  // before the grid has a tempo: the newest buckets, newest at the right
    bars,  // with one: the last 4 whole bars, and the bar in progress
};

struct ui_model {
    static constexpr std::size_t strip_columns = 128;

    /// The bars view's whole bars: a 4-bar capture's, until there is a length control (§9.1).
    static constexpr std::size_t whole_bars = 4;

    /// The whole bars, and the bar in progress.
    static constexpr std::size_t bar_slots = whole_bars + 1;

    static constexpr std::size_t buckets_per_time_column = 11;

    /// The first column of bar slot `slot`: 0, 25, 51, 76 and 102, and 128 past the last.
    [[nodiscard]] static constexpr std::size_t slot_start(std::size_t slot) noexcept
    {
        return (slot * strip_columns) / bar_slots;
    }

    std::uint32_t serial{0};  // how many times the engine has built it

    // The clock, as clock_report gives it, as of the block's last frame.
    clock_state                 clock{clock_state::no_clock};
    std::optional<sample_time>  beat_period{std::nullopt};
    sample_rate                 rate{48'000U};
    std::optional<std::int64_t> bar{std::nullopt};   // the bar in progress
    std::optional<std::int64_t> beat{std::nullopt};  // its beat in progress, from 0
    std::int64_t                beats_per_bar{4};

    // The strip.
    strip_view  view{strip_view::time};
    std::size_t first_bar_slot{0};  // bars view: earlier slots hold bars the grid cannot place
    std::size_t now_column{strip_columns};  // bars view: from here on the bar is still to come
    std::array<strip_column, strip_columns> strip{};

    [[nodiscard]] friend constexpr bool operator==(const ui_model&,
                                                   const ui_model&) noexcept = default;
};

// An adapter hands the model to the thread that draws it as a copy of its bytes (DESIGN.md §9.3).
static_assert(std::is_trivially_copyable_v<ui_model>, "the model is copied byte for byte");

/// Rebuilds `model` from the grid and the history as they stand at the history's end, and counts
/// it. Audio thread. Realtime-safe: bounded time, no allocation.
void update_ui_model(ui_model&            model,
                     const clock_tracker& grid,
                     const recorder&      history,
                     sample_rate          rate) noexcept;

}  // namespace previously
