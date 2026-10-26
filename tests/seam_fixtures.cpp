// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: how a seam sounds.
//
// As in test_playback.cpp: 120 BPM in 4/4 at 48 kHz, the clock from sample 0, so a pulse is 6,000
// samples, a bar 96,000, and bar n's line is at n × 96,000. F is 384 samples, and a seek 144. Each
// signal plays until its loop is captured, and stops, so that the output is the loop alone.

#include "seam_fixtures.hpp"

#include "previously/capture_bars.hpp"
#include "previously/captured_loop.hpp"
#include "previously/engine.hpp"
#include "previously/sample_time.hpp"
#include "previously/units.hpp"

#include "capture_fixtures.hpp"
#include "planted_clock.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace previously::test {
namespace {

constexpr sample_rate  rate_48k{48'000U};
constexpr std::int64_t pulses_per_bar = 16;
constexpr std::int64_t pulse_samples  = 6'000;
constexpr std::int64_t bar_samples    = pulses_per_bar * pulse_samples;
constexpr std::int64_t entry          = 384;
constexpr std::int64_t seek           = 144;
constexpr std::int64_t body_margin    = 4'800;  // the body leaves 0.1 s at each end of its bar
constexpr std::size_t  tape_pages     = 187;

[[nodiscard]] constexpr std::int64_t line(std::int64_t bar)
{
    return bar * bar_samples;
}

[[nodiscard]] std::string name_of(test_signal signal)
{
    switch (signal) {
        case test_signal::sine:
            return "sine";
        case test_signal::bass:
            return "bass";
        case test_signal::dc_offset:
            return "DC offset";
        case test_signal::noise:
            return "noise";
        case test_signal::kick:
            return "kick";
        case test_signal::anti_phase:
            return "anti-phase";
    }

    return "";
}

[[nodiscard]] std::string name_of(seam_kind kind)
{
    switch (kind) {
        case seam_kind::wrap:
            return "wrap";
        case seam_kind::move:
            return "move";
        case seam_kind::early_start:
            return "early start";
        case seam_kind::early_replacement:
            return "early replacement";
        case seam_kind::late_start:
            return "late start";
        case seam_kind::late_replacement:
            return "late replacement";
    }

    return "";
}

[[nodiscard]] stereo_frame signal_at(test_signal signal, std::int64_t sample)
{
    constexpr double two_pi = 6.283185307179586;

    const double t    = static_cast<double>(sample) / 48'000.0;
    const auto   sine = [t](double hertz) {
        return std::sin(two_pi * hertz * t);
    };

    switch (signal) {
        case test_signal::sine: {
            const auto v = static_cast<float>(0.5 * sine(441.7));
            return {.left = v, .right = v};
        }
        case test_signal::bass: {
            const auto v = static_cast<float>(0.5 * sine(40.0));
            return {.left = v, .right = v};
        }
        case test_signal::dc_offset: {
            const auto v = static_cast<float>(0.4 + (0.3 * sine(441.7)));
            return {.left = v, .right = v};
        }
        case test_signal::noise: {
            test_noise noise{static_cast<std::uint64_t>(sample) + 7U};
            const auto left = static_cast<float>(noise.uniform(0.5));
            return {.left = left, .right = static_cast<float>(noise.uniform(0.5))};
        }
        case test_signal::kick: {
            const double into = static_cast<double>(sample % bar_samples) / 48'000.0;
            const auto   v =
                static_cast<float>(0.8 * std::exp(-30.0 * into) * std::sin(two_pi * 60.0 * into));
            return {.left = v, .right = v};
        }
        case test_signal::anti_phase: {
            const auto v = static_cast<float>(0.5 * sine(441.7));
            return {.left = v, .right = -v};
        }
    }

    return {};
}

/// `signal` until `until`, and silence from then on.
[[nodiscard]] auto signal_until(test_signal signal, std::int64_t until)
{
    return [signal, until](std::int64_t sample) {
        return sample < until ? signal_at(signal, sample) : stereo_frame{};
    };
}

/// Where a seam and its body are.
struct seam_span {
    std::int64_t                first{0};  // the crossfade's first sample
    std::int64_t                end{0};    // one past its last
    std::int64_t                body_first{0};
    std::int64_t                body_end{0};
    std::optional<std::int64_t> downbeat{std::nullopt};  // an entry's
};

[[nodiscard]] double power(stereo_frame frame)
{
    const auto l = static_cast<double>(frame.left);
    const auto r = static_cast<double>(frame.right);

    return (l * l) + (r * r);
}

/// Measures a seam from the output, against the two streams it joins: each a frame at each
/// sample, as it would be heard alone.
template <typename Outgoing, typename Incoming>
[[nodiscard]] seam_quality quality_of(const clocked_engine& rig,
                                      const seam_span&      seam,
                                      const Outgoing&       outgoing,
                                      const Incoming&       incoming)
{
    // The steepest step: the largest power of the first difference, a high-pass that a cut stands
    // out from.
    const auto steepest = [&rig](std::int64_t first, std::int64_t end) {
        double most = 0.0;

        for (std::int64_t n = first; n < end; ++n) {
            const stereo_frame a = rig.output_at(n - 1);
            const stereo_frame b = rig.output_at(n);

            most =
                std::max(most,
                         power(stereo_frame{.left = b.left - a.left, .right = b.right - a.right}));
        }

        return most;
    };

    const auto length      = static_cast<double>(seam.end - seam.first);
    const auto envelope_of = [&](std::int64_t first, std::int64_t end) {
        double out     = 0.0;
        double streams = 0.0;

        for (std::int64_t n = first; n < end; ++n) {
            const double x = (static_cast<double>(n - seam.first) + 0.5) / length;

            out += power(rig.output_at(n));
            streams += ((1.0 - x) * power(outgoing(n))) + (x * power(incoming(n)));
        }

        return streams == 0.0 ? 0.0 : 10.0 * std::log10(out / streams);
    };

    seam_quality quality;

    quality.click =
        steepest(seam.first - 32, seam.end + 32) / steepest(seam.body_first, seam.body_end);

    const std::int64_t middle      = seam.first + ((seam.end - seam.first) / 2);
    const double       first_half  = envelope_of(seam.first, middle);
    const double       second_half = envelope_of(middle, seam.end);

    quality.envelope   = envelope_of(seam.first, seam.end);
    quality.worse_half = std::abs(first_half) > std::abs(second_half) ? first_half : second_half;

    double products = 0.0;
    double energy_a = 0.0;
    double energy_b = 0.0;

    for (std::int64_t n = seam.first; n < seam.end; ++n) {
        const stereo_frame a = outgoing(n);
        const stereo_frame b = incoming(n);

        products += (static_cast<double>(a.left) * static_cast<double>(b.left)) +
                    (static_cast<double>(a.right) * static_cast<double>(b.right));
        energy_a += power(a);
        energy_b += power(b);
    }

    quality.correlation = energy_a == 0.0 || energy_b == 0.0
                              ? 1.0
                              : products / (std::sqrt(energy_a) * std::sqrt(energy_b));

    if (seam.downbeat) {
        for (std::int64_t n = *seam.downbeat; n < *seam.downbeat + 48; ++n) {
            const stereo_frame got  = rig.output_at(n);
            const stereo_frame want = incoming(n);

            if (got.left != want.left || got.right != want.right) {
                ++quality.downbeat_mismatches;
            }
        }
    }

    return quality;
}

[[nodiscard]] engine_config blocks_of_480()
{
    return engine_config{.rate = rate_48k, .max_block_frames = 480};
}

[[nodiscard]] stereo_frame silence(std::int64_t /*sample*/)
{
    return {};
}

/// A loop's frames from `first` to `end` on the tape, kept before it can leave the slot.
class kept_frames {
public:
    kept_frames(const engine&        audio,
                const captured_loop& loop,
                std::int64_t         first,
                std::int64_t         end)
        : first_{first}
    {
        frames_.reserve(static_cast<std::size_t>(end - first));

        for (std::int64_t position = first; position < end; ++position) {
            frames_.push_back(frame_held(audio, loop, position));
        }
    }

    [[nodiscard]] stereo_frame at(std::int64_t position) const
    {
        return frames_.at(static_cast<std::size_t>(position - first_));
    }

private:
    std::int64_t              first_;
    std::vector<stereo_frame> frames_;
};

[[nodiscard]] seam_quality through_wrap(test_signal signal)
{
    // A 1-bar loop of bar 3, from a late press 0.3 beat into bar 4. At bar 6's line it wraps: over
    // the F before it, the end of bar 3 crossfades into the F before bar 3's line.
    constexpr std::int64_t pressed = line(4) + 7'200;

    std::vector<stereo_frame> tape = tape_of(tape_pages, rate_48k);
    clocked_engine            rig{blocks_of_480(), tape};
    const engine&             audio = rig.target();

    rig.play(signal_until(signal, pressed));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 8 * pulses_per_bar);
    rig.press(at_sample(pressed), capture_bars::one);
    rig.keep_output();
    rig.run_until(samples{line(7)});

    const captured_loop& loop = audio.slot();

    REQUIRE(end_of(loop) == line(4));

    return quality_of(
        rig,
        seam_span{
            .first      = line(6) - entry,
            .end        = line(6),
            .body_first = line(6) + body_margin,
            .body_end   = line(7) - body_margin,
            .downbeat   = line(6),
        },
        [&](std::int64_t n) {
            return frame_held(audio, loop, line(4) - (line(6) - n));
        },
        [&](std::int64_t n) {
            return frame_held(audio, loop, line(3) - (line(6) - n));
        });
}

[[nodiscard]] seam_quality through_move(test_signal signal)
{
    // A 2-bar loop of bars 2 and 3, from a late press 0.3 beat into bar 4, and the clock from bar
    // 5's line at 119.5 BPM. Once the tracker has the new tempo, each downbeat is due 401 or 402
    // samples after its natural one: over the F before the natural downbeat, the end of the loop's
    // first bar crossfades into the audio before its second bar's line, that far back.
    constexpr std::int64_t pressed = line(4) + 7'200;

    const sample_time period = sample_time::from_ticks(std::llround(720'000.0 / 119.5 * 65'536.0));

    std::vector<stereo_frame> tape = tape_of(tape_pages, rate_48k);
    clocked_engine            rig{blocks_of_480(), tape};
    const engine&             audio = rig.target();

    rig.play(signal_until(signal, pressed));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 5 * pulses_per_bar);
    rig.steady_clock(at_sample(line(5)), period, 12 * pulses_per_bar);
    rig.press(at_sample(pressed), capture_bars::two);
    rig.keep_output();
    rig.run_until(samples{line(14)});

    const captured_loop& loop = audio.slot();

    REQUIRE(loop.bars() == 2U);

    // The first move into the second bar four bars after the change.
    const auto& played = rig.downbeats();
    std::size_t found  = 0;

    for (std::size_t i = 1; i < played.size() && found == 0; ++i) {
        if (played[i - 1].sample >= line(9) && played[i].bar == 1U) {
            found = i;
        }
    }

    REQUIRE(found > 0);

    const std::int64_t downbeat = loop.lines()[1].position.value();
    const std::int64_t natural  = played[found - 1].sample + bar_samples;
    const std::int64_t due      = played[found].sample;

    REQUIRE(due - natural >= 401);

    return quality_of(
        rig,
        seam_span{
            .first      = natural - entry,
            .end        = natural,
            .body_first = due + body_margin,
            .body_end   = due + bar_samples - body_margin,
            .downbeat   = due,
        },
        [&](std::int64_t n) {
            return frame_held(audio, loop, downbeat - (natural - n));
        },
        [&](std::int64_t n) {
            return frame_held(audio, loop, downbeat - (due - n));
        });
}

[[nodiscard]] seam_quality through_early_start(test_signal signal)
{
    // A 1-bar press 0.2 beat before bar 7's line holds bar 6, with nothing playing: over the F
    // before the line, silence crossfades into bar 6's pre-roll.
    constexpr std::int64_t pressed = line(7) - 4'800;

    std::vector<stereo_frame> tape = tape_of(tape_pages, rate_48k);
    clocked_engine            rig{blocks_of_480(), tape};
    const engine&             audio = rig.target();

    rig.play(signal_until(signal, pressed));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
    rig.press(at_sample(pressed), capture_bars::one);
    rig.keep_output();
    rig.run_until(samples{line(8)});

    const captured_loop& loop = audio.slot();

    REQUIRE(end_of(loop) == line(7));

    return quality_of(rig,
                      seam_span{
                          .first      = line(7) - entry,
                          .end        = line(7),
                          .body_first = line(7) + body_margin,
                          .body_end   = line(8) - body_margin,
                          .downbeat   = line(7),
                      },
                      silence,
                      [&](std::int64_t n) {
                          return frame_held(audio, loop, line(6) - (line(7) - n));
                      });
}

[[nodiscard]] seam_quality through_early_replacement(test_signal signal)
{
    // Bar 3 plays as a 1-bar loop, and a 1-bar press 0.2 beat before bar 7's line holds bar 6:
    // over the F before the line, the end of bar 3 crossfades into bar 6's pre-roll.
    constexpr std::int64_t pressed = line(7) - 4'800;

    std::vector<stereo_frame> tape = tape_of(tape_pages, rate_48k);
    clocked_engine            rig{blocks_of_480(), tape};
    const engine&             audio = rig.target();

    rig.play(signal_until(signal, pressed));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
    rig.press(at_sample(line(4) + 7'200), capture_bars::one);
    rig.press(at_sample(pressed), capture_bars::one);
    rig.keep_output();
    rig.run_until(samples{line(6)});

    const kept_frames bar_3{audio, audio.slot(), line(3), line(4)};

    rig.run_until(samples{line(8)});

    const captured_loop& loop = audio.slot();

    REQUIRE(end_of(loop) == line(7));

    return quality_of(
        rig,
        seam_span{
            .first      = line(7) - entry,
            .end        = line(7),
            .body_first = line(7) + body_margin,
            .body_end   = line(8) - body_margin,
            .downbeat   = line(7),
        },
        [&](std::int64_t n) {
            return bar_3.at(line(4) - (line(7) - n));
        },
        [&](std::int64_t n) {
            return frame_held(audio, loop, line(6) - (line(7) - n));
        });
}

[[nodiscard]] seam_quality through_late_start(test_signal signal)
{
    // A 1-bar press 0.3 beat into bar 7 holds bar 6, with nothing playing: a seek from silence into
    // bar 6, 7,200 samples in.
    constexpr std::int64_t pressed = line(7) + 7'200;

    std::vector<stereo_frame> tape = tape_of(tape_pages, rate_48k);
    clocked_engine            rig{blocks_of_480(), tape};
    const engine&             audio = rig.target();

    rig.play(signal_until(signal, line(7)));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
    rig.press(at_sample(pressed), capture_bars::one);
    rig.keep_output();
    rig.run_until(samples{line(8)});

    const captured_loop& loop = audio.slot();

    REQUIRE(end_of(loop) == line(7));

    return quality_of(rig,
                      seam_span{
                          .first      = pressed,
                          .end        = pressed + seek,
                          .body_first = pressed + body_margin,
                          .body_end   = line(8) - body_margin,
                      },
                      silence,
                      [&](std::int64_t n) {
                          return frame_held(audio, loop, line(6) + (n - line(7)));
                      });
}

[[nodiscard]] seam_quality through_late_replacement(test_signal signal)
{
    // Bar 3 plays as a 1-bar loop, and a 1-bar press 0.3 beat into bar 7 holds bar 6: a seek from
    // bar 3 into bar 6, both 7,200 samples in.
    constexpr std::int64_t pressed = line(7) + 7'200;

    std::vector<stereo_frame> tape = tape_of(tape_pages, rate_48k);
    clocked_engine            rig{blocks_of_480(), tape};
    const engine&             audio = rig.target();

    rig.play(signal_until(signal, line(7)));
    rig.steady_clock(at_sample(0), at_sample(pulse_samples), 9 * pulses_per_bar);
    rig.press(at_sample(line(4) + 7'200), capture_bars::one);
    rig.press(at_sample(pressed), capture_bars::one);
    rig.keep_output();
    rig.run_until(samples{line(7)});

    const kept_frames bar_3{audio, audio.slot(), line(3), line(4)};

    rig.run_until(samples{line(8)});

    const captured_loop& loop = audio.slot();

    REQUIRE(end_of(loop) == line(7));

    return quality_of(
        rig,
        seam_span{
            .first      = pressed,
            .end        = pressed + seek,
            .body_first = pressed + body_margin,
            .body_end   = line(8) - body_margin,
        },
        [&](std::int64_t n) {
            return bar_3.at(line(3) + (n - line(7)));
        },
        [&](std::int64_t n) {
            return frame_held(audio, loop, line(6) + (n - line(7)));
        });
}

}  // namespace

seam_quality measure_seam(test_signal signal, seam_kind kind)
{
    switch (kind) {
        case seam_kind::wrap:
            return through_wrap(signal);
        case seam_kind::move:
            return through_move(signal);
        case seam_kind::early_start:
            return through_early_start(signal);
        case seam_kind::early_replacement:
            return through_early_replacement(signal);
        case seam_kind::late_start:
            return through_late_start(signal);
        case seam_kind::late_replacement:
            return through_late_replacement(signal);
    }

    return {};
}

void check_seam(test_signal signal, seam_kind kind)
{
    const std::string signal_name = name_of(signal);
    const std::string seam_name   = name_of(kind);

    CAPTURE(signal_name);
    CAPTURE(seam_name);

    const seam_quality quality = measure_seam(signal, kind);

    // No click. Measured: at most 2.74, a late start's 3 ms fade-in of the bass from silence; with
    // no crossfade, a cut measures 29 to 16,705 on every signal but two. White noise steps as
    // steeply as any cut, and hides one. The kick's body is its own decaying tail, less steep than
    // its attack anywhere, so the kick is checked by its transient instead.
    if (signal != test_signal::kick) {
        CHECK(quality.click < 4.0);
    }

    // The downbeat plays at full gain, after the crossfade: the kick's attack survives, to the bit.
    if (kind != seam_kind::late_start && kind != seam_kind::late_replacement) {
        CHECK(quality.downbeat_mismatches == 0);
    }

    // A start from silence is a fade-in, not a seam (DESIGN.md §5.6).
    if (kind == seam_kind::early_start || kind == seam_kind::late_start) {
        return;
    }

    // Two streams opposed in phase cancel as they cross: through an equal-power crossfade, their
    // mean power is 1 + ρ · 2/π for their correlation ρ over it. The sine's wrap, at ρ = −0.81,
    // measures −3.13 dB, where that predicts −3.15. Within 1 dB of that dip, and 1 dB over.
    const double dip =
        10.0 * std::log10(1.0 + (std::min(quality.correlation, 0.0) * 2.0 / std::numbers::pi));

    CHECK(quality.envelope >= dip - 1.0);
    CHECK(quality.envelope <= 1.0);

    // The same over each half, but for the bass. A 40 Hz cycle is three seams long, so its streams'
    // correlation over one half of the seam is not their correlation over the other, and one shape
    // cannot suit both (§5.2): pinned at 2 dB, where its move measures 1.27 dB.
    const double half_bound = signal == test_signal::bass ? 2.0 : 1.0;

    CHECK(quality.worse_half >= dip - half_bound);
    CHECK(quality.worse_half <= half_bound);
}

}  // namespace previously::test
