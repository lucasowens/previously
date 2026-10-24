// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: drawing the screen.

#include "previously/display.hpp"

#include "previously/clock_tracker.hpp"
#include "previously/contract.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace previously {
namespace {

static_assert(ui_model::strip_columns == framebuffer::width, "a column of the strip is a pixel");
static_assert(ui_model::whole_bars > 1, "the bracket's label says BARS");

constexpr std::size_t  tempo_digits  = 5;
constexpr std::int64_t largest_tempo = 99'999;

// The bar, its point and its beat take at most 6 characters. The longest left text then ends 10
// pixels before the right text starts: more than a space between words, which is 7.
constexpr std::size_t bar_beat_characters = 6;

/// `numerator / denominator`, rounded to the nearest with halves up. `numerator` is not negative
/// and `denominator` is positive. The remainder is compared with the rest of the denominator
/// rather than doubled, so no denominator can overflow.
[[nodiscard]] constexpr std::int64_t rounded_quotient(std::int64_t numerator,
                                                      std::int64_t denominator) noexcept
{
    const std::int64_t quotient  = numerator / denominator;
    const std::int64_t remainder = numerator % denominator;

    return remainder >= denominator - remainder ? quotient + 1 : quotient;
}

[[nodiscard]] constexpr std::int64_t power_of_ten(std::size_t exponent) noexcept
{
    std::int64_t power = 1;

    for (std::size_t step = 0; step < exponent; ++step) {
        power *= 10;
    }

    return power;
}

/// Appends `value`, which is not negative, in decimal, padded on the left with `pad` to `width`.
void append_decimal(text_line& line, std::int64_t value, std::size_t width, char pad) noexcept
{
    std::array<char, 19> digits{};  // as many as the largest int64 has
    std::size_t          count = 0;

    // Zero has a digit too.
    while (count == 0 || value > 0) {
        digits[count] = static_cast<char>('0' + (value % 10));
        ++count;
        value /= 10;
    }

    for (std::size_t padding = count; padding < width; ++padding) {
        line.append(pad);
    }

    while (count > 0) {
        --count;
        line.append(digits[count]);
    }
}

/// Appends what `index` counts from 0, counted from 1 as musicians count, in at most `digits`
/// digits. Past that come its last digits, padded with zeros so the wrap shows. The engine's
/// indices are never below -1, a bar before a pending reset's bar 0 (clock_report), shown as 0.
void append_counted(text_line& line, std::int64_t index, std::size_t digits) noexcept
{
    const std::int64_t limit = power_of_ten(digits);

    if (index >= -1 && index < limit - 1) {
        append_decimal(line, index + 1, 1, '0');
        return;
    }

    append_decimal(line, floor_modulo(floor_modulo(index, limit) + 1, limit), digits, '0');
}

void draw_header(const ui_model& model, framebuffer& screen) noexcept
{
    const text_line left  = header_left_text(model);
    const text_line right = header_right_text(model);

    draw_text(screen, 0, screen_layout::header_top, left.view());
    draw_text(screen,
              framebuffer::width - text_width(right.view()),
              screen_layout::header_top,
              right.view());
}

/// A recorded column: the centre row, its left level as that many pixels above, and its right
/// level below. The recorder makes no level above the top; a model with one draws it as the top.
void draw_column(framebuffer& screen, std::size_t x, const peak_level& peaks) noexcept
{
    const std::size_t left  = std::min(peaks.left, peak_level::max_level);
    const std::size_t right = std::min(peaks.right, peak_level::max_level);

    for (std::size_t y = screen_layout::centre_row - left; y <= screen_layout::centre_row + right;
         ++y) {
        screen.light(x, y);
    }
}

void draw_strip(const ui_model& model, framebuffer& screen) noexcept
{
    for (std::size_t x = 0; x < ui_model::strip_columns; ++x) {
        // The rest of the bar in progress is still to come: dots on the centre row, at even
        // columns so that they stand still as the bar fills.
        if (model.view == strip_view::bars && x >= model.now_column) {
            if (x % 2 == 0) {
                screen.light(x, screen_layout::centre_row);
            }

            continue;
        }

        const strip_column& column = model.strip[x];

        if (column.recorded) {
            draw_column(screen, x, column.peaks);
        }
    }
}

/// Ticks where each bar slot after the first starts, above and below the strip. Slots before the
/// first placed bar hold bars from before the clock, whose lines the grid cannot place, so they
/// get none.
void draw_bar_lines(const ui_model& model, framebuffer& screen) noexcept
{
    for (std::size_t slot = std::max<std::size_t>(1, model.first_bar_slot);
         slot < ui_model::bar_slots;
         ++slot) {
        const std::size_t x = ui_model::slot_start(slot);

        for (std::size_t row = 0; row < screen_layout::tick_rows; ++row) {
            screen.light(x, screen_layout::top_ticks + row);
            screen.light(x, screen_layout::bottom_ticks + row);
        }
    }
}

/// A bracket under the whole bars, what a capture pressed now asks for, and its label.
void draw_whole_bars(framebuffer& screen) noexcept
{
    constexpr std::size_t left  = 0;
    constexpr std::size_t right = ui_model::slot_start(ui_model::whole_bars) - 1;

    for (std::size_t y = screen_layout::bracket_top; y <= screen_layout::bracket_bottom; ++y) {
        screen.light(left, y);
        screen.light(right, y);
    }

    for (std::size_t x = left; x <= right; ++x) {
        screen.light(x, screen_layout::bracket_bottom);
    }

    text_line label;

    append_decimal(label, static_cast<std::int64_t>(ui_model::whole_bars), 1, ' ');
    label.append(" BARS");

    const std::size_t centred = (left + right + 1 - text_width(label.view())) / 2;

    draw_text(screen, centred, screen_layout::label_top, label.view());
}

}  // namespace

text_line tempo_text(sample_time beat_period, sample_rate rate) noexcept
{
    expects(beat_period.ticks() > 0);

    // Beats a minute: the ticks in a minute over the ticks in a beat.
    const std::int64_t ticks_per_minute =
        std::int64_t{60} * std::int64_t{rate.hz()} * sample_time::ticks_per_sample;
    const std::int64_t tenths = rounded_quotient(10 * ticks_per_minute, beat_period.ticks());

    text_line line;

    if (tenths < 10'000) {
        append_decimal(line, tenths / 10, tempo_digits - 2, ' ');
        line.append('.');
        append_decimal(line, tenths % 10, 1, '0');
    }
    else {
        const std::int64_t whole = rounded_quotient(ticks_per_minute, beat_period.ticks());

        append_decimal(line, std::min(whole, largest_tempo), tempo_digits, ' ');
    }

    return line;
}

text_line header_left_text(const ui_model& model) noexcept
{
    const std::optional<sample_time> beat_period = model.beat_period;

    text_line line;

    // The tracker's beat periods are positive. A model with any other has no tempo to show.
    if (beat_period && beat_period->ticks() > 0) {
        line = tempo_text(*beat_period, model.rate);
        line.append(" BPM");

        if (model.clock == clock_state::freewheeling) {
            line.append(" FREE");
        }

        return line;
    }

    line.append(model.clock == clock_state::no_clock ? "NO CLOCK" : "SYNCING");

    return line;
}

text_line header_right_text(const ui_model& model) noexcept
{
    const std::optional<std::int64_t> bar  = model.bar;
    const std::optional<std::int64_t> beat = model.beat;

    text_line line;

    if (!bar) {
        return line;
    }

    // A beat takes 1 digit up to 9 beats a bar and 2 from 10, and the bar the rest.
    const std::size_t beat_digits = model.beats_per_bar <= 9 ? 1 : 2;
    const std::size_t bar_digits  = bar_beat_characters - 1 - beat_digits;

    append_counted(line, *bar, bar_digits);

    if (beat) {
        line.append('.');
        append_counted(line, *beat, beat_digits);
    }

    return line;
}

void draw(const ui_model& model, framebuffer& screen) noexcept
{
    screen.clear();

    draw_header(model, screen);
    draw_strip(model, screen);

    if (model.view == strip_view::bars) {
        draw_bar_lines(model, screen);
        draw_whole_bars(screen);
    }
}

}  // namespace previously
