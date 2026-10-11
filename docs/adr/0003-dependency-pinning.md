# 0003 — Pin every dependency and tool version

- **Status:** Accepted
- **Date:** 2026-10-11

## Context

Golden tests compare output byte for byte, and formatting is checked byte for byte. A drifting
dependency or tool version shows up as spurious failures, or worse, as silent behaviour changes.
The author's local checkouts in `~/Source` were also out of date: libDaisy there predates Seed3
support.

## Decision

- **Git submodules in `vendor/`, pinned to exact commits:**

  | Submodule | Version | Why this version |
  |---|---|---|
  | libDaisy | v9.0.0 | First release that detects Seed3 (TAC5242) |
  | DaisySP | Pinned commit | |
  | doctest | v2.5.0 | |

  libDaisy's own nested submodules (ST HAL, CMSIS 5, CMSIS-DSP v1.14.4, USB) come with it.
- **CMSIS-DSP:** we use **libDaisy's copy** rather than vendoring a second version, so device and
  host can never disagree about it.
- **Rack SDK:** 2.6.6, downloaded by `tools/scripts/fetch-rack-sdk.sh` and verified against a
  SHA-256 checksum for each platform. Locally, `RACK_DIR` may point at a Rack source tree instead.
- **LLVM tools** (clang-format, clang-tidy): major version **23**. The scripts refuse to run with
  any other version.
- **Arm GNU toolchain:** 15.2.Rel1, pinned in CI.

## Consequences

- **Upgrading a dependency is a deliberate change:** bump it, run the full gate, and re-bless
  goldens if output changes. That gets its own commit and, where it matters, its own ADR.
- **The pinned LLVM version will drift on Homebrew.** When Homebrew moves to LLVM 24, the scripts
  fail loudly until the pin is updated on purpose.
