// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: realtime guard and the replacement global allocation functions.

#include "realtime_guard.hpp"

#include <cstdlib>
#include <new>

namespace previously::test {

namespace {

thread_local int         guard_depth           = 0;
thread_local std::size_t allocation_violations = 0;

void note_allocation() noexcept
{
    if (guard_depth > 0) {
        ++allocation_violations;
    }
}

void* allocate_or_throw(std::size_t size)
{
    note_allocation();

    void* memory = std::malloc(size == 0 ? 1 : size);

    if (memory == nullptr) {
        throw std::bad_alloc{};
    }

    return memory;
}

}  // namespace

realtime_guard::realtime_guard() noexcept
{
    ++guard_depth;
}

realtime_guard::~realtime_guard()
{
    --guard_depth;
}

std::size_t realtime_guard::violations() noexcept
{
    return allocation_violations;
}

void realtime_guard::reset_violations() noexcept
{
    allocation_violations = 0;
}

}  // namespace previously::test

// Replacement global allocation functions (the standard allows a program to define these).
// Every form of new funnels through allocate_or_throw so no allocation can slip past the guard.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)

void* operator new(std::size_t size)
{
    return previously::test::allocate_or_throw(size);
}

void* operator new[](std::size_t size)
{
    return previously::test::allocate_or_throw(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept
{
    std::free(memory);
}

// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
