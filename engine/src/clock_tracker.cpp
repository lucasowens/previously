// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: clock tracker. The header explains the approach.

#include "previously/clock_tracker.hpp"

#include "previously/contract.hpp"

#include <algorithm>

namespace previously {

namespace {

/// Shortest believable pulse interval while acquiring: 24 samples (0.5 ms at 48 kHz). That is
/// faster than 24 PPQN at 2,000 BPM; anything shorter is noise.
constexpr sample_time min_acquisition_interval = sample_time::from_samples(samples{24});

/// How close a reset must be to a pulse to count as the same moment.
constexpr sample_time reset_coincidence_window = sample_time::from_samples(samples{96});

/// Upper bound on pulses added in one advance() call while freewheeling, so the call is
/// bounded even if the caller skips a long stretch of time.
constexpr int max_freewheel_pulses_per_advance = 64;

/// Multiplies a time by a gain, rounding to the nearest tick. Gains and errors are small, so
/// float precision is ample here; the result goes back into exact integer time.
[[nodiscard]] sample_time scale(sample_time value, float gain) noexcept
{
    return sample_time::from_samples_float(value.to_samples_float() * gain);
}

[[nodiscard]] sample_time absolute(sample_time value) noexcept
{
    return value < sample_time{} ? -value : value;
}

/// sample_time divided by a whole number, rounding toward zero.
[[nodiscard]] sample_time divide(sample_time value, std::int64_t divisor) noexcept
{
    return sample_time::from_ticks(value.ticks() / divisor);
}

}  // namespace

clock_tracker::clock_tracker(const clock_tracker_config& config) noexcept
    : config_{config},
      steady_alpha_{config.phase_gain},
      steady_beta_{(config.phase_gain * config.phase_gain) / (2.0F - config.phase_gain)}
{
    expects(config.pulses_per_beat > 0);
    expects(config.beats_per_bar > 0);
    expects(config.phase_gain > 0.0F && config.phase_gain <= 1.0F);
}

void clock_tracker::pulse(sample_time when) noexcept
{
    switch (state_) {
        case clock_state::no_clock:
            start_acquisition(when);
            break;

        case clock_state::acquiring: {
            const sample_time interval = when - latest_time();

            if (interval < min_acquisition_interval) {
                return;  // glitch: too soon after the first pulse to be a clock
            }

            period_        = interval;
            pulses_in_fit_ = 2;
            record(when);
            predicted_ = when + period_;
            state_     = clock_state::locked;
            break;
        }

        case clock_state::locked:
        case clock_state::freewheeling: {
            if (when - latest_time() < divide(period_, 2)) {
                return;  // glitch: a double trigger, not a pulse
            }

            const sample_time error = when - predicted_;

            if (absolute(error) > divide(period_, 4)) {
                reacquire(when);
            }
            else {
                track(error);
            }

            state_ = clock_state::locked;
            break;
        }
    }
}

void clock_tracker::reset(sample_time when) noexcept
{
    // The reset and the first pulse of the bar are usually simultaneous, but they can be
    // delivered in either order. If the pulse came first, relabel it; otherwise label the next.
    const bool coincides_with_latest =
        latest_sequence_ >= 0 && absolute(when - latest_time()) <= reset_coincidence_window;

    if (coincides_with_latest) {
        index_offset_  = latest_sequence_;
        reset_pending_ = false;
    }
    else {
        reset_time_    = when;
        reset_pending_ = true;
    }
}

void clock_tracker::advance(sample_time now) noexcept
{
    if (state_ == clock_state::locked && now - latest_time() > period_ * 2) {
        state_ = clock_state::freewheeling;
    }

    if (state_ != clock_state::freewheeling) {
        return;
    }

    for (int added = 0; added < max_freewheel_pulses_per_advance && predicted_ <= now; ++added) {
        record(predicted_);
        predicted_ += period_;
    }
}

clock_state clock_tracker::state() const noexcept
{
    return state_;
}

std::int64_t clock_tracker::pulses_per_bar() const noexcept
{
    return config_.pulses_per_beat * config_.beats_per_bar;
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
        return *period * config_.pulses_per_beat;
    }

    return std::nullopt;
}

std::optional<std::int64_t> clock_tracker::latest_pulse() const noexcept
{
    if (latest_sequence_ < 0) {
        return std::nullopt;
    }

    return latest_sequence_ - index_offset_;
}

std::optional<sample_time> clock_tracker::pulse_time(std::int64_t index) const noexcept
{
    if (latest_sequence_ < 0) {
        return std::nullopt;
    }

    const std::int64_t sequence = index + index_offset_;

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

    return latest_time() + (*period * (sequence - latest_sequence_));
}

std::optional<std::int64_t> clock_tracker::pulse_at_or_before(sample_time when) const noexcept
{
    if (latest_sequence_ < 0) {
        return std::nullopt;
    }

    // At or after the latest pulse: count whole periods forward.
    if (when >= latest_time()) {
        const auto period = pulse_period();

        if (!period) {
            return latest_sequence_ - index_offset_;
        }

        const std::int64_t periods_ahead =
            floor_divide((when - latest_time()).ticks(), period->ticks());

        return latest_sequence_ + periods_ahead - index_offset_;
    }

    // In the past: binary search the history, which is sorted by time.
    std::int64_t low  = oldest_sequence();
    std::int64_t high = latest_sequence_;

    if (when < history_at(low)) {
        return std::nullopt;
    }

    // Invariant: history_at(low) <= when < history_at(high). Halves the range each step, so
    // this runs at most log2(history_capacity) = 11 times.
    while (high - low > 1) {
        const std::int64_t middle = low + ((high - low) / 2);

        if (history_at(middle) <= when) {
            low = middle;
        }
        else {
            high = middle;
        }
    }

    return low - index_offset_;
}

std::optional<std::int64_t> clock_tracker::bar_at_or_before(sample_time when) const noexcept
{
    if (const auto pulse_index = pulse_at_or_before(when)) {
        return floor_divide(*pulse_index, pulses_per_bar());
    }

    return std::nullopt;
}

std::optional<sample_time> clock_tracker::bar_start(std::int64_t bar) const noexcept
{
    return pulse_time(bar * pulses_per_bar());
}

void clock_tracker::start_acquisition(sample_time when) noexcept
{
    pulses_in_fit_ = 1;
    record(when);
    state_ = clock_state::acquiring;
}

void clock_tracker::reacquire(sample_time when) noexcept
{
    // The tempo changed: restart the fit from the interval since the previous pulse. The pulse
    // count carries on, so bars keep their numbers.
    period_        = when - latest_time();
    pulses_in_fit_ = 2;
    record(when);
    predicted_ = when + period_;
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

    period_ += scale(error, beta);
    record(corrected);
    predicted_ = corrected + period_;
}

void clock_tracker::record(sample_time when) noexcept
{
    ++latest_sequence_;
    history_[static_cast<std::size_t>(latest_sequence_) % history_capacity] = when;

    if (reset_pending_ && when >= reset_time_) {
        index_offset_  = latest_sequence_;
        reset_pending_ = false;
    }
}

sample_time clock_tracker::latest_time() const noexcept
{
    return history_at(latest_sequence_);
}

sample_time clock_tracker::history_at(std::int64_t sequence) const noexcept
{
    expects(sequence >= oldest_sequence() && sequence <= latest_sequence_);

    return history_[static_cast<std::size_t>(sequence) % history_capacity];
}

std::int64_t clock_tracker::oldest_sequence() const noexcept
{
    return std::max<std::int64_t>(0,
                                  latest_sequence_ - static_cast<std::int64_t>(history_capacity) +
                                      1);
}

}  // namespace previously
