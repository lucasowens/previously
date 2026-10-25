// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: estimating where a past bar line was.

#include "previously/bar_line.hpp"

#include "previously/contract.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace previously {
namespace {

// The measured choices of DESIGN.md §3.4, which has the measurements.
constexpr std::size_t side_pulses       = 8;      // fitted on each side of the line
constexpr std::size_t jitter_minimum    = 16;     // usable third differences a repair needs
constexpr float       floor_samples     = 1.0F;   // the keep and repair thresholds' floor
constexpr float       keep_jitters      = 4.0F;   // step 2 keeps within this many σ̂
constexpr float       repair_jitters    = 20.0F;  // step 4 repairs beyond this many σ̂
constexpr float       spread_limit      = 64.0F;  // samples: further apart, the sides disagree
constexpr float       curvature_share   = 0.25F;  // of |c_within|, in the curvature check
constexpr float       curvature_floor   = 0.01F;  // samples per pulse², in the curvature check
constexpr float       curvature_jitters = 3.0F;   // σ̂, in the curvature check

// σ̂ is 1.4826 times the median absolute deviation, which estimates a standard deviation, over
// √20: a third difference of white jitter has 1 + 9 + 9 + 1 = 20 times its variance.
constexpr float deviation_per_mad = 1.4826F;
constexpr float sqrt_20           = 4.47213595F;

// Fed a parabola u² on pulses 1 to 8 either side, the repeated median gives slopes of ∓9 and a
// value of −16 at the line, all linear in the curvature c. So the slopes differ by 18·c, and each
// side's straight line misses the curve by 16·c.
constexpr float slopes_apart_per_curvature = 18.0F;
constexpr float miss_per_curvature         = 16.0F;

/// The third differences σ̂ can use: those of the 64 pulses before the line and the 8 after it
/// that do not touch the line's own edge.
constexpr std::size_t jitter_capacity = 66;

/// The second differences inside the two sides, 6 each.
constexpr std::size_t within_capacity = 12;

/// Up to `Capacity` floats, and their median.
template <std::size_t Capacity>
class float_list {
public:
    void push(float value) noexcept
    {
        expects(size_ < Capacity);

        values_[size_] = value;
        ++size_;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }

    /// The median; of an even count, the mean of the middle two. Zero for an empty list. It sorts
    /// the list in place, so that no copy takes the audio thread's stack.
    [[nodiscard]] float median() noexcept
    {
        if (size_ == 0) {
            return 0.0F;
        }

        // An insertion sort: the lists are short, at most 66 values. std::sort's own insertion
        // step draws a false -Warray-bounds from GCC 14 and 15 at -O3 on the 8-value lists.
        for (std::size_t next = 1; next < size_; ++next) {
            const float value = values_[next];
            std::size_t place = next;

            while (place > 0 && values_[place - 1] > value) {
                values_[place] = values_[place - 1];
                --place;
            }

            values_[place] = value;
        }

        const std::size_t middle = size_ / 2;

        return size_ % 2 == 1 ? values_[middle] : (values_[middle - 1] + values_[middle]) / 2.0F;
    }

    /// Replaces each value by its distance from `centre`.
    void to_distances_from(float centre) noexcept
    {
        const std::span<float> live = std::span{values_}.first(size_);

        std::ranges::transform(live, live.begin(), [centre](float value) {
            return std::abs(value - centre);
        });
    }

private:
    std::array<float, Capacity> values_{};
    std::size_t                 size_{0};
};

/// The edges around a line, as residuals from the straight line through the line's edge at the
/// period: exact in ticks, and as floats in samples for the side fits. Offsets run from the first
/// pulse read before the line, at most 64 back, to 8 after it.
///
/// The edges are read through `edge_at(offset)`, from the tracker's history or from a span, so
/// that no copy of them takes the audio thread's stack.
class residual_window {
public:
    template <typename EdgeAt>
    residual_window(const EdgeAt& edge_at, int before, sample_time period) noexcept
        : before_{before}
    {
        expects(before >= 0 && before <= before_most);

        const sample_time origin = edge_at(0).time;

        for (int offset = -before_; offset <= after; ++offset) {
            const pulse_edge  edge = edge_at(offset);
            const sample_time on_line =
                origin + sample_time::from_ticks(period.ticks() * std::int64_t{offset});

            exact_[slot(offset)]     = (edge.time - on_line).ticks();
            filled_in_[slot(offset)] = edge.filled_in;
        }
    }

    [[nodiscard]] bool has(int offset) const noexcept
    {
        return offset >= -before_ && offset <= after;
    }

    /// The residual at `offset`, in samples.
    [[nodiscard]] float at(int offset) const noexcept
    {
        return sample_time::from_ticks(exact_[slot(offset)]).to_samples_float();
    }

    /// The residual at `offset`, exactly, in ticks.
    [[nodiscard]] std::int64_t ticks_at(int offset) const noexcept
    {
        return exact_[slot(offset)];
    }

    [[nodiscard]] bool filled_in(int offset) const noexcept
    {
        return filled_in_[slot(offset)];
    }

private:
    static constexpr int before_most = static_cast<int>(line_pulses_before);
    static constexpr int after       = static_cast<int>(line_pulses_after);

    [[nodiscard]] static std::size_t slot(int offset) noexcept
    {
        return static_cast<std::size_t>(std::ptrdiff_t{offset} + std::ptrdiff_t{before_most});
    }

    int                                                                  before_;
    std::array<std::int64_t, line_pulses_before + line_pulses_after + 1> exact_{};
    std::array<bool, line_pulses_before + line_pulses_after + 1>         filled_in_{};
};

struct side_fit {
    float slope;
    float value;  // at the line
};

/// The repeated-median line through the 8 residuals on one side of the line: `toward` is -1 for
/// the side before it and +1 for the side after. Each edge's slope is the median of its slopes
/// to the other 7, the side's slope the median of those 8, and its value at the line the median
/// over its edges of each residual less the slope times its offset.
[[nodiscard]] side_fit fit_side(const residual_window& window, int toward) noexcept
{
    constexpr int pulses = static_cast<int>(side_pulses);

    float_list<side_pulses> edge_slopes;

    for (int a = 1; a <= pulses; ++a) {
        float_list<side_pulses - 1> slopes;

        for (int b = 1; b <= pulses; ++b) {
            if (b != a) {
                const int from = toward * a;
                const int to   = toward * b;

                slopes.push((window.at(to) - window.at(from)) / static_cast<float>(to - from));
            }
        }

        edge_slopes.push(slopes.median());
    }

    const float slope = edge_slopes.median();

    float_list<side_pulses> values;

    for (int a = 1; a <= pulses; ++a) {
        const int offset = toward * a;

        values.push(window.at(offset) - (slope * static_cast<float>(offset)));
    }

    return {.slope = slope, .value = values.median()};
}

[[nodiscard]] bool touches_fill_in(const residual_window& window, int first, int last) noexcept
{
    for (int offset = first; offset <= last; ++offset) {
        if (window.filled_in(offset)) {
            return true;
        }
    }

    return false;
}

struct jitter_estimate {
    float       sigma{0.0F};
    std::size_t values{0};
};

/// σ̂ from the third differences x[j+2] − 3·x[j+1] + 3·x[j] − x[j−1] over the window, leaving out
/// those that touch the line's own edge, a fill-in, or a pulse older than the history.
[[nodiscard]] jitter_estimate estimate_jitter(const residual_window& window) noexcept
{
    constexpr int last_centre = static_cast<int>(line_pulses_after) - 2;

    float_list<jitter_capacity> differences;

    for (int j = 1 - static_cast<int>(line_pulses_before); j <= last_centre; ++j) {
        const int first = j - 1;
        const int last  = j + 2;

        const bool touches_line = first <= 0 && last >= 0;

        if (touches_line || !window.has(first) || touches_fill_in(window, first, last)) {
            continue;
        }

        // In ticks, exactly, and only then a float. Far from the line, after a tempo change at a
        // slow clock, a residual can be millions of samples, beyond a float's sub-sample
        // precision; their third difference is not.
        const std::int64_t third = window.ticks_at(j + 2) - (3 * window.ticks_at(j + 1)) +
                                   (3 * window.ticks_at(j)) - window.ticks_at(j - 1);

        differences.push(sample_time::from_ticks(third).to_samples_float());
    }

    // The median absolute deviation, taken in place.
    const std::size_t values = differences.size();
    const float       middle = differences.median();

    differences.to_distances_from(middle);

    return {
        .sigma  = deviation_per_mad * differences.median() / sqrt_20,
        .values = values,
    };
}

struct curvature_estimate {
    std::optional<float> curvature;  // empty with no second differences left
    std::size_t          values{0};
};

/// c_within: half the median of the second differences x[j+1] − 2·x[j] + x[j−1] inside the two
/// sides, 6 each, leaving out those that touch a fill-in.
[[nodiscard]] curvature_estimate curvature_within(const residual_window& window) noexcept
{
    float_list<within_capacity> differences;

    for (const int toward : {-1, 1}) {
        for (int a = 2; a <= static_cast<int>(side_pulses) - 1; ++a) {
            const int j = toward * a;

            if (!touches_fill_in(window, j - 1, j + 1)) {
                const std::int64_t second =
                    window.ticks_at(j + 1) - (2 * window.ticks_at(j)) + window.ticks_at(j - 1);

                differences.push(sample_time::from_ticks(second).to_samples_float());
            }
        }
    }

    if (differences.size() == 0) {
        return {.curvature = std::nullopt, .values = 0};
    }

    return {.curvature = differences.median() / 2.0F, .values = differences.size()};
}

/// The rule's steps on the window around a line whose edge is `edge`, at least 8 pulses deep.
[[nodiscard]] line_estimate
estimate_from(const residual_window& window, sample_time edge, sample_time period) noexcept
{
    line_estimate estimate{.time = edge};

    // Step 1: fit each side, and estimate the jitter.
    const jitter_estimate jitter = estimate_jitter(window);
    const side_fit        left   = fit_side(window, -1);
    const side_fit        right  = fit_side(window, 1);

    estimate.jitter        = jitter.sigma;
    estimate.jitter_values = jitter.values;

    // Step 2: keep an edge that lies on a straight line. The residuals are measured from the
    // edge, so its distance from a side's line is that side's value.
    const float keep_within = std::max(floor_samples, keep_jitters * jitter.sigma);

    if (std::abs(left.value) <= keep_within || std::abs(right.value) <= keep_within) {
        estimate.basis = line_basis::on_a_line;
        return estimate;
    }

    if (jitter.values < jitter_minimum) {
        estimate.basis = line_basis::few_jitter_values;
        return estimate;
    }

    // Step 3: correct for curvature, when the slopes between the sides and the second differences
    // within them agree that there is curvature. A tempo step has none within either side, but
    // its slope difference reads as curvature between them.
    const float                between   = (right.slope - left.slope) / slopes_apart_per_curvature;
    const curvature_estimate   estimated = curvature_within(window);
    const std::optional<float> within    = estimated.curvature;

    estimate.curvature_within = within.value_or(0.0F);
    estimate.curvature_values = estimated.values;

    bool curved = false;

    if (within) {
        const float tolerance = std::max({
            curvature_share * std::abs(*within),
            curvature_floor,
            curvature_jitters * jitter.sigma,
        });

        curved = std::abs(between - *within) <= tolerance;
    }

    const float curvature   = curved ? between : 0.0F;
    const float left_value  = left.value + (miss_per_curvature * curvature);
    const float right_value = right.value + (miss_per_curvature * curvature);
    const float spread      = std::abs(left_value - right_value);
    const float mean        = (left_value + right_value) / 2.0F;

    // Step 4: repair an edge that is clearly off, when the sides agree on where it belongs. The
    // last term is twice the largest correction the curvature check could have made or left out
    // by mistake, so a wrong decision there cannot trigger a repair on its own. A pulse more than
    // half a period off is closer to its neighbour's slot.
    const float repair_beyond = std::max({
        floor_samples,
        2.0F * spread,
        repair_jitters * jitter.sigma,
        2.0F * std::abs(miss_per_curvature * between),
    });
    const float half_period   = period.to_samples_float() / 2.0F;

    if (spread <= spread_limit && std::abs(mean) > repair_beyond && std::abs(mean) <= half_period) {
        estimate.time  = edge + sample_time::from_samples_float(mean);
        estimate.basis = line_basis::repaired;
        return estimate;
    }

    // Step 5: otherwise keep the edge.
    estimate.basis = line_basis::kept;

    return estimate;
}

}  // namespace

line_estimate
estimate_line(std::span<const pulse_edge> edges, std::size_t line, sample_time period) noexcept
{
    expects(line + line_pulses_after < edges.size());
    expects(period.ticks() > 0);

    if (line < side_pulses) {
        return {.time = edges[line].time, .basis = line_basis::short_history};
    }

    const auto edge_at = [&](int offset) {
        return edges[static_cast<std::size_t>(static_cast<std::ptrdiff_t>(line) + offset)];
    };
    const residual_window window{
        edge_at,
        static_cast<int>(std::min(line, line_pulses_before)),
        period,
    };

    return estimate_from(window, edges[line].time, period);
}

std::optional<line_estimate> estimate_line(const clock_tracker& grid, std::int64_t pulse) noexcept
{
    const std::optional<sample_time> period = grid.pulse_period();

    if (!period) {
        return std::nullopt;
    }

    // The line and the 8 pulses after it: until all are recorded, the line is not final.
    for (std::int64_t offset = 0; offset <= std::int64_t{line_pulses_after}; ++offset) {
        if (!grid.edge(pulse + offset)) {
            return std::nullopt;
        }
    }

    // Before it, as far back as the history reaches. The window reads the edges in place.
    std::size_t recorded = 0;

    while (recorded < line_pulses_before &&
           grid.edge(pulse - static_cast<std::int64_t>(recorded) - 1)) {
        ++recorded;
    }

    const auto edge_at = [&](int offset) {
        return grid.edge(pulse + offset).value_or(pulse_edge{});
    };
    const sample_time edge = edge_at(0).time;

    if (recorded < side_pulses) {
        return line_estimate{.time = edge, .basis = line_basis::short_history};
    }

    return estimate_from(residual_window{edge_at, static_cast<int>(recorded), *period},
                         edge,
                         *period);
}

}  // namespace previously
