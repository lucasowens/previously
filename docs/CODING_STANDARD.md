# Previously Coding Standard

This codebase should read as if **one careful author** wrote it. Every rule below has a reason,
and wherever possible a tool enforces it, so nobody has to remember the rules.

The standard draws on the **C++ Core Guidelines** and **MISRA C++:2023**, adapted for realtime
audio on a Cortex-M7. Where we depart from those sources, the reason is written down.

| Enforced by | What |
|---|---|
| `.clang-format` (LLVM 23) | All formatting (§2) |
| `.clang-tidy` (LLVM 23) | Naming (§5) and most language rules (§3) |
| `cmake/previously_options.cmake` | Warnings as errors, no exceptions, no RTTI (§3, §4) |
| cppcheck | Additional static analysis (§4) |
| Sanitizers (ASan, UBSan) | Memory and undefined-behaviour errors in every host test run |
| Realtime guard (tests) | No allocation inside `engine::process()` (§3.3) |
| Review | Everything else, such as named constants, comments and test quality |

Run `make lint` to check everything. CI and the pre-commit hook run the same checks.

---

## 1. Principles

1. **Clarity first.** Code is read far more often than it is written. Write for the reader who
   arrives in a year with no context.
2. **Realtime is a hard constraint.** The audio path has a fixed deadline every 0.67 ms. Anything
   that can take unbounded time (allocation, locks, I/O, unbounded loops) is banned there.
3. **Make wrong code fail to compile.** Prefer types and `constexpr` checks over comments and
   discipline. Strong unit types are the main example (§3.4).
4. **Same code, same result, everywhere.** The engine builds identically for tests, Rack and
   firmware. Behaviour does not depend on the platform.
5. **No silent exceptions to the rules.** Every suppression names the rule it suppresses and says
   why (§4.3).

---

## 2. Formatting

Formatting is entirely mechanical: run `make format`. The choices and their reasons:

### 2.1 Layout

- **4-space indentation, no tabs, 100-column limit.** Wide enough for descriptive names, narrow
  enough for side-by-side diffs.
- **Stroustrup braces.** A function body opens on its own line, so each function's start stands
  out when scanning. Control statements (`if`, `for`, `switch`) open on the same line, which keeps
  logic compact. `else` starts on a new line under the closing brace.
- **Always use braces**, even for one-line bodies. No body is ever squeezed onto the line of its
  `if` or `for`.

### 2.2 Vertical rhythm

- Separate logical steps inside a function with **one blank line**. A function should read as
  short paragraphs.
- Separate function definitions with exactly one blank line. Never use more than one blank line.

### 2.3 Alignment

Consecutive declarations and assignments are aligned so related lines read as a table:

```cpp
const std::size_t frames     = data.size() / format.block_align;
const std::size_t bytes_each = format.bits_per_sample / 8U;
```

### 2.4 Arguments and parameters

Arguments fit on one line, or go one per line. They are never packed several to a line in a block.

### 2.5 Pointers and references

`*` and `&` bind to the type: `float* samples`, `const engine_config& config`. The type is "pointer
to float", and this keeps it together.

### 2.6 Headers

- **`#pragma once`**, not include guards. Every compiler we target supports it, and it cannot
  collide the way hand-written guard names can.
- **Include order:** the file's own header, then `"previously/..."`, then other project headers, then
  third-party (`<doctest/...>`, `<daisy_pod.h>`, `<rack.hpp>`), then the standard library. Each
  group is separated by a blank line and sorted. clang-format enforces this.

---

## 3. Language rules (the embedded subset)

### 3.1 Language level

**C++20** everywhere. Use the modern facilities that make code safer and clearer:

| Use | Instead of |
|---|---|
| `std::span` | pointer + length |
| `std::array` | C arrays |
| `enum class` | plain `enum` |
| `constexpr` | macros |
| designated initializers (`{.rate = ..., .max_block_frames = ...}`) | positional aggregate init |
| `std::source_location` | `__FILE__` / `__LINE__` macros |

### 3.2 No exceptions, no RTTI

The engine, tools and firmware build with `-fno-exceptions -fno-rtti`:

- On the M7, exceptions add code size and non-deterministic unwinding, and RTTI adds tables we
  never use.
- Failures are reported with return values (`[[nodiscard]] bool`, `status`, `std::optional`).
- Programming errors are caught by contracts (§3.7).

Host tests may use exceptions, because doctest uses them internally.

### 3.3 Memory and the realtime path

- **No heap allocation after initialization**, anywhere in the engine. Memory comes from fixed
  storage set up once at startup (DESIGN.md §2.3).
- `engine::process()` and everything it calls must be **realtime-safe**:
  - no allocation (enforced by the test realtime guard)
  - no locks, no blocking, no I/O
  - **bounded loops** only, each with a visible bound
  - no recursion
- Data crosses threads **only** through `spsc_queue` or a documented atomic (§3.11).

### 3.4 Types

- **Strong unit types** for every physical quantity: `samples`, `seconds`, `sample_rate`, with
  `beats` and `ratio` to come. Conversions are named functions (`to_seconds`, `to_samples`). Raw
  numbers carrying units across interfaces are not allowed.
- **Absolute time is never a float.**
  - Whole-sample counts are `samples` (`int64`).
  - Sub-sample time is `sample_time`: Q47.16 fixed point, 64-bit ticks with 65,536 per sample.
- **`sample_time` is not a `quantity<>`, on purpose.** A quantity is constructed from its raw
  value, so `sample_time{48'000}` would read as 48,000 samples but mean 48,000 ticks.
  `sample_time` keeps its raw constructor private, and every way in is named: `from_ticks`,
  `from_samples`, `from_samples_float`.
- **`float`, not `double`.** The M7 has a single-precision FPU, and double precision is emulated
  in software. `-Wdouble-promotion` catches accidental promotion. Floating-point literals carry
  the `F` suffix: `0.5F`.
- **Fixed-width integers** (`std::int64_t`, `std::uint32_t`) wherever size matters.
  `std::size_t` for sizes and indices.
- Literal suffixes are upper case (`48'000U`, `0.5F`). Use digit separators (`'`) in numbers with
  more than four digits.

### 3.5 Declarations

- **`const` by default.** Mark a local mutable only when it actually changes.
- **`constexpr`** for anything computable at compile time.
- **`noexcept`** on every function that cannot fail, which is nearly all engine code.
- **`[[nodiscard]]`** on every function whose result must not be ignored: getters, fallible
  operations, pure functions.
- **`explicit`** on single-argument constructors.
- One declaration per line.

### 3.6 Named constants

DSP code is full of numbers. A number gets a **named `constexpr` constant** when it has meaning
beyond its value (`volts_per_full_scale = 5.0F`, `cache_line_bytes = 64`). Self-explanatory
literals in formulas (`0.5F * x`, `2.0F * pi`) may stay inline. Review enforces this; the tool
rule is off (see `.clang-tidy`).

### 3.7 Contracts

Preconditions and postconditions are stated with `previously::expects()` and `previously::ensures()`
(`contract.hpp`):

- **They catch programming errors, not runtime conditions.** A full queue is a runtime condition,
  so it's a return value. Passing mismatched buffer lengths is a bug, so it's a contract.
- Conditions must have **no side effects**, because contracts compile out under `NDEBUG`.
- **On a violation:** host builds print the location and abort. Firmware halts for the debugger
  (and, from P0 rig onwards, records the failure for telemetry).

### 3.8 Casts

- `static_cast` for value conversions.
- **No C-style casts** (`-Wold-style-cast`).
- `reinterpret_cast` only at I/O boundaries (bytes to a file), always with a `NOLINT` and a
  reason.
- `std::bit_cast` for reinterpreting bits, for example float ↔ uint32 in WAV encoding.

### 3.9 Classes and structs

- **`struct`** for plain aggregates with all-public data and no invariants (`audio_block`,
  `engine_config`). **`class`** when there is an invariant to protect. Its data is private, and
  members end in `_`.
- **Rule of zero by default.** When a class must manage something itself, declare all five
  special members explicitly. Delete the ones that make no sense, as `realtime_guard` does.
- **Member order:** public interface first, then private. In each section: types, constants,
  constructors, functions, data.

### 3.10 Enums and switches

- `enum class` with an explicit underlying type when stored or transmitted (`: std::uint8_t`).
- A `switch` over an enum **lists every enumerator and has no `default`**, so `-Wswitch` flags any
  case that's missing when the enum grows.

### 3.11 Concurrency

- **Threads:** the audio thread (the DMA interrupt on device) and at most one control thread.
- Cross-thread data uses `spsc_queue`, or a single atomic whose **memory ordering is commented**
  at each use.

### 3.12 Macros

- **None**, except where the toolchain or a third-party API requires one (doctest's `TEST_CASE`,
  `NDEBUG`).
- Constants are `constexpr`, functions are functions, and contracts use `std::source_location`.

---

## 4. Warnings and static analysis

### 4.1 Compiler warnings

All of these are on, and **all are errors** (`cmake/previously_options.cmake`):

```
-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wdouble-promotion
-Wnull-dereference -Wold-style-cast -Wcast-align -Wnon-virtual-dtor -Woverloaded-virtual
-Wimplicit-fallthrough -Wformat=2 -Werror
```

Third-party headers (libDaisy, the Rack SDK) are included as **system headers**, so these rules
apply to our code only.

### 4.2 clang-tidy and cppcheck

clang-tidy runs the `bugprone`, `clang-analyzer`, `cppcoreguidelines`, `misc`, `modernize`,
`performance`, `portability` and `readability` groups, with every finding treated as an error.
The few disabled checks are listed with their reasons in `.clang-tidy`. Test-specific
relaxations are in `tests/.clang-tidy`, each with a reason.

cppcheck runs over the engine and tools (`tools/scripts/cppcheck.sh`). It cannot parse doctest's
macros, so clang-tidy alone covers the tests.

### 4.3 Suppressions

A suppression is allowed only when the code is right and the tool is wrong for this case. It must:

1. Name the exact check: `// NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)`. A bare
   `// NOLINT` is accepted only where the third-party API dictates the code (Rack's
   `pluginInstance`).
2. Say why, on the same line or the line above.
3. Cover the smallest possible scope: `NOLINTNEXTLINE` over `NOLINTBEGIN`/`NOLINTEND`.

Current deviations are listed in §10.

---

## 5. Naming

C++ Core Guidelines naming (NL.10), the same style the standard library uses:

| Kind | Style | Example |
|---|---|---|
| Namespaces | `snake_case` | `looper`, `previously::vcv` |
| Types (class, struct, enum, alias, concept) | `snake_case` | `spsc_queue`, `audio_block` |
| Functions and methods | `snake_case` | `process`, `to_samples` |
| Variables and parameters | `snake_case` | `block_frames` |
| Private and protected data members | `snake_case_` | `sample_clock_` |
| Public struct members | `snake_case` | `in_left` |
| Enumerators | `snake_case` | `command_type::reset_sample_clock` |
| Constants (`constexpr`) | `snake_case` | `cache_line_bytes` |
| Template parameters | `CamelCase` | `Tag`, `Capacity` |
| Macros (rare) | `UPPER_CASE` | — |

**Names say what something is, in full words:**

- `frame_count`, not `n`. Short names are fine for a loop index whose scope fits on screen.
- Units go in names whenever the type doesn't carry them: `rate_hz`, `x_mm`, `flash_bytes`.
- Booleans read as predicates: `has_matching_lengths()`, `contracts_enabled`.

Where we override a third-party API we must use its names. Rack's `process`, `onSampleRateChange`
and `ModuleWidget` are examples. Those names mark the boundary and are fine.

---

## 6. Files

- One main type or concern per file, named after it: `spsc_queue.hpp` holds `spsc_queue`.
- Headers are `.hpp`, sources `.cpp`. Engine public headers live in
  `engine/include/previously/`.
- **Every file starts with the same header:**

  ```cpp
  // SPDX-License-Identifier: GPL-3.0-or-later
  // Previously — <area>: <one line saying what this file is>.
  //
  // <Optional paragraph: the idea a reader needs before reading the code.>
  ```

- **Then, in order:** `#pragma once` (headers), includes, namespace, contents. Close namespaces
  with a comment: `}  // namespace previously`.
- Internal helpers in a `.cpp` go in an **unnamed namespace**.

---

## 7. Comments and documentation

- **Comment the why, not the what.** The code says what happens. A comment explains the reason,
  the constraint, or the idea behind it. `spsc_queue.hpp` is the model: one paragraph explaining
  how it works, then code that needs no further explanation.
- `///` documents public interfaces: what the function does, which thread may call it, and its
  realtime guarantees.
- When a design decision or a learning note covers a piece of code, link it:
  `See DESIGN.md §3.4`.
- No commented-out code. Version control remembers it.

---

## 8. Tests

- **Every behaviour has a test at the deepest tier it can reach** (DESIGN.md §12).
- **Engine work is test-first:** write the failing test, then the code.
- **Test names are sentences** describing behaviour: `"output is the input delayed by exactly one
  block"`.
- **Deterministic:** fixed seeds, no wall-clock time, no sleeps. A test that fails once fails
  every time.
- Each test checks one behaviour. Use `REQUIRE` for preconditions of the test itself and `CHECK`
  for the behaviour under test.
- Prefer `static_assert` when a property can be proved at compile time (see `test_units.cpp`).

---

## 9. Third-party code

- Everything third-party lives in `vendor/` (git submodules pinned to exact commits) or `.deps/`
  (fetched with a checksum).
- **It is never modified or reformatted.**
- Third-party APIs are used only in **adapters** (`firmware/src`, `rack/src`). The engine never
  includes a third-party header.

---

## 10. Deviation log

Every suppression in the codebase, with its justification:

| Location | Suppressed | Why |
|---|---|---|
| `tools/render/wav.cpp` | `*-reinterpret-cast` | Writing bytes to `std::ofstream` requires `const char*` |
| `tests/test_wav.cpp` | `*-reinterpret-cast` | Same, for test fixtures |
| `engine/src/engine.cpp` | `readability-trivial-switch` | A one-case switch is kept so `-Wswitch` catches new commands |
| `rack/src/plugin.*` | `NOLINT` on `pluginInstance`, `init` | Name, linkage and signature are dictated by the Rack SDK |
| `rack/src/previously_module.cpp` | `*-avoid-non-const-global-variables` | Rack's model registry is a global by design |
| `firmware/src/main.cpp` | `*-avoid-non-const-global-variables` | Hardware and engine are reached from the audio interrupt |
| `firmware/src/contract_violation.cpp` | `hicpp-no-assembler` | Disabling interrupts and breakpoints need inline assembly |
| `tests/.clang-tidy` | Six test-only checks | Each one is listed with its reason in the file |
