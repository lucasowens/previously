// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: the loop player.

#include "previously/loop_player.hpp"

#include "previously/audio_floor.hpp"
#include "previously/contract.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace previously {
namespace {

/// F: the entry into a downbeat (DESIGN.md §5.2).
constexpr seconds entry_length{0.008F};

/// A seek (§5.4).
constexpr seconds seek_length{0.003F};

/// The shortest entry: with less time left before its downbeat, the downbeat is joined with a seek
/// instead (§5.3's floor).
constexpr seconds least_entry_length{0.002F};

/// A line already passed, which a reset can make, is passed over for the next. Lines are at least
/// 16 ms apart, so a couple of steps reach one still to come.
constexpr int most_lines_passed = 4;

/// The first whole sample at or after `when`.
[[nodiscard]] std::int64_t sample_at_or_after(sample_time when) noexcept
{
    const std::int64_t whole = when.whole_samples().value();

    return when > sample_time::from_samples(samples{whole}) ? whole + 1 : whole;
}

[[nodiscard]] sample_time at_sample(std::int64_t sample) noexcept
{
    return sample_time::from_samples(samples{sample});
}

[[nodiscard]] std::int64_t frames_of(seconds duration, sample_rate rate) noexcept
{
    return std::max(to_samples(duration, rate).value(), std::int64_t{1});
}

[[nodiscard]] std::int64_t position_of(const captured_loop& loop, std::size_t line) noexcept
{
    return loop.lines()[line].position.value();
}

/// sin(π/2 · x), for x from 0 to 1: the equal-power curve. Its slope is finite at both ends, so a
/// stream fading in starts at 1/500 of its level, where √x would start at 1/28: a step a 40 Hz bass
/// would show. Our own polynomial, not libm (ADR 0005): below the middle, the sine of an angle up
/// to π/4, and above it the cosine of what is left, each as its Taylor series to within 2 × 10⁻⁹.
[[nodiscard]] float quarter_sine(float x) noexcept
{
    constexpr float half_pi = 1.57079632679F;

    if (x <= 0.5F) {
        const float a  = half_pi * x;
        const float a2 = a * a;

        return a * (1.0F -
                    ((a2 * (1.0F / 6.0F)) *
                     (1.0F - ((a2 * (1.0F / 20.0F)) *
                              (1.0F - ((a2 * (1.0F / 42.0F)) * (1.0F - (a2 * (1.0F / 72.0F)))))))));
    }

    // 1 − x is exact here (Sterbenz).
    const float a  = half_pi * (1.0F - x);
    const float a2 = a * a;

    return 1.0F - ((a2 * 0.5F) * (1.0F - ((a2 * (1.0F / 12.0F)) *
                                          (1.0F - ((a2 * (1.0F / 30.0F)) *
                                                   (1.0F - ((a2 * (1.0F / 56.0F)) *
                                                            (1.0F - (a2 * (1.0F / 90.0F))))))))));
}

}  // namespace

float crossfade_gain(float correlation, float x) noexcept
{
    return (correlation * x) + ((1.0F - correlation) * quarter_sine(x));
}

loop_player::loop_player(const page_pool& pool,
                         sample_rate      rate,
                         sample_time      output_latency) noexcept
    : pool_{&pool},
      output_latency_{output_latency},
      entry_frames_{frames_of(entry_length, rate)},
      seek_frames_{frames_of(seek_length, rate)},
      least_entry_{frames_of(least_entry_length, rate)},
      window_frames_{loop_margin(rate).value()}
{
    expects(output_latency >= sample_time{});
}

void loop_player::update(sample_time when, capture& captured, const clock_tracker& grid) noexcept
{
    const std::int64_t now = sample_at_or_after(when);

    expects(now == now_);

    updated_at_ = when;

    // What time alone brings: a crossfade's end, and a downbeat.
    if (fade_ && now >= fade_->start + fade_->length) {
        fade_.reset();
    }

    if (next_ && now >= next_->at) {
        const downbeat_plan plan = *next_;

        next_.reset();
        reach_downbeat(now, plan);
    }

    follow_capture(now, captured, grid);

    // A waiting capture's entry, which comes before the playing loop's own move into that line.
    if (!fade_ && !entering_) {
        if (const std::optional<std::int64_t> due = entry_due(captured, grid);
            due && at_sample(*due) <= when) {
            enter(now, captured, grid);
        }
    }

    if (!fade_ && !entering_ && !next_ && playing_.loop != nullptr) {
        decide_line(when, grid);
    }

    // A leaving loop no longer read gives its pages back.
    const captured_loop& leaving = captured.leaving();

    if (!leaving.empty() && playing_.loop != &leaving &&
        (!fade_ || fade_->outgoing.loop != &leaving)) {
        captured.release_leaving();
    }
}

std::optional<std::int64_t> loop_player::next_moment(const capture&       captured,
                                                     const clock_tracker& grid) const noexcept
{
    // Every moment updates the player, so only moments still to come count.
    std::optional<std::int64_t> soonest;

    const auto consider = [&](std::int64_t at) {
        if (at_sample(at) > updated_at_ && (!soonest || at < *soonest)) {
            soonest = at;
        }
    };

    if (fade_) {
        consider(fade_->start + fade_->length);
    }

    if (next_) {
        consider(next_->at);
    }

    if (!fade_ && !entering_) {
        if (const std::optional<std::int64_t> due = entry_due(captured, grid)) {
            consider(*due);
        }
    }

    if (!fade_ && !entering_ && !next_ && playing_.loop != nullptr) {
        if (const std::optional<line_plan> plan = plan_line(now_, grid)) {
            consider(plan->decide_at);
        }
    }

    return soonest;
}

bool loop_player::holds_presses(std::int64_t now) const noexcept
{
    return fading(now) || entering_;
}

void loop_player::render(std::int64_t           first,
                         std::span<const float> in_left,
                         std::span<const float> in_right,
                         std::span<float>       out_left,
                         std::span<float>       out_right) noexcept
{
    expects(first == now_);
    expects(in_left.size() == out_left.size() && in_right.size() == out_left.size() &&
            out_right.size() == out_left.size());

    const std::size_t frames = out_left.size();
    std::size_t       i      = 0;

    // Through a crossfade, sample by sample. A crossfade begins at the next sample to render, so
    // it is all of a render's samples or the first of them.
    if (fade_) {
        crossfade& fade = *fade_;

        for (; i < frames && fading(first + static_cast<std::int64_t>(i)); ++i) {
            // At the middle of each sample, so that the two gains mirror each other exactly.
            const auto step =
                static_cast<float>((2 * (first + static_cast<std::int64_t>(i) - fade.start)) + 1);
            const float x        = step / static_cast<float>(2 * fade.length);
            const float incoming = crossfade_gain(fade.correlation, x);
            const float outgoing = crossfade_gain(fade.correlation, 1.0F - x);

            const stereo_frame leaving = read(fade.outgoing);
            const stereo_frame joining = read(playing_);

            const stereo_frame loop_frame{
                .left  = flush_non_audio((outgoing * leaving.left) + (incoming * joining.left)),
                .right = flush_non_audio((outgoing * leaving.right) + (incoming * joining.right)),
            };

            out_left[i]  = flush_non_audio(in_left[i] + loop_frame.left);
            out_right[i] = flush_non_audio(in_right[i] + loop_frame.right);
        }
    }

    // Then the playing stream alone, a page's run at a time. With no loop, the input passes through
    // to the bit.
    while (i < frames) {
        if (playing_.loop == nullptr) {
            std::copy(in_left.begin() + static_cast<std::ptrdiff_t>(i),
                      in_left.end(),
                      out_left.begin() + static_cast<std::ptrdiff_t>(i));
            std::copy(in_right.begin() + static_cast<std::ptrdiff_t>(i),
                      in_right.end(),
                      out_right.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }

        const held_run run = run_from(playing_, static_cast<std::int64_t>(frames - i));

        for (std::int64_t k = 0; k < run.count; ++k, ++i) {
            const stereo_frame frame =
                run.frames.empty() ? stereo_frame{} : run.frames[static_cast<std::size_t>(k)];

            out_left[i]  = flush_non_audio(in_left[i] + frame.left);
            out_right[i] = flush_non_audio(in_right[i] + frame.right);
        }

        playing_.position += run.count;
    }

    now_ = first + static_cast<std::int64_t>(out_left.size());
}

void loop_player::begin_block(std::int64_t block_start) noexcept
{
    block_start_    = block_start;
    downbeat_count_ = 0;
}

bool loop_player::playing() const noexcept
{
    return playing_.loop != nullptr || fade_.has_value();
}

std::size_t loop_player::bar() const noexcept
{
    return bar_;
}

std::span<const loop_downbeat> loop_player::downbeats() const noexcept
{
    return std::span{downbeats_}.first(downbeat_count_);
}

std::int64_t loop_player::downbeat_of(sample_time line) const noexcept
{
    return (line - output_latency_).nearest_sample().value();
}

std::optional<loop_player::line_plan>
loop_player::plan_line(std::int64_t now, const clock_tracker& grid) const noexcept
{
    const captured_loop& loop = *playing_.loop;

    // The line after the playing bar's, under the labels now: a reset may have renumbered the
    // playing bar's line, but not the pulse it starts with. Found by that pulse, not by the time of
    // the bar's downbeat, which can be before its line: by under a sample, or by more, where the
    // pre-roll could not reach back far enough.
    const std::int64_t pulses_per_bar = grid.pulses_per_bar();
    const std::int64_t playing_bar    = floor_divide(grid.index_of(line_sequence_), pulses_per_bar);

    std::int64_t               grid_bar = playing_bar + 1;
    std::optional<sample_time> line     = grid.bar_start(grid_bar);

    for (int passed = 0; line && downbeat_of(*line) <= now && passed < most_lines_passed;
         ++passed) {
        ++grid_bar;
        line = grid.bar_start(grid_bar);
    }

    if (!line || downbeat_of(*line) <= now) {
        return std::nullopt;
    }

    // The natural downbeat: when the playhead reaches the end of its bar. It does not move as the
    // playhead plays on, past it too.
    const std::int64_t natural = now + (next_downbeat_position() - playing_.position);

    // The due downbeat: on the line's nearest sample, unless the natural one is within a sample.
    const sample_time  target   = *line - output_latency_;
    const sample_time  distance = sample_time::from_samples(samples{natural}) - target;
    const bool         on_time  = abs(distance) < sample_time::from_samples(samples{1});
    const std::int64_t due      = on_time ? natural : target.nearest_sample().value();

    const auto due_bar = static_cast<std::size_t>(
        floor_modulo(grid_bar - loop.first_bar(), static_cast<std::int64_t>(loop.bars())));

    const std::int64_t line_sequence = grid.sequence_of(grid_bar * pulses_per_bar);

    if (due_bar == bar_ + 1 && due == natural) {
        return line_plan{
            .decide_at     = natural - entry_frames_,
            .entry_end     = natural,
            .natural       = natural,
            .due           = due,
            .bar           = due_bar,
            .line_sequence = line_sequence,
            .plays_on      = true,
        };
    }

    // The entry ends at whichever downbeat comes first: from the natural one to the due one, the
    // audio before the due downbeat plays. That audio must be held, and before the first bar it is
    // the 50 ms of pre-roll. A downbeat due later than that reaches is entered at the line from
    // the playing bar's own continuation instead: the 50 ms held after its end, and silence after
    // that, until the second playback PR seeks inside the last bar first (DESIGN.md §5.2).
    const std::int64_t room      = position_of(loop, due_bar) - loop.held().first.value();
    const bool         reachable = due - natural <= room - entry_frames_;
    const std::int64_t entry_end = reachable ? std::min(natural, due) : due;

    return line_plan{
        .decide_at     = entry_end - entry_frames_,
        .entry_end     = entry_end,
        .natural       = natural,
        .due           = due,
        .bar           = due_bar,
        .line_sequence = line_sequence,
        .plays_on      = false,
    };
}

std::optional<std::int64_t> loop_player::entry_due(const capture&       captured,
                                                   const clock_tracker& grid) const noexcept
{
    const captured_loop& waiting = captured.waiting();

    if (waiting.empty() || captured.waiting_entered()) {
        return std::nullopt;
    }

    const std::optional<std::int64_t> downbeat = end_downbeat(waiting, grid);

    // With under 2 ms left there is no entry: the loop starts with a seek once it has the slot.
    if (!downbeat || *downbeat - now_ < least_entry_) {
        return std::nullopt;
    }

    return std::max(*downbeat - entry_frames_, now_);
}

std::optional<std::int64_t> loop_player::end_downbeat(const captured_loop& loop,
                                                      const clock_tracker& grid) const noexcept
{
    const std::optional<sample_time> line =
        grid.pulse_time(grid.index_of(loop.lines().back().sequence));

    if (!line) {
        return std::nullopt;
    }

    return downbeat_of(*line);
}

std::int64_t loop_player::next_downbeat_position() const noexcept
{
    // The downbeat the playing stream is heading for: one decided, or the end of its bar.
    const std::size_t line = next_ ? next_->bar : bar_ + 1;

    return position_of(*playing_.loop, line);
}

void loop_player::follow_capture(std::int64_t         now,
                                 capture&             captured,
                                 const clock_tracker& grid) noexcept
{
    const captured_loop& slot = captured.slot();

    if (entering_) {
        // The entered capture took the slot, or was refused and left it. Refused, it gives way
        // once its entry's crossfade has ended, which it has already: a capture is refused only at
        // its line's edge, and only when that edge is late, since an edge earlier than predicted
        // makes its window shorter. The crossfade ended on the predicted line, or L_out before it.
        if (playing_.loop == &slot) {
            entering_ = false;
        }
        else if (playing_.loop == &captured.leaving() && !fade_) {
            entering_ = false;
            return_to_slot(now, captured);
        }

        return;
    }

    // A loop that took the slot without an entry: a late press, or a line too near for one. It
    // starts where it would be had it started on its end line, with a seek, once any crossfade
    // running has ended.
    if (slot.empty() || playing_.loop == &slot || fade_) {
        return;
    }

    const std::optional<std::int64_t> downbeat = end_downbeat(slot, grid);

    if (!downbeat) {
        return;
    }

    const std::int64_t start  = position_of(slot, 0);
    const stream       target = {.loop = &slot, .position = start + (now - *downbeat)};

    begin_crossfade(now, seek_frames_, target, window_correlation(playing_, target));
    play_from(slot, now, *downbeat, 0, slot.lines().back().sequence);
}

void loop_player::enter(std::int64_t now, capture& captured, const clock_tracker& grid) noexcept
{
    const captured_loop&              waiting  = captured.waiting();
    const std::optional<std::int64_t> downbeat = end_downbeat(waiting, grid);

    if (!downbeat || *downbeat - now < least_entry_) {
        return;
    }

    // From the playing stream, or silence, into the capture's pre-roll, landing its first downbeat
    // on its end line.
    const std::int64_t lead     = *downbeat - now;
    const std::int64_t length   = std::min(lead, entry_frames_);
    const stream       incoming = {.loop = &waiting, .position = position_of(waiting, 0) - lead};

    begin_crossfade(now, length, incoming, entry_correlation(playing_, incoming, length));

    next_ = downbeat_plan{
        .at            = *downbeat,
        .bar           = 0,
        .line_sequence = waiting.lines().back().sequence,
    };
    entering_       = true;
    entry_downbeat_ = *downbeat;
    captured.enter_waiting();
}

void loop_player::return_to_slot(std::int64_t now, const capture& captured) noexcept
{
    const captured_loop& slot = captured.slot();

    // A seek back to the loop in the slot, where it would be had it played on into the entry's
    // line, or to silence.
    if (slot.empty()) {
        begin_crossfade(now, seek_frames_, stream{}, window_correlation(playing_, stream{}));
        next_.reset();
        return;
    }

    // Its bar due at the entry's line, which is the refused capture's end line. Not the grid's bar
    // at the line's predicted time: the edge that refused it was late, and the grid's bar there is
    // still the one before.
    const captured_loop& refused  = captured.leaving();
    const std::int64_t   line_bar = refused.first_bar() + static_cast<std::int64_t>(refused.bars());

    const auto due_bar = static_cast<std::size_t>(
        floor_modulo(line_bar - slot.first_bar(), static_cast<std::int64_t>(slot.bars())));

    const stream target = {
        .loop     = &slot,
        .position = position_of(slot, due_bar) + (now - entry_downbeat_),
    };

    begin_crossfade(now, seek_frames_, target, window_correlation(playing_, target));
    play_from(slot, now, entry_downbeat_, due_bar, refused.lines().back().sequence);
}

void loop_player::decide_line(sample_time when, const clock_tracker& grid) noexcept
{
    const std::int64_t             now  = now_;
    const std::optional<line_plan> plan = plan_line(now, grid);

    // Decided at its moment, from the grid's prediction then.
    if (!plan || when < at_sample(plan->decide_at)) {
        return;
    }

    if (plan->plays_on) {
        next_ = downbeat_plan{
            .at            = plan->natural,
            .bar           = plan->bar,
            .line_sequence = plan->line_sequence,
        };
        return;
    }

    // Too near for an entry: the downbeat is joined with a seek at the line.
    const std::int64_t left = plan->entry_end - now;

    if (left < least_entry_) {
        next_ = downbeat_plan{
            .at            = plan->due,
            .bar           = plan->bar,
            .line_sequence = plan->line_sequence,
            .seek          = true,
        };
        return;
    }

    const captured_loop& loop     = *playing_.loop;
    const stream         incoming = {
        .loop     = &loop,
        .position = position_of(loop, plan->bar) - (plan->due - now),
    };

    begin_crossfade(now, left, incoming, entry_correlation(playing_, incoming, left));

    next_ = downbeat_plan{.at = plan->due, .bar = plan->bar, .line_sequence = plan->line_sequence};
}

void loop_player::play_from(const captured_loop& loop,
                            std::int64_t         now,
                            std::int64_t         downbeat,
                            std::size_t          due_bar,
                            std::int64_t         line_sequence) noexcept
{
    // The bar the stream is in, counted on from the bar due at `downbeat`, the line whose pulse is
    // `line_sequence`; or, before that downbeat, the downbeat to come. A bar it has played into
    // early, the loop's bar being shorter than the grid's, is due again at the next line.
    const std::size_t  bars     = loop.bars();
    const std::int64_t position = playing_.position;

    entering_ = false;
    next_.reset();

    if (now < downbeat) {
        next_ =
            downbeat_plan{.at = downbeat, .bar = due_bar % bars, .line_sequence = line_sequence};
        return;
    }

    line_sequence_ = line_sequence;

    std::size_t  loop_bar = due_bar % bars;
    std::int64_t played   = downbeat;

    // Bounded: within one pass of the loop, to its last bar.
    while (loop_bar + 1 < bars && position >= position_of(loop, loop_bar + 1)) {
        played += position_of(loop, loop_bar + 1) - position_of(loop, loop_bar);
        ++loop_bar;
    }

    bar_ = loop_bar;

    // A stream that starts on a downbeat plays it.
    if (played == now) {
        record_downbeat(now, loop_bar);
    }
}

void loop_player::begin_crossfade(std::int64_t now,
                                  std::int64_t length,
                                  stream       incoming,
                                  float        correlation) noexcept
{
    fade_ = crossfade{
        .outgoing    = playing_,
        .start       = now,
        .length      = length,
        .correlation = correlation,
    };

    playing_ = incoming;
}

void loop_player::reach_downbeat(std::int64_t now, const downbeat_plan& plan) noexcept
{
    // Joined with a seek at the line: the downbeat fades in (DESIGN.md §5.6's stated exception).
    if (plan.seek && playing_.loop != nullptr) {
        const stream target = {
            .loop     = playing_.loop,
            .position = position_of(*playing_.loop, plan.bar),
        };

        begin_crossfade(now, seek_frames_, target, window_correlation(playing_, target));
    }

    bar_           = plan.bar;
    line_sequence_ = plan.line_sequence;
    record_downbeat(now, plan.bar);
}

void loop_player::record_downbeat(std::int64_t at, std::size_t loop_bar) noexcept
{
    if (downbeat_count_ < downbeats_.size()) {
        downbeats_[downbeat_count_] =
            loop_downbeat{.offset = samples{at - block_start_}, .bar = loop_bar};
        ++downbeat_count_;
    }
}

loop_player::held_run loop_player::run_from(const stream& from, std::int64_t most) const noexcept
{
    const std::int64_t page_frames = pool_->page_frames().value();
    const std::int64_t page        = floor_divide(from.position, page_frames);
    const std::int64_t offset      = from.position - (page * page_frames);
    const std::int64_t count       = std::min(most, page_frames - offset);
    const std::int64_t held        = page - from.loop->first_page();

    // Only the loop's own pages are read. A frame outside them, which a correlation window can
    // reach on a very fast clock, reads as silence.
    if (held < 0 || std::cmp_greater_equal(held, from.loop->pages().size())) {
        return held_run{.frames = {}, .count = count};
    }

    const std::span<const stereo_frame> frames =
        pool_->frames(from.loop->pages()[static_cast<std::size_t>(held)]);

    return held_run{
        .frames = frames.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(count)),
        .count  = count,
    };
}

stereo_frame loop_player::frame_at(const stream& from) const noexcept
{
    if (from.loop == nullptr) {
        return {};
    }

    const held_run run = run_from(from, 1);

    return run.frames.empty() ? stereo_frame{} : run.frames.front();
}

stereo_frame loop_player::read(stream& from) const noexcept
{
    const stereo_frame frame = frame_at(from);

    ++from.position;

    return frame;
}

float loop_player::entry_correlation(stream       outgoing,
                                     stream       incoming,
                                     std::int64_t length) const noexcept
{
    // An entry's two streams are measured over the 50 ms each would play up to the downbeat, as
    // the crossfade ends (DESIGN.md §5.2). For a move into the next bar that is the same bar's
    // end twice, a move's length apart: not the same audio.
    const auto ending = [&](stream from) {
        return stream{.loop = from.loop, .position = from.position + length - window_frames_};
    };

    return window_correlation(ending(outgoing), ending(incoming));
}

float loop_player::window_correlation(stream a, stream b) const noexcept
{
    // Over both channels together: on one channel alone an anti-phase pair would be 0/0. Each sum
    // of squares goes through its square root first, so no product of two sums is formed
    // (ADR 0005).
    float products = 0.0F;
    float energy_a = 0.0F;
    float energy_b = 0.0F;

    for (std::int64_t i = 0; i < window_frames_; ++i) {
        const stereo_frame x = read(a);
        const stereo_frame y = read(b);

        products += (x.left * y.left) + (x.right * y.right);
        energy_a += (x.left * x.left) + (x.right * x.right);
        energy_b += (y.left * y.left) + (y.right * y.right);
    }

    // Silence on either side counts as correlated: equal gain.
    if (energy_a == 0.0F || energy_b == 0.0F) {
        return 1.0F;
    }

    return std::clamp(products / (std::sqrt(energy_a) * std::sqrt(energy_b)), 0.0F, 1.0F);
}

bool loop_player::fading(std::int64_t at) const noexcept
{
    return fade_ && at >= fade_->start && at < fade_->start + fade_->length;
}

}  // namespace previously
