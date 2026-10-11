# 0001 — C++20 embedded subset, Core Guidelines style

- **Status:** Accepted
- **Date:** 2026-10-11

## Context

The project targets the Daisy Seed (Cortex-M7), a VCV Rack plugin and host tools, and every
dependency (libDaisy, DaisySP, the Rack SDK) is C++. The author is an experienced engineer, mainly
C# and JavaScript, learning systems programming through this project. They want to write the
language the way experienced practitioners do. They read code like a book, so a consistent "one
author" style matters a great deal.

Writing the engine in C17 was considered, behind a thin C++ adapter layer. It would teach C
directly and suits MISRA C well. It was rejected because the goal is to learn the language
that's *native to modern audio and embedded work*, and that is C++.

## Decision

- **Language:** C++20 across all targets, restricted to an embedded subset: no exceptions, no
  RTTI, no heap after initialization, no recursion, bounded loops in the realtime path, `float`
  not `double`, and strong unit types.
- **Style:** C++ Core Guidelines naming (`snake_case`, `trailing_underscore_` members, `CamelCase`
  template parameters) with Stroustrup brace placement, a 100-column limit, aligned consecutive
  declarations, and blank lines between logical steps.
- **Enforcement by tools, not discipline:** clang-format and clang-tidy (both pinned to LLVM 23),
  cppcheck, and compiler warnings as errors. CI fails on any difference.
- The written rules and their reasons live in [CODING_STANDARD.md](../CODING_STANDARD.md).

## Consequences

- The firmware adapter calls libDaisy and the Rack adapter calls the Rack SDK, both of which use
  PascalCase. Those files mix two styles, and the mix marks exactly where our code meets theirs.
- **The libDaisy and Rack builds default to older standards**, so they're overridden:
  - libDaisy's CMake sets C++14; the firmware target overrides it to C++20.
  - Rack's makefile appends `-std=c++11`; the plugin Makefile appends `-std=c++20` after it.
  - Both work with gcc 15 and Apple clang 21.
- **`alignas(64)` was avoided in `spsc_queue`.** Over-aligned types need aligned `operator new`,
  which Rack's macOS 10.9 deployment target lacks. Explicit spacing gives the same protection
  against false sharing.
