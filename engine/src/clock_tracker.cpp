// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: clock tracker. The header explains the approach and the rules.

#include "previously/clock_tracker.hpp"

#include "previously/contract.hpp"

#include <algorithm>

namespace previously {

namespace {

// Windows in real time, converted to samples using the configured rate.

/// Contact bounce and double triggers settle within a few milliseconds. 4 ms is still shorter
/// than any musical pulse interval: 24 PPQN at 300 BPM is 8.3 ms.
constexpr seconds double_trigger_window = clock_tracker::double_trigger_spacing;

/// Slower than any musical pulse (1 PPQN at 15 BPM). A longer first interval means the first
/// edge was stray.
constexpr seconds max_acquisition_interval{4.0F};

/// How close a reset must be to a pulse to count as the same moment.
constexpr seconds reset_coincidence_window{0.002F};

// Rules measured in pulse periods.

/// A prediction error beyond a quarter period is a tempo change, not jitter.
constexpr std::int64_t tempo_change_divisor = 4;

/// A freewheel slot stays open for half a period after its time, so a late real pulse can claim
/// it. The same half period divides "late pulse" from "early next pulse".
constexpr std::int64_t slot_window_divisor = 2;

/// Two gaps whose lengths differ by at most a quarter of the first are "the same length".
constexpr std::int64_t same_interval_divisor = 4;

/// Silence for this long means the clock stopped. Above 2 periods with a margin wider than the
/// quarter-period tolerance, so a tempo exactly halving can never be mistaken for a stop.
[[nodiscard]] constexpr sample_time stopped_after(sample_time period) noexcept
{
    return (period * 2) + (period / 2);
}

/// Indices further than this from the present are refused rather than risk overflow. 2^40
/// pulses is centuries of clock at any tempo.
constexpr std::int64_t max_index_distance = 1'099'511'627'776;

/// The smallest allowed phase gain: below this the tracker would barely move.
constexpr float min_phase_gain = 0.01F;

/// Multiplies a time by a gain, rounding to the nearest tick. Gains and errors are small, so
/// float precision is ample here; the result goes back into exact integer time.
[[nodiscard]] sample_time scale(sample_time value, float gain) noexcept
{
    return sample_time::from_samples_float(value.to_samples_float() * gain);
}

/// A real-time window as sample time, never shorter than one sample (at very low sample rates a
/// short window would otherwise round to zero).
[[nodiscard]] sample_time window(seconds duration, sample_rate rate) noexcept
{
    const samples whole = std::max(to_samples(duration, rate), samples{1});

    return sample_time::from_samples(whole);
}

/// Integer division rounding toward plus infinity.
[[nodiscard]] constexpr std::int64_t ceil_divide(std::int64_t numerator,
                                                 std::int64_t denominator) noexcept
{
    return -floor_divide(-numerator, denominator);
}

}  // namespace

clock_tracker::clock_tracker(const clock_tracker_config& config,
                             std::span<sample_time>      history) noexcept
    : history_{history},
      pulses_per_beat_{std::max<std::int64_t>(config.pulses_per_beat, 1)},
      pulses_per_bar_{pulses_per_beat_ * std::max<std::int64_t>(config.beats_per_bar, 1)},
      steady_alpha_{std::clamp(config.phase_gain, min_phase_gain, 1.0F)},
      steady_beta_{(steady_alpha_ * steady_alpha_) / (2.0F - steady_alpha_)},
      double_trigger_window_{window(double_trigger_window, config.rate)},
      max_acquisition_interval_{window(max_acquisition_interval, config.rate)},
      reset_window_{window(reset_coincidence_window, config.rate)}
{
    // Bad settings are programming errors, caught here in debug builds. They are also clamped
    // above, so a release build still behaves sanely.
    expects(config.rate.hz() > 0U);
    expects(config.pulses_per_beat > 0);
    expects(config.beats_per_bar > 0);
    expects(config.phase_gain > 0.0F && config.phase_gain <= 1.0F);
    expects(history.size() >= 2);
}

void clock_tracker::pulse(sample_time when) noexcept
{
    if (history_.empty()) {
        return;
    }

    // Every state except no_clock has seen a real pulse, so `previous_raw` is a real edge
    // wherever it is used below; the fallback only serves no_clock, which ignores it.
    const sample_time previous_raw = latest_raw_.value_or(when);

    expects(when >= previous_raw);

    // A pulse at `when` proves that time has reached `when`: settle the freewheel slots that
    // closed before it, so the result never depends on how often advance() is called.
    advance(when);

    if (latest_raw_.has_value() && when - previous_raw < double_trigger_window_) {
        return;  // a double trigger, not a pulse
    }

    switch (state_) {
        case clock_state::no_clock:
            pulses_in_fit_ = 1;
            record(when);
            state_ = clock_state::acquiring;
            break;

        case clock_state::acquiring: {
            const sample_time interval = when - previous_raw;

            if (interval > max_acquisition_interval_) {
                // The first edge was stray (a cable being patched, a power-on glitch): this
                // pulse takes its place, and acquisition starts again from here.
                history_[static_cast<std::size_t>(latest_sequence_) % history_.size()] = when;
            }
            else {
                lock_on_interval(when, interval);
            }
            break;
        }

        case clock_state::locked: {
            const sample_time error = when - predicted_;

            if (abs(error) > period_ / tempo_change_divisor) {
                handle_outlier(when, when - previous_raw);
            }
            else {
                outlier_interval_.reset();  // back on the grid: any outlier was a one-off
                track(error);
            }
            break;
        }

        case clock_state::freewheeling:
            return_from_freewheel(when, previous_raw);
            break;
    }

    note_real_pulse(when);
}

void clock_tracker::reset(sample_time when) noexcept
{
    // Simultaneous with the latest real pulse, delivered in either order. Judged against the raw
    // edge: the filtered time can sit tens of samples away with a low phase gain.
    if (latest_raw_ && abs(when - *latest_raw_) <= reset_window_) {
        relabel(latest_raw_sequence_);
        pending_reset_.reset();
        return;
    }

    // Older than the latest pulse, so delivered late: label the first pulse at or after it.
    if (latest_sequence_ >= 0 && when < latest_time()) {
        const auto before =
            sequence_at_or_before(when - reset_window_ - sample_time::from_ticks(1));

        relabel(before ? *before + 1 : oldest_sequence());
        pending_reset_.reset();
        return;
    }

    // In the future: label the next pulse, real or filled in.
    pending_reset_ = when;
}

void clock_tracker::advance(sample_time now) noexcept
{
    if (state_ == clock_state::locked && latest_raw_ &&
        now - *latest_raw_ > stopped_after(period_)) {
        state_ = clock_state::freewheeling;
    }

    if (state_ == clock_state::freewheeling) {
        fill_in_closed_slots(now);
    }
}

clock_state clock_tracker::state() const noexcept
{
    return state_;
}

std::int64_t clock_tracker::pulses_per_beat() const noexcept
{
    return pulses_per_beat_;
}

std::int64_t clock_tracker::pulses_per_bar() const noexcept
{
    return pulses_per_bar_;
}

std::size_t clock_tracker::history_capacity() const noexcept
{
    return history_.size();
}

std::optional<sample_time> clock_tracker::pulse_period() const noexcept
{
    if (state_ == clock_state::no_clock || state_ == clock_state::acquiring) {
        return std::nullopt;
    }

    return period_;
}

std::optional<sample_time> clock_tracker::beat_period() const noexcept
{
    if (const auto period = pulse_period()) {
        return *period * pulses_per_beat_;
    }

    return std::nullopt;
}

std::optional<std::int64_t> clock_tracker::latest_pulse() const noexcept
{
    if (latest_sequence_ < 0) {
        return std::nullopt;
    }

    return latest_sequence_ - effective_offset();
}

std::optional<sample_time> clock_tracker::pulse_time(std::int64_t index) const noexcept
{
    const auto latest = latest_pulse();

    if (!latest || index > *latest + max_index_distance || index < *latest - max_index_distance) {
        return std::nullopt;
    }

    const std::int64_t sequence = index + effective_offset();

    if (sequence <= latest_sequence_) {
        if (sequence < oldest_sequence()) {
            return std::nullopt;
        }

        return history_at(sequence);
    }

    const auto period = pulse_period();

    if (!period) {
        return std::nullopt;
    }

    return predicted_time(sequence);
}

std::optional<std::int64_t> clock_tracker::pulse_at_or_before(sample_time when) const noexcept
{
    if (const auto sequence = sequence_at_or_before(when)) {
        return *sequence - effective_offset();
    }

    return std::nullopt;
}

std::optional<std::int64_t> clock_tracker::bar_at_or_before(sample_time when) const noexcept
{
    if (const auto pulse_index = pulse_at_or_before(when)) {
        return floor_divide(*pulse_index, pulses_per_bar_);
    }

    return std::nullopt;
}

std::optional<sample_time> clock_tracker::bar_start(std::int64_t bar) const noexcept
{
    const std::int64_t max_bar = max_index_distance / pulses_per_bar_;

    if (bar > max_bar || bar < -max_bar) {
        return std::nullopt;
    }

    return pulse_time(bar * pulses_per_bar_);
}

void clock_tracker::lock_on_interval(sample_time when, sample_time interval) noexcept
{
    // The tempo is the interval between two real edges. The fit restarts from here; the pulse
    // count carries on, so bars keep their numbers.
    //
    // Every caller has already rejected intervals inside the double-trigger window, and that
    // window is at least one sample, so the period can never be zero.
    expects(interval >= double_trigger_window_);

    period_        = interval;
    pulses_in_fit_ = 2;
    outlier_interval_.reset();
    record(when);
    predicted_ = when + period_;
    state_     = clock_state::locked;
}

void clock_tracker::handle_outlier(sample_time when, sample_time interval) noexcept
{
    // A pulse far from its prediction is either a one-off (a hiccup: one late or early pulse in
    // a steady clock) or the first pulse of a new tempo. One pulse can't tell them apart; the
    // next one can, by comparing the intervals between real edges. Two consecutive intervals
    // that agree mean a new tempo. A hiccup's two intervals disagree (P + d, then P - d), and
    // its next pulse lands back on the old grid. Comparing intervals rather than counting
    // outliers matters for tempo doubling: there, every other pulse lands on the old grid.
    const bool tempo_changed = outlier_interval_ && abs(interval - *outlier_interval_) <=
                                                        *outlier_interval_ / same_interval_divisor;

    // A tempo resting on a single interval is provisional: an outlier against it is as likely
    // to mean that interval was wrong (a stray edge before the clock started). Doubt outliers
    // only once the fit has confirmed the tempo with a third pulse.
    const bool tempo_provisional = pulses_in_fit_ < 3;

    if (tempo_changed || tempo_provisional) {
        lock_on_interval(when, interval);
        return;
    }

    // Not yet known: record the edge where it really happened (it is a real pulse, and the
    // history must be exact for capture), keep the tempo, and keep predicting on the old grid
    // from the slot after this one.
    outlier_interval_ = interval;

    const sample_time  old_prediction = predicted_;
    const std::int64_t slots_ahead =
        std::max<std::int64_t>(1,
                               floor_divide((when - old_prediction).ticks(), period_.ticks()) + 1);

    record(when);
    predicted_ = old_prediction + (period_ * slots_ahead);
}

void clock_tracker::track(sample_time error) noexcept
{
    ++pulses_in_fit_;

    // Least-squares gains for a line fit through k points, used while they are larger than the
    // steady gains. They make the first few pulses converge as fast as the data allows.
    const auto  k        = static_cast<float>(pulses_in_fit_);
    const float ls_alpha = (2.0F * ((2.0F * k) - 1.0F)) / (k * (k + 1.0F));
    const float ls_beta  = 6.0F / (k * (k + 1.0F));

    const float alpha = std::max(ls_alpha, steady_alpha_);
    const float beta  = std::max(ls_beta, steady_beta_);

    const sample_time corrected = predicted_ + scale(error, alpha);

    // Defensive floor, unreachable with real input: a correction is at most a quarter period and
    // real intervals are at least the double-trigger window apart. It stays because a zero
    // period would be a division by zero on the device.
    period_ = std::max(period_ + scale(error, beta), double_trigger_window_);
    record(corrected);
    predicted_ = corrected + period_;
}

void clock_tracker::return_from_freewheel(sample_time when, sample_time previous_raw) noexcept
{
    outlier_interval_.reset();

    const sample_time raw_interval = when - previous_raw;

    // Two consecutive long gaps of the same length: the clock slowed down, it did not stop.
    // Retire the fill-ins by rewinding to the real pulse before the previous one, then re-record
    // both real pulses at the new tempo.
    const bool clock_slowed =
        previous_raw_interval_ && abs(raw_interval - *previous_raw_interval_) <=
                                      *previous_raw_interval_ / same_interval_divisor;

    if (clock_slowed && previous_raw_sequence_ >= 0) {
        latest_sequence_ = previous_raw_sequence_;
        record(previous_raw);
        latest_raw_sequence_ = latest_sequence_;
        lock_on_interval(when, raw_interval);
        return;
    }

    // The clock restarted. This pulse claims the current slot and re-anchors the phase only: it
    // may arrive anywhere in its slot, so the time since the last fill-in measures phase, not
    // tempo. The next real pulse measures the tempo from two real edges (the k = 2 least-squares
    // gains are exactly 1).
    record(when);
    predicted_     = when + period_;
    pulses_in_fit_ = 1;
    state_         = clock_state::locked;
}

void clock_tracker::fill_in_closed_slots(sample_time now) noexcept
{
    const sample_time open_for = period_ / slot_window_divisor;

    if (predicted_ + open_for > now) {
        return;
    }

    const std::int64_t closed =
        floor_divide((now - open_for - predicted_).ticks(), period_.ticks()) + 1;

    // Slots older than the history would be overwritten before anyone could read them, so skip
    // them in O(1). The loop below then runs at most history_capacity() times.
    const std::int64_t skipped =
        std::max<std::int64_t>(0, closed - static_cast<std::int64_t>(history_.size()));

    if (skipped > 0) {
        if (pending_reset_) {
            const std::int64_t first_at_reset = std::max<std::int64_t>(
                0,
                ceil_divide((*pending_reset_ - reset_window_ - predicted_).ticks(),
                            period_.ticks()));

            if (first_at_reset < skipped) {
                relabel(latest_sequence_ + 1 + first_at_reset);
                pending_reset_.reset();
            }
        }

        latest_sequence_ += skipped;
        predicted_ += period_ * skipped;
    }

    for (std::int64_t slot = skipped; slot < closed; ++slot) {
        record(predicted_);
        apply_pending_reset(predicted_, latest_sequence_);
        predicted_ += period_;
    }

    fill_ins_since_raw_ += closed;
}

void clock_tracker::record(sample_time when) noexcept
{
    ++latest_sequence_;
    history_[static_cast<std::size_t>(latest_sequence_) % history_.size()] = when;
}

void clock_tracker::apply_pending_reset(sample_time when, std::int64_t sequence) noexcept
{
    if (pending_reset_ && when >= *pending_reset_ - reset_window_) {
        relabel(sequence);
        pending_reset_.reset();
    }
}

void clock_tracker::note_real_pulse(sample_time when) noexcept
{
    // Remember a gap's length only when it held fill-ins: that is what return_from_freewheel()
    // compares to tell a slowed clock from a stopped one.
    previous_raw_interval_ = (fill_ins_since_raw_ > 0 && latest_raw_)
                                 ? std::optional<sample_time>{when - *latest_raw_}
                                 : std::nullopt;

    previous_raw_sequence_ = latest_raw_sequence_;
    latest_raw_            = when;
    latest_raw_sequence_   = latest_sequence_;
    fill_ins_since_raw_    = 0;

    apply_pending_reset(when, latest_sequence_);
}

void clock_tracker::relabel(std::int64_t sequence_of_bar_zero) noexcept
{
    index_offset_ = sequence_of_bar_zero;
    ++resets_applied_;
}

std::int64_t clock_tracker::effective_offset() const noexcept
{
    // While a reset is pending, every index is labelled as it will be once the reset applies:
    // the first pulse at or after it is 0, and earlier pulses count back from -1. One view for
    // every query, so a pulse is never announced under a number it is about to lose, and the
    // bar number agrees with the pulse labels. The slot is computed with the same arithmetic as
    // fill_in_closed_slots() and apply_pending_reset(), so a prediction labelled 0 here is the
    // pulse the reset will claim.
    if (pending_reset_ && latest_sequence_ >= 0) {
        if (const auto period = pulse_period()) {
            // The first predicted pulse at or after the reset window's start.
            const std::int64_t slots_after_next = std::max<std::int64_t>(
                0,
                ceil_divide((*pending_reset_ - reset_window_ - predicted_).ticks(),
                            period->ticks()));

            return latest_sequence_ + 1 + slots_after_next;
        }
    }

    return index_offset_;
}

std::uint32_t clock_tracker::resets_applied() const noexcept
{
    return resets_applied_;
}

std::size_t clock_tracker::pulses_between(sample_time           after,
                                          sample_time           before,
                                          std::span<grid_point> out) const noexcept
{
    std::size_t written = 0;

    if (latest_sequence_ < 0 || out.empty()) {
        return written;
    }

    // One labelling for the whole call (see effective_offset()).
    const std::int64_t offset = effective_offset();

    // Recorded pulses: from the first one after `after`, up to the latest.
    const auto   at_or_before = sequence_at_or_before(after);
    std::int64_t sequence     = at_or_before ? *at_or_before + 1 : oldest_sequence();

    for (; sequence <= latest_sequence_ && written < out.size(); ++sequence) {
        const sample_time time = history_at(sequence);

        if (time >= before) {
            return written;
        }

        out[written] = grid_point{.time = time, .index = sequence - offset};
        ++written;
    }

    // Predicted pulses, once the tempo is known.
    const auto period = pulse_period();

    if (!period) {
        return written;
    }

    for (; written < out.size(); ++sequence) {
        const sample_time time = predicted_time(sequence);

        if (time >= before) {
            return written;
        }

        if (time > after) {
            out[written] = grid_point{.time = time, .index = sequence - offset};
            ++written;
        }
    }

    return written;
}

sample_time clock_tracker::predicted_time(std::int64_t sequence) const noexcept
{
    // Every extrapolation anchors on the next predicted pulse, not on the latest recorded one.
    // They are the same while tracking, but after an outlier the latest pulse is recorded where
    // it really happened while the prediction stays on the old grid, and while freewheeling the
    // prediction is the next open slot. One anchor means reports, queries and fill-ins agree.
    expects(sequence > latest_sequence_);

    return predicted_ + (period_ * (sequence - latest_sequence_ - 1));
}

sample_time clock_tracker::latest_time() const noexcept
{
    return history_at(latest_sequence_);
}

sample_time clock_tracker::history_at(std::int64_t sequence) const noexcept
{
    expects(sequence >= oldest_sequence() && sequence <= latest_sequence_);

    return history_[static_cast<std::size_t>(sequence) % history_.size()];
}

std::int64_t clock_tracker::oldest_sequence() const noexcept
{
    return std::max<std::int64_t>(0,
                                  latest_sequence_ - static_cast<std::int64_t>(history_.size()) +
                                      1);
}

std::optional<std::int64_t> clock_tracker::sequence_at_or_before(sample_time when) const noexcept
{
    if (latest_sequence_ < 0) {
        return std::nullopt;
    }

    // At or after the latest pulse: count whole periods forward.
    if (when >= latest_time()) {
        const auto period = pulse_period();

        if (!period) {
            return latest_sequence_;
        }

        // Before the next predicted pulse, the latest is still the latest; from it on, count
        // whole periods along the predicted grid.
        if (when < predicted_) {
            return latest_sequence_;
        }

        return latest_sequence_ + 1 + floor_divide((when - predicted_).ticks(), period->ticks());
    }

    // In the past: binary search the history, which is sorted by time.
    std::int64_t low  = oldest_sequence();
    std::int64_t high = latest_sequence_;

    if (when < history_at(low)) {
        return std::nullopt;
    }

    // Invariant: history_at(low) <= when < history_at(high). Halves the range each step, so it
    // runs at most log2(history_capacity()) times.
    while (high - low > 1) {
        const std::int64_t middle = low + ((high - low) / 2);

        if (history_at(middle) <= when) {
            low = middle;
        }
        else {
            high = middle;
        }
    }

    return low;
}

}  // namespace previously
