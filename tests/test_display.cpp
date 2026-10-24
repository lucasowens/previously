// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the screen, a UI model drawn (DESIGN.md §9.3).
//
// The framebuffer's byte order, the font, the header's texts and each rule of the drawing are
// checked directly, against values worked by hand. Golden images then pin whole pictures: models
// built here for each state and edge, and one the engine builds playing DESIGN §9.3's example.
// Every picture comes from integers alone, so the images are the same on every platform.

#include "previously/clock_events.hpp"
#include "previously/clock_tracker.hpp"
#include "previously/display.hpp"
#include "previously/engine.hpp"
#include "previously/font.hpp"
#include "previously/framebuffer.hpp"
#include "previously/page_pool.hpp"
#include "previously/peak_level.hpp"
#include "previously/sample_time.hpp"
#include "previously/ui_model.hpp"
#include "previously/units.hpp"

#include "golden_image.hpp"
#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace previously {
namespace {

constexpr sample_rate rate_48k{48'000U};
constexpr sample_rate rate_44k1{44'100U};

[[nodiscard]] sample_time in_frames(std::int64_t frames)
{
    return sample_time::from_samples(samples{frames});
}

/// The rows lit in column `x`, a bit each, row 0 in bit 0.
[[nodiscard]] std::uint64_t column_rows(const framebuffer& screen, std::size_t x)
{
    std::uint64_t rows = 0;

    for (std::size_t y = 0; y < framebuffer::height; ++y) {
        if (screen.lit(x, y)) {
            rows |= std::uint64_t{1} << y;
        }
    }

    return rows;
}

/// Rows `first` to `last`, both included, a bit each.
[[nodiscard]] constexpr std::uint64_t rows_from(std::size_t first, std::size_t last)
{
    std::uint64_t rows = 0;

    for (std::size_t y = first; y <= last; ++y) {
        rows |= std::uint64_t{1} << y;
    }

    return rows;
}

// Worked by hand from screen_layout: the strip is rows 11 to 43 around the centre row 27, the
// ticks are rows 9 and 10 above it and 44 and 45 below.
constexpr std::uint64_t strip_rows = rows_from(11, 43);
constexpr std::uint64_t tick_rows  = rows_from(9, 10) | rows_from(44, 45);
constexpr std::uint64_t centre_row = rows_from(27, 27);

static_assert(screen_layout::strip_top == 11);
static_assert(screen_layout::centre_row == 27);
static_assert(screen_layout::strip_bottom == 43);
static_assert(screen_layout::top_ticks == 9);
static_assert(screen_layout::bottom_ticks == 44);

/// The rows of a pixel area, `first` to `last`, as they are on `screen`.
[[nodiscard]] bool same_rows(const framebuffer& screen,
                             const framebuffer& expected,
                             std::size_t        first,
                             std::size_t        last)
{
    for (std::size_t y = first; y <= last; ++y) {
        for (std::size_t x = 0; x < framebuffer::width; ++x) {
            if (screen.lit(x, y) != expected.lit(x, y)) {
                return false;
            }
        }
    }

    return true;
}

/// 120 BPM at 48 kHz, a quarter into bar 21 (the grid's bar 20, beat 1), with nothing recorded.
[[nodiscard]] ui_model bars_at_120()
{
    ui_model model;

    model.clock       = clock_state::locked;
    model.beat_period = in_frames(24'000);
    model.rate        = rate_48k;
    model.bar         = 20;
    model.beat        = 1;
    model.view        = strip_view::bars;
    model.now_column  = 109;

    return model;
}

[[nodiscard]] strip_column recorded(std::uint8_t left, std::uint8_t right)
{
    return strip_column{
        .peaks    = peak_level{.left = left, .right = right},
        .recorded = true,
    };
}

[[nodiscard]] framebuffer drawn(const ui_model& model)
{
    framebuffer screen;

    draw(model, screen);

    return screen;
}

// The framebuffer.

TEST_CASE("a pixel is a bit in the SSD1306's page order: 8 down a column, the top in bit 0")
{
    // Worked by hand: page 0 holds rows 0 to 7 in bytes 0 to 127, page 1 starts at byte 128.
    struct pixel {
        std::size_t  x;
        std::size_t  y;
        std::size_t  byte;
        std::uint8_t value;
    };

    for (const pixel& example : {
             pixel{.x = 0, .y = 0, .byte = 0, .value = 0x01},
             pixel{.x = 1, .y = 7, .byte = 1, .value = 0x80},
             pixel{.x = 0, .y = 8, .byte = 128, .value = 0x01},
             pixel{.x = 5, .y = 9, .byte = 133, .value = 0x02},
             pixel{.x = 127, .y = 63, .byte = 1'023, .value = 0x80},
         }) {
        CAPTURE(example.x);
        CAPTURE(example.y);

        framebuffer screen;

        screen.light(example.x, example.y);

        framebuffer::bytes_type expected{};

        expected[example.byte] = example.value;

        CHECK(screen.bytes() == expected);
    }

    // Every pixel, lit alone, is its own bit and reads back lit.
    std::size_t wrong = 0;

    for (std::size_t y = 0; y < framebuffer::height; ++y) {
        for (std::size_t x = 0; x < framebuffer::width; ++x) {
            framebuffer screen;

            screen.light(x, y);

            framebuffer::bytes_type expected{};

            expected[((y / 8) * 128) + x] = static_cast<std::uint8_t>(1U << (y % 8));

            if (screen.bytes() != expected || !screen.lit(x, y)) {
                ++wrong;
            }
        }
    }

    CHECK(wrong == 0);
}

TEST_CASE("a pixel off the screen is ignored, and reads as dark")
{
    framebuffer screen;

    screen.light(framebuffer::width, 0);
    screen.light(0, framebuffer::height);
    screen.light(std::numeric_limits<std::size_t>::max(), 3);

    CHECK(screen == framebuffer{});

    // Unchecked, (128, 0) would read the byte of (0, 8), and (0, 64) a byte past the end.
    screen.light(0, 8);

    CHECK_FALSE(screen.lit(framebuffer::width, 0));
    CHECK_FALSE(screen.lit(0, framebuffer::height));
}

TEST_CASE("clearing darkens every pixel")
{
    framebuffer screen;

    for (std::size_t y = 0; y < framebuffer::height; ++y) {
        for (std::size_t x = 0; x < framebuffer::width; ++x) {
            screen.light(x, y);
        }
    }

    CHECK(std::ranges::all_of(screen.bytes(), [](std::uint8_t byte) {
        return byte == 0xFF;
    }));

    screen.clear();

    CHECK(screen == framebuffer{});
}

// The font.

TEST_CASE("the font draws digits, capitals and a few marks, each once")
{
    CHECK(font_characters() == "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ?!'= .,:-+/%()");
}

TEST_CASE("a glyph is its picture: columns from the left, the top pixel in bit 0")
{
    // The 4, worked by hand from its picture in font.cpp:
    //   ...#.
    //   ..##.
    //   .#.#.
    //   #..#.
    //   #####
    //   ...#.
    //   ...#.
    CHECK(glyph_of('4') == glyph{0x18, 0x14, 0x12, 0x7F, 0x10});
}

TEST_CASE("each glyph fits 5×7 pixels, only the space is blank, and no two are alike")
{
    const std::string_view characters = font_characters();
    const glyph            blank{};
    const glyph            box{0x7F, 0x41, 0x41, 0x41, 0x7F};

    for (std::size_t index = 0; index < characters.size(); ++index) {
        const char  character = characters[index];
        const glyph pixels    = glyph_of(character);

        CAPTURE(character);

        CHECK(std::ranges::none_of(pixels, [](std::uint8_t column) {
            return column >= 0x80;
        }));
        CHECK((pixels == blank) == (character == ' '));
        CHECK(pixels != box);

        for (std::size_t other = index + 1; other < characters.size(); ++other) {
            CAPTURE(characters[other]);
            CHECK(glyph_of(characters[other]) != pixels);
        }
    }
}

TEST_CASE("a character the font does not draw is a hollow box")
{
    const glyph box{0x7F, 0x41, 0x41, 0x41, 0x7F};

    for (const char character : {'a', '~', '\0', '\n', static_cast<char>(200)}) {
        CAPTURE(static_cast<int>(character));
        CHECK(glyph_of(character) == box);
    }
}

TEST_CASE("text is a glyph every 6 pixels from its top-left, and is cut off at the screen's edge")
{
    CHECK(text_width("") == 0);
    CHECK(text_width("4") == 5);
    CHECK(text_width("4 4") == 17);

    framebuffer screen;

    draw_text(screen, 10, 20, "4 4");

    // The 4's pixels at (10, 20) and at (22, 20), and nothing else.
    framebuffer expected;

    for (const std::size_t left : {std::size_t{10}, std::size_t{22}}) {
        const glyph four = glyph_of('4');

        for (std::size_t column = 0; column < glyph_width; ++column) {
            for (std::size_t row = 0; row < glyph_height; ++row) {
                if (((static_cast<unsigned>(four[column]) >> row) & 1U) != 0) {
                    expected.light(left + column, 20 + row);
                }
            }
        }
    }

    CHECK(screen == expected);

    // At the bottom-right corner, only the 4's first 3 columns of its first 4 rows are on the
    // screen, and of those, its diagonal: ..# in row 1, .#. in row 2 and #.. in row 3.
    framebuffer corner;

    draw_text(corner, 125, 60, "4");

    framebuffer diagonal;

    diagonal.light(127, 61);
    diagonal.light(126, 62);
    diagonal.light(125, 63);

    CHECK(corner == diagonal);
}

TEST_CASE("text from an origin off the screen draws nothing, however far off it is")
{
    // Added to an origin near the largest size_t, a glyph's offsets would wrap onto the screen.
    constexpr std::size_t far = std::numeric_limits<std::size_t>::max();

    struct origin {
        std::size_t      x;
        std::size_t      y;
        std::string_view text;
    };

    for (const origin& off : {
             origin{.x = far, .y = 0, .text = "8"},
             origin{.x = 0, .y = far, .text = "8"},
             origin{.x = far - 2, .y = 10, .text = "8"},
             origin{.x = 10, .y = far - 2, .text = "8"},
             origin{.x = far - 5, .y = 10, .text = "88"},  // the second glyph would start at 0
             origin{.x = framebuffer::width, .y = 0, .text = "8"},
             origin{.x = 0, .y = framebuffer::height, .text = "8"},
         }) {
        CAPTURE(off.x);
        CAPTURE(off.y);

        framebuffer screen;

        draw_text(screen, off.x, off.y, off.text);

        CHECK(screen == framebuffer{});
    }
}

TEST_CASE("text that runs off the right edge stops there")
{
    // 22 glyphs reach the edge: the 22nd starts at 126, and shows its first 2 columns, .# of the
    // 8's top row and #. of its second.
    const std::string long_line(30, '8');

    framebuffer whole;
    framebuffer reaching;

    draw_text(whole, 0, 20, long_line);
    draw_text(reaching, 0, 20, std::string_view{long_line}.substr(0, 22));

    CHECK(whole == reaching);
    CHECK_FALSE(whole.lit(126, 20));
    CHECK(whole.lit(127, 20));
    CHECK(whole.lit(126, 21));
}

// The header's texts.

TEST_CASE("a line of text holds the 21 characters that fit across the screen, and drops the rest")
{
    static_assert(text_line::capacity == 21);

    text_line line;

    line.append("0123456789");
    line.append("0123456789");
    line.append('X');
    line.append("YZ");
    line.append('!');

    CHECK(line.view() == "01234567890123456789X");
}

TEST_CASE("the tempo has tenths below 1,000 BPM, right-aligned in 5 characters")
{
    CHECK(tempo_text(in_frames(24'000), rate_48k).view() == "120.0");
    CHECK(tempo_text(in_frames(22'050), rate_44k1).view() == "120.0");
    CHECK(tempo_text(in_frames(384'000), rate_48k).view() == "  7.5");     // 8 s a beat
    CHECK(tempo_text(in_frames(28'800'000), rate_48k).view() == "  0.1");  // 10 min a beat
}

TEST_CASE("the tempo rounds to the nearest, halves up")
{
    // 51,200 frames a beat at 48 kHz is exactly 56.25 BPM: a half tenth, rounded up. A tick
    // longer is a little under it, and a tick shorter a little over.
    const sample_time half = in_frames(51'200);

    CHECK(tempo_text(half, rate_48k).view() == " 56.3");
    CHECK(tempo_text(half + sample_time::from_ticks(1), rate_48k).view() == " 56.2");
    CHECK(tempo_text(half - sample_time::from_ticks(1), rate_48k).view() == " 56.3");

    // 1,024 frames is exactly 2,812.5 BPM: a half BPM, rounded up.
    const sample_time whole_half = in_frames(1'024);

    CHECK(tempo_text(whole_half, rate_48k).view() == " 2813");
    CHECK(tempo_text(whole_half + sample_time::from_ticks(1), rate_48k).view() == " 2812");
}

TEST_CASE("from 999.95 BPM the tempo is whole BPM, and above 99,999 it shows 99999")
{
    // A minute at 48 kHz is 188,743,680,000 ticks. The tempo rounds to 1,000.0 while the beat is
    // at most 20 × that / 19,999 ticks: 188,753,117.66, so 188,753,117 ticks and no more.
    constexpr std::int64_t minute    = 60LL * 48'000 * sample_time::ticks_per_sample;
    constexpr std::int64_t last_1000 = (20 * minute) / 19'999;

    static_assert(last_1000 == 188'753'117);

    CHECK(tempo_text(sample_time::from_ticks(last_1000), rate_48k).view() == " 1000");
    CHECK(tempo_text(sample_time::from_ticks(last_1000 + 1), rate_48k).view() == "999.9");

    CHECK(tempo_text(in_frames(768), rate_48k).view() == " 3750");  // pulses 4 ms apart

    // 1,887,437 ticks is 99,999.98 BPM, which rounds to 100,000.
    CHECK(tempo_text(sample_time::from_ticks(1'887'437), rate_48k).view() == "99999");
    CHECK(tempo_text(sample_time::from_ticks(1), rate_48k).view() == "99999");
}

TEST_CASE("the header's left text is the tempo, BPM and FREE, or else the clock's state")
{
    ui_model model;

    CHECK(header_left_text(model).view() == "NO CLOCK");

    model.clock = clock_state::acquiring;
    CHECK(header_left_text(model).view() == "SYNCING");

    model.clock       = clock_state::locked;
    model.beat_period = in_frames(24'000);
    CHECK(header_left_text(model).view() == "120.0 BPM");

    model.clock = clock_state::freewheeling;
    CHECK(header_left_text(model).view() == "120.0 BPM FREE");

    model.beat_period = sample_time::from_ticks(1);
    CHECK(header_left_text(model).view() == "99999 BPM FREE");

    // The tracker's periods are positive; any other is not divided by.
    model.clock = clock_state::locked;

    for (const std::int64_t ticks : {std::int64_t{0}, std::int64_t{-24'000}}) {
        model.beat_period = sample_time::from_ticks(ticks);
        CHECK(header_left_text(model).view() == "SYNCING");
    }
}

/// The header's right text for a model in `bar` and `beat`, with `beats_per_bar`.
[[nodiscard]] std::string right_text(std::optional<std::int64_t> bar,
                                     std::optional<std::int64_t> beat,
                                     std::int64_t                beats_per_bar)
{
    ui_model model;

    model.bar           = bar;
    model.beat          = beat;
    model.beats_per_bar = beats_per_bar;

    return std::string{header_right_text(model).view()};
}

TEST_CASE("the header's right text is the bar and beat, counted from 1, in 6 characters at most")
{
    CHECK(right_text(std::nullopt, std::nullopt, 4).empty());
    CHECK(right_text(std::nullopt, 2, 4).empty());
    CHECK(right_text(0, 0, 4) == "1.1");
    CHECK(right_text(20, 1, 4) == "21.2");
    CHECK(right_text(5, std::nullopt, 4) == "6");
    CHECK(right_text(-1, 3, 4) == "0.4");  // before a pending reset's bar 0

    // Up to 9 beats a bar, the bar has 4 digits, and past them its last 4.
    CHECK(right_text(9'998, 3, 4) == "9999.4");
    CHECK(right_text(9'999, 0, 4) == "0000.1");
    CHECK(right_text(10'003, 0, 4) == "0004.1");
    CHECK(right_text(std::numeric_limits<std::int64_t>::max(), 0, 4) == "5808.1");
    CHECK(right_text(9'998, 8, 9) == "9999.9");
    CHECK(right_text(7, 9, 4) == "8.0");  // a beat past the bar's, which the tracker makes none of

    // From 10 beats a bar, the beat has 2 digits and the bar 3.
    CHECK(right_text(998, 9, 10) == "999.10");
    CHECK(right_text(999, 0, 10) == "000.1");
    CHECK(right_text(7, 98, 99) == "8.99");
    CHECK(right_text(7, 99, 100) == "8.00");
    CHECK(right_text(998, 98, 99) == "999.99");
}

TEST_CASE("the widest header texts are further apart than words")
{
    // The longest left text ends at pixel 82, and the longest right text, drawn to the right
    // edge, starts at 93: 10 dark columns between them, where a space between words leaves 7.
    CHECK(text_width("99999 BPM FREE") - 1 == 82);
    CHECK(framebuffer::width - text_width("9999.4") == 93);
    CHECK(framebuffer::width - text_width("999.99") == 93);
    CHECK(text_width("A B") - (2 * glyph_width) == 7);
}

// The drawing's rules.

TEST_CASE("the header's left text starts at the left edge, and its right text ends at the right")
{
    const framebuffer screen = drawn(bars_at_120());

    framebuffer expected;

    draw_text(expected, 0, 0, "120.0 BPM");
    draw_text(expected, 105, 0, "21.2");  // 128 less its 23 pixels

    CHECK(same_rows(screen, expected, 0, 8));
}

TEST_CASE("a recorded column lights the centre row, its left level above it and its right below")
{
    // Column 100 is clear of the header's text.
    constexpr std::size_t column = 100;

    for (std::uint8_t left = 0; left <= peak_level::max_level; ++left) {
        for (std::uint8_t right = 0; right <= peak_level::max_level; ++right) {
            CAPTURE(static_cast<int>(left));
            CAPTURE(static_cast<int>(right));

            ui_model model;

            model.strip[column] = recorded(left, right);

            const framebuffer screen = drawn(model);

            CHECK(column_rows(screen, column) == rows_from(27U - left, 27U + right));
            CHECK(column_rows(screen, column - 1) == 0);
            CHECK(column_rows(screen, column + 1) == 0);
        }
    }

    // Worked by hand: full scale on both sides fills the strip, and silence is the centre alone.
    ui_model model;

    model.strip[column] = recorded(peak_level::max_level, peak_level::max_level);
    CHECK(column_rows(drawn(model), column) == strip_rows);

    model.strip[column] = recorded(0, 0);
    CHECK(column_rows(drawn(model), column) == centre_row);
}

TEST_CASE("a level above the top draws as the top")
{
    ui_model model;

    model.strip[100] = recorded(17, 255);

    CHECK(column_rows(drawn(model), 100) == strip_rows);
}

TEST_CASE("a column the tape does not hold stays dark, centre included")
{
    ui_model model;

    model.strip[100] = strip_column{.peaks = peak_level{.left = 9, .right = 9}, .recorded = false};

    CHECK(column_rows(drawn(model), 100) == 0);
}

TEST_CASE("in the bars view, the columns still to come are dots on the centre row, at even columns")
{
    for (const std::size_t now :
         {std::size_t{102}, std::size_t{103}, std::size_t{127}, std::size_t{128}}) {
        CAPTURE(now);

        // Every column recorded, even those to come, which the model never marks: they show as
        // still to come regardless.
        ui_model model = bars_at_120();

        model.now_column = now;
        model.strip.fill(recorded(4, 4));

        const framebuffer screen = drawn(model);

        for (std::size_t x = 0; x < framebuffer::width; ++x) {
            CAPTURE(x);

            const std::uint64_t strip = column_rows(screen, x) & strip_rows;

            if (x < now) {
                CHECK(strip == rows_from(23, 31));
            }
            else {
                CHECK(strip == (x % 2 == 0 ? centre_row : 0));
            }
        }
    }
}

TEST_CASE("the time view has no dots, whatever its now column says")
{
    ui_model model;

    model.now_column = 50;
    model.strip.fill(recorded(4, 4));

    const framebuffer screen = drawn(model);

    for (std::size_t x = 0; x < framebuffer::width; ++x) {
        CAPTURE(x);
        CHECK((column_rows(screen, x) & strip_rows) == rows_from(23, 31));
    }
}

TEST_CASE(
    "bar lines are ticks above and below the strip where each placed bar but the first starts")
{
    // The slots start at columns 0, 25, 51, 76 and 102. Just after the clock starts, slots before
    // first_bar_slot hold bars whose lines the grid cannot place, and get no tick.
    struct example {
        std::size_t                first_bar_slot;
        std::array<std::size_t, 4> lines;  // 128 for none
    };

    for (const example& placed : {
             example{.first_bar_slot = 0, .lines = {25, 51, 76, 102}},
             example{.first_bar_slot = 1, .lines = {25, 51, 76, 102}},
             example{.first_bar_slot = 2, .lines = {51, 76, 102, 128}},
             example{.first_bar_slot = 4, .lines = {102, 128, 128, 128}},
         }) {
        CAPTURE(placed.first_bar_slot);

        ui_model model = bars_at_120();

        model.now_column     = ui_model::strip_columns;  // no dots
        model.first_bar_slot = placed.first_bar_slot;

        const framebuffer bars = drawn(model);

        for (std::size_t x = 0; x < framebuffer::width; ++x) {
            CAPTURE(x);

            const bool line = std::ranges::find(placed.lines, x) != placed.lines.end();

            CHECK((column_rows(bars, x) & (tick_rows | strip_rows)) == (line ? tick_rows : 0));
        }
    }

    // The time view has none.
    ui_model model = bars_at_120();

    model.view = strip_view::time;

    const framebuffer time = drawn(model);

    for (std::size_t x = 0; x < framebuffer::width; ++x) {
        CHECK((column_rows(time, x) & tick_rows) == 0);
    }
}

TEST_CASE("a bracket under the whole bars carries their count, in the bars view only")
{
    // Worked by hand: the whole bars are columns 0 to 101, and the label, 35 pixels wide, starts
    // at (102 - 35) / 2 = 33.
    framebuffer expected;

    for (std::size_t y = 48; y <= 51; ++y) {
        expected.light(0, y);
        expected.light(101, y);
    }

    for (std::size_t x = 0; x <= 101; ++x) {
        expected.light(x, 51);
    }

    draw_text(expected, 33, 55, "4 BARS");

    ui_model model = bars_at_120();

    CHECK(same_rows(drawn(model), expected, 46, 63));

    model.view = strip_view::time;

    CHECK(same_rows(drawn(model), framebuffer{}, 46, 63));
}

TEST_CASE("drawing replaces what the screen held, and allocates nothing")
{
    ui_model model = bars_at_120();

    model.strip.fill(recorded(6, 3));

    const framebuffer fresh = drawn(model);

    framebuffer used;

    for (std::size_t y = 0; y < framebuffer::height; ++y) {
        for (std::size_t x = 0; x < framebuffer::width; ++x) {
            used.light(x, y);
        }
    }

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;
        draw(model, used);
    }

    CHECK(test::realtime_guard::violations() == 0U);
    CHECK(used == fresh);
}

// Golden images: the format.

TEST_CASE("a golden image is a plain PBM: a comment, the size, then a line for each row")
{
    framebuffer screen;

    screen.light(0, 0);
    screen.light(127, 63);

    const std::string dark(128, '0');

    std::string expected = "P1\n# Two corners.\n128 64\n";

    expected += "1" + dark.substr(1) + "\n";

    for (std::size_t row = 1; row < 63; ++row) {
        expected += dark + "\n";
    }

    expected += dark.substr(1) + "1\n";

    const std::string text = test::to_pbm(screen, "Two corners.");

    CHECK(text == expected);
    CHECK(test::from_pbm(text) == screen);

    // Whitespace and comments may fall anywhere in the header.
    CHECK(test::from_pbm("P1 # a\n# b\n 128\n# c\n64" + text.substr(text.find("64\n") + 2)) ==
          screen);

    // Anything else is not a picture of the screen.
    std::string wrong_size = text;

    wrong_size.replace(wrong_size.find("128 64"), 6, "128 63");

    std::string not_a_pixel = text;

    not_a_pixel[not_a_pixel.size() - 2] = '2';

    CHECK_FALSE(test::from_pbm(wrong_size).has_value());
    CHECK_FALSE(test::from_pbm(not_a_pixel).has_value());
    CHECK_FALSE(test::from_pbm(text.substr(0, text.size() - 2)).has_value());  // a pixel short
    CHECK_FALSE(test::from_pbm(text + "0").has_value());                       // a pixel over
    CHECK_FALSE(test::from_pbm("P4" + text.substr(2)).has_value());
}

// Golden images: the models.

enum class bar_sound : std::uint8_t {
    none,     // a bar the grid cannot place
    drums,    // a hit on each beat that fades, hats between
    riff,     // the drums, with a riff under them
    silence,  // recorded, and quieter than -45 dBFS
};

/// Column `index` of a bar of `width` columns. Integers only.
[[nodiscard]] strip_column bar_column(bar_sound sound, std::size_t index, std::size_t width)
{
    if (sound == bar_sound::none) {
        return {};
    }

    if (sound == bar_sound::silence) {
        return recorded(0, 0);
    }

    const std::size_t beat       = (index * 4) / width;
    const std::size_t beat_start = ((beat * width) + 3) / 4;
    const auto        since      = static_cast<int>(index - beat_start);
    const int         hit        = beat % 2 == 0 ? 15 : 13;
    const int         drum       = std::max(0, hit - (2 * since));
    const int         hat        = (index % 3 == 0) ? 6 : 3;
    const int         riff = sound == bar_sound::riff ? 7 + static_cast<int>((index / 3) % 3) : 0;

    const int left  = std::max({drum, riff, hat});
    const int right = std::max({drum - static_cast<int>(index % 2), riff - 1, hat});

    return recorded(static_cast<std::uint8_t>(left), static_cast<std::uint8_t>(right));
}

/// The bars view at 120 BPM with each slot playing `sounds`, up to `now`.
[[nodiscard]] ui_model bars_playing(std::array<bar_sound, ui_model::bar_slots> sounds,
                                    std::size_t                                now)
{
    ui_model model = bars_at_120();

    model.now_column = now;

    while (model.first_bar_slot < ui_model::bar_slots - 1 &&
           sounds[model.first_bar_slot] == bar_sound::none) {
        ++model.first_bar_slot;
    }

    for (std::size_t slot = 0; slot < ui_model::bar_slots; ++slot) {
        const std::size_t start = ui_model::slot_start(slot);
        const std::size_t width = ui_model::slot_start(slot + 1) - start;

        for (std::size_t index = 0; index < width && start + index < now; ++index) {
            model.strip[start + index] = bar_column(sounds[slot], index, width);
        }
    }

    return model;
}

/// A busy mix as the time view shows it: levels 5 to 11, louder every 9th column. Integers only.
[[nodiscard]] strip_column mix_column(std::size_t x)
{
    std::uint32_t hash = static_cast<std::uint32_t>(x) * 2'654'435'761U;

    hash ^= hash >> 15U;

    const std::uint32_t accent = x % 9 == 0 ? 4 : 0;

    return recorded(static_cast<std::uint8_t>(5 + (hash % 7) + accent),
                    static_cast<std::uint8_t>(5 + ((hash >> 8U) % 7) + accent));
}

TEST_CASE("golden image: power on")
{
    test::check_golden_image(drawn(ui_model{}),
                             "power_on",
                             "Nothing yet: no clock, and nothing recorded.");
}

TEST_CASE("golden image: no clock")
{
    ui_model model;

    for (std::size_t x = 77; x < ui_model::strip_columns; ++x) {
        model.strip[x] = mix_column(x);
    }

    test::check_golden_image(drawn(model),
                             "no_clock",
                             "No clock: the time view, with 12 s of tape at the right.");
}

TEST_CASE("golden image: syncing")
{
    ui_model model;

    model.clock = clock_state::acquiring;
    model.bar   = 0;
    model.beat  = 0;

    for (std::size_t x = 0; x < ui_model::strip_columns; ++x) {
        model.strip[x] = mix_column(x);
    }

    test::check_golden_image(drawn(model),
                             "syncing",
                             "The first pulse: SYNCING, bar 1 beat 1, and 30 s of tape.");
}

TEST_CASE("golden image: clock started")
{
    using enum bar_sound;

    ui_model model = bars_playing({none, none, drums, drums, drums}, 123);

    model.bar  = 2;
    model.beat = 3;

    test::check_golden_image(
        drawn(model),
        "clock_started",
        "The clock started 2 bars ago, in bar 3 beat 4: the bars before it stay empty, their "
        "lines unmarked.");
}

TEST_CASE("golden image: freewheeling")
{
    using enum bar_sound;

    ui_model model = bars_playing({riff, riff, silence, silence, silence}, 104);

    model.clock = clock_state::freewheeling;
    model.bar   = 40;
    model.beat  = 0;

    test::check_golden_image(
        drawn(model),
        "freewheeling",
        "Freewheeling, with the audio stopped 2 bars ago: silence is a bare centre line.");
}

TEST_CASE("golden image: the widest header")
{
    using enum bar_sound;

    ui_model model = bars_playing({riff, riff, riff, riff, riff}, 127);

    model.clock       = clock_state::freewheeling;
    model.beat_period = sample_time::from_ticks(1);
    model.bar         = 9'998;
    model.beat        = 3;

    test::check_golden_image(drawn(model),
                             "widest_header",
                             "The widest header texts: 99999 BPM FREE, and bar 9999 beat 4. "
                             "One column to come, an odd one, so no dot.");
}

TEST_CASE("golden image: the bar in progress just started")
{
    using enum bar_sound;

    ui_model model = bars_playing({riff, riff, riff, riff, riff}, 102);

    model.beat = 0;

    test::check_golden_image(drawn(model),
                             "bar_started",
                             "Bar 21 just started: its whole slot is still to come.");
}

TEST_CASE("golden image: levels")
{
    ui_model model;

    // Each level on the left, then on the right, then on both, then a hole, levels above the
    // top, and silence.
    for (std::size_t level = 0; level <= peak_level::max_level; ++level) {
        const auto value = static_cast<std::uint8_t>(level);

        model.strip[level]      = recorded(value, 0);
        model.strip[17 + level] = recorded(0, value);
        model.strip[34 + level] = recorded(value, value);
    }

    for (std::size_t x = 61; x < 64; ++x) {
        model.strip[x] = recorded(255, 17);
    }

    for (std::size_t x = 64; x < 81; ++x) {
        model.strip[x] = recorded(0, 0);
    }

    test::check_golden_image(drawn(model),
                             "levels",
                             "Each level from 0 to 16 on the left, the right and both; a hole; "
                             "levels above 16, drawn as 16; and silence.");
}

TEST_CASE("golden image: the font")
{
    const std::string_view characters = font_characters();

    framebuffer screen;

    draw_text(screen, 0, 0, characters.substr(0, 21));
    draw_text(screen, 0, 10, characters.substr(21, 21));
    draw_text(screen, 0, 20, characters.substr(42));
    draw_text(screen, 0, 30, "a~");

    test::check_golden_image(screen,
                             "font",
                             "Every glyph, then a and ~, which have none: hollow boxes.");
}

// Golden images: DESIGN §9.3's example, played through the engine.

// 120 BPM at 4 PPQN and 48 kHz: a pulse every 6,000 frames, a bar of 4/4 every 96,000.
constexpr std::int64_t pulse_frames = 6'000;
constexpr std::int64_t beat_frames  = 4 * pulse_frames;
constexpr std::int64_t bar_frames   = 4 * beat_frames;

/// Drums alone until bar 17, then a 2-bar riff: riff A in the odd bars and B in the even ones,
/// counted from 1. Made from integers, each sample a whole number of 1/32,768ths, so every
/// platform plays the same samples.
class example_signal {
public:
    struct stereo {
        float left;
        float right;
    };

    /// Frame `index`. Frames are asked for in order, since the noise runs on.
    [[nodiscard]] stereo at(std::int64_t index) noexcept
    {
        const std::int64_t bar       = index / bar_frames;
        const std::int64_t in_bar    = index % bar_frames;
        const std::int64_t beat      = in_bar / beat_frames;
        const std::int64_t in_beat   = in_bar % beat_frames;
        const std::int64_t in_eighth = in_bar % (beat_frames / 2);

        // A kick on beats 1 and 3: a 50 Hz square that fades over a quarter of a second.
        std::int64_t kick = 0;

        if (beat % 2 == 0 && in_beat < 12'000) {
            const std::int64_t sign = (in_beat / 480) % 2 == 0 ? 1 : -1;

            kick = sign * 29'000 * (12'000 - in_beat) / 12'000;
        }

        // A snare on beats 2 and 4, and a hat on every eighth: noise that fades.
        std::int64_t snare = 0;

        if (beat % 2 == 1 && in_beat < 8'000) {
            snare = noise() * 16 * (8'000 - in_beat) / 8'000;
        }

        std::int64_t hat = 0;

        if (in_eighth < 1'500) {
            hat = noise() * 2 * (1'500 - in_eighth) / 1'500;
        }

        // The riff from bar 17, the grid's bar 16: a square a beat, its pitch and level by beat.
        std::int64_t riff = 0;

        if (bar >= riff_from) {
            const auto& notes = (bar - riff_from) % 2 == 0 ? riff_a : riff_b;
            const note& now   = notes[static_cast<std::size_t>(beat)];

            riff = ((in_beat / now.half_period) % 2 == 0 ? 1 : -1) * now.level;
        }

        return {
            .left  = sample(kick + (snare * 3 / 5) + hat + riff),
            .right = sample(kick + snare + (hat / 2) + (riff * 4 / 5)),
        };
    }

private:
    struct note {
        std::int64_t half_period;  // frames
        std::int64_t level;        // 1/32,768ths
    };

    static constexpr std::int64_t        riff_from = 16;
    static constexpr std::array<note, 4> riff_a{
        note{.half_period = 109, .level = 4'000},
        note{.half_period = 92, .level = 6'500},
        note{.half_period = 82, .level = 4'000},
        note{.half_period = 109, .level = 2'600},
    };
    static constexpr std::array<note, 4> riff_b{
        note{.half_period = 73, .level = 6'500},
        note{.half_period = 82, .level = 2'600},
        note{.half_period = 92, .level = 5'200},
        note{.half_period = 123, .level = 4'000},
    };

    /// Noise in [-1000, 1000], from a linear congruential generator.
    [[nodiscard]] std::int64_t noise() noexcept
    {
        state_ = (state_ * 1'664'525U) + 1'013'904'223U;

        return static_cast<std::int64_t>((state_ >> 16U) % 2'001U) - 1'000;
    }

    [[nodiscard]] static float sample(std::int64_t value) noexcept
    {
        return static_cast<float>(std::clamp<std::int64_t>(value, -32'767, 32'767)) / 32'768.0F;
    }

    std::uint32_t state_{1};
};

TEST_CASE("golden image: DESIGN's example, played through the engine")
{
    constexpr std::size_t block_frames = 512;

    // Half a second into bar 21, the grid's bar 20, and on to the end of that bucket: 0.512 s.
    constexpr std::int64_t end = (20 * bar_frames) + 24'576;

    static_assert(end % 1'024 == 0 && end % static_cast<std::int64_t>(block_frames) == 0);

    std::vector<stereo_frame> tape(engine::history_pages *
                                   static_cast<std::size_t>(page_frames_for(rate_48k).value()));

    engine audio_engine{engine_config{.rate = rate_48k, .max_block_frames = block_frames}, tape};
    example_signal signal;

    std::vector<float>       left(block_frames);
    std::vector<float>       right(block_frames);
    std::vector<float>       out(block_frames);
    std::vector<clock_event> events;

    for (std::int64_t start = 0; start < end; start += static_cast<std::int64_t>(block_frames)) {
        for (std::size_t index = 0; index < block_frames; ++index) {
            const example_signal::stereo sound =
                signal.at(start + static_cast<std::int64_t>(index));

            left[index]  = sound.left;
            right[index] = sound.right;
        }

        // The block's pulses, on whole frames.
        events.clear();

        const std::int64_t stop = start + static_cast<std::int64_t>(block_frames);

        for (std::int64_t pulse = ((start + pulse_frames - 1) / pulse_frames) * pulse_frames;
             pulse < stop;
             pulse += pulse_frames) {
            events.push_back(clock_event{
                .type   = clock_event_type::pulse,
                .offset = in_frames(pulse - start),
            });
        }

        audio_engine.process(
            audio_block{
                .in_left   = left,
                .in_right  = right,
                .out_left  = out,
                .out_right = out,
            },
            events);
    }

    // The scene is the one DESIGN describes: bar 21 beat 2 at 120 BPM, its slot 7 columns in.
    const ui_model& model = audio_engine.ui();

    CHECK(model.clock == clock_state::locked);
    CHECK(model.beat_period == in_frames(beat_frames));
    CHECK(model.bar == 20);
    CHECK(model.beat == 1);
    CHECK(model.view == strip_view::bars);
    CHECK(model.first_bar_slot == 0);
    CHECK(model.now_column == 109);

    test::check_golden_image(drawn(model),
                             "example",
                             "DESIGN 9.3's example through the engine: 120 BPM, 0.512 s into "
                             "bar 21, a 2-bar riff from bar 17 over drums.");
}

}  // namespace
}  // namespace previously
