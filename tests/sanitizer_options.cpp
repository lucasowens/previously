// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: what ASan is told at start-up, however the test binary is run.
//
// ASan reads this function's string before ASAN_OPTIONS, so an option set here holds when the
// binary is run directly, from an IDE or with -tc, not only under ctest, and a developer's own
// ASAN_OPTIONS still apply on top of it. Without ASan the function is never called.

// alloc_dealloc_mismatch: a block must be freed by the allocator that made it, new[] by delete[]
// and malloc by free. Linux turns the check on by default and macOS leaves it off, so without
// this a mismatch failed CI and passed the local gate.
// NOLINTNEXTLINE(bugprone-reserved-identifier, readability-identifier-naming): ASan's hook
extern "C" const char* __asan_default_options()
{
    return "alloc_dealloc_mismatch=1";
}
