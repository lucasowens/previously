// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the screen, a UI model drawn into a framebuffer (DESIGN.md §9.3).
//
//   rows  0–6    the header: the tempo, FREE while the clock freewheels, and the bar and beat
//   rows  9–10   bar lines (bars view)
//   rows 11–43   the strip: left peaks above the centre row, 27, and right peaks below it
//   rows 44–45   bar lines (bars view)
//   rows 48–51   a bracket under the whole bars (bars view)
//   rows 55–61   its label, "4 BARS"
//
// Drawing uses integers only and allocates nothing, so the same code can draw for Rack's panel,
// the golden images and an OLED. It runs on the thread that draws, from a copy of the model.

#pragma once

#include "previously/font.hpp"
#include "previously/framebuffer.hpp"
#include "previously/peak_level.hpp"
#include "previously/sample_time.hpp"
#include "previously/ui_model.hpp"
#include "previously/units.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace previously {

namespace screen_layout {

inline constexpr std::size_t header_top   = 0;
inline constexpr std::size_t centre_row   = 27;
inline constexpr std::size_t strip_top    = centre_row - peak_level::max_level;
inline constexpr std::size_t strip_bottom = centre_row + peak_level::max_level;

// Bar lines are ticks outside the strip, so they hide no peak.
inline constexpr std::size_t tick_rows    = 2;
inline constexpr std::size_t top_ticks    = strip_top - tick_rows;
inline constexpr std::size_t bottom_ticks = strip_bottom + 1;

inline constexpr std::size_t bracket_top    = 48;
inline constexpr std::size_t bracket_bottom = 51;
inline constexpr std::size_t label_top      = 55;

}  // namespace screen_layout

/// A line of the screen's text. It holds its characters itself, so making one allocates nothing.
class text_line {
public:
    /// As many characters as fit across the screen.
    static constexpr std::size_t capacity = framebuffer::width / cell_width;

    /// Appends `text`. Characters past the capacity are dropped: they would be off the screen.
    constexpr void append(std::string_view text) noexcept
    {
        for (const char character : text) {
            append(character);
        }
    }

    constexpr void append(char character) noexcept
    {
        if (size_ < capacity) {
            chars_[size_] = character;
            ++size_;
        }
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept
    {
        return {chars_.data(), size_};
    }

private:
    std::array<char, capacity> chars_{};
    std::size_t                size_{0};
};

/// The tempo of `beat_period` at `rate`, right-aligned in 5 characters so the label after it
/// stays put: tenths of a BPM below 1,000, whole BPM from there, rounded to the nearest with
/// halves up, and at most 99999. `beat_period` must be positive.
[[nodiscard]] text_line tempo_text(sample_time beat_period, sample_rate rate) noexcept;

/// The header's left text: the tempo and BPM, then FREE while the clock freewheels. Without a
/// beat period, the clock's state instead: NO CLOCK, or SYNCING after the first pulse.
[[nodiscard]] text_line header_left_text(const ui_model& model) noexcept;

/// The header's right text: the bar and beat in progress, counted from 1, in at most 6
/// characters. A beat takes 1 digit up to 9 beats a bar and 2 from 10, and the bar 4 or 3. Past
/// that they show their last digits, with leading zeros so the wrap shows.
[[nodiscard]] text_line header_right_text(const ui_model& model) noexcept;

/// Draws `model` into `screen`, replacing what it held.
void draw(const ui_model& model, framebuffer& screen) noexcept;

}  // namespace previously
