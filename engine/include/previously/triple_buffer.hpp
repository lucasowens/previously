// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — lock-free triple buffer: the newest value, from one thread to another.
//
// The audio thread publishes the UI model, and the thread that draws wants only the newest one. A
// queue would hand it every model in turn, and once full would drop the newest (DESIGN.md §9.3).
// Here neither side ever waits or allocates, and the reader always takes the newest value.
//
// How it works: three slots. The writer owns one, the back; the reader owns one, the front; and
// the third, the middle, is shared. The middle's index lives in an atomic byte with a "fresh" bit.
// Publishing writes the back slot, then exchanges it with the middle and sets fresh: the old
// middle becomes the writer's next back slot. Taking, when fresh, exchanges the front with the
// middle and clears fresh. Each side writes only the slot it owns, so no value is written while
// it is read. Both exchanges acquire and release: the writer's release makes a value's bytes
// visible to the reader's acquire, and the reader's release makes sure it has finished reading a
// slot before the writer, through its acquire, can write it again.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace previously {

template <typename T>
class triple_buffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "triple_buffer values are copied by value and must be trivially copyable");
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                  "triple_buffer requires lock-free atomics");

public:
    /// Writer thread only. Makes `value` the newest. A value published before it and not yet
    /// taken is dropped.
    void publish(const T& value) noexcept
    {
        slots_[back_] = value;

        const std::uint8_t old_middle =
            middle_.exchange(static_cast<std::uint8_t>(back_ | fresh), std::memory_order_acq_rel);

        back_ = static_cast<std::uint8_t>(old_middle & index_mask);
    }

    /// Reader thread only. Takes the newest value, if one was published since the last take, and
    /// says whether one was.
    [[nodiscard]] bool take() noexcept
    {
        // Only the reader clears fresh, so a value seen here is still there to take below.
        if ((middle_.load(std::memory_order_relaxed) & fresh) == 0) {
            return false;
        }

        const std::uint8_t old_middle = middle_.exchange(front_, std::memory_order_acq_rel);

        front_ = static_cast<std::uint8_t>(old_middle & index_mask);

        return true;
    }

    /// Reader thread only. The value last taken: a default T until the first take.
    [[nodiscard]] const T& read() const noexcept
    {
        return slots_[front_];
    }

private:
    static constexpr std::uint8_t index_mask = 0b011;
    static constexpr std::uint8_t fresh      = 0b100;

    std::array<T, 3>          slots_{};
    std::atomic<std::uint8_t> middle_{1};
    std::uint8_t              back_{0};   // the writer's
    std::uint8_t              front_{2};  // the reader's
};

}  // namespace previously
