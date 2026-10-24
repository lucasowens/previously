// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the screen's pixels, a bit each (DESIGN.md §9.3).
//
// The bytes keep the page order of the SSD1306 and SH1106 OLED controllers: 8 pages of 128 bytes,
// page p holding rows 8p to 8p + 7, and each byte 8 pixels down one column, the top one in bit 0.
// That is the order the controller takes over SPI, so a device can send the bytes as they are.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace previously {

class framebuffer {
public:
    static constexpr std::size_t width     = 128;
    static constexpr std::size_t height    = 64;
    static constexpr std::size_t page_rows = 8;
    static constexpr std::size_t pages     = height / page_rows;

    using bytes_type = std::array<std::uint8_t, width * pages>;

    /// Lights the pixel `x` from the left and `y` from the top. A pixel off the screen is ignored,
    /// so a drawing that runs over an edge is cut off there rather than written past the bytes.
    constexpr void light(std::size_t x, std::size_t y) noexcept
    {
        if (x < width && y < height) {
            bytes_[byte_of(x, y)] |= bit_of(y);
        }
    }

    /// Whether the pixel is lit. Off the screen, none is.
    [[nodiscard]] constexpr bool lit(std::size_t x, std::size_t y) const noexcept
    {
        return x < width && y < height && (bytes_[byte_of(x, y)] & bit_of(y)) != 0;
    }

    constexpr void clear() noexcept
    {
        bytes_.fill(0);
    }

    /// The pixels in the controller's order.
    [[nodiscard]] constexpr const bytes_type& bytes() const noexcept
    {
        return bytes_;
    }

    [[nodiscard]] friend constexpr bool operator==(const framebuffer&,
                                                   const framebuffer&) noexcept = default;

private:
    [[nodiscard]] static constexpr std::size_t byte_of(std::size_t x, std::size_t y) noexcept
    {
        return ((y / page_rows) * width) + x;
    }

    [[nodiscard]] static constexpr std::uint8_t bit_of(std::size_t y) noexcept
    {
        return static_cast<std::uint8_t>(1U << (y % page_rows));
    }

    bytes_type bytes_{};
};

}  // namespace previously
