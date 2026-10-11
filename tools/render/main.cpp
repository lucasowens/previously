// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — render tool: runs WAV files through the engine offline.
//
//   previously_render process  <in.wav> <out.wav> [--block <frames>]
//   previously_render generate <sine|impulses|noise> <out.wav> [--frames <count>] [--rate <hz>]
//
// `process` drives the exact engine the firmware runs, block by block, and writes the result.
// `generate` makes the deterministic stimulus files used by the golden tests.

#include "previously/engine.hpp"

#include "wav.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace previously::tools {
namespace {

constexpr std::size_t default_block_frames = 32;
constexpr std::size_t default_frames       = 12'000;
constexpr std::size_t default_rate_hz      = 48'000;

/// Writes one line to stderr.
void report(std::string_view message)
{
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
}

void print_usage()
{
    report("usage:\n"
           "  previously_render process  <in.wav> <out.wav> [--block <frames>]\n"
           "  previously_render generate <sine|impulses|noise> <out.wav> [--frames <count>] "
           "[--rate <hz>]");
}

[[nodiscard]] std::optional<std::size_t> parse_count(std::string_view text)
{
    const char* const first = std::to_address(text.begin());
    const char* const last  = std::to_address(text.end());

    std::size_t value        = 0;
    const auto [stop, error] = std::from_chars(first, last, value);

    if (error != std::errc{} || stop != last || value == 0) {
        return std::nullopt;
    }

    return value;
}

/// Finds `--name <count>` in the trailing options, or returns the fallback when absent.
[[nodiscard]] std::optional<std::size_t>
option_count(std::span<const std::string_view> options, std::string_view name, std::size_t fallback)
{
    const auto found = std::ranges::find(options, name);

    if (found == options.end()) {
        return fallback;
    }

    const auto value = std::next(found);

    return value != options.end() ? parse_count(*value) : std::nullopt;
}

[[nodiscard]] int run_process(std::span<const std::string_view> args)
{
    if (args.size() < 2) {
        print_usage();
        return 2;
    }

    const auto block_frames = option_count(args.subspan(2), "--block", default_block_frames);

    if (!block_frames) {
        report("--block must be a positive integer");
        return 2;
    }

    stereo_audio input;

    if (const status result = read_wav(args[0], input); !result.ok()) {
        report(result.error);
        return 1;
    }

    // The engine's limit depends on the sample rate, which comes from the file.
    const std::size_t limit = engine::max_block_frames(sample_rate{input.sample_rate_hz});

    if (*block_frames > limit) {
        report("--block must be at most " + std::to_string(limit) + " at " +
               std::to_string(input.sample_rate_hz) + " Hz");
        return 2;
    }

    stereo_audio output;
    output.sample_rate_hz = input.sample_rate_hz;
    output.left.resize(input.frame_count());
    output.right.resize(input.frame_count());

    engine audio_engine{engine_config{
        .rate             = sample_rate{input.sample_rate_hz},
        .max_block_frames = *block_frames,
    }};

    for (std::size_t start = 0; start < input.frame_count(); start += *block_frames) {
        const std::size_t count = std::min(*block_frames, input.frame_count() - start);

        audio_engine.process(audio_block{
            .in_left   = std::span{input.left}.subspan(start, count),
            .in_right  = std::span{input.right}.subspan(start, count),
            .out_left  = std::span{output.left}.subspan(start, count),
            .out_right = std::span{output.right}.subspan(start, count),
        });
    }

    if (const status result = write_wav(args[1], output); !result.ok()) {
        report(result.error);
        return 1;
    }

    return 0;
}

/// A tiny deterministic generator (xorshift32), identical on every platform.
[[nodiscard]] float next_noise(std::uint32_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;

    return (static_cast<float>(state) / 2'147'483'648.0F) - 1.0F;
}

[[nodiscard]] int run_generate(std::span<const std::string_view> args)
{
    if (args.size() < 2) {
        print_usage();
        return 2;
    }

    const std::string_view kind   = args[0];
    const auto             frames = option_count(args.subspan(2), "--frames", default_frames);

    if (!frames) {
        report("--frames must be a positive integer");
        return 2;
    }

    const auto rate = option_count(args.subspan(2), "--rate", default_rate_hz);

    if (!rate || *rate > 768'000) {
        report("--rate must be between 1 and 768000");
        return 2;
    }

    stereo_audio audio;
    audio.sample_rate_hz = static_cast<std::uint32_t>(*rate);
    audio.left.resize(*frames);
    audio.right.resize(*frames);

    constexpr float two_pi    = 2.0F * std::numbers::pi_v<float>;
    const auto      rate_hz   = static_cast<float>(*rate);
    std::uint32_t   noise_rng = 0x2545F491U;

    for (std::size_t i = 0; i < *frames; ++i) {
        const float t = static_cast<float>(i) / rate_hz;

        if (kind == "sine") {
            audio.left[i]  = 0.5F * std::sin(two_pi * 440.0F * t);
            audio.right[i] = 0.5F * std::sin(two_pi * 660.0F * t);
        }
        else if (kind == "impulses") {
            const bool impulse = i % 4'800U == 0U;

            audio.left[i]  = impulse ? 1.0F : 0.0F;
            audio.right[i] = impulse ? -1.0F : 0.0F;
        }
        else if (kind == "noise") {
            audio.left[i]  = 0.5F * next_noise(noise_rng);
            audio.right[i] = 0.5F * next_noise(noise_rng);
        }
        else {
            report("unknown stimulus: " + std::string{kind});
            return 2;
        }
    }

    if (const status result = write_wav(args[1], audio); !result.ok()) {
        report(result.error);
        return 1;
    }

    return 0;
}

}  // namespace
}  // namespace previously::tools

int main(int argc, char** argv)
{
    if (argc < 2) {
        previously::tools::print_usage();
        return 2;
    }

    const std::span<char*>              raw_args{argv, static_cast<std::size_t>(argc)};
    const std::vector<std::string_view> args{raw_args.begin(), raw_args.end()};

    const std::string_view                  subcommand = args[1];
    const std::span<const std::string_view> rest       = std::span{args}.subspan(2);

    if (subcommand == "process") {
        return previously::tools::run_process(rest);
    }

    if (subcommand == "generate") {
        return previously::tools::run_generate(rest);
    }

    previously::tools::print_usage();
    return 2;
}
