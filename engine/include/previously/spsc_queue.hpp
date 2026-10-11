// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — lock-free single-producer, single-consumer queue.
//
// This is how the UI, MIDI and tests talk to the audio thread without locks. Exactly one thread
// pushes and exactly one thread pops. Neither ever blocks or allocates, so the audio callback can
// drain it safely.
//
// How it works: `head_` and `tail_` are counters that only ever grow, and their difference is the
// item count. Capacity is a power of two, so `counter & mask` gives the slot index, and unsigned
// wrap-around keeps the arithmetic correct forever. The release store that publishes an index
// pairs with the acquire load on the other side, so the item's bytes are visible before the
// index says they are there.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace previously {

template <typename T, std::size_t Capacity>
class spsc_queue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "spsc_queue capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "spsc_queue items are copied by value and must be trivially copyable");
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "spsc_queue requires lock-free atomics");

public:
    /// Producer thread only. Returns false, and leaves the queue unchanged, when it is full.
    [[nodiscard]] bool try_push(const T& item) noexcept
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);

        if (tail - head == Capacity) {
            return false;
        }

        slots_[tail & mask] = item;
        tail_.store(tail + 1, std::memory_order_release);

        return true;
    }

    /// Consumer thread only. Returns the oldest item, or nothing when the queue is empty.
    [[nodiscard]] std::optional<T> try_pop() noexcept
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);

        if (head == tail) {
            return std::nullopt;
        }

        const T item = slots_[head & mask];
        head_.store(head + 1, std::memory_order_release);

        return item;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept
    {
        return Capacity;
    }

private:
    static constexpr std::size_t mask = Capacity - 1;

    // Producer and consumer each write their own counter. Keeping the two counters at least one
    // cache line apart stops two cores (on host) fighting over a shared line ("false sharing").
    // Explicit spacing is used instead of alignas(64): over-aligned types need aligned operator
    // new, which older platforms (macOS < 10.13, Rack's minimum) do not provide.
    static constexpr std::size_t cache_line_bytes = 64;

    std::atomic<std::size_t>                                 head_{0};
    [[maybe_unused]] std::array<std::byte, cache_line_bytes> separation_{};
    std::atomic<std::size_t>                                 tail_{0};
    std::array<T, Capacity>                                  slots_{};
};

}  // namespace previously
