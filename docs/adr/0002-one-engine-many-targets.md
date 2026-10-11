# 0002 — One platform-free engine shared by tests, tools, Rack and firmware

- **Status:** Accepted
- **Date:** 2026-10-11

## Context

There is no usable Daisy emulator: QEMU does not model the STM32H750, and Renode does not model
its audio path. Sample-accurate looping logic needs fast, repeatable tests, and the performance
workflow needs hands-on playing long before hardware exists.

## Decision

- **`engine/` is pure C++ with no platform headers.** It receives audio blocks, clock events and
  commands, and fills output buffers. Each target is a thin adapter around it:

  | Target | Adapter |
  |---|---|
  | Host tests | doctest |
  | Render tool | WAV files |
  | VCV Rack plugin | `rack/src` |
  | Firmware | `firmware/src` with libDaisy |

- **The Rack plugin links the engine as a static library built by CMake** (preset `rack`) with the
  project's flags. Rack's own build adds `-funsafe-math-optimizations`, which would make the
  plugin's DSP differ from the tests and the firmware.
- **The Rack adapter re-blocks** Rack's per-sample calls into 32-frame blocks. Block boundaries and
  latency (one block) then match the device.
- **Host builds of the engine also use `-fno-exceptions -fno-rtti`**, so the host behaves like the
  device.

## Consequences

- Most behaviour can be proven on the host in milliseconds, under sanitizers.
- The engine must never include a platform header. Review and the directory layout enforce this.
- **What can only be verified on hardware** is listed in DESIGN.md §12 (tiers T5–T7):
  - cycle budgets
  - SDRAM bandwidth
  - codec latency
  - interrupt timestamp precision
- **Headless Rack smoke tests stall on macOS** while Core Audio initializes (microphone permission
  cannot be granted headlessly). The adapter's logic is therefore unit-tested on the host
  (`tests/test_block_adapter.cpp`). Loading the plugin in Rack is a manual or Linux-only check.
