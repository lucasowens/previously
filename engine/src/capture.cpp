// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: capture.

#include "previously/capture.hpp"

#include "previously/bar_line.hpp"
#include "previously/contract.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace previously {
namespace {

/// The bars of a count, as an index into a loop's lines: at most max_bars.
[[nodiscard]] std::size_t bars_of(capture_bars bars) noexcept
{
    static_assert(captured_loop::max_bars == 8, "bar_count() counts at most 8 bars");

    return static_cast<std::size_t>(bar_count(bars));
}

}  // namespace

capture::capture(page_pool&      pool,
                 const recorder& history,
                 sample_rate     rate,
                 sample_time     input_latency) noexcept
    : pool_{&pool},
      history_{&history},
      margin_{loop_margin(rate)},
      input_latency_{input_latency},
      loops_{captured_loop{pool}, captured_loop{pool}, captured_loop{pool}}
{
    expects(input_latency >= sample_time{});
}

void capture::press(sample_time          when,
                    sample_time          now,
                    capture_bars         bars,
                    const clock_tracker& grid) noexcept
{
    expects(is_bar_count(bars));
    expects(when <= now);

    // Lines at least two pulses apart cannot cross, each moving by at most half a pulse.
    expects(grid.pulses_per_bar() >= 2);

    // Everything the grid knows by now comes first: a waiting capture's line may have arrived.
    check(now, grid);

    // A capture whose entry has begun has started, and presses wait for it (DESIGN.md §5.7).
    expects(!waiting_entered_);

    // A capture still waiting for its line is superseded, whatever becomes of this press.
    waiting_loop().clear();

    waiting_pressed_ = when;
    waiting_asked_   = bars;

    // The history at the press, as far as it is still there when a press has waited.
    const samples history_begin = std::max(history_begin_at(when), history_begin_at(now));

    conclude(plan(when, bars, history_begin, grid));
    report_states();
}

void capture::check(sample_time now, const clock_tracker& grid) noexcept
{
    // First the slot's lines whose pulses a retirement took: their sequences name other pulses.
    keep_retired(slot_loop(), grid);

    if (!waiting_loop().empty() && labels_of(grid) != waiting_labels_) {
        plan_again(now, grid);
    }

    if (!waiting_loop().empty()) {
        settle(waiting_loop(), grid);

        if (grid.edge(grid.index_of(waiting_loop().lines().back().sequence))) {
            arrive(grid);
        }
    }

    // After the arrival, which may have put a new loop in the slot: its end line can be final
    // already.
    if (!slot_loop().empty() && !slot_loop().is_final()) {
        settle(slot_loop(), grid);
    }

    report_states();
}

void capture::follow() noexcept
{
    // A final loop still holds pages to come: on a fast clock its 8 pulses end before its read
    // margin does.
    for (captured_loop& loop : loops_) {
        if (!loop.empty()) {
            loop.catch_up(*history_);
        }
    }

    report_states();
}

const loop_report& capture::report() const noexcept
{
    return report_;
}

const captured_loop& capture::slot() const noexcept
{
    return loops_[slot_index_];
}

const captured_loop& capture::waiting() const noexcept
{
    return loops_[waiting_index_];
}

const captured_loop& capture::leaving() const noexcept
{
    return loops_[leaving_index_];
}

void capture::enter_waiting() noexcept
{
    expects(!waiting_loop().empty());

    waiting_entered_ = true;
}

bool capture::waiting_entered() const noexcept
{
    return waiting_entered_;
}

void capture::release_leaving() noexcept
{
    leaving_loop().clear();
    report_states();
}

capture::labelling capture::labels_of(const clock_tracker& grid) noexcept
{
    return labelling{.bar_zero = grid.sequence_of(0), .retirements = grid.retirements()};
}

std::optional<capture::line_fix> capture::locate(const clock_tracker& grid,
                                                 std::int64_t         sequence) noexcept
{
    const std::int64_t index = grid.index_of(sequence);

    if (const auto estimate = estimate_line(grid, index)) {
        return line_fix{.sequence = sequence, .time = estimate->time, .status = line_status::final};
    }

    if (const auto edge = grid.edge(index)) {
        return line_fix{.sequence = sequence, .time = edge->time, .status = line_status::edge};
    }

    // Not recorded: still to come, or older than the clock's history, which has no time for it.
    if (const auto predicted = grid.pulse_time(index)) {
        return line_fix{.sequence = sequence, .time = *predicted, .status = line_status::predicted};
    }

    return std::nullopt;
}

std::optional<capture::end_line> capture::end_line_of(sample_time          when,
                                                      const clock_tracker& grid) noexcept
{
    const std::optional<sample_time>  period = grid.pulse_period();
    const std::optional<std::int64_t> bar    = grid.bar_at_or_before(when);

    if (!period || !bar) {
        return std::nullopt;
    }

    // The next line if it is at most a quarter beat away, compared four times over so that no
    // quarter beat is rounded. Otherwise the most recent line.
    const std::optional<sample_time> next_line = grid.bar_start(*bar + 1);
    const sample_time                beat      = *period * grid.pulses_per_beat();
    const bool                       early     = next_line && ((*next_line - when) * 4) <= beat;
    const std::int64_t               end_bar   = early ? *bar + 1 : *bar;

    return end_line{
        .bar      = end_bar,
        .sequence = grid.sequence_of(end_bar * grid.pulses_per_bar()),
    };
}

capture_outcome capture::plan(sample_time          when,
                              capture_bars         bars,
                              samples              history_begin,
                              const clock_tracker& grid) noexcept
{
    const std::optional<sample_time> period = grid.pulse_period();
    const std::optional<end_line>    end    = end_line_of(when, grid);

    waiting_entered_ = false;

    if (!period || !end) {
        waiting_loop().clear();
        return {.result = capture_result::no_tempo, .asked = bars};
    }

    // Every line the press could reach, counted back from the end line: a shorter capture takes
    // the newest of them.
    const std::int64_t end_bar      = end->bar;
    const std::int64_t end_sequence = end->sequence;
    const std::size_t  most         = bars_of(bars);
    line_fixes         fixes{};

    for (std::size_t back = 0; back <= most; ++back) {
        fixes[back] =
            locate(grid, end_sequence - (static_cast<std::int64_t>(back) * grid.pulses_per_bar()));
    }

    const sample_time reserve = *period / 2;
    const fitting     fitted  = fit(fixes, bars, history_begin, reserve);

    if (!fitted.plan) {
        waiting_loop().clear();
        return {.result = capture_result::nothing_fits, .asked = bars, .limit = fitted.limit};
    }

    const window_plan& planned = *fitted.plan;

    if (!fits_the_pool(planned.held)) {
        waiting_loop().clear();
        return {.result = capture_result::pool_full, .asked = bars};
    }

    take(waiting_loop(), planned, end_bar, grid.numbered_by_reset());

    waiting_kept_          = planned.bars;
    waiting_history_begin_ = history_begin;
    waiting_end_bar_       = end_bar;
    waiting_reserve_       = reserve;
    waiting_labels_        = labels_of(grid);

    // A capture whose end line is recorded starts now. One whose line is still to come waits.
    if (planned.lines[bars_of(planned.bars)].status != line_status::predicted) {
        take_slot();
    }

    return {
        .result = planned.bars == bars ? capture_result::committed : capture_result::shortened,
        .asked  = bars,
        .kept   = planned.bars,
        .limit  = fitted.limit,
    };
}

capture::fitting capture::fit(const line_fixes& fixes,
                              capture_bars      most,
                              samples           history_begin,
                              sample_time       reserve) const noexcept
{
    // Bounded: at most four counts.
    capture_limit kept_out = capture_limit::none;

    for (std::optional<capture_bars> bars = most; bars; bars = fewer(*bars)) {
        const std::optional<window_plan> planned = plan_window(fixes, *bars, reserve);
        const capture_limit              limit =
            planned ? limit_of(*planned, history_begin) : capture_limit::history;

        if (limit == capture_limit::none) {
            return fitting{.plan = planned, .limit = kept_out};
        }

        kept_out = limit;
    }

    return fitting{.limit = kept_out};
}

std::optional<capture::window_plan>
capture::plan_window(const line_fixes& fixes, capture_bars bars, sample_time reserve) const noexcept
{
    const std::size_t count = bars_of(bars);
    window_plan       planned{.bars = bars};

    for (std::size_t k = 0; k <= count; ++k) {
        const std::optional<line_fix>& fix = fixes[count - k];

        // Every line before the end line is behind the press, so it must be recorded. One that is
        // not is older than the clock's history, or before its first pulse.
        if (!fix || (k < count && fix->status == line_status::predicted)) {
            return std::nullopt;
        }

        planned.lines[k] = loop_line{
            .sequence = fix->sequence,
            .time     = fix->time,
            .position = position_of(fix->time),
            .status   = fix->status,
        };
    }

    // A line not yet final may still move by up to half a pulse (bar_line.hpp): the reserve.
    const loop_line&  first_line = planned.lines[0];
    const loop_line&  last_line  = planned.lines[count];
    const sample_time before = first_line.status == line_status::final ? sample_time{} : reserve;
    const sample_time after  = last_line.status == line_status::final ? sample_time{} : reserve;

    planned.held = tape_range{
        .first = position_of(first_line.time - before) - margin_,
        .end   = position_of(last_line.time + after) + margin_,
    };

    return planned;
}

capture_limit capture::limit_of(const window_plan& window, samples history_begin) const noexcept
{
    // 46 pages of frames touch at most 47 pages, however they fall.
    const samples most_frames =
        pool_->page_frames() * static_cast<std::int64_t>(captured_loop::max_pages - 1);

    if (window.held.first < history_begin) {
        return capture_limit::history;
    }

    if (window.held.end - window.held.first > most_frames) {
        return capture_limit::max_pages;
    }

    return capture_limit::none;
}

samples capture::history_begin_at(sample_time when) const noexcept
{
    // The 45 pages before the press's page, and that page up to the press: what the history holds
    // at that moment, whatever the blocks. The recorder may hold a page more, until it records the
    // press's frame. Before the history has filled, and without pages, it holds less.
    const std::int64_t page_frames = pool_->page_frames().value();
    const std::int64_t page        = when.whole_samples().value() / page_frames;
    const auto         complete    = static_cast<std::int64_t>(history_->history_pages()) - 1;

    const samples whole_pages{std::max(page - complete, std::int64_t{0}) * page_frames};

    return std::max(whole_pages, history_->begin());
}

bool capture::fits_the_pool(tape_range range) const noexcept
{
    // Counted as though no page were shared with the history, so the recorder always finds one.
    const std::int64_t page_frames = pool_->page_frames().value();
    const auto         pages = static_cast<std::size_t>(((range.end.value() - 1) / page_frames) -
                                                        (range.first.value() / page_frames) + 1);

    return history_->history_pages() + slot().page_count() + leaving().page_count() + pages <=
           pool_->page_count();
}

samples capture::position_of(sample_time time) const noexcept
{
    return (time + input_latency_).nearest_sample();
}

void capture::plan_again(sample_time now, const clock_tracker& grid) noexcept
{
    // A capture whose entry has begun has started, and keeps its window (DESIGN.md §5.7).
    if (waiting_entered_) {
        captured_loop& entered = waiting_loop();

        // A retirement took the pulse its end line waited for, which will not come as that pulse.
        // Having started, it keeps its cut where it was planned, final, as a loop in the slot keeps
        // a retired line's edge; and it takes the slot.
        if (labels_of(grid).retirements != waiting_labels_.retirements) {
            const std::size_t last = entered.bars();
            loop_line         end  = entered.lines()[last];

            end.status = line_status::final;
            entered.set_line(last, end);
            settle(entered, grid);
            finish(entered);

            waiting_labels_ = labels_of(grid);
            take_slot();
            return;
        }

        // Its end line still the same pulse under the new labels, it takes their numbering.
        const std::optional<end_line> end = end_line_of(waiting_pressed_, grid);

        if (end && end->sequence == entered.lines().back().sequence) {
            entered.renumber(end->bar - static_cast<std::int64_t>(entered.bars()),
                             grid.numbered_by_reset());
            waiting_end_bar_ = end->bar;
        }

        waiting_labels_ = labels_of(grid);
        return;
    }

    // The frames it can still have: the history it was planned from, as far as the history now
    // or the pages it holds still have it. A waiting capture's window is behind its press, so its
    // run's first page is held.
    const samples held_from{waiting_loop().first_page() * pool_->page_frames().value()};
    const samples history_begin =
        std::max(waiting_history_begin_, std::min(history_begin_at(now), held_from));

    const capture_outcome outcome = plan(waiting_pressed_, waiting_asked_, history_begin, grid);

    if (outcome != report_.outcome) {
        conclude(outcome);
    }
}

void capture::arrive(const clock_tracker& grid) noexcept
{
    captured_loop&                   arriving = waiting_loop();
    const std::span<const loop_line> lines    = arriving.lines();
    const std::size_t                count    = lines.size() - 1;

    // The lines as held, and the end line from its edge, with the reserve from the press.
    line_fixes fixes{};

    for (std::size_t back = 1; back <= count; ++back) {
        const loop_line& line = lines[count - back];

        fixes[back] = line_fix{.sequence = line.sequence, .time = line.time, .status = line.status};
    }

    fixes[0] = locate(grid, lines.back().sequence);

    const fitting fitted = fit(fixes, waiting_kept_, waiting_history_begin_, waiting_reserve_);

    // Refused at its line. One whose entry has begun is heard already: it leaves the slot's way,
    // and so does one it would shorten, since its first bars are playing (DESIGN.md §5.7).
    const bool shortened_entered =
        waiting_entered_ && fitted.plan && fitted.plan->bars != waiting_kept_;

    if (!fitted.plan || shortened_entered) {
        if (waiting_entered_) {
            leave_waiting();
        }
        else {
            arriving.clear();
        }

        conclude({
            .result = capture_result::nothing_fits,
            .asked  = waiting_asked_,
            .limit  = fitted.limit,
        });
        return;
    }

    if (!fits_the_pool(fitted.plan->held)) {
        if (waiting_entered_) {
            leave_waiting();
        }
        else {
            arriving.clear();
        }

        conclude({.result = capture_result::pool_full, .asked = waiting_asked_});
        return;
    }

    if (fitted.plan->bars != waiting_kept_) {
        conclude({
            .result = capture_result::shortened,
            .asked  = waiting_asked_,
            .kept   = fitted.plan->bars,
            .limit  = fitted.limit,
        });
    }

    take(arriving, *fitted.plan, waiting_end_bar_, arriving.numbered_by_reset());
    take_slot();
}

void capture::keep_retired(captured_loop& loop, const clock_tracker& grid) noexcept
{
    if (loop.empty() || loop.is_final()) {
        return;
    }

    // A line whose pulse was retired cannot be estimated any more: its sequence names another
    // pulse, or none yet. It keeps its edge, final. A line still on its own pulse is unchanged.
    const std::size_t last = loop.bars();

    for (std::size_t k = 0; k <= last; ++k) {
        const loop_line line = loop.lines()[k];

        if (line.status != line_status::edge) {
            continue;
        }

        const std::optional<pulse_edge> edge = grid.edge(grid.index_of(line.sequence));

        if (edge && edge->time == line.time) {
            continue;
        }

        loop.set_line(k,
                      loop_line{
                          .sequence = line.sequence,
                          .time     = line.time,
                          .position = line.position,
                          .status   = line_status::final,
                      });
    }

    finish(loop);
}

void capture::settle(captured_loop& loop, const clock_tracker& grid) noexcept
{
    const std::size_t last = loop.bars();
    const tape_range  held = loop.held();

    for (std::size_t k = 0; k <= last; ++k) {
        const loop_line line = loop.lines()[k];

        if (line.status != line_status::edge) {
            continue;
        }

        const std::optional<line_estimate> estimate =
            estimate_line(grid, grid.index_of(line.sequence));

        if (!estimate) {
            continue;
        }

        // The estimate, unless it would take the window's frames past what the loop holds. That
        // needs a tempo that slowed after the press, so that half a pulse grew: the edge is kept.
        const samples position = position_of(estimate->time);
        const bool    inside   = (k != 0 || position - margin_ >= held.first) &&
                                 (k != last || position + margin_ <= held.end);

        loop.set_line(k,
                      loop_line{
                          .sequence = line.sequence,
                          .time     = inside ? estimate->time : line.time,
                          .position = inside ? position : line.position,
                          .status   = line_status::final,
                      });
    }

    finish(loop);
}

void capture::finish(captured_loop& loop) noexcept
{
    // Final: only the window and its margins are held now, and the reserve goes back.
    if (loop.is_final()) {
        loop.hold(
            tape_range{
                .first = loop.lines().front().position - margin_,
                .end   = loop.lines().back().position + margin_,
            },
            *history_);
    }
}

void capture::take(captured_loop&     loop,
                   const window_plan& window,
                   std::int64_t       end_bar,
                   bool               numbered_by_reset) noexcept
{
    const std::size_t count = bars_of(window.bars);

    loop.assign(std::span{window.lines}.first(count + 1),
                end_bar - static_cast<std::int64_t>(count),
                numbered_by_reset);
    loop.hold(window.held, *history_);
}

void capture::take_slot() noexcept
{
    // The waiting capture takes the slot, and the loop there leaves it: it keeps its pages until
    // the player has faded it out (release_leaving()). The player holds presses while it fades,
    // so no loop is leaving already.
    expects(leaving_loop().empty());
    leaving_loop().clear();

    const std::size_t left = leaving_index_;

    leaving_index_   = slot_index_;
    slot_index_      = waiting_index_;
    waiting_index_   = left;
    waiting_entered_ = false;
}

void capture::leave_waiting() noexcept
{
    // A capture refused after its entry began leaves as a loop leaving the slot does.
    expects(leaving_loop().empty());
    leaving_loop().clear();

    std::swap(waiting_index_, leaving_index_);
    waiting_entered_ = false;
}

void capture::conclude(const capture_outcome& outcome) noexcept
{
    report_.outcome = outcome;
    ++report_.outcome_serial;
}

void capture::report_states() noexcept
{
    const captured_loop& held = slot_loop();

    if (held.empty()) {
        report_.slot = slot_state::empty;
    }
    else {
        report_.slot = held.is_final() ? slot_state::final : slot_state::provisional;
    }

    report_.waiting = !waiting_loop().empty();
}

captured_loop& capture::slot_loop() noexcept
{
    return loops_[slot_index_];
}

captured_loop& capture::waiting_loop() noexcept
{
    return loops_[waiting_index_];
}

captured_loop& capture::leaving_loop() noexcept
{
    return loops_[leaving_index_];
}

}  // namespace previously
