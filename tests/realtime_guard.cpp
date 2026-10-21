// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: realtime guard and the replacement global allocation functions.

#include "realtime_guard.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>

namespace previously::test {

namespace {

thread_local int         guard_depth           = 0;
thread_local int         failure_depth         = 0;
thread_local std::size_t allocation_violations = 0;

void note_allocation() noexcept
{
    if (guard_depth > 0) {
        ++allocation_violations;
    }
}

void* allocate_or_null(std::size_t size) noexcept
{
    note_allocation();

    if (failure_depth > 0) {
        return nullptr;
    }

    return std::malloc(size == 0 ? 1 : size);
}

void* allocate_or_throw(std::size_t size)
{
    void* memory = allocate_or_null(size);

    if (memory == nullptr) {
        throw std::bad_alloc{};
    }

    return memory;
}

/// For over-aligned types. aligned_alloc needs a size that is a multiple of the alignment, and
/// macOS needs the alignment to be at least a pointer's.
void* allocate_aligned_or_null(std::size_t size, std::align_val_t alignment) noexcept
{
    note_allocation();

    if (failure_depth > 0) {
        return nullptr;
    }

    const std::size_t align = std::max(static_cast<std::size_t>(alignment), sizeof(void*));

    // A size within the alignment of the limit would wrap while it is rounded up, and ask for
    // next to nothing. No allocator can serve it, so it fails here as libc++'s forms do. A
    // new-expression whose size overflows relies on that: clang passes SIZE_MAX.
    if (size > std::numeric_limits<std::size_t>::max() - (align - 1)) {
        return nullptr;
    }

    const std::size_t rounded = ((std::max(size, std::size_t{1}) + align - 1) / align) * align;

    return std::aligned_alloc(align, rounded);
}

void* allocate_aligned_or_throw(std::size_t size, std::align_val_t alignment)
{
    void* memory = allocate_aligned_or_null(size, alignment);

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

allocation_failure::allocation_failure() noexcept
{
    ++failure_depth;
}

allocation_failure::~allocation_failure()
{
    --failure_depth;
}

}  // namespace previously::test

// Replacement global allocation functions (the standard allows a program to define these).
// Every replaceable form is defined here: throwing, nothrow and over-aligned, for objects and
// arrays, with every matching delete. So no allocation slips past the guard, and every block is
// freed by the allocator that made it. A form left out would come from the sanitizer's runtime
// and be freed here: ASan on Linux reports that as an alloc-dealloc mismatch.
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

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    return previously::test::allocate_or_null(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    return previously::test::allocate_or_null(size);
}

void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept
{
    std::free(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return previously::test::allocate_aligned_or_throw(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return previously::test::allocate_aligned_or_throw(size, alignment);
}

void* operator new(std::size_t      size,
                   std::align_val_t alignment,
                   const std::nothrow_t& /*tag*/) noexcept
{
    return previously::test::allocate_aligned_or_null(size, alignment);
}

void* operator new[](std::size_t      size,
                     std::align_val_t alignment,
                     const std::nothrow_t& /*tag*/) noexcept
{
    return previously::test::allocate_aligned_or_null(size, alignment);
}

void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    std::free(memory);
}

void operator delete(void* memory,
                     std::align_val_t /*alignment*/,
                     const std::nothrow_t& /*tag*/) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory,
                       std::align_val_t /*alignment*/,
                       const std::nothrow_t& /*tag*/) noexcept
{
    std::free(memory);
}

// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
