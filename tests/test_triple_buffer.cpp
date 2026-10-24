// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the lock-free triple buffer, the newest value from one thread to another.

#include "previously/triple_buffer.hpp"

#include "realtime_guard.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

namespace previously {
namespace {

TEST_CASE("before anything is published, the reader takes nothing and reads a default value")
{
    triple_buffer<int> buffer;

    CHECK_FALSE(buffer.take());
    CHECK(buffer.read() == 0);
}

TEST_CASE("a published value is taken once, and read until the next is taken")
{
    triple_buffer<int> buffer;

    buffer.publish(5);

    CHECK(buffer.take());
    CHECK(buffer.read() == 5);
    CHECK_FALSE(buffer.take());
    CHECK(buffer.read() == 5);
}

TEST_CASE("of several values published between takes, the newest is taken")
{
    triple_buffer<int> buffer;

    buffer.publish(1);
    buffer.publish(2);
    buffer.publish(3);

    CHECK(buffer.take());
    CHECK(buffer.read() == 3);
    CHECK_FALSE(buffer.take());
}

TEST_CASE("the value being read is never written, however many are published meanwhile")
{
    triple_buffer<int> buffer;

    buffer.publish(1);
    REQUIRE(buffer.take());

    const int* const held = &buffer.read();

    for (int value = 2; value <= 1'000; ++value) {
        buffer.publish(value);
    }

    CHECK(&buffer.read() == held);
    CHECK(*held == 1);

    CHECK(buffer.take());
    CHECK(buffer.read() == 1'000);
}

TEST_CASE("any order of publishes and takes reads the newest value taken")
{
    // A seeded sequence of publishes and takes, checked against what they should give: a take
    // succeeds when something was published since the last, and then reads the newest.
    triple_buffer<int> buffer;
    std::optional<int> pending;
    int                current = 0;
    std::uint32_t      state   = 1;
    std::size_t        wrong   = 0;

    for (int step = 1; step <= 10'000; ++step) {
        state = (state * 1'664'525U) + 1'013'904'223U;

        if ((state >> 16U) % 3 != 0) {
            buffer.publish(step);
            pending = step;
        }
        else {
            const bool taken = buffer.take();

            if (taken != pending.has_value()) {
                ++wrong;
            }

            if (pending) {
                current = *pending;
                pending.reset();
            }
        }

        if (buffer.read() != current) {
            ++wrong;
        }
    }

    CHECK(wrong == 0);
}

/// A value too large to copy in one instruction, its words all the same, so a torn read shows.
struct stamped {
    std::array<std::uint64_t, 32> words{};
};

TEST_CASE("from one thread to another, every value taken is whole, newer than the last, and the "
          "last is the last published")
{
    constexpr std::uint64_t value_count = 200'000;

    triple_buffer<stamped> buffer;
    std::atomic<bool>      finished{false};

    std::thread writer{[&buffer, &finished] {
        for (std::uint64_t serial = 1; serial <= value_count; ++serial) {
            stamped value;

            value.words.fill(serial);
            buffer.publish(value);
        }

        finished.store(true, std::memory_order_release);
    }};

    std::uint64_t last  = 0;
    std::size_t   takes = 0;
    std::size_t   torn  = 0;
    std::size_t   stale = 0;

    const auto take_one = [&] {
        if (!buffer.take()) {
            return;
        }

        const stamped&      value  = buffer.read();
        const std::uint64_t serial = value.words[0];

        const bool whole = std::ranges::all_of(value.words, [serial](std::uint64_t word) {
            return word == serial;
        });

        ++takes;
        torn += whole ? 0 : 1;
        stale += serial <= last ? 1 : 0;
        last = serial;
    };

    // Taking until the writer has finished, then once more: everything it published is visible
    // by then, so a buffer that works has the last value to give, and a broken one fails here
    // rather than leaving the loop waiting for it.
    while (!finished.load(std::memory_order_acquire)) {
        take_one();
    }

    take_one();
    writer.join();

    CHECK(takes > 1);
    CHECK(torn == 0);
    CHECK(stale == 0);
    CHECK(last == value_count);
    CHECK_FALSE(buffer.take());
}

TEST_CASE("publishing and taking allocate nothing")
{
    triple_buffer<stamped> buffer;

    test::realtime_guard::reset_violations();
    {
        const test::realtime_guard guard;

        buffer.publish(stamped{});
        static_cast<void>(buffer.take());
        static_cast<void>(buffer.read());
    }

    CHECK(test::realtime_guard::violations() == 0U);
}

}  // namespace
}  // namespace previously
