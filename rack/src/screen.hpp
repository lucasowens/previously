// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack adapter: the screen, from the audio thread to the panel (DESIGN.md §9.3).
//
// The engine rebuilds its UI model on the audio thread; the panel draws on the UI thread. The
// module keeps a feed of models, a triple buffer outside the engine, so it outlives the engine's
// rebuilds at each sample rate. After each block the audio thread publishes the model if the
// engine rebuilt it, and the widget takes the newest when it draws.
//
// The widget paints the drawn framebuffer as rectangles, one for each column's run of lit
// pixels, so a full strip is a few hundred rectangles rather than thousands of pixels.

#pragma once

#include "previously/engine.hpp"
#include "previously/framebuffer.hpp"
#include "previously/triple_buffer.hpp"
#include "previously/ui_model.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace previously::vcv {

class screen_feed {
public:
    /// Audio thread, after each block: publishes the engine's model if it was rebuilt, which its
    /// serial says. Copying it only then keeps the copies to 46.9 a second at 48 kHz.
    void after_block(const engine& source) noexcept
    {
        const ui_model& model = source.ui();

        if (published_serial_ == model.serial) {
            return;
        }

        models_.publish(model);
        published_serial_ = model.serial;
    }

    /// Audio thread: the engine was rebuilt, and its serials start again, so the next block's
    /// model is published whatever its serial.
    void restart() noexcept
    {
        published_serial_.reset();
    }

    /// UI thread: takes the newest model, and says whether one was published since the last take.
    [[nodiscard]] bool take() noexcept
    {
        return models_.take();
    }

    /// UI thread: the model last taken, empty until the first.
    [[nodiscard]] const ui_model& model() const noexcept
    {
        return models_.read();
    }

private:
    triple_buffer<ui_model>      models_;
    std::optional<std::uint32_t> published_serial_;  // the audio thread's
};

/// Lit pixels in a column `x`, rows `top` to `bottom`, the bottom not included.
struct pixel_run {
    std::size_t x;
    std::size_t top;
    std::size_t bottom;
};

/// Calls `paint` with every run of lit pixels on `screen`, column by column from the left, and
/// from the top in each. Each run is as long as it can be: dark pixels or the edges bound it.
template <typename Paint>
void for_each_run(const framebuffer& screen, const Paint& paint)
{
    for (std::size_t x = 0; x < framebuffer::width; ++x) {
        std::size_t y = 0;

        while (y < framebuffer::height) {
            if (!screen.lit(x, y)) {
                ++y;
                continue;
            }

            const std::size_t top = y;

            while (y < framebuffer::height && screen.lit(x, y)) {
                ++y;
            }

            paint(pixel_run{.x = x, .top = top, .bottom = y});
        }
    }
}

}  // namespace previously::vcv
