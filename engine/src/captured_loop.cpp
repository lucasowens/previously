// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — engine: a captured loop.

#include "previously/captured_loop.hpp"

#include "previously/contract.hpp"

#include <algorithm>
#include <cstddef>

namespace previously {

captured_loop::captured_loop(page_pool& pool) noexcept
    : pool_{&pool}
{
}

captured_loop::~captured_loop()
{
    clear();
}

bool captured_loop::empty() const noexcept
{
    return bars_ == 0;
}

std::size_t captured_loop::bars() const noexcept
{
    return bars_;
}

std::span<const loop_line> captured_loop::lines() const noexcept
{
    return std::span{lines_}.first(empty() ? 0 : bars_ + 1);
}

std::int64_t captured_loop::first_bar() const noexcept
{
    return first_bar_;
}

bool captured_loop::numbered_by_reset() const noexcept
{
    return numbered_by_reset_;
}

bool captured_loop::is_final() const noexcept
{
    return !empty() && std::ranges::all_of(lines(), [](const loop_line& line) {
        return line.status == line_status::final;
    });
}

tape_range captured_loop::held() const noexcept
{
    return held_;
}

std::int64_t captured_loop::first_page() const noexcept
{
    return first_page_;
}

std::size_t captured_loop::page_count() const noexcept
{
    return page_count_;
}

std::span<const page_pool::page_id> captured_loop::pages() const noexcept
{
    return std::span{pages_}.first(pages_held_);
}

void captured_loop::assign(std::span<const loop_line> window_lines,
                           std::int64_t               number_of_first,
                           bool                       numbered_by_a_reset) noexcept
{
    expects(window_lines.size() >= 2 && window_lines.size() <= lines_.size());

    std::ranges::copy(window_lines, lines_.begin());
    bars_              = window_lines.size() - 1;
    first_bar_         = number_of_first;
    numbered_by_reset_ = numbered_by_a_reset;
}

void captured_loop::set_line(std::size_t index, const loop_line& line) noexcept
{
    expects(index <= bars_ && !empty());

    lines_[index] = line;
}

void captured_loop::hold(tape_range range, const recorder& history) noexcept
{
    expects(range.first.value() >= 0 && range.end > range.first);

    const std::int64_t first = page_number_of(range.first);
    const std::int64_t last  = page_number_of(range.end - samples{1});
    const auto         count = static_cast<std::size_t>(last - first + 1);

    expects(count <= max_pages);

    // The new run's pages, in order: those the loop holds already, and the rest from the history
    // while it has them. Before the held pages it must still have them: a run grows at its front
    // only when a capture planned again starts earlier, from the history (DESIGN.md §3.4). Pages
    // still to be recorded are left to catch_up().
    const std::int64_t held_from   = first_page_;
    const std::int64_t held_to     = first_page_ + static_cast<std::int64_t>(pages_held_);
    const samples      page_frames = pool_->page_frames();

    std::array<page_pool::page_id, max_pages> run{};
    std::size_t                               run_held = 0;

    while (run_held < count) {
        const std::int64_t number = first + static_cast<std::int64_t>(run_held);

        if (number >= held_from && number < held_to) {
            run[run_held] = pages_[static_cast<std::size_t>(number - held_from)];
            ++run_held;
            continue;
        }

        const samples start{number * page_frames.value()};

        if (start >= history.end()) {
            break;
        }

        const auto page = history.page_at(start);

        expects(page.has_value());

        if (!page) {
            break;
        }

        pool_->retain(*page);
        run[run_held] = *page;
        ++run_held;
    }

    // The pages held before that the new run does not keep go back.
    for (std::int64_t number = held_from; number < held_to; ++number) {
        if (number < first || number >= first + static_cast<std::int64_t>(run_held)) {
            pool_->release(pages_[static_cast<std::size_t>(number - held_from)]);
        }
    }

    std::ranges::copy(std::span{run}.first(run_held), pages_.begin());

    held_       = range;
    first_page_ = first;
    page_count_ = count;
    pages_held_ = run_held;
}

void captured_loop::catch_up(const recorder& history) noexcept
{
    const samples page_frames = pool_->page_frames();

    while (pages_held_ < page_count_) {
        const samples start{(first_page_ + static_cast<std::int64_t>(pages_held_)) *
                            page_frames.value()};

        // The recorder takes a page as it records the page's first frame.
        if (start >= history.end()) {
            return;
        }

        // Admission keeps a page free whenever the recorder needs one, so a run has no hole, and a
        // page the run is still to hold is the newest or near it: still in the history.
        const auto page = history.page_at(start);

        expects(page.has_value());

        if (!page) {
            return;
        }

        pool_->retain(*page);
        pages_[pages_held_] = *page;
        ++pages_held_;
    }
}

void captured_loop::clear() noexcept
{
    for (const page_pool::page_id page : pages()) {
        pool_->release(page);
    }

    bars_       = 0;
    held_       = tape_range{};
    first_page_ = 0;
    page_count_ = 0;
    pages_held_ = 0;
}

std::int64_t captured_loop::page_number_of(samples position) const noexcept
{
    // Positions in a run are never negative, so plain division finds each one's page.
    return position.value() / pool_->page_frames().value();
}

}  // namespace previously
