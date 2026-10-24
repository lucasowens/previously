// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the screen's font, drawn as it looks.

#include "previously/font.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string_view>

namespace previously {
namespace {

/// Glyphs side by side, as they look: for each row from the top, each glyph's pixels ('#' lit,
/// '.' dark), a space between one glyph and the next.
struct font_sheet {
    std::string_view                           characters;
    std::array<std::string_view, glyph_height> rows;
};

// The zero is slashed, so it differs from the O.
constexpr std::array sheets{
    font_sheet{
        .characters = "0123456789",
        .rows =
            {
                ".###. ..#.. .###. .###. ...#. ##### .###. ##### .###. .###.",
                "#...# .##.. #...# #...# ..##. #.... #.... ....# #...# #...#",
                "#..## ..#.. ....# ....# .#.#. ####. #.... ...#. #...# #...#",
                "#.#.# ..#.. ..##. ..##. #..#. ....# ####. ..#.. .###. .####",
                "##..# ..#.. .#... ....# ##### ....# #...# ..#.. #...# ....#",
                "#...# ..#.. #.... #...# ...#. #...# #...# ..#.. #...# ....#",
                ".###. .###. ##### .###. ...#. .###. .###. ..#.. .###. .###.",
            },
    },
    font_sheet{
        .characters = "ABCDEFGHIJ",
        .rows =
            {
                ".###. ####. .###. ####. ##### ##### .###. #...# .###. ..###",
                "#...# #...# #...# #...# #.... #.... #...# #...# ..#.. ...#.",
                "#...# #...# #.... #...# #.... #.... #.... #...# ..#.. ...#.",
                "##### ####. #.... #...# ####. ####. #.### ##### ..#.. ...#.",
                "#...# #...# #.... #...# #.... #.... #...# #...# ..#.. ...#.",
                "#...# #...# #...# #...# #.... #.... #...# #...# ..#.. #..#.",
                "#...# ####. .###. ####. ##### #.... .###. #...# .###. .##..",
            },
    },
    font_sheet{
        .characters = "KLMNOPQRST",
        .rows =
            {
                "#...# #.... #...# #...# .###. ####. .###. ####. .#### #####",
                "#..#. #.... ##.## #...# #...# #...# #...# #...# #.... ..#..",
                "#.#.. #.... #.#.# ##..# #...# #...# #...# #...# #.... ..#..",
                "##... #.... #.#.# #.#.# #...# ####. #...# ####. .###. ..#..",
                "#.#.. #.... #...# #..## #...# #.... #.#.# #.#.. ....# ..#..",
                "#..#. #.... #...# #...# #...# #.... #..#. #..#. ....# ..#..",
                "#...# ##### #...# #...# .###. #.... .##.# #...# ####. ..#..",
            },
    },
    font_sheet{
        .characters = "UVWXYZ?!'=",
        .rows =
            {
                "#...# #...# #...# #...# #...# ##### .###. ..#.. ..#.. .....",
                "#...# #...# #...# #...# #...# ....# #...# ..#.. ..#.. .....",
                "#...# #...# #...# .#.#. .#.#. ...#. ....# ..#.. .#... #####",
                "#...# #...# #.#.# ..#.. ..#.. ..#.. ...#. ..#.. ..... .....",
                "#...# #...# #.#.# .#.#. ..#.. .#... ..#.. ..#.. ..... #####",
                "#...# .#.#. #.#.# #...# ..#.. #.... ..... ..... ..... .....",
                ".###. ..#.. .#.#. #...# ..#.. ##### ..#.. ..#.. ..... .....",
            },
    },
    font_sheet{
        .characters = " .,:-+/%()",
        .rows =
            {
                "..... ..... ..... ..... ..... ..... ..... ##..# ...#. .#...",
                "..... ..... ..... ..##. ..... ..#.. ....# ##..# ..#.. ..#..",
                "..... ..... ..... ..##. ..... ..#.. ...#. ...#. .#... ...#.",
                "..... ..... ..... ..... .###. ##### ..#.. ..#.. .#... ...#.",
                "..... ..... ..##. ..##. ..... ..#.. .#... .#... .#... ...#.",
                "..... ..##. ...#. ..##. ..... ..#.. #.... #..## ..#.. ..#..",
                "..... ..##. ..#.. ..... ..... ..... ..... #..## ...#. .#...",
            },
    },
};

// The table covers printable ASCII, from the space to the tilde.
constexpr char        first_printable = ' ';
constexpr char        last_printable  = '~';
constexpr std::size_t printable_count =
    static_cast<std::size_t>(last_printable - first_printable) + 1;

[[nodiscard]] constexpr bool is_printable(char character) noexcept
{
    return character >= first_printable && character <= last_printable;
}

[[nodiscard]] constexpr std::size_t slot_of(char character) noexcept
{
    return static_cast<std::size_t>(character - first_printable);
}

/// Whether `row` draws a row of `glyphs` glyphs: 5 pixels each, '#' lit or '.' dark, with a
/// space between one glyph and the next.
[[nodiscard]] constexpr bool is_well_drawn(std::string_view row, std::size_t glyphs) noexcept
{
    if (row.size() != (glyphs * cell_width) - 1) {
        return false;
    }

    for (std::size_t at = 0; at < row.size(); ++at) {
        const bool between = at % cell_width == glyph_width;
        const char pixel   = row[at];

        if (between ? pixel != ' ' : pixel != '#' && pixel != '.') {
            return false;
        }
    }

    return true;
}

/// Whether every sheet is drawn as its characters say, and each character is printable and
/// drawn once.
[[nodiscard]] constexpr bool sheets_are_well_drawn() noexcept
{
    std::array<bool, printable_count> drawn{};

    for (const font_sheet& sheet : sheets) {
        const bool rows_well_drawn =
            std::ranges::all_of(sheet.rows, [&sheet](std::string_view row) {
                return is_well_drawn(row, sheet.characters.size());
            });

        if (!rows_well_drawn) {
            return false;
        }

        for (const char character : sheet.characters) {
            if (!is_printable(character) || drawn[slot_of(character)]) {
                return false;
            }

            drawn[slot_of(character)] = true;
        }
    }

    return true;
}

static_assert(sheets_are_well_drawn(), "each sheet must draw its characters, each once");

struct glyph_table {
    std::array<glyph, printable_count> glyphs{};
    std::array<bool, printable_count>  drawn{};
};

[[nodiscard]] consteval glyph_table read_sheets() noexcept
{
    glyph_table table;

    for (const font_sheet& sheet : sheets) {
        for (std::size_t index = 0; index < sheet.characters.size(); ++index) {
            const std::size_t slot = slot_of(sheet.characters[index]);

            for (std::size_t column = 0; column < glyph_width; ++column) {
                unsigned bits = 0;

                for (std::size_t row = 0; row < glyph_height; ++row) {
                    if (sheet.rows[row][(index * cell_width) + column] == '#') {
                        bits |= 1U << row;
                    }
                }

                table.glyphs[slot][column] = static_cast<std::uint8_t>(bits);
            }

            table.drawn[slot] = true;
        }
    }

    return table;
}

constexpr glyph_table font_table = read_sheets();

[[nodiscard]] consteval std::size_t count_characters() noexcept
{
    return std::accumulate(sheets.begin(),
                           sheets.end(),
                           std::size_t{0},
                           [](std::size_t count, const font_sheet& sheet) {
                               return count + sheet.characters.size();
                           });
}

[[nodiscard]] consteval std::array<char, count_characters()> list_characters() noexcept
{
    std::array<char, count_characters()> characters{};
    std::size_t                          count = 0;

    for (const font_sheet& sheet : sheets) {
        for (const char character : sheet.characters) {
            characters[count] = character;
            ++count;
        }
    }

    return characters;
}

constexpr std::array character_list = list_characters();

constexpr glyph hollow_box{0x7F, 0x41, 0x41, 0x41, 0x7F};

}  // namespace

std::string_view font_characters() noexcept
{
    return {character_list.data(), character_list.size()};
}

glyph glyph_of(char character) noexcept
{
    if (!is_printable(character) || !font_table.drawn[slot_of(character)]) {
        return hollow_box;
    }

    return font_table.glyphs[slot_of(character)];
}

void draw_text(framebuffer& screen, std::size_t x, std::size_t y, std::string_view text) noexcept
{
    for (const char character : text) {
        const glyph pixels = glyph_of(character);

        for (std::size_t column = 0; column < glyph_width; ++column) {
            for (std::size_t row = 0; row < glyph_height; ++row) {
                if (((static_cast<unsigned>(pixels[column]) >> row) & 1U) != 0) {
                    screen.light(x + column, y + row);
                }
            }
        }

        x += cell_width;
    }
}

}  // namespace previously
