// SPDX-License-Identifier: GPL-3.0-or-later
// Previously firmware — Daisy Pod (Seed 1.x or Seed3). P0: stereo passthrough through the engine.
//
// This file is the hardware adapter: it owns libDaisy, hands audio blocks to the engine, and will
// grow the control surface (pots, buttons, MIDI). libDaisy names are PascalCase; ours are not.

#include "previously/engine.hpp"

#include <daisy_pod.h>

#include <cstddef>
#include <span>

namespace {

constexpr std::size_t block_frames = 32;

// Hardware and engine live for the whole program and are reached from the audio interrupt,
// so they are file-scope objects. Neither allocates.
// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
daisy::DaisyPod hardware;

previously::engine audio_engine{previously::engine_config{
    .rate             = previously::sample_rate{48'000U},
    .max_block_frames = block_frames,
}};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// Runs in the audio DMA interrupt once per block.
void audio_callback(daisy::AudioHandle::InputBuffer  in,
                    daisy::AudioHandle::OutputBuffer out,
                    std::size_t                      size)
{
    const std::span<const float* const> inputs{in, 2};
    const std::span<float* const>       outputs{out, 2};

    audio_engine.process(previously::audio_block{
        .in_left   = std::span<const float>{inputs[0], size},
        .in_right  = std::span<const float>{inputs[1], size},
        .out_left  = std::span<float>{outputs[0], size},
        .out_right = std::span<float>{outputs[1], size},
    });
}

}  // namespace

int main()
{
    hardware.Init();
    hardware.SetAudioBlockSize(block_frames);
    hardware.SetAudioSampleRate(daisy::SaiHandle::Config::SampleRate::SAI_48KHZ);
    hardware.StartAudio(audio_callback);

    while (true) {
        // P0: nothing to do outside audio. Controls, MIDI and telemetry arrive in later phases.
    }
}
