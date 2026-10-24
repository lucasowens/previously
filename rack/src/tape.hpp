// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — Rack adapter: the tape, allocated once and kept through every sample-rate change.
//
// The engine's tape is counted in pages, and a page's frames follow the sample rate (DESIGN.md
// §2.3). Rack can change the rate on its audio thread: its Audio module sets the engine's rate
// from inside the audio callback, and the module rebuilds its engine there. Nothing is allocated
// on that thread. Instead the module allocates one tape when it is created, large enough for all
// 210 pages at 192 kHz, and the engine is rebuilt over the same memory at every rate (DESIGN.md
// §15, question 10).
//
// The memory is not written when it is allocated, so a fresh tape costs address space, not RAM,
// until the recorder writes it. Measured on macOS: allocating 210 MiB took under 10 µs and added
// no resident memory; 46 pages written at 48 kHz added 11.5 MiB. Two things keep memory resident
// after that (DESIGN.md §2.3): each rebuilt engine records from page 0 again, so a module stays at
// the history's size for the highest rate it has run at, 184 MiB at 768 kHz; and on macOS a
// deleted module's written pages stay in libmalloc's cache, and the next tape of this size takes
// them over. Above 192 kHz the same memory holds fewer pages, 105 at 384 kHz and 52 at 768 kHz,
// which still hold the whole history, with less room for loops.

#pragma once

#include "previously/engine.hpp"
#include "previously/page_pool.hpp"
#include "previously/units.hpp"

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>

namespace previously::vcv {

class tape {
public:
    /// The highest rate at which the tape holds every page an engine can use.
    static constexpr sample_rate full_rate{192'000U};

    /// 210 pages of 2^17 frames: 210 MiB.
    static constexpr std::size_t full_frames =
        page_pool::max_pages * static_cast<std::size_t>(page_frames_for(full_rate).value());

    /// Allocates `frames` frames without writing them. If the allocation fails the tape is empty,
    /// and an engine given it records nothing.
    explicit tape(std::size_t frames = full_frames) noexcept
        : memory_{new (std::nothrow) stereo_frame[frames]},
          frames_{memory_ ? frames : 0U}
    {
    }

    ~tape() = default;

    // An engine holds a span into the tape, so a tape stays where it is. A moved-from tape would
    // also keep its frame count with no memory behind it, and hand out a null span that claims
    // every frame.
    tape(const tape&)            = delete;
    tape& operator=(const tape&) = delete;
    tape(tape&&)                 = delete;
    tape& operator=(tape&&)      = delete;

    /// The memory for an engine at `rate`: all of it, or none when it cannot hold the history at
    /// that rate. A full tape holds it at every rate Rack offers, up to 768 kHz.
    [[nodiscard]] std::span<stereo_frame> for_rate(sample_rate rate) noexcept
    {
        if (!engine::holds_history(frames_, rate)) {
            return {};
        }

        return {memory_.get(), frames_};
    }

private:
    // new[] leaves a trivial frame unwritten, which is what keeps the tape out of RAM until it is
    // recorded on. A default member initializer would write all 210 MiB here.
    static_assert(std::is_trivially_default_constructible_v<stereo_frame>,
                  "the tape must not be written when it is allocated");

    // A std::vector would write every frame; this owns memory left unwritten.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
    std::unique_ptr<stereo_frame[]> memory_;
    std::size_t                     frames_;
};

}  // namespace previously::vcv
