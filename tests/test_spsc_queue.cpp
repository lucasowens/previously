// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: lock-free single-producer, single-consumer queue.

#include "previously/spsc_queue.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <thread>

namespace previously {
namespace {

TEST_CASE("an empty queue pops nothing")
{
    spsc_queue<int, 4> queue;

    CHECK_FALSE(queue.try_pop().has_value());
}

TEST_CASE("items come out in the order they went in")
{
    spsc_queue<int, 4> queue;

    REQUIRE(queue.try_push(1));
    REQUIRE(queue.try_push(2));
    REQUIRE(queue.try_push(3));

    CHECK(queue.try_pop() == 1);
    CHECK(queue.try_pop() == 2);
    CHECK(queue.try_pop() == 3);
    CHECK_FALSE(queue.try_pop().has_value());
}

TEST_CASE("a full queue rejects pushes and keeps its contents")
{
    spsc_queue<int, 4> queue;

    for (int i = 0; i < 4; ++i) {
        REQUIRE(queue.try_push(i));
    }

    CHECK_FALSE(queue.try_push(99));

    for (int i = 0; i < 4; ++i) {
        CHECK(queue.try_pop() == i);
    }
}

TEST_CASE("indices wrap around the buffer many times")
{
    spsc_queue<int, 4> queue;

    for (int i = 0; i < 1'000; ++i) {
        REQUIRE(queue.try_push(i));
        REQUIRE(queue.try_pop() == i);
    }
}

TEST_CASE("one producer and one consumer thread transfer every item in order")
{
    constexpr std::uint32_t item_count = 200'000;

    spsc_queue<std::uint32_t, 64> queue;

    std::thread producer{[&queue] {
        for (std::uint32_t next = 0; next < item_count;) {
            if (queue.try_push(next)) {
                ++next;
            }
        }
    }};

    std::uint32_t expected      = 0;
    bool          order_correct = true;

    while (expected < item_count) {
        if (const auto item = queue.try_pop()) {
            order_correct = order_correct && (*item == expected);
            ++expected;
        }
    }

    producer.join();

    CHECK(order_correct);
    CHECK_FALSE(queue.try_pop().has_value());
}

}  // namespace
}  // namespace previously
