# C++ for C# developers — through this codebase

A map from what you know in C# to what you'll read here. Every example points at real code in
this repo. Read it alongside `engine/include/previously/`.

---

## 1. The big shifts

| In C# | In this C++ codebase |
|---|---|
| The garbage collector owns memory | **You** decide where every byte lives and for how long. In the engine, memory is set up once and never freed. |
| Classes are references; structs are values | **Everything is a value by default.** `engine e2 = e1;` would *copy* the whole engine (ours forbids that; see §6). |
| One `.cs` file per type, compiled together | **Headers (`.hpp`) declare and sources (`.cpp`) define.** Each `.cpp` compiles alone; `#include` pastes headers in textually. |
| Exceptions for errors | **No exceptions.** Return values for expected failures, contracts for bugs (CODING_STANDARD §3.2, §3.7). |
| JIT, runtime type info, reflection | **None.** What you write compiles to machine code. Templates are resolved entirely at compile time. |

---

## 2. Values, references, pointers

```cpp
void process(const audio_block& block) noexcept;   // engine.hpp
```

- **`audio_block&`** is a *reference*: an alias for the caller's object with no copy. Similar to
  C#'s `ref` or `in` parameters, but it can never be null and never be re-pointed.
- **`const audio_block&`** is like C# `in`: you get read-only access to the caller's object,
  without copying it.
- **`float*`** is a *pointer*: an address that can be null and can move. Modern C++ avoids raw
  pointers in interfaces. You'll see them only at the libDaisy boundary (`firmware/src/main.cpp`),
  where they're immediately wrapped in `std::span`.

**Rule of thumb in this codebase:**

| What you pass | How |
|---|---|
| Small values (`samples`, `float`, `sample_rate`) | By value |
| Bigger read-only objects | By `const&` |
| An object the callee mutates | By `&` |

---

## 3. `std::span<T>` is `Span<T>`

You already know this one. `std::span<float>` is a non-owning view over contiguous memory:
pointer plus length, just like C#'s `Span<T>`.

```cpp
struct audio_block {                    // audio_block.hpp
    std::span<const float> in_left;     // ReadOnlySpan<float>
    std::span<float>       out_left;    // Span<float>
    ...
};
```

`std::span<const float>` corresponds to `ReadOnlySpan<float>`. `.subspan(start, count)`
corresponds to `.Slice(start, count)` (see the tests and the render tool).

---

## 4. `const`, `constexpr`, `noexcept`, `[[nodiscard]]`

| C++ | Closest C# | Meaning here |
|---|---|---|
| `const` local | `readonly` / immutable local | The value will not change after initialization. The default in this codebase. |
| `const` member function `value() const` | `readonly` struct member | Promises not to modify the object. |
| `constexpr` | `const` (but far more powerful) | Can be computed at compile time. Whole functions can be `constexpr`; `units.hpp` arithmetic runs inside `static_assert`. |
| `noexcept` | — | Promises never to throw. Most engine code is `noexcept`. |
| `[[nodiscard]]` | `[MustUseReturnValue]` analyzers | The compiler warns if you ignore the result. |

---

## 5. Strong types: generics vs templates

`units.hpp` defines `quantity<Tag, Rep>`, a template much like a C# generic `Quantity<TTag, TRep>`:

```cpp
using samples = quantity<struct samples_tag, std::int64_t>;
using seconds = quantity<struct seconds_tag, float>;
```

**Key differences from C# generics:**

- **Templates are stamped out per type at compile time.** `quantity<samples_tag, int64_t>` and
  `quantity<seconds_tag, float>` become two unrelated types with zero runtime cost. A `samples` is
  exactly an `int64_t` in memory.
- **The `Tag` is never used at runtime.** It only makes the types distinct. `struct samples_tag`
  is declared inline and never defined, which is fine because nothing needs its size.
- **The result is that `samples + seconds` does not compile.** `test_units.cpp` proves this with
  `static_assert(!std::is_invocable_v<std::plus<>, samples, seconds>)`.

---

## 6. Object lifetime: RAII is `using` / `IDisposable`, but automatic

In C#, `using var x = ...;` disposes `x` at the end of the scope. In C++ *every* object works this
way: its **destructor runs automatically** when it goes out of scope, deterministically, with no
GC. This is RAII (Resource Acquisition Is Initialization).

```cpp
{
    const test::realtime_guard guard;   // constructor: start watching for allocations
    fixture.process_in_blocks(...);
}                                       // destructor: stop watching. Always, even on early return.
```

**The "special members"** control how an object is copied and moved:

| C++ special member | Roughly in C# |
|---|---|
| Copy constructor / assignment | Struct copy semantics |
| Move constructor / assignment | Transferring ownership (no C# equivalent; think "steal the guts") |
| Destructor | `Dispose()`, called automatically |

`realtime_guard` deletes all copy and move operations (`= delete`), because copying a guard would
make no sense. Most of our types need none of this ("rule of zero").

---

## 7. Headers and the One Definition Rule

- **A header declares** what exists (`engine.hpp`: the class shape and method signatures). **A
  source file defines** it (`engine.cpp`: the method bodies).
- **`#include` is textual paste.** `#pragma once` stops a header being pasted twice into one file.
- **Functions defined in a header** must be `inline`, a template, or `constexpr`. Otherwise every
  `.cpp` that includes the header would define them again, and the linker reports duplicates (the
  One Definition Rule). That's why `units.hpp` and `spsc_queue.hpp` are header-only: everything in
  them is a template or `constexpr`.
- **The unnamed namespace** (`namespace { ... }`) in a `.cpp` is like C# `file`-scoped types:
  invisible outside that file.

---

## 8. Initialization

```cpp
engine previously_engine{engine_config{
    .rate             = sample_rate{48'000U},
    .max_block_frames = 48,
}};
```

- **Braces `{}` are "uniform initialization".** They also refuse narrowing conversions, such as
  quietly turning a `double` into a `float`.
- **`.rate = ...`** is a *designated initializer*, like a C# object initializer
  `new EngineConfig { Rate = ... }`. Members must appear in declaration order.
- **`48'000U`**: the `'` is a digit separator (C#'s `48_000`), and `U` makes it unsigned.

---

## 9. `std::optional<T>` is `T?` (`Nullable<T>`)

`spsc_queue::try_pop()` returns `std::optional<T>`. Check it with `.has_value()` or
`if (auto item = ...)`, then read it with `*item`. Same idea as `int?` with `.HasValue` and
`.Value`.

---

## 10. Concurrency without locks

`spsc_queue.hpp` is the one place where two threads touch the same memory. `std::atomic` with
explicit *memory ordering* plays the role of C#'s `Volatile.Read` / `Volatile.Write`:

- **Release store** ("publish"): everything written before it becomes visible to a thread that
  later sees the stored value.
- **Acquire load** ("subscribe"): pairs with the release store, so once you see the new index you
  are guaranteed to see the item it covers.

C# hides most of this behind `lock` and `ConcurrentQueue`. On a microcontroller audio interrupt
we can't take a lock, so the queue is built from these primitives directly. The comment block at
the top of the file walks through the reasoning.
