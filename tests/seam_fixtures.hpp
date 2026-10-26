// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: how a seam sounds (DESIGN.md §5.6). §5.6's six signals, each through every
// way a loop enters a downbeat or seeks, measured by the click metric and the RMS envelope.

#pragma once

#include <array>
#include <cstdint>

namespace previously::test {

/// §5.6's signals.
enum class test_signal : std::uint8_t {
    sine,        // 441.7 Hz, whose period divides no bar
    bass,        // 40 Hz
    dc_offset,   // the sine on 0.4 of DC, which the recorder blocks
    noise,       // white
    kick,        // a decaying 60 Hz thump on every line
    anti_phase,  // the sine, inverted on the right
};

inline constexpr std::array<test_signal, 6> every_signal{
    test_signal::sine,
    test_signal::bass,
    test_signal::dc_offset,
    test_signal::noise,
    test_signal::kick,
    test_signal::anti_phase,
};

/// Every way a seam is made (DESIGN.md §5.6, §5.7).
enum class seam_kind : std::uint8_t {
    wrap,               // a 1-bar loop's end into its own pre-roll
    move,               // a 2-bar loop's first bar into its second, on a clock 0.4% slower
    early_start,        // an entry from silence into a first downbeat
    early_replacement,  // an entry from one loop into another's first downbeat
    late_start,         // a seek from silence, mid-bar
    late_replacement,   // a seek from one loop into another, mid-bar
};

inline constexpr std::array<seam_kind, 6> every_seam{
    seam_kind::wrap,
    seam_kind::move,
    seam_kind::early_start,
    seam_kind::early_replacement,
    seam_kind::late_start,
    seam_kind::late_replacement,
};

/// What a seam measures.
struct seam_quality {
    /// The click metric: the steepest step around the seam, from 32 samples before it to 32 after,
    /// over the steepest in the body of the bar the incoming stream plays. A step is a first
    /// difference's power: a high-pass. The steepest, not the mean: over a seam's length, a mean
    /// spreads a one-sample cut too thin to tell from a crossfade.
    double click{0.0};

    /// The RMS envelope, in dB: the output's power through the seam against its two streams',
    /// weighted as the crossfade moves from one to the other, (1 − x)·a² + x·b². On a stationary
    /// signal that is the body's power; on a 40 Hz bass, whose cycle is three seams long, it is the
    /// power of the part of the cycle each stream is in. 0 where both streams are silent.
    double envelope{0.0};

    /// The same over whichever half of the seam is further from 0 dB.
    double worse_half{0.0};

    /// The two streams' correlation over the seam itself: 1 where either is silent.
    double correlation{0.0};

    /// For an entry, how many of the first 48 samples from the downbeat on (1 ms) differ from the
    /// incoming stream's.
    std::int64_t downbeat_mismatches{0};
};

/// Plays `signal` into an engine at 120 BPM in 4/4 at 48 kHz, makes a seam of `kind`, and measures
/// it from the engine's output.
[[nodiscard]] seam_quality measure_seam(test_signal signal, seam_kind kind);

/// Checks a seam against §5.6's bounds.
void check_seam(test_signal signal, seam_kind kind);

}  // namespace previously::test
