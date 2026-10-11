# 0005 — Floating point is bit-exact across targets

- **Status:** Accepted
- **Date:** 2026-10-11

## Context

Golden renders are compared byte for byte, and CI runs them on macOS (arm64) and on Linux
(x86-64). Until now rendered audio has been a passthrough, with no arithmetic. Retro capture adds
crossfades.

Here is what compilers do with a crossfade written `tail * (1.0F - w) + pre * w`:

| Compiler | Result |
|---|---|
| AppleClang 21, arm64, `-O0` and `-O2` | Fused into `fmadd`: Clang contracts within an expression by default |
| arm-none-eabi-g++, Cortex-M7, `-std=c++20 -O3` | Fused into `vfma.f32`: GCC contracts by default, even across statements |
| Any compiler, x86-64 baseline | Never fused: the baseline has no FMA instruction |

Over 100 seams of 8 ms of noise, 23.6% of the fused results differ from the unfused ones by one
ulp. A golden blessed on a Mac would fail on Linux. Separately, libm's `sinf` and `cosf` are not
guaranteed to agree across Apple's libm, glibc and newlib.

## Decision

- The engine is built with **`-ffp-contract=off`** on every target, so every `*` and `+` rounds on
  its own. Where a fused multiply-add is wanted, `std::fma` asks for it explicitly, and its result
  is exactly defined.
- **Rendered paths call no libm transcendental functions.** Crossfade curves come from our own
  polynomial or a `constexpr` table. `std::sqrt` is correctly rounded everywhere, so it stays.

## Consequences

- Golden renders match across the CI platforms, and device output can match the host.
- The M7 loses automatic FMA in DSP loops. The cost is measured in T5, which is deferred with the
  hardware work. If it matters, explicit `std::fma` recovers it where results stay identical.
- A test pins the exact bits of one crossfade sample, so a build that drops the flag fails on
  arm64.
