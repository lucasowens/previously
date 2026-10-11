// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: WAV reading and writing used by the render tool.

#include "wav.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace previously::tools {
namespace {

/// A file path in the system temp directory, removed when the test ends.
class temp_file {
public:
    explicit temp_file(const std::string& name)
        : path_{std::filesystem::temp_directory_path() / ("previously_test_" + name)}
    {
    }

    ~temp_file()
    {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    temp_file(const temp_file&)            = delete;
    temp_file& operator=(const temp_file&) = delete;
    temp_file(temp_file&&)                 = delete;
    temp_file& operator=(temp_file&&)      = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char*>(bytes.data()),  // NOLINT(*-reinterpret-cast)
               static_cast<std::streamsize>(bytes.size()));
}

/// Builds the bytes of a WAV file field by field, so a test reads like the file layout.
class wav_bytes {
public:
    wav_bytes& id(std::string_view four_cc)
    {
        for (const char c : four_cc) {
            bytes_.push_back(static_cast<std::uint8_t>(c));
        }

        return *this;
    }

    wav_bytes& u16(std::uint16_t value)
    {
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
        bytes_.push_back(static_cast<std::uint8_t>(value >> 8U));

        return *this;
    }

    wav_bytes& u32(std::uint32_t value)
    {
        u16(static_cast<std::uint16_t>(value & 0xFFFFU));
        u16(static_cast<std::uint16_t>(value >> 16U));

        return *this;
    }

    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept
    {
        return bytes_;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

/// A "fmt " chunk for the given encoding, channel count and bit depth at 48 kHz.
wav_bytes&
append_format(wav_bytes& out, std::uint16_t encoding, std::uint16_t channels, std::uint16_t bits)
{
    const auto block_align = static_cast<std::uint16_t>(channels * (bits / 8U));

    out.id("fmt ").u32(16);
    out.u16(encoding);
    out.u16(channels);
    out.u32(48'000U);
    out.u32(48'000U * block_align);
    out.u16(block_align);
    out.u16(bits);

    return out;
}

TEST_CASE("float stereo audio round-trips bit-exactly")
{
    const temp_file file{"roundtrip.wav"};

    stereo_audio original;
    original.sample_rate_hz = 44'100U;
    original.left           = {0.0F, 0.25F, -1.0F, 0.999F, 1.0e-30F};
    original.right          = {1.0F, -0.5F, 0.125F, -0.999F, -1.0e-30F};

    REQUIRE(write_wav(file.path(), original).ok());

    stereo_audio loaded;
    REQUIRE(read_wav(file.path(), loaded).ok());

    CHECK(loaded.sample_rate_hz == 44'100U);
    CHECK(loaded.left == original.left);
    CHECK(loaded.right == original.right);
}

TEST_CASE("16-bit PCM mono decodes and is duplicated to both channels")
{
    const temp_file file{"pcm16_mono.wav"};

    // A 16-bit mono file with two samples: 0x4000 (+0.5) and 0xC000 (-0.5).
    wav_bytes wav;
    wav.id("RIFF").u32(40).id("WAVE");
    append_format(wav, 1, 1, 16);
    wav.id("data").u32(4);
    wav.u16(0x4000U).u16(0xC000U);

    write_bytes(file.path(), wav.bytes());

    stereo_audio audio;
    REQUIRE(read_wav(file.path(), audio).ok());

    CHECK(audio.sample_rate_hz == 48'000U);
    CHECK(audio.left == std::vector<float>{0.5F, -0.5F});
    CHECK(audio.right == audio.left);
}

TEST_CASE("malformed files are rejected with a reason")
{
    const temp_file file{"malformed.wav"};
    stereo_audio    audio;

    SUBCASE("missing file")
    {
        CHECK_FALSE(read_wav(file.path(), audio).ok());
    }

    SUBCASE("not a RIFF file")
    {
        wav_bytes wav;
        wav.id("JUNK").u32(4).id("WAVE");

        write_bytes(file.path(), wav.bytes());
        CHECK_FALSE(read_wav(file.path(), audio).ok());
    }

    SUBCASE("chunk size runs past the end of the file")
    {
        wav_bytes wav;
        wav.id("RIFF").u32(0).id("WAVE");
        wav.id("fmt ").u32(0xFFFFU);

        write_bytes(file.path(), wav.bytes());
        CHECK_FALSE(read_wav(file.path(), audio).ok());
    }

    SUBCASE("unsupported 8-bit encoding")
    {
        wav_bytes wav;
        wav.id("RIFF").u32(36).id("WAVE");
        append_format(wav, 1, 1, 8);
        wav.id("data").u32(0);

        write_bytes(file.path(), wav.bytes());
        CHECK_FALSE(read_wav(file.path(), audio).ok());
    }

    SUBCASE("data before fmt")
    {
        wav_bytes wav;
        wav.id("RIFF").u32(12).id("WAVE");
        wav.id("data").u32(0);

        write_bytes(file.path(), wav.bytes());
        CHECK_FALSE(read_wav(file.path(), audio).ok());
    }
}

}  // namespace
}  // namespace previously::tools
