# 0005 — Floating point is bit-exact across targets

- **Status:** Accepted
- **Date:** 2026-10-11

## Context

Golden renders are compared byte for byte, and CI runs them on macOS (arm64) and on Linux
(x86-64). Until now rendered audio has been a passthrough, with no arithmetic. Retro capture adds
crossfades and the first recursive filter, the DC blocker.

Here is what compilers do with a crossfade written `tail * (1.0F - w) + pre * w`:

| Compiler | Result |
|---|---|
| AppleClang 21, arm64, `-O0` and `-O2` | Fused into `fmadd`: Clang contracts within an expression by default |
| arm-none-eabi-g++, Cortex-M7, `-std=c++20 -O3` | Fused into `vfma.f32`: GCC contracts by default, even across statements |
| Any compiler, x86-64 baseline | Never fused: the baseline has no FMA instruction |

Over 100 seams of 8 ms of noise, 23.6% of the fused results differ from the unfused ones by one
ulp. A golden blessed on a Mac would fail on Linux. Separately, libm's `sinf` and `cosf` are not
guaranteed to agree across Apple's libm, glibc and newlib.

Subnormals depend on the FPU mode:

| Where the engine runs | Subnormals |
|---|---|
| Rack's engine thread | Flushed to zero: `rack::system::resetFpuFlags()` sets FPCR bit 24 (FZ) on arm64 (from disassembly of `libRack.dylib`) |
| Host tests and the render tool | Kept (IEEE default) |
| Cortex-M7 | Kept: libDaisy enables the FPU but never writes FPSCR |

A 5 Hz one-pole DC blocker fed silence after a full-scale signal goes subnormal after 2.78 s and
stays subnormal (measured: still about 1.4 × 10⁻⁴² after 60 s). From then on, Rack and the host
compute different results, and x86 slows down on every subnormal operation. DESIGN §12's T2
invariant promises "no denormals".

## Decision

- The engine is built with **`-ffp-contract=off`** on every target, so every `*` and `+` rounds on
  its own. Where a fused multiply-add is wanted, `std::fma` asks for it explicitly, and its result
  is exactly defined.
- **Rendered paths call no libm transcendental functions.** Crossfade curves come from our own
  polynomial or a `constexpr` table. `std::sqrt` is correctly rounded everywhere, so it stays.
- **No engine arithmetic produces or consumes a subnormal**, whatever the FPU mode:
  - recursive state whose magnitude falls below 2⁻⁵⁰ (−301 dBFS) is set to exactly zero;
  - audio entering an arithmetic path, and every value passed between processing stages, is
    flushed the same way;
  - within a stage, nothing multiplies more than two such values. A product of two
    accumulations is formed from their square roots, as in the seam correlation's normaliser
    √Σx² · √Σy².

  Why these bounds hold:
  - 2⁻⁵⁰ is a power of two, so the comparison is exact everywhere.
  - It sits far above the subnormal range (2⁻¹²⁶) and far below the 24-bit floor (−144 dBFS).
  - A product of two values at least that large is at least 2⁻¹⁰⁰. Sums and differences of such
    products are normal or exactly zero, because their ulps are at least 2⁻¹²³.
  - A product of two products is not safe. At the floor, two 50 ms sums of squares are
    1.9 × 10⁻²⁷ each, and their product, 3.6 × 10⁻⁵⁴, falls below even the smallest subnormal
    (1.4 × 10⁻⁴⁵). Hence the square roots.

## Consequences

- Golden renders match across the CI platforms, and device output can match the host.
- The engine's results no longer depend on whether the host sets flush-to-zero, so Rack, the
  tests and the firmware compute the same thing.
- The M7 loses automatic FMA in DSP loops. The cost is measured in T5, which is deferred with the
  hardware work. If it matters, explicit `std::fma` recovers it where results stay identical.
- Tests guard each rule (`test_dc_blocker.cpp`; the crossfade gets the same when it lands):
  - a test pins the exact bits of one DC-blocker output, so a build that drops the flag fails on
    arm64. Its input, 0.1 and then silence, cancels almost exactly, so the fused result is
    205 ulps away, not one;
  - the DC blocker runs through a minute of silence after full scale with no subnormal in its
    output, and settles to exactly zero;
  - the same input, with subnormal and near-floor values added, gives identical bits with
    flush-to-zero on (as Rack sets it) and off.
