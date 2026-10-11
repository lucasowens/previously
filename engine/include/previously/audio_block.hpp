// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — a view of one block of stereo audio.

#pragma once

#include <cstddef>
#include <span>

namespace previously {

/// Non-owning view of one block of non-interleaved stereo audio: inputs to read, outputs to fill.
/// The buffers belong to the caller (the codec DMA on device, the host otherwise). All four
/// spans have the same length.
struct audio_block {
    std::span<const float> in_left;
    std::span<const float> in_right;
    std::span<float>       out_left;
    std::span<float>       out_right;

    [[nodiscard]] constexpr std::size_t frame_count() const noexcept
    {
        return out_left.size();
    }

    [[nodiscard]] constexpr bool has_matching_lengths() const noexcept
    {
        const std::size_t frames = frame_count();

        return in_left.size() == frames && in_right.size() == frames && out_right.size() == frames;
    }
};

}  // namespace previously
