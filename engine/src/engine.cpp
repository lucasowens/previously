// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — the engine: commands, the clock grid, capture, the display's model, and (for now)
// a sample-exact passthrough.

#include "previously/engine.hpp"

#include "previously/contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace previously {
namespace {

/// The first whole sample at or after `when`: the first one a decision at `when` reaches.
[[nodiscard]] std::int64_t sample_at_or_after(sample_time when) noexcept
{
    const std::int64_t whole = when.whole_samples().value();

    return when > sample_time::from_samples(samples{whole}) ? whole + 1 : whole;
}

// The settings and the tape are checked before anything is built from them, so a bad one is
// reported here rather than by whichever member meets it first.

[[nodiscard]] engine_config checked_config(const engine_config& config) noexcept
{
    expects(config.rate.hz() > 0U);
    expects(config.max_block_frames > 0U);
    expects(config.max_block_frames <= engine::max_block_frames(config.rate));
    expects(config.input_latency >= sample_time{});
    expects(config.output_latency >= sample_time{});

    return config;
}

[[nodiscard]] std::span<stereo_frame> checked_tape(std::span<stereo_frame> tape,
                                                   sample_rate             rate) noexcept
{
    expects(tape.empty() || engine::holds_history(tape.size(), rate));

    return tape;
}

}  // namespace

engine::engine(const engine_config& config, std::span<stereo_frame> tape) noexcept
    : config_{checked_config(config)},
      clock_tracker_{clock_tracker_config::gate(config.rate), clock_history_},
      pool_{checked_tape(tape, config.rate), page_frames_for(config.rate)},
      recorder_{pool_, config.rate, history_pages},
      capture_{pool_, recorder_, config.rate, config.input_latency},
      player_{pool_, config.rate, config.output_latency},
      ui_{.rate = config.rate}
{
}

bool engine::holds_history(std::size_t tape_frames, sample_rate rate) noexcept
{
    // In 64 bits: a page reaches 2^32 frames above 3,145,728,000 Hz (2^16 × 48 kHz), which
    // sample_rate can hold and the M7's 32-bit std::size_t cannot.
    const auto page_frames = static_cast<std::uint64_t>(page_frames_for(rate).value());
    const auto whole_pages = static_cast<std::uint64_t>(tape_frames) / page_frames;

    return whole_pages >= history_pages;
}

std::size_t engine::max_block_frames(sample_rate rate) noexcept
{
    // Real pulses are at least the double-trigger spacing apart, but during a tempo transition
    // reports can be half a period apart. Budget for that worst case.
    const samples spacing        = to_samples(clock_tracker::double_trigger_spacing, rate);
    const auto    report_spacing = std::max(spacing.value() / 2, std::int64_t{1});

    return static_cast<std::size_t>(report_spacing) * clock_report::max_pulses;
}

void engine::process(const audio_block& block, std::span<const clock_event> events) noexcept
{
    expects(block.has_matching_lengths());
    expects(block.frame_count() <= config_.max_block_frames);

    const auto        frames      = samples{static_cast<std::int64_t>(block.frame_count())};
    const sample_time block_start = sample_time::from_samples(sample_clock_);
    const sample_time block_end   = block_start + sample_time::from_samples(frames);

    // A block of no frames holds no moment: nothing happens, and commands wait for the next block.
    if (frames == samples{0}) {
        return;
    }

    player_.begin_block(block_start.whole_samples().value());

    // The moments, and the output between them, the live input plus the loop, recorded as it
    // goes.
    visit_moments(block_start, block_end, block, events);

    // The loop report: capture's, and the player's.
    loop_report_                = capture_.report();
    loop_report_.playing        = player_.playing();
    loop_report_.bar            = player_.bar();
    loop_report_.downbeat_count = player_.downbeats().size();
    std::ranges::copy(player_.downbeats(), loop_report_.downbeats.begin());

    sample_clock_ += frames;

    report_clock(block_start, block_end);

    // The display's model, once a bucket has filled, from everything the block did. Sample clock
    // values are never negative, so plain division finds each one's bucket.
    const std::int64_t bucket = recorder_.bucket_frames().value();

    if (block_start.whole_samples().value() / bucket != sample_clock_.value() / bucket) {
        update_ui_model(ui_, clock_tracker_, recorder_, config_.rate);
    }
}

bool engine::post(const command& request) noexcept
{
    return commands_.try_push(request);
}

samples engine::sample_clock() const noexcept
{
    return sample_clock_;
}

const clock_report& engine::clock() const noexcept
{
    return clock_report_;
}

const recorder& engine::history() const noexcept
{
    return recorder_;
}

const ui_model& engine::ui() const noexcept
{
    return ui_;
}

const loop_report& engine::loop() const noexcept
{
    return loop_report_;
}

const captured_loop& engine::leaving() const noexcept
{
    return capture_.leaving();
}

const captured_loop& engine::slot() const noexcept
{
    return capture_.slot();
}

const captured_loop& engine::waiting() const noexcept
{
    return capture_.waiting();
}

const page_pool& engine::pages() const noexcept
{
    return pool_;
}

const engine_config& engine::config() const noexcept
{
    return config_;
}

std::size_t
engine::apply_pending_commands(sample_time                                     block_start,
                               std::span<capture_bars, command_queue_capacity> presses) noexcept
{
    std::size_t pressed = 0;

    // Bounded: at most one queue's worth per block, so a flood of commands can never stall audio.
    for (std::size_t i = 0; i < command_queue_capacity; ++i) {
        const auto request = commands_.try_pop();

        if (!request.has_value()) {
            break;
        }

        switch (request->type) {
            case command_type::reset_grid:
                clock_tracker_.reset(block_start);
                break;

            case command_type::capture:
                presses[pressed] = request->bars;
                ++pressed;
                break;
        }
    }

    return pressed;
}

void engine::visit_moments(sample_time                  block_start,
                           sample_time                  block_end,
                           const audio_block&           block,
                           std::span<const clock_event> events) noexcept
{
    const std::int64_t first_sample = block_start.whole_samples().value();
    const std::int64_t end_sample   = block_end.whole_samples().value();

    // Commands apply at the block's start: a reset there as an edge, and a press after that
    // moment's edges, fill-in and check, as an edge's press would come.
    std::array<capture_bars, command_queue_capacity> commanded{};
    const std::size_t command_presses = apply_pending_commands(block_start, commanded);

    // The block's moments in time order: its start, each event's, each fill-in's commit, and each
    // of the player's. At each: the clock edges at that moment, in the order given; then any
    // fill-in due then; then capture's check; then the presses at that moment, wherever they were
    // among its edges; then the player. A fill-in due at the block's start was left by the block
    // before, so it commits after the edges at this block's first frame, as it would mid-block.
    //
    // Bounded: fill-ins commit at least a double-trigger spacing apart and max_block_frames()
    // keeps a block to 32 spacings, so a block holds at most 33 commits; and the player has a
    // handful of moments a bar, of bars at least 16 ms long.
    const std::size_t most_moments = events.size() + (2 * clock_report::max_pulses) + 64;

    std::size_t rendered = 0;
    std::size_t next     = 0;

    for (std::size_t visited = 0; visited < most_moments; ++visited) {
        sample_time when = block_start;

        if (visited > 0) {
            const std::optional<sample_time> soonest =
                soonest_moment(block_start, events.subspan(next));

            if (!soonest || *soonest >= block_end) {
                break;
            }

            when = *soonest;
        }

        rendered = render_segment(block, first_sample, rendered, sample_at_or_after(when));

        const std::size_t moment_end = apply_edges(when, block_start, block_end, events, next);

        clock_tracker_.advance(when);
        capture_.check(when, clock_tracker_);

        if (visited == 0) {
            for (const capture_bars bars : std::span{commanded}.first(command_presses)) {
                press_or_hold(when, bars);
            }
        }

        for (; next < moment_end; ++next) {
            if (events[next].type == clock_event_type::capture) {
                press_or_hold(when, events[next].bars);
            }
        }

        update_player(when);
    }

    render_segment(block, first_sample, rendered, end_sample);

    // A fill-in due at the block's end is the next block's: the tracker goes only to a tick
    // before it.
    clock_tracker_.advance(block_end - sample_time::from_ticks(1));
}

std::optional<sample_time>
engine::soonest_moment(sample_time block_start, std::span<const clock_event> events) const noexcept
{
    std::optional<sample_time> soonest;

    const auto consider = [&soonest](sample_time at) {
        if (!soonest || at < *soonest) {
            soonest = at;
        }
    };

    if (!events.empty()) {
        consider(block_start + events.front().offset);
    }

    if (const std::optional<sample_time> due = clock_tracker_.next_fill_in()) {
        consider(*due);
    }

    if (const std::optional<std::int64_t> moment = player_.next_moment(capture_, clock_tracker_)) {
        consider(sample_time::from_samples(samples{*moment}));
    }

    return soonest;
}

std::size_t engine::render_segment(const audio_block& block,
                                   std::int64_t       first_sample,
                                   std::size_t        rendered,
                                   std::int64_t       until) noexcept
{
    // The audio before a moment is rendered with the state the moment before it left, and is
    // recorded at once, its pages held as the recorder takes them: so at every moment the tape
    // holds everything before it, at any block size. A bar can be as short as a block, so the
    // player may read audio that recent.
    const auto to = static_cast<std::size_t>(until - first_sample);

    if (to <= rendered) {
        return rendered;
    }

    const std::size_t count = to - rendered;

    player_.render(first_sample + static_cast<std::int64_t>(rendered),
                   block.in_left.subspan(rendered, count),
                   block.in_right.subspan(rendered, count),
                   block.out_left.subspan(rendered, count),
                   block.out_right.subspan(rendered, count));
    recorder_.record(block.in_left.subspan(rendered, count),
                     block.in_right.subspan(rendered, count));
    capture_.follow();

    return to;
}

std::size_t engine::apply_edges(sample_time                  when,
                                sample_time                  block_start,
                                sample_time                  block_end,
                                std::span<const clock_event> events,
                                std::size_t                  first) noexcept
{
    std::size_t end = first;

    for (; end < events.size() && block_start + events[end].offset == when; ++end) {
        const clock_event& event = events[end];

        expects(event.offset >= sample_time{} && block_start + event.offset < block_end);
        expects(end == 0 || event.offset >= events[end - 1].offset);

        switch (event.type) {
            case clock_event_type::pulse:
                clock_tracker_.pulse(when);
                break;

            case clock_event_type::reset:
                clock_tracker_.reset(when);
                break;

            case clock_event_type::capture:
                break;
        }
    }

    return end;
}

void engine::update_player(sample_time when) noexcept
{
    player_.update(when, capture_, clock_tracker_);

    // A press held for a crossfade applies as it ends, with its own timestamp (DESIGN.md §5.7).
    if (held_press_ && !player_.holds_presses(sample_at_or_after(when))) {
        capture_.press(held_press_->when, when, held_press_->bars, clock_tracker_);
        held_press_.reset();
        player_.update(when, capture_, clock_tracker_);
    }
}

void engine::press_or_hold(sample_time when, capture_bars bars) noexcept
{
    // During a crossfade, or while a capture's entry has begun and it has not taken the slot, a
    // press waits. Of several, the latest waits: the others were a double press.
    if (player_.holds_presses(sample_at_or_after(when))) {
        held_press_ = held_press{.when = when, .bars = bars};
        return;
    }

    capture_.press(when, when, bars, clock_tracker_);
}

void engine::report_clock(sample_time block_start, sample_time block_end) noexcept
{
    clock_report_.state       = clock_tracker_.state();
    clock_report_.beat_period = clock_tracker_.beat_period();
    clock_report_.bar = clock_tracker_.bar_at_or_before(block_end - sample_time::from_ticks(1));
    clock_report_.pulse_count   = 0;
    clock_report_.bar_restarted = false;

    // The rule: a grid pulse is reported once it lies at least half a period after the last
    // report, using the period in force AT that report. Hysteresis by time, not index, because
    // indices are relabelled by resets; and the period at the last report, because when the clock
    // slows, the new longer period would hide the first real slow pulse inside the window.
    const sample_time window = last_reported_pulse_ ? last_reported_period_ / 2 : sample_time{};
    const sample_time reportable_from =
        last_reported_pulse_ ? *last_reported_pulse_ + window : block_start;

    // A reset that made bar 0 start at a pulse which will not be reported as index 0, because
    // it was reported already under its old number or falls inside the window: say so once.
    if (const std::uint32_t resets = clock_tracker_.resets_applied(); resets != resets_seen_) {
        resets_seen_ = resets;

        const auto bar_zero      = clock_tracker_.pulse_time(0);
        const bool shown_as_zero = bar_zero && last_reported_bar_zero_ &&
                                   abs(*bar_zero - *last_reported_bar_zero_) < window;

        clock_report_.bar_restarted = bar_zero && *bar_zero < reportable_from && !shown_as_zero;
    }

    const auto period = clock_tracker_.pulse_period();

    if (!period) {
        return;
    }

    // One extra: the first point found can be inside the window of the previous block's last
    // report and be skipped, and that must not cost a reportable pulse its place.
    std::array<grid_point, clock_report::max_pulses + 1> found{};
    const std::size_t                                    count =
        clock_tracker_.pulses_between(reportable_from - sample_time::from_ticks(1),
                                      block_end,
                                      found);

    const std::int64_t pulses_per_beat = clock_tracker_.pulses_per_beat();
    const std::int64_t pulses_per_bar  = clock_tracker_.pulses_per_bar();

    for (std::size_t i = 0; i < count && clock_report_.pulse_count < clock_report::max_pulses;
         ++i) {
        const grid_point& point = found[i];

        // Pulses are at least half a period apart, so only the first can be inside the window
        // of the last report from an earlier block; later ones are checked against this block's.
        if (last_reported_pulse_ &&
            point.time < *last_reported_pulse_ + (last_reported_period_ / 2)) {
            continue;
        }

        clock_report_.pulses[clock_report_.pulse_count] = grid_pulse{
            // Never negative in practice: a pulse not yet reported lies at or after this block's
            // start (its prediction did), and a real edge correcting it is inside this block.
            // Clamped anyway, because an adapter turns the offset into an unsigned position.
            .offset      = std::max(point.time - block_start, sample_time{}),
            .index       = point.index,
            .starts_beat = floor_modulo(point.index, pulses_per_beat) == 0,
            .starts_bar  = floor_modulo(point.index, pulses_per_bar) == 0,
        };
        ++clock_report_.pulse_count;

        last_reported_pulse_  = point.time;
        last_reported_period_ = *period;

        if (point.index == 0) {
            last_reported_bar_zero_ = point.time;
        }
    }
}

}  // namespace previously
