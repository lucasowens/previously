// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — render tool: WAV reading and writing.
//
// The file is read into memory and parsed with explicit bounds checks on every field. Multi-byte
// values are assembled from bytes with shifts, so the code does not depend on host byte order.

#include "wav.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iterator>
#include <span>
#include <string_view>

namespace previously::tools {

namespace {

constexpr std::uint16_t format_pcm        = 0x0001;
constexpr std::uint16_t format_ieee_float = 0x0003;
constexpr std::uint16_t format_extensible = 0xFFFE;

using byte_span = std::span<const std::uint8_t>;

[[nodiscard]] std::uint16_t read_u16(byte_span bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(static_cast<unsigned>(bytes[offset]) |
                                      (static_cast<unsigned>(bytes[offset + 1]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32(byte_span bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool has_id(byte_span bytes, std::size_t offset, std::string_view id) noexcept
{
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (bytes[offset + i] != static_cast<std::uint8_t>(id[i])) {
            return false;
        }
    }

    return true;
}

/// The fields of a "fmt " chunk that decoding needs.
struct wav_format {
    std::uint16_t encoding{0};
    std::uint16_t channels{0};
    std::uint32_t sample_rate_hz{0};
    std::uint16_t block_align{0};
    std::uint16_t bits_per_sample{0};
};

[[nodiscard]] status parse_format(byte_span chunk, wav_format& format)
{
    if (chunk.size() < 16) {
        return {"fmt chunk is too short"};
    }

    format.encoding        = read_u16(chunk, 0);
    format.channels        = read_u16(chunk, 2);
    format.sample_rate_hz  = read_u32(chunk, 4);
    format.block_align     = read_u16(chunk, 12);
    format.bits_per_sample = read_u16(chunk, 14);

    // WAVE_FORMAT_EXTENSIBLE keeps the real encoding in the first two bytes of the sub-format GUID.
    if (format.encoding == format_extensible) {
        if (chunk.size() < 40) {
            return {"extensible fmt chunk is too short"};
        }

        format.encoding = read_u16(chunk, 24);
    }

    const bool pcm_ok   = format.encoding == format_pcm &&
                          (format.bits_per_sample == 16 || format.bits_per_sample == 24 ||
                           format.bits_per_sample == 32);
    const bool float_ok = format.encoding == format_ieee_float && format.bits_per_sample == 32;

    if (!pcm_ok && !float_ok) {
        return {"unsupported encoding (need 16/24/32-bit PCM or 32-bit float)"};
    }

    if (format.channels != 1 && format.channels != 2) {
        return {"unsupported channel count (need mono or stereo)"};
    }

    if (format.block_align != format.channels * (format.bits_per_sample / 8U)) {
        return {"block alignment does not match the sample format"};
    }

    return {};
}

/// Decodes the sample at `offset` to a float in [-1, 1).
[[nodiscard]] float decode_sample(byte_span data, std::size_t offset, const wav_format& format)
{
    if (format.encoding == format_ieee_float) {
        return std::bit_cast<float>(read_u32(data, offset));
    }

    switch (format.bits_per_sample) {
        case 16: {
            const auto value = static_cast<std::int16_t>(read_u16(data, offset));
            return static_cast<float>(value) / 32'768.0F;
        }
        case 24: {
            // Place the three bytes in the top of a 32-bit word so the sign bit lands in bit 31,
            // then scale as a 32-bit value.
            const std::uint32_t raw = (static_cast<std::uint32_t>(data[offset]) << 8U) |
                                      (static_cast<std::uint32_t>(data[offset + 1]) << 16U) |
                                      (static_cast<std::uint32_t>(data[offset + 2]) << 24U);
            return static_cast<float>(static_cast<std::int32_t>(raw)) / 2'147'483'648.0F;
        }
        default: {
            const auto value = static_cast<std::int32_t>(read_u32(data, offset));
            return static_cast<float>(value) / 2'147'483'648.0F;
        }
    }
}

[[nodiscard]] status decode_data(byte_span data, const wav_format& format, stereo_audio& audio)
{
    const std::size_t frames           = data.size() / format.block_align;
    const std::size_t bytes_per_sample = format.bits_per_sample / 8U;

    audio.sample_rate_hz = format.sample_rate_hz;
    audio.left.resize(frames);
    audio.right.resize(frames);

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t offset = frame * format.block_align;

        audio.left[frame]  = decode_sample(data, offset, format);
        audio.right[frame] = format.channels == 2
                                 ? decode_sample(data, offset + bytes_per_sample, format)
                                 : audio.left[frame];
    }

    return {};
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void append_id(std::vector<std::uint8_t>& out, std::string_view id)
{
    std::ranges::transform(id, std::back_inserter(out), [](char c) {
        return static_cast<std::uint8_t>(c);
    });
}

}  // namespace

status read_wav(const std::filesystem::path& path, stereo_audio& audio)
{
    std::ifstream file{path, std::ios::binary};

    if (!file) {
        return {"cannot open " + path.string()};
    }

    const std::vector<std::uint8_t> contents{std::istreambuf_iterator<char>{file},
                                             std::istreambuf_iterator<char>{}};
    const byte_span                 bytes{contents};

    if (bytes.size() < 12 || !has_id(bytes, 0, "RIFF") || !has_id(bytes, 8, "WAVE")) {
        return {path.string() + " is not a RIFF/WAVE file"};
    }

    wav_format format;
    bool       have_format = false;

    // Walk the chunks. Each one is a 4-byte id, a 4-byte size, then the body, padded to even size.
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const std::size_t body_size = read_u32(bytes, offset + 4);
        const std::size_t body      = offset + 8;

        if (body_size > bytes.size() - body) {
            return {path.string() + ": chunk runs past the end of the file"};
        }

        const byte_span chunk = bytes.subspan(body, body_size);

        if (has_id(bytes, offset, "fmt ")) {
            if (const status result = parse_format(chunk, format); !result.ok()) {
                return {path.string() + ": " + result.error};
            }

            have_format = true;
        }
        else if (has_id(bytes, offset, "data")) {
            if (!have_format) {
                return {path.string() + ": data chunk appears before fmt chunk"};
            }

            return decode_data(chunk, format, audio);
        }

        offset = body + body_size + (body_size % 2U);
    }

    return {path.string() + ": no data chunk"};
}

status write_wav(const std::filesystem::path& path, const stereo_audio& audio)
{
    if (audio.left.size() != audio.right.size()) {
        return {"left and right channels differ in length"};
    }

    constexpr std::uint16_t channels        = 2;
    constexpr std::uint16_t bits_per_sample = 32;
    constexpr std::uint16_t block_align     = channels * (bits_per_sample / 8U);

    const auto frames     = static_cast<std::uint32_t>(audio.frame_count());
    const auto data_bytes = frames * block_align;

    std::vector<std::uint8_t> out;
    out.reserve(58U + data_bytes);

    // RIFF header. Size covers everything after this field: WAVE + fmt (26) + fact (12) + data.
    append_id(out, "RIFF");
    append_u32(out, 4U + 26U + 12U + 8U + data_bytes);
    append_id(out, "WAVE");

    // fmt chunk: 18 bytes, as the spec requires for non-PCM encodings (cbSize = 0).
    append_id(out, "fmt ");
    append_u32(out, 18U);
    append_u16(out, format_ieee_float);
    append_u16(out, channels);
    append_u32(out, audio.sample_rate_hz);
    append_u32(out, audio.sample_rate_hz * block_align);
    append_u16(out, block_align);
    append_u16(out, bits_per_sample);
    append_u16(out, 0U);

    // fact chunk: frame count, required for non-PCM encodings.
    append_id(out, "fact");
    append_u32(out, 4U);
    append_u32(out, frames);

    append_id(out, "data");
    append_u32(out, data_bytes);

    for (std::size_t frame = 0; frame < audio.frame_count(); ++frame) {
        append_u32(out, std::bit_cast<std::uint32_t>(audio.left[frame]));
        append_u32(out, std::bit_cast<std::uint32_t>(audio.right[frame]));
    }

    std::ofstream file{path, std::ios::binary | std::ios::trunc};

    if (!file) {
        return {"cannot create " + path.string()};
    }

    file.write(reinterpret_cast<const char*>(out.data()),  // NOLINT(*-reinterpret-cast)
               static_cast<std::streamsize>(out.size()));

    if (!file) {
        return {"failed writing " + path.string()};
    }

    return {};
}

}  // namespace previously::tools
