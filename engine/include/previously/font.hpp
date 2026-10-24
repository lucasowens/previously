// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the screen's font (DESIGN.md §9.3).
//
// Our own 5×7 glyphs: digits, capitals and a few marks. Each glyph is followed by a column of
// space, so characters are 6 pixels apart and 21 fit across the screen. font.cpp draws the glyphs
// as pictures.

#pragma once

#include "previously/framebuffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace previously {

inline constexpr std::size_t glyph_width  = 5;
inline constexpr std::size_t glyph_height = 7;
inline constexpr std::size_t cell_width   = glyph_width + 1;

/// A glyph's columns from the left, each with its top pixel in bit 0.
using glyph = std::array<std::uint8_t, glyph_width>;

/// The characters the font draws, in the order font.cpp draws them.
[[nodiscard]] std::string_view font_characters() noexcept;

/// The glyph of `character`. A character the font does not draw gets a hollow box, so a missing
/// glyph shows instead of leaving a gap.
[[nodiscard]] glyph glyph_of(char character) noexcept;

/// The width of `text` drawn: its cells, less the space after the last.
[[nodiscard]] constexpr std::size_t text_width(std::string_view text) noexcept
{
    return text.empty() ? 0 : (text.size() * cell_width) - 1;
}

/// Draws `text` with the top-left pixel of its first glyph at (`x`, `y`). Whatever runs off the
/// screen is cut off, however far off the origin is.
void draw_text(framebuffer& screen, std::size_t x, std::size_t y, std::string_view text) noexcept;

}  // namespace previously
