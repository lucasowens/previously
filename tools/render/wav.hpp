// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — render tool: minimal, strict WAV file reading and writing.
//
// Reads PCM (16-, 24- or 32-bit integer) and IEEE float (32-bit) files, mono or stereo, including
// WAVE_FORMAT_EXTENSIBLE headers. Mono is duplicated to both channels. Always writes 32-bit float
// stereo, so rendered output keeps the engine's full precision.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace previously::tools {

/// Non-interleaved stereo audio held in memory.
struct stereo_audio {
    std::uint32_t      sample_rate_hz{48'000U};
    std::vector<float> left;
    std::vector<float> right;

    [[nodiscard]] std::size_t frame_count() const noexcept
    {
        return left.size();
    }
};

/// The outcome of an operation that can fail: success, or a message saying why not.
struct status {
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

/// Reads a WAV file into `audio`. On failure `audio` is left unspecified.
[[nodiscard]] status read_wav(const std::filesystem::path& path, stereo_audio& audio);

/// Writes `audio` as a 32-bit float stereo WAV file.
[[nodiscard]] status write_wav(const std::filesystem::path& path, const stereo_audio& audio);

}  // namespace previously::tools
