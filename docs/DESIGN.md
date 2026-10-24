# Previously — Design

> A stereo, end-of-chain Eurorack looper built on the Daisy Seed3. It captures clock-locked loops
> with sample accuracy and crossfades between the live input and those loops, which supports
> live transitions, layering and performance effects.

| | |
|---|---|
| **Status** | v0.3 — retro capture designed (§2.3, §3.4–3.6, §5); P1 in progress |
| **Platform** | Daisy Seed3 (STM32H750, 480 MHz Cortex-M7, 64 MB SDRAM, TAC5242 codec) |
| **Dev hardware** | Daisy Pod (Seed 1.x and Seed3), Expert Sleepers ES-9 test rig |
| **Language** | C++20, embedded subset (see [CODING_STANDARD.md](CODING_STANDARD.md)) |
| **License** | GPLv3 |

---

## Contents

1. [Goals and principles](#1-goals-and-principles)
2. [Platform budget](#2-platform-budget)
3. [Clock and sync](#3-clock-and-sync)
4. [Tempo: tracking and time stretching](#4-tempo-tracking-and-time-stretching)
5. [Click-free loop points](#5-click-free-loop-points)
6. [Signal flow and mixing](#6-signal-flow-and-mixing)
7. [Loop slots and performance operations](#7-loop-slots-and-performance-operations)
8. [Effects](#8-effects)
9. [Controls and UI](#9-controls-and-ui)
10. [Software architecture](#10-software-architecture)
11. [Development control surface](#11-development-control-surface)
12. [Testing strategy](#12-testing-strategy)
13. [Hardware plan](#13-hardware-plan)
14. [Roadmap](#14-roadmap)
15. [Open questions](#15-open-questions)
16. [References](#16-references)

---

## 1. Goals and principles

### What it is

The module sits at the **end of a Eurorack chain**, so everything in the patch passes through it.
It continuously listens. At any moment the performer can **capture the last N bars**, perfectly
aligned to the clock, and **crossfade** from the live signal to that loop. While the loop holds the
groove, the patch underneath can be torn down and rebuilt. A second capture and a mix between loops
then makes the transition.

### Principles

1. **Hard realtime.** The audio path never allocates memory, never takes a lock, and never runs
   for an unbounded time.
2. **Sample-accurate.** Loop lengths are whole numbers of samples derived from the clock. Capture
   and playback line up with clock edges *at the jacks*, after latency compensation.
3. **Quantized by default.** Every musical action (capture, launch, fade, stutter) lands on the
   grid. Free timing is an explicit opt-in.
4. **Deep, but simple controls.** Each core move has one dedicated control. Depth sits behind a
   shift layer and is never needed mid-performance.
5. **Nothing destructive without undo.** Capture, overdub and clear can all be reversed.
6. **No clicks, ever.** Every discontinuity (seam, jump, mute, punch) goes through a crossfade or
   a ramp.
7. **Everything is testable.** Every action is reachable programmatically, and every claim in this
   document has an automated test, from host unit tests down to hardware-in-the-loop.

---

## 2. Platform budget

### 2.1 Hardware summary (from the Seed3 datasheet v2.1)

| Resource | Seed3 | Notes |
|---|---|---|
| MCU | STM32H750, Cortex-M7 @ 480 MHz, single-precision FPU | Double precision is emulated and slow, so it is banned on device |
| SDRAM | 64 MB | Loop and FX memory |
| Codec | TAC5242, stereo in/out, up to 192 kHz / 32-bit | Seed 1.x uses a different codec (PCM3060 on rev7) |
| Audio level | ±1.8 V max in (3.6 Vpp), 0 dBFS ≈ 1 Vrms out | Eurorack scaling circuits in §13 |
| ADC | 12 channels, 16-bit | Pots and CV |
| DAC | 2 × 12-bit (D22, D23) | CV or clock out |
| GPIO | 31, mostly 5 V tolerant (except D17, D18, D21, D22, D23) | Gates, buttons, LEDs |
| Storage | SDMMC, 4-bit | Save and load loops |
| USB | USB-C (FS, device) + external HS pins D29/D30 (OTG: device or host) | MIDI, dev UI, controllers |
| Spare audio bus | SAI2 on D24–D28 | A second codec for a cue output |

### 2.2 CPU

At 48 kHz the M7 has **10,000 cycles per stereo frame**. We budget like this:

| Block | Estimate (cycles/frame) | Notes |
|---|---|---|
| Framework, I/O, DC blockers, smoothing | ~200 | |
| Retro recording | ~30 | Sequential SDRAM writes |
| Crossfade shapes (P1, planned) | ~10 averaged at a 1/32 stutter; a burst of about 30,000 per crossfade | A correlation over 50 ms of both streams, read from SDRAM at each seam, move or seek. A 1/32 stutter at 120 BPM makes one every 62 ms. The burst is what the 70% rule meets. At the 32-frame block, one crossfade takes 13% of a callback's budget, and two in one block take 27% (a replacement seek and a bar-locked move on the same line). At 16 frames that is 27% and 54%. Measured in T5 at the block size that runs |
| Loop slot, Tape mode (×4) | ~50 each | Cubic interpolation |
| Loop slot, WSOLA stretch | ~200–500 each | Decimated correlation search |
| Loop slot, HQ phase vocoder | ~800–2,000 each | CMSIS-DSP real FFT |
| Mixer and crossfader | ~50 | |
| DJ filter (SVF) | ~40 | |
| Stutter / reverse / varispeed | ~50–100 | |
| Tempo-synced delay | ~100 | |
| Granular (16–32 grains) | ~1,000–2,500 | SDRAM random reads dominate |
| FFTs (phase vocoder) | — | CMSIS-DSP, the copy pinned inside libDaisy (one version for device and host) |
| Reverb (FDN) | ~400–800 | |

These are **estimates**. Real numbers come from on-device DWT cycle benchmarks (§12, tier T5),
which have budgets enforced in CI. The audio callback must never use more than **70%** of its
time slice, to leave headroom for interrupt jitter and UI work.

### 2.3 Memory

48 kHz × 2 channels × 4 bytes (float32) = **384,000 bytes per second**. 64 MiB therefore holds
about **174 s** of stereo audio in total.

**Paged tape** (ADR 0004). All loop audio lives in one pool of fixed-size **pages**. Audio is
never copied; loops hold pages. The pool and the recorder are built (`page_pool`, `recorder`);
loops and captures come with capture (§3.4).

- **A page** is the smallest power-of-two number of stereo frames that lasts at least as long as
  2¹⁵ frames at 48 kHz (0.68267 s):
  - 2¹⁵ frames (256 KiB of float32) at 44.1 and 48 kHz;
  - 2¹⁶ at 88.2 and 96 kHz;
  - 2¹⁷ at 176.4 and 192 kHz.

  `page_frames_for()` computes it in integers, as the smallest power of two P with
  P × 48,000 ≥ 2¹⁵ × rate, so no rounding can move a boundary. Every page count below is therefore
  its 48 kHz value or less, at any sample rate; only the bytes per page change. A position inside
  a page is a shift and a mask.
- **A frame** is stored as its two channels side by side, so one read fetches both.
- **The recorder** writes the input into pages without pause, through the DC blocker (§5.5): the
  frame the engine receives at sample n is at position n. It takes a free page whenever the
  current one fills, and gives a page back once it is older than the history and no loop holds it.
  - **Page n and page n − 46 share a slot** in the recorder's ring, so the old page goes back
    before the new one is taken. The recorder never holds more than the history.
  - **A page is free whenever one is needed.** The pool has at least the history's pages, the
    recorder is today their only holder, and captures will be admitted only while that stays
    true. Should the pool be empty all the same, that page goes unrecorded and recording resumes
    on the next. The history has a hole; no loop's audio is touched. The DC blocker runs on
    through the hole, so what is recorded after it is unchanged.
- **A loop** holds the ids of the pages its window touches, with a reference count on each. When
  the loop goes, its pages go back to the pool.
  - Each page counts its holders: the history is one, and each capture or loop another.
  - Free pages wait on a stack, so taking and returning a page are O(1).

| SDRAM | Size | Role |
|---|---|---|
| 210 pages | 52.5 MiB | Audio: the history, plus loops |
| Remaining | 11.5 MiB | FX (delay up to 10 s stereo = 3.8 MB), stretch scratch |

**History.** The history is the newest 46 pages. Once the recorder has filled them, 45 are
complete: at least **30.7 s**. That is exact at 48, 96 and 192 kHz; at 44.1, 88.2 and 176.4 kHz
it is 33.4 s. A capture of N bars reaches back N bars from its end line, and the end line
can be up to a bar before the press (§3.4). The history therefore covers a capture whenever
N + 1 bars plus the pre-roll (50 ms, §5.3) fit in 30.7 s. In 4/4:

| Bars | Covered at any tempo of at least |
|---|---|
| 1 | 16 BPM |
| 2 | 24 BPM |
| 4 | 40 BPM |
| 8 | 71 BPM |

**Loops.** The 164 pages beyond the history (112 s) hold loops:

- **A capture holds at most 47 pages, in flight or final.** That covers four things:
  - its window;
  - its 50 ms margins;
  - a reserve of half a period after the end line for its repair, and before the start line
    only if that line is not yet final at the press (§3.4; never at 4 PPQN);
  - up to two partial pages.

  That is a span of at most 46 pages, 31.4 s at 48 kHz. A capture that would need more is
  shortened. The limit binds only at slow tempos: below 7.91, 15.57 and 30.91 BPM for 1, 2 and
  4 bars at 4 PPQN, and never for 8 bars. Without it, a 1-bar capture at 7.4 BPM would hold 51
  pages.
- A capture is admitted only if every loop's pages plus the history still fit. The recorder
  therefore never runs out, and the history is never interrupted, however many loops exist.
- P1 needs at most 3 × 47 + 46 = 187 pages: two slots, one of them being replaced, with one
  capture in flight at a time (§3.4). P2's four slots and undo need a policy (§15).
- Because pages scale with the sample rate, these counts hold on every target whose tape holds
  210 pages. A host sizes the pool in bytes for its rate: at 96 kHz each page is 512 KiB. Rack
  sizes its tape once, for 192 kHz, and holds fewer pages above that rate (below).
- **Who gives the engine a tape.** The tests, and Rack. The firmware passes none, and records
  nothing, until the hardware work.
  - **Rack** allocates one tape when the module is created: 210 pages at 192 kHz, 210 MiB
    (`rack/src/tape.hpp`). It is not written when it is allocated, so a fresh tape costs address
    space rather than RAM until the recorder writes it. Measured on macOS: allocating it takes
    under 10 µs and adds no resident memory, and the 48 kHz history written adds 11.5 MiB, then
    stops: the recorder gives a page back before it takes one, so the same 46 pages are written
    over.
  - **Resident memory follows the highest rate the module has run at,** not the current one.
    Each rebuilt engine records from page 0 again, over pages an earlier engine wrote, and
    nothing gives memory back. The history is 11.5 MiB at 48 kHz, 46 MiB at 192 kHz and 184 MiB
    at 768 kHz. Measured on macOS: 48 kHz, then 192 kHz, then 48 kHz again left 46 MiB resident.
  - **A deleted module's written pages stay resident on macOS,** and the next tape of that size
    takes them over: libmalloc keeps a freed block this large in its cache and hands it to the
    next allocation of that size. Not a leak, since the block is reused, but only the first tape
    starts unwritten. `MallocLargeCache=0` turns the cache off.
  - **A rate change** can come on Rack's audio thread (§15, question 10). The engine is rebuilt
    over the same memory, allocating nothing, and the history starts again from nothing. Once
    loops exist, a rate change drops them too, since the engine that holds their pages is
    rebuilt. What a loop should do then is for capture to decide.
  - **Above 192 kHz** the memory holds fewer, longer pages. The history's 46 fit up to 768 kHz,
    Rack's highest rate, but the 164 pages beyond it that P1's 187 rely on do not:

    | Rate | Pool pages | Beyond the history | What fits |
    |---|---|---|---|
    | Up to 192 kHz | 210 | 164 | P1's 187 |
    | 352.8, 384 kHz | 105 | 59 | One loop of any length. A second only while the two total 59 pages: two 8-bar loops from 101.37 BPM at 384 kHz and 93.09 at 352.8 kHz. Replacing a full-length loop with another is refused: 94 > 59 |
    | 705.6, 768 kHz | 52 | 6 | Loops totalling 6 pages, which hold any span up to 5 pages: 3.41 s at 768 kHz, 3.72 s at 705.6 kHz |

    At 705.6 and 768 kHz, by §3.4's reserve rule (half a period after the end line, 50 ms
    margins), one capture fits from these tempos, in BPM at 4 PPQN in 4/4:

    | Bars | 768 kHz | 705.6 kHz |
    |---|---|---|
    | 1 | 74.70 | 68.46 |
    | 2 | 147.13 | 134.85 |
    | 4 | 292.0 | 267.6 |
    | 8 | 581.7 | 533.2 |

    The 192 kHz sizing is kept because nobody performs at 768 kHz. A tape sized for it would be
    840 MiB of address space per instance, the same resident memory at a given rate, and 840 MiB
    of commit on Windows (below). Capture owes a test here: with 52 pages, a capture that needs 7
    is refused and the history stays whole.
  - **Not measured:** Windows and Linux. The Windows heap is expected to serve a block this large
    with `VirtualAlloc(MEM_COMMIT)`, so all 210 MiB would count against the commit limit at once
    (Private Bytes) while staying out of the working set until written; many instances would run
    into that first. Linux with its default overcommit is expected to behave like macOS.
- The engine never touches the pool before its first `process()`. On the Daisy, SDRAM is
  configured inside `hardware.Init()`, after static constructors have run. A test fills a tape
  with a pattern, constructs the engine, and checks the pattern is intact.
- The counts are `static_assert`s: 3 × 47 + 46 ≤ 210 in `engine.hpp`, and the page sizes in the
  tests.

**Precision rule:** absolute time is **never stored in a float**. A float has a 24-bit mantissa,
so it stops resolving individual samples after 2²⁴ samples (about 350 s at 48 kHz). Positions are
`int64_t` sample counts, and fractional positions are fixed-point (Q32.32). §3.6 covers this.

**Flash.** The STM32H750 has only **128 KB of internal flash**, and the firmware is filling it.
T0 counts it as the linker does: from the start of flash to the end of the furthest load image,
alignment padding included. The region's origin and length are read from the linker script the
build links with, so the budget follows the layout.

| Stage | Flash used | Mostly |
|---|---|---|
| P0 passthrough | 61% | libDaisy and newlib |
| With the clock wiring and hiccup handling (P1) | 74% (97,512 bytes) | About 11 KB of engine and tracker code, unoptimized (below) |
| With the tracker's edge times (P1) | 75% (98,476 bytes) | |
| With the tape, recorder and DC blocker (P1) | 83% (108,832 bytes) | 10,356 bytes more unoptimized; 1,632 at `-O3` with LTO |

The figures are from a local build. The image embeds source paths (the engine's contract
locations, and libstdc++'s header names), so it grows with the length of the repository's and the
toolchain's paths: CI builds the same commit 340 bytes larger.

**The engine is built without optimization in the firmware** (found in PR #10, issue #11).
libDaisy's `DaisyProject.cmake` adds `-O3` and `-flto` with `add_compile_options`, which reaches
only targets defined after it in the firmware's directory. The engine is added before it, and the
toolchain file empties `CMAKE_CXX_FLAGS_RELEASE`, so the engine compiles at GCC's default, `-O0`.
- **Flash.** Every figure in the table above includes this. With the engine at `-O3` and LTO, a
  scratch build takes 73,608 bytes (56%) before the tape and 75,240 bytes (57%) with it.
- **Speed.** The engine's speed on the device would not be representative until it is fixed.
- **Stack.** PR #7 found that LTO's stack report leaves out the engine's functions; this is why.
- **Order of remedies.** Fixing this comes before `-Os` in the steps below.

**Hardware work is deferred** (decided after PR #2): development stays host- and Rack-first. The
build fails if the flash budget is exceeded (T0). When it does:

1. Build the firmware for size (`-Os`). This is hardware-free, typically recovers 20–30%, and the
   speed cost is measured and reported.
2. Then move to libDaisy's **bootloader** layout: the program lives in the 8 MB QSPI flash and
   runs from SRAM.

**Internal SRAM.** Fixed engine state lives in internal SRAM, not SDRAM. The variables
(`.data` and `.bss`) move when the firmware moves to the bootloader layout:

| Region | Current layout | QSPI bootloader layout |
|---|---|---|
| AXI SRAM, 512 KB | The variables | The program |
| DTCM, 128 KB | The stack | The variables and the stack |

**The RAM budget (T0).** The build fails if the variables outgrow **112 KiB**: DTCM less 16 KiB
for the stack. Within it, the move to the bootloader layout stays open. They are counted like
flash: `.data`, `.got`, `.got.plt` and `.bss` from the first to the end of the furthest, alignment
padding included, plus `.dtcmram_bss`.

- **Today** they take 81,420 bytes (70%). The largest are libDaisy's `DaisyPod` (37,856 bytes) and
  the engine (26,816 bytes: 24,768 of them the clock history, and 808 the pool's and the
  recorder's bookkeeping).
- **The stack reserve** is about 9 times the measured need. GCC's `-fstack-usage` report, before
  LTO, summed along the deepest call chains, gives about 1.8 KB once audio runs and about 1 KB at
  start-up.

Budget items so far:

| Item | Size | Note |
|---|---|---|
| Clock tracker history | 16 bytes and 1 bit × capacity | The owner provides it (`clock_history`): each pulse's filtered time, its raw edge and a fill-in flag. 256 entries (4 KB and 32 bytes) cover 16 bars at 4 PPQN. The engine's 1,536 (24 KB and 192 bytes) cover 16 bars at 24 PPQN. |
| Page pool | 3 bytes per page: 630 bytes | A free stack of 2-byte page ids and a 1-byte holder count, for each of the 210 pages |
| Recorder | 2 bytes per history page: 92 bytes | The history's page ids, beside two DC blockers |
| Loop page tables (P1, planned) | 2 bytes per page | At most 47 pages per loop |

The firmware owner places these buffers explicitly.

**SDRAM access.** Sequential access (recording, Tape playback) is cheap. Random access (granular,
WSOLA search) costs cache misses. Hot read windows can be staged into internal SRAM, measured
in T5.

### 2.4 Sample rate and block size

- **48 kHz** at runtime, with sample rate as a parameter everywhere so 96 kHz can be benchmarked
  later.
- **Block size 32** (0.67 ms) is the starting point. Engine behaviour must be identical at any
  block size; tests run with sizes 1, 16, 32 and 48.

---

## 3. Clock and sync

This is the core of the module's accuracy.

### 3.1 Time base

- One global `int64_t` **sample counter**, advanced by the audio callback. At 48 kHz it overflows
  after about six million years.
- Every event (clock edge, MIDI byte, button press, CV crossing) is stamped in **samples**, with a
  sub-sample fraction where the source allows it.

### 3.2 Timestamping clock sources

| Source | Mechanism | Precision |
|---|---|---|
| Gate clock (Eurorack) | EXTI interrupt on the rising edge. The ISR reads the DWT cycle counter. The audio DMA interrupt also records `(cycle_count, sample_index)` at each block boundary. Edge time = block sample + (Δcycles ÷ 10,000). | Sub-sample (interrupt latency is about 0.1 µs ≈ 0.005 samples) |
| MIDI clock (0xF8) | Same, stamped in the UART RX interrupt | Limited by MIDI wire timing (≈ 320 µs per byte), so smoothed |
| Tap tempo | Button debounce time, filtered | Coarse, assisted by the tracker |
| Internal | Generated from the current tempo | Exact |
| VCV Rack | Per-sample threshold crossing, with linear interpolation for the fractional part | Sub-sample |

On the Pod, the gate clock needs a jack and the BJT input circuit (datasheet Fig 2.4) wired to a
header pin. The proposal is **D9 (PB4, EXTI4) for clock and D10 (PB5, EXTI5) for reset**. Both
are 5 V tolerant.

### 3.3 Clock tracker

`clock_tracker` (P1, implemented) turns noisy edge timestamps into a tempo and a pulse-indexed
grid. Full explanation, measurements and the mutation-testing record:
[docs/learning/clock-tracking.md](learning/clock-tracking.md).

- **Time:** `sample_time`, Q47.16 fixed point (64-bit ticks, 1/65,536 sample, a range of 93
  years). Absolute time never lives in a float.
- **Filter:** an α-β tracker (equivalent to a second-order DLL), with β derived from α for
  critical damping and least-squares gains during acquisition. Tempo is exact after 2 pulses.
- **Grid:**
  - Integer pulse indices; bars are `index / (pulses_per_beat × beats_per_bar)`. The **meter**
    (beats per bar, default 4) is configurable.
  - Bars use floor division, so pulses before a reset fall in negative bars.
  - A history of past pulses, in storage provided by the owner (`clock_history`), answers
    questions about the past; extrapolation answers questions about the future.
- **Two times per pulse.** The history keeps each pulse's filtered time and its raw edge.
  - **The filtered times serve the grid:** predictions, clock out and the existing queries.
  - **The edges serve capture.** A filtered time is built from earlier pulses only. Capture
    estimates where a bar line was from the pulses on both sides of it, which needs the edges
    themselves (§3.4).
  - **A fill-in** has no real edge. Its edge time is the slot it was filled in at, and a flag
    (one bit per entry) marks it.
  - `edge(index)` reads them, labelled like every other query. It never extrapolates: a pulse has
    an edge once it is recorded, which for a fill-in is when its slot closes, half a period
    after its time.
- **PPQN:** configurable. The gate preset is 4 (16th notes); 1, 2 and 24 are planned as UI
  choices. MIDI is 24.
- **Presets, measured:**

  | Preset | α | Ramp lag (10% over 8 bars) | Jitter |
  |---|---|---|---|
  | Gate | 0.9 | 0.6 samples | ×0.92 RMS (not amplified) |
  | MIDI | 0.2 | 4 samples | 27.7 → 10.8 samples RMS on a jittery ramp |

  An α-β tracker lags slightly behind steady tempo ramps; lower α smooths more but lags more.
- **Robustness. Rule: tempo is only ever measured between two real edges:**
  - a pulse within 4 ms of the previous one is a double trigger
  - an interval over 4 s while acquiring discards a stray first edge
  - an error over ¼ period is an outlier: recorded where it happened, tempo kept; the next pulse
    decides. Landing back on the grid means a hiccup (nothing changes); an interval matching
    the current period means a phase shift (re-anchor the phase); an interval matching the
    outlier's means a tempo change (re-acquire). A tempo resting on one interval re-acquires at
    once.
  - 2.5 silent periods switch to freewheeling: pulses are filled in at the last tempo
  - a returning clock re-anchors the phase only; the next real pulse measures the tempo
  - two equal long gaps mean a slowed clock: the fill-ins are retired. They have overwritten the
    oldest pulses, so the history is shorter until new pulses overwrite them in turn. When the
    fill-ins of both gaps and the pulse between them outnumber the history, the slots a
    retirement needs are gone, and the clock reads as stopped and restarted: the fill-ins stay.
    On the engine's 1,536 pulses that happens from 768.5× (gaps of 96 s at 120 BPM and
    4 PPQN), far slower than any tempo.
- **Reset** (gate, or MIDI Start): the pulse within 2 ms of it, delivered in either order,
  becomes bar 0. A late-delivered reset is resolved from the history.
- **Timing contract:** pulses are delivered in time order, each block's pulses before
  `advance(block_end)`.
- **Clock loss** is visible: the beat LED shows the freewheel state (§9.1).
- **Not yet implemented (P1 continues):**
  - MIDI Start, Stop, Continue and Song Position Pointer
  - the **internal** tempo source (§3.2)
  - tap tempo
  - **source priority:** gate, then MIDI, then the tempo set by the first loop (§4.1), then tap
    tempo. Patching a clock takes over smoothly.
  - PPQN selection from the UI

### 3.4 Retrospective capture (the headline feature)

*Designed for P1; implementation in progress (§14).*

The input is **always being recorded** onto the paged tape (§2.3). A capture copies nothing and
never needs arming. It chooses a window of whole bars, pins the window's pages, and plays them.

- **One capture is in flight at a time**, engine-wide, from its press until it becomes final.
- **The capture PRs are single-slot:** one loop plays at a time. P1's second slot arrives with the
  crossfader (§14).

#### Which bars

- **The press.** A CAPTURE press asks for the last **N bars** (1, 2, 4 or 8). It comes from the
  button or the gate input:
  - edges with sample-accurate timestamps (Rack's button and gate, the device's gate input)
    arrive with the block, like clock edges;
  - presses from a UI or MIDI thread arrive as commands at the start of a block (§10.1).
- **The end line** is the next bar line if it is at most ¼ beat away (an early press, "on the
  one"). Otherwise it is the most recent bar line.
  - ¼ beat is a starting value, to be tried in Rack.
  - The end line is always chosen from the moment of the press, even when the press has to wait
    (below).
- **The window** is `[end − N bars, end)`. Each edge of the window sits at a bar line's estimated
  time (below) plus L_in (§3.5), rounded to the nearest sample (halves up).
- **Around the window:** before it lies the **pre-roll**, at least 50 ms (§5.3). After it there
  is a 50 ms read margin.
- **One labelling.** While a reset is pending, bars are numbered as they will be once it applies
  (§3.3).
  - A capture still waiting for its line is planned again whenever the labels change.
  - A capture that has started playing keeps its window: the same pulses, whatever their new
    numbers. This includes a late press that races a reset delivered just after its pulse.
    §3.6 says what the loop then plays.
- **Freewheeling.** While the clock is stopped, fill-ins count as edges, so a capture cuts on the
  freewheeling grid. If the clock turns out to have slowed by 2.5× or more, the tracker retires
  the fill-ins and re-records the real pulses (§3.3):
  - a capture still waiting for its line is planned again;
  - a loop that has started playing keeps its window and bar positions, final or not. An end that
    is not yet final stays at its provisional edge.
  - The kept cut is then not on a real bar line, and the renumbering re-phases the loop at the
    next line (§3.6).

    When can that happen? Take a loop whose end line is the first fill-in, and a slowdown of k
    times:
    - up to 5.5×, the 8th pulse after the line is the 4th fill-in after the clock returns. It
      commits k + 4.5 periods after the last normal pulse;
    - beyond 5.5×, more fill-ins commit before the return, so that pulse commits earlier
      still;
    - the retirement comes at 2k periods.

    So for any slowdown above 4.5× the loop is final first. At exactly 4.5×, the tracker
    commits that fill-in and retires it within one `pulse()` call. The capture checks after
    each edge and at each fill-in's commit time, and an edge at the same moment goes first
    (§10.1), so the capture sees the retirement.

    Tests pin 4.5× (planned again) and 4.51× (kept and re-phased). The tracker measured 4.4×,
    4.49× and 4.5× as retirement first, and 4.51× up to 12× as final first.

#### Where a bar line is

The tracker's history holds *filtered* pulse times, which serve prediction. The past is better
estimated from the raw **edges** on both sides of a line (§3.3). A bar line at pulse *i* is
estimated from the edges *x* of the 8 pulses on each side:

1. **Fit each side, and estimate the jitter.** Each side gets a repeated-median line through its
   8 edges, which ignores up to 3 bad edges of 8:
   - **slope *s*:** for each edge, the median of its slopes to the other 7 edges; then the median
     of those 8 values. A median of an even count is the mean of the middle two.
   - **value *v* at pulse *i*:** the median, over the side's edges, of *x_j* − *s*·(*j* − *i*).

   The **jitter estimate *σ̂*** comes from **third differences**,
   *e_j* = *x_{j+2}* − 3·*x_{j+1}* + 3·*x_j* − *x_{j−1}*.
   - **What cancels.**
     - A constant curvature gives third differences of zero.
     - A curvature that changes at a constant rate (a cubic) gives a constant third difference,
       which subtracting the median removes.

     So the deviations carry noise alone. The exceptions are the few values where a ramp starts
     or ends, which the median ignores.
   - **The formula:** *σ̂* = 1.4826 × the median of |*e_j* − median(*e*)|, divided by √20.
   - **Its window** is the 64 pulses before the line and the 8 after: 66 values, none using
     *x_i*.
   - **Fill-ins are skipped.** A third difference that touches a filled-in pulse is left out.
     Fill-ins sit exactly on the predicted grid, so they carry no jitter. A freewheel stretch
     covering half the window would otherwise pull *σ̂* to zero while the fitted sides are still
     jittery. Measured at ±10 samples with 40 fill-ins before the line:
     - without the skip, the median *σ̂* was 0.00 and 384 of 5,000 lines came out worse than
       their raw edge;
     - with it, *σ̂* was 6.03 (true 5.77) and none did.
   - **At least 16.** If fewer than 16 usable values remain (the clock is young, or a long
     freewheel fills the window), step 4 does not repair: the edge is kept.
     - With 10 values, *σ̂* falls below half the true jitter in 8.1% of seeds, and 1 line in
       50,000 came out worse than its raw edge (by 0.5 samples).
     - With the minimum of 16, none did.
   - **No later finality.** The extra values are all in the past, so a loop does not become
     final any later.
   - **Why third differences, and why 66.** Measured at 50,000 seeds:

     | Estimate | Below half the true jitter | Zero-jitter σ̂ with a ramp starting or ending in the window |
     |---|---|---|
     | 12 second differences, inside the fitted sides | 4.9% of seeds | — |
     | 44 second differences | 0.044% | up to 11.35 samples |
     | 66 third differences (adopted) | 0.004% | at most 0.03 |

     - **The 44-value row** inflates because a ramp in the window splits its second differences
       into two populations. On +20% over 2 bars, σ̂ is 11.35 samples (0.6·c) at the offset
       where they balance, and 1.6 to 3 for two bars after an exponential ramp. That only
       loosens thresholds, but a hiccup under 227 samples there would be kept.
     - **The 12-value row's tail** is what broke the earlier rule (below).
   - **Blind spot.** *σ̂* sees white jitter only: slow wander barely shows in a third difference.
   - **On the gate clock**, *σ̂* is 0.0050 samples (the median for 0.005-rms jitter). The keep
     and repair thresholds then sit at their 1-sample floors, and the curvature tolerance is
     about 0.015, just above its 0.01 floor.
2. **Keep an edge that lies on a straight line.** If *x_i* is within max(1 sample, 4·*σ̂*) of
   *v_left* or *v_right*, it stays. This is the path for a steady tempo. It is also the path for
   the first pulse of a phase shift or tempo change, which the side after it confirms.
3. **Correct for curvature, when there is curvature.** On a ramp, both straight lines miss the
   curve. Fed a pure parabola *u²*, the repeated median gives slopes of ∓9 and a value of −16 at
   the line, all linear in the curvature. That gives two estimates of the curvature *c*:
   - **between the sides:** *c_between* = (*s_right* − *s_left*) / 18;
   - **within the sides:** *c_within* = half the median of the 12 second differences
     *x_{j+1}* − 2·*x_j* + *x_{j−1}* inside the two fitted sides (6 each, none using *x_i*). Each
     is 2·*c* plus noise.
     - A second difference that touches a fill-in is skipped, as for *σ̂*: fill-ins follow the
       last period and carry no curvature.
     - With none left, *c* = 0. That affects accuracy only, because step 4's bound covers it.

   A tempo step has no curvature inside either side, but its slope difference reads as
   curvature. So *c* = *c_between* only when |*c_between* − *c_within*| ≤ max(¼·|*c_within*|,
   0.01, 3·*σ̂*), in samples per pulse². Otherwise *c* = 0.
   - The check can still be wrong either way under jitter:
     - **failing on a real ramp:** *c* = 0 then leaves both lines 16·*c* off the curve;
     - **passing at a small tempo step:** the correction then puts the mean 16·*c_between* off.

     Step 4's repair bound covers both mistakes, so the check decides accuracy only. When it is
     right, a repair lands on the true line.
   - The corrected values are *v′* = *v* + 16·*c* on each side.
   - Their **spread** is |*v′_left* − *v′_right*|, and their **mean** is
     (*v′_left* + *v′_right*) / 2.
4. **Repair an edge that is clearly off.** This step needs all three conditions:
   - the spread is at most 64 samples, so the two sides agree on where the pulse belongs;
   - *x_i* is more than max(1 sample, 2 × spread, 20·*σ̂*, 2·|16·*c_between*|) from the mean.
     The last term is twice the largest correction the curvature check could have applied, or
     left out, by mistake, so a wrong decision there cannot on its own trigger a repair;
   - the mean is at most half a period from *x_i*. A pulse displaced further is closer to its
     neighbour's slot (§3.3), and the capture's page reserve (below) stops at half a period.

   Then the line lies at the mean: the clock came back to its grid. This repairs hiccups and
   bursts.
5. **Otherwise keep the edge.** Pulses on a ramp end here.
   - A straight line misses the curve by 16·*c*: 37.5 samples on the 10%-over-8-bars ramp. Step 2
     therefore fires only on ramps about 40 times gentler than that.
   - On a ramp the corrected lines agree with the edge, so step 4 leaves it alone.

**Arithmetic:**

- Residuals are taken from the straight line through *x_i* at the tracker's period. They are
  computed exactly, in Q47.16 ticks.
- The fits run in `float` on those residuals.
- The result goes back into ticks, rounded to the nearest tick.
- Window edges then round to the nearest sample, halves up, as above.

The tolerances are measured choices: 1 and 64 samples, ¼ and 0.01 for the curvature check, the
factor of 2, and the jitter multipliers.

Two mechanisms carry the jitter cases: the correction term in step 4's bound, and a steady *σ̂*
(step 1). Each was removed in turn and the rule rerun.
- Ramp and steady clocks: 6 to 9 families at 20,000 to 50,000 seeds each.
- Small steps: 21 cells, ×1.002 to ×1.08 at the line at ±0.5 to ±10 samples, at 20,000 seeds
  each.

| Rule | Ramp and steady lines worse than the raw edge | Small tempo steps worse than the raw edge |
|---|---|---|
| The adopted rule | 0 in 180,000 | 0 in 420,000 |
| With the earlier 12-value *σ̂* | 0 in 300,000 | 1 in 420,000, by 8.9 samples |
| Without the correction term | 0 in 180,000 | 31 in 420,000: up to 11 in a cell, by up to 159 samples |
| Without either (the earlier rule) | about 1 in 10,000, by up to 428 samples | up to 83 in a cell, by up to 161 samples |

So the correction term is what makes a wrong curvature decision harmless. The steady *σ̂* then
keeps every threshold scaled by it from collapsing in a bad seed.

With both mechanisms, the repair multiplier hardly matters. Every multiplier from 6 to 20 gave no
line worse than its raw edge:
- over 140,000 ramp lines;
- in every steady and step family tested.

20 is kept for margin against noise shapes not tried, such as Gaussian tails, and because a
repair is only worth making on clear evidence.

The keep multiplier made no difference anywhere from 2 to 8, measured under the adopted rule at
280,000 lines per setting: none came out worse than the raw edge, and a 100-sample hiccup on a
steady clock at ±2 was repaired in all 20,000 seeds. 4 sits in the middle.

Measured on the gate clock at 4 PPQN and 120 BPM, in a double-precision probe, over 132
scenarios: the 136 of earlier rounds, less four jittered ones that moved to the jitter table. Errors are in samples, the worst over a line and the 3 pulses either side; "—" means
not measured. The implementation pins the same cases in `float`.

| Case | History | Edge | Estimate |
|---|---|---|---|
| One pulse off by 2 or 30 samples, or by ±0.1, ±0.2, ±0.3 or ±0.45 of a period | 2,700 (at 0.45) | 2,700 | 0.01 |
| Bursts of 2 to 4 off-grid pulses, at or around the line, or inside a side | 1,800 | 1,800 | 0.05 |
| Phase shifts of 0.002, 0.1, 0.2, 0.3 and −0.2 of a period, 2 pulses before the line, at it, or 1 after | 120 (at 0.2) | 0.01 | 0.01 |
| Tempo changes of ×0.5, ×0.9, ×1.01, ×1.1, ×1.5 and ×2, 2 pulses before the line, at it, or 1 after | 60 (at ×1.1) | 0.01 | 0.01 |
| The same tempo changes beginning at the line, with its edge slipped by 1.01, 2 or 30 samples | — | 30 | 30: kept (a limit, below) |
| Linear ramps: +10% over 8 bars, +10% and +20% over 2, −20% over 1, +50% over 4; ramps starting or ending at the line | 0.64 (the first) | 0 | 0 |
| Exponential ramps: +10% over 8 bars, +20% over 2, +50% over 4, −30% over 1 | — | 0 | 0 |
| A 1,800-sample hiccup on a linear ramp, or on an exponential one (+20% over 2 bars, +50% over 4) | — | 1,800 | 0.30 |

Under jitter, measured with uniform jitter, errors at the line. Rows are 50,000 seeds, except
the step grid at 20,000 per cell:

| Case | Raw edge, worst | Estimate, worst | Lines worse than the raw edge |
|---|---|---|---|
| Steady, ±0.5, ±10 and ±20 samples | the jitter | the jitter | 0 |
| +10% over 8 bars, ±0.5 to ±5 samples | the jitter | the jitter | 0 |
| +20% over 2 bars, ±2 to ±20 samples | the jitter | the jitter | 0 |
| Tempo steps of ×1.01 at ±10 samples, and ×1.1 at ±20 | the jitter | the jitter | 0 |
| Tempo steps of ×1.002 to ×1.08 at the line, ±0.5 to ±10 samples (21 cells) | the jitter | the jitter | 0 |
| A 1,800-sample hiccup on +10% over 8 bars, ±2 samples | 1,802 | 12.1 | 0, and repaired in every seed |

**The limits.** In each of these cases the two sides disagree about where the pulse belongs, so
step 4 cannot repair it and the edge is kept, at its own error:

- a ramp that bends hard within 8 pulses: an exponential −30% over 1 bar leaves the sides 378
  samples apart;
- a hiccup on the very pulse where a ramp starts or ends: the sides are 16·*c* apart, 300
  samples on +20% over 2 bars;
- a slipped edge on the first few pulses after a tempo step, while the step still splits the
  left side's edges (measured on the first and the third pulse after it): the sides are the
  step apart.

The correction term in step 4 adds two of its own. Each keeps an edge rather than risk moving
a good one:

- **a slipped edge at a tempo step on the line** stays at its slip. The term is twice the
  correction the step could be mistaken for: 1,066 samples at ×1.1.
- **a hiccup on a ramp smaller than twice its curvature bias** is kept: under 75 samples on the
  10%-over-8-bars ramp, under 600 on +20% over 2 bars.

And three under jitter:

- **a displacement smaller than 20·*σ̂*** is kept, because it cannot be told from the jitter;
- **a hiccup on a steep ramp under heavy jitter** can be kept, because the 64-sample spread bound
  does not scale with the jitter (+20% over 2 bars at ±10 samples);
- **wander,** slow timing drift that *σ̂* cannot see (step 1).

**Mutation checks, one per mechanism:**

| Remove | And this fails |
|---|---|
| The curvature check | A 1,800-sample hiccup on the pulse of a ×1.1 step lands 533 samples off, not on the line |
| The correction term | The small-step cells: up to 11 of 20,000 lines in a cell, by up to 159 samples |
| The 66-value window | *σ̂*'s own tail: below half the true jitter in 4.9% of seeds (12 values), not 0.004% |
| Third differences (back to second) | A ramp inside the window inflates *σ̂* to 11.35 samples, not 0.03 |

The other notes:

- **When a loop is final.** The estimate needs the 8 pulses after a line, so a loop's end, and so
  its length, is final 8 pulses after its end line: 2 beats at 4 PPQN. If the clock stops,
  fill-ins arrive from 2.5 periods later (§3.3), so a loop always becomes final.
- **Short history.** A line with fewer than 8 edges on a side keeps its edge. That happens only
  when the clock has just started. A line with fewer than 16 usable third differences for *σ̂*
  also keeps its edge (step 1). That happens just after the clock starts, or after a long
  freewheel.
- **MIDI clock is not yet measured.** The thresholds scale with *σ̂*, so white MIDI jitter needs
  no tolerance of its own. USB MIDI's scheduling, though, produces slow wander, which *σ̂*
  cannot see. That wander, not a peak figure like ±48 samples, is what to test when MIDI clock
  is wired.

#### When the loop starts

- **Early press, with at least F (8 ms) before the line.** The new loop's first bar starts
  exactly on the end line, entered from its own pre-roll like every downbeat (§5.2). Its start
  was settled long before.
- **Early press, with less lead than F.** The entry shortens to the time left. Below 2 ms (the
  §5.3 floor), the loop starts on the line with `crossfaded_seek` instead.
- **Late press.** The loop starts at once, mid-bar, where it would be had it started on the end
  line. `crossfaded_seek` fades it in (§5.4).
- **A provisional end.** Until its end line is final (8 pulses after the line), the loop uses the
  raw end edge. That applies to early presses, and to late presses made within 8 pulses of their
  line.
  - If the estimate then changes, the change takes effect at the next line through the usual
    bar-locked move (§3.6). For the end line, the next line is the first wrap.
  - The end is final before the first wrap whenever a bar has more than 8 pulses (4 PPQN in 4/4
    has 16). With fewer, the first wrap uses the raw end edge.
- **Replacing a playing loop** uses the same moves: an early press hands over on the line, and a
  late press hands over with a seek.
- **The loop then plays bar-locked** (§3.6).

#### Pinning and limits

- **Pinning.** At the press, the capture pins every recorded page it may need. Some pages its
  window may reach are not recorded yet:
  - an early press's end line, up to ¼ beat ahead;
  - the 50 ms read margin;
  - half a period of repair after the end line. Step 4 never moves a line further.

  No reserve is needed before the start line when it is already final at the press. That is
  the case whenever 8 pulses after it have arrived, which at 4 PPQN is always (a bar has 16).
  Otherwise half a period is reserved before the start line too.

  Admission reserves those pages, and the recorder pins each page it allocates while the capture
  is in flight. When the capture becomes final, it releases the pages its window does not use.
- **Shortened captures.** A capture is shortened to the largest of 1, 2, 4 or 8 bars that both
  fits the history at its press and keeps within 47 pages (§2.3). At slow tempos this is routine.
  The floors, in BPM at 4 PPQN in 4/4:

  | Bars | Any press fits from | Every early press fits from | No press fits below |
  |---|---|---|---|
  | 1 | 15.65 | 7.91 | 7.91 |
  | 2 | 23.48 | 15.65 | 15.57 |
  | 4 | 39.13 | 31.30 | 30.91 |
  | 8 | 70.43 | 62.60 | 62.11 |

  - **Any press:** the history at its tightest, for a late press up to a bar after its line
    (§2.3).
  - **Every early press:** a press on its line, with no lead.
  - **No press:** an early press with ¼ beat of lead. Where the 47-page limit binds first, the
    floor is that limit's: for 1, 2 and 4 bars, and for 1 bar in the early-press column.

  Every column uses the same reserve rule: half a period after the end line only (pinning,
  above).

  A capture is also shortened just after power-up, or when the clock started recently.
- **Refused captures.** A capture is refused when not even one bar fits, when there is no tempo
  yet, or when the pool can't take its pages (§2.3). The loop report says which.
- **The bar count** is taken at the press.
- **A new press while a capture is in flight:**
  - an early press still waiting for its line is superseded by the new one;
  - a press during an entry crossfade waits for that crossfade to finish, and keeps its own
    timestamp for choosing its end line;
  - otherwise the in-flight capture is playing but not yet final. The new capture replaces its
    loop as above, and becomes the one in flight.
- **Forward recording** uses the same mechanism: start at the next bar line, stop after N bars or
  at a second press. It comes after P1.

### 3.5 Latency compensation

The signal at the input jack reaches the engine **L_in** samples late (ADC filter delay plus DMA).
Audio produced by the engine reaches the output jack **L_out** samples late (DAC filter delay plus
DMA). Then:

- **Capture** cuts the window L_in samples *after* the grid's bar lines. Audio that coincided with
  a clock edge at the jacks reaches the engine L_in samples after that edge, so the window then
  lines up with the edges as they arrived *at the jacks*.
- **Playback** reads L_out samples ahead, so the loop's downbeat leaves the output jack exactly on
  the clock edge.
- L_in and L_out are measured by **calibration mode**: an impulse goes out, is patched back in,
  and the round trip is measured. The two are separated using the codec's published filter
  delays. Results are stored per board revision and verified by the HIL rig (§12).

### 3.6 Drift and phase locking

A loop has an integer length that was correct for the tempo when it was recorded. If the clock
drifts or changes:

- **Follow mode** (the default once Tape mode exists): playback rate = recorded period ÷ current
  period. A phase-locking loop nudges the rate so the loop downbeat stays on the clock downbeat.
  Corrections under 0.1% are inaudible, and the playhead never jumps.
- **Fixed mode** (P1's first playback, at native speed) is **bar-locked**: each loop bar starts
  exactly on its grid line.
  - **The loop's own bar lines.** The loop stores its N + 1 bar positions (§3.4), so a window that
    spans a tempo change keeps its unequal bars.
  - **Which bar is due.** Each loop bar has a **phrase position**: its bar number modulo N, in
    the numbering it was recorded under. At grid bar *g*, the loop bar whose phrase position is
    *g* mod N is due. The loop's first bar therefore plays in the bar right after the bars it
    captured.
  - **When the numbering changes** (a reset, or retired fill-ins, §3.3), the next line applies
    one of two rules:
    - **The numbering at capture came from a reset.** The loop keeps its phrase positions and
      plays the bar the new numbering calls for. A reset on a phrase boundary changes nothing.
      An off-phrase reset moves the loop to the bar recorded at the same phrase position. That
      keeps the loop aligned with a sequencer that restarted on the same reset.
    - **No reset had yet set the numbering.** The phrase positions were the module's own count,
      so the change disturbs the loop as little as possible. At the first new line, the loop bar
      whose start is nearest the playhead is due, and the loop takes its phrase positions from
      there:
      - a reset on an old bar line changes nothing;
      - one that lands δ into a bar repeats those δ if δ is under half a bar, and otherwise
        skips the rest of the bar.

    Example. The music runs in 4-bar phrases, with a fill in each 4th bar, and its numbering
    came from a reset when the sequencer started. A capture of the last 4 bars is pressed at
    bar 6:
    - the loop holds A3 A4 B1 B2;
    - its fill, A4, lands on the phrase ends, bars 7 and 11.

    Then the sequencer is restarted from its top at bar 9, and sends a reset:

    ```
    old bar                     9    10   11   12
    new bar                     0    1    2    3
    sequencer                   A1   A2   A3   A4     its fill on new bar 3
    the first rule              B1   B2   A3   A4     the fill lands with the sequencer's
    restarting the loop         A3   A4   B1   B2     the fill two bars early
    ignoring the reset          B2   A3   A4   B1     the fill one bar early
    ```

    - **The first rule** plays B1 on new bar 0, because B1 was recorded at bar 4, a phrase start.
    - **Restarting the loop on every reset**, the DAW convention, was considered and rejected.
      It puts a loop captured mid-phrase out of step with the sequencer, and retro capture is
      pressed whenever something sounds good, not only at the end of a phrase.
    - **Ignoring the reset** is the second rule. It is kept only for numbering that never came
      from a reset.
    - **When the choice makes no difference.** A capture pressed at a phrase end, with resets on
      phrase starts, plays the same under all three behaviours.
  - **At each line,** before the due downbeat plays, its natural time is compared with the line in
    Q47.16 ticks.
    - If they are a whole sample or more apart, the downbeat moves onto the line's nearest
      sample. The move is made just before whichever comes first, the line or the natural
      downbeat, as a crossfade from the audio that preceded that downbeat (§5.2).
    - Otherwise it plays on.

    Every loop downbeat therefore leaves less than one sample from its line.
  - **What moves.** Only audio from just before a downbeat is repeated or skipped. The downbeat
    itself is never early, late or doubled. For example, a clock 0.4% slower than the recording
    (120 → 119.5 BPM) repeats 8.4 ms of the end of each bar, once per bar.
  - **The same move everywhere.** The wrap enters the loop's first downbeat from the pre-roll
    (§5.2). Resets, phase shifts and replacements make the same move.
  - **Lines learned late.** A line learned after it has passed is joined by `crossfaded_seek`
    (§5.4). Examples: a reset delivered just after its pulse, or a phase-shift verdict on a
    downbeat.
  - **Tolerance.** The 1-sample tolerance is for the gate clock. MIDI's is chosen with MIDI.
  - **Granularity.** Locking every bar is the P1 choice. Every beat would make smaller moves; that
    is to be measured in Rack (§15).
- **Playheads** are `uint64_t` Q32.32 fixed-point positions with an integer rate increment. This
  is exact and deterministic with no accumulated float error, which is why the host tests can be
  bit-exact.

---

## 4. Tempo: tracking and time stretching

### 4.1 Where tempo comes from

1. **External clock**, gate or MIDI (§3).
2. **The first loop sets it** (the default when no clock is patched). Capture is started and
   stopped freely. The module then picks the bar count (1, 2, 4 or 8) that puts the tempo in
   **80–160 BPM**: 4.0 s becomes 4 bars at 120 BPM. That loop becomes the master clock, and **clock
   out** (gate or DAC) drives the rest of the rack.
3. **Tap tempo.**

**Audio tempo detection** (onset detection function plus autocorrelation) only *assists* the
bar-count guess in step 2, for example choosing between 2 and 4 bars. It never leads, because it
is ambiguous between half and double tempo and unreliable on ambient material.

### 4.2 Adapting loops to a new tempo

Each slot stores its **recorded period** (samples per beat, Q32.32). Stretch ratio = current
period ÷ recorded period.

Loops are already in memory, so stretchers can **read ahead**. Their algorithmic latency is
compensated completely, which makes FFT-based methods usable live.

Drift under 0.1% is always handled by varispeed (§3.6). A slot's **stretch mode** engages only for
real tempo changes:

| Mode | Method | Character | Use for |
|---|---|---|---|
| **Tape** | Varispeed; pitch follows tempo | Tape and DJ pitch, tape stop, octave jumps | Effects, small changes |
| **Slice** | Re-trigger at grid and onsets; fade the gaps | Perfect transients; gaps on sustained sound | Drums |
| **Stretch** *(default)* | WSOLA (waveform-similarity overlap-add) | Good on full mixes; mild flutter at large ratios | Full mixes (the main use case) |
| **Stretch HQ** | Phase vocoder, identity phase locking, transient handling | Best on tonal and mixed material | Pads, critical material |

- **Stereo coherence:** alignment (the WSOLA offset or PV phase) is computed on mid = (L+R)/2 and
  applied to both channels, so the stereo image does not wobble.
- **Pitch shift:** stretch plus resample gives independent pitch per slot (key changes, octave
  transitions).
- **CPU:** HQ is expected to fit on 1–2 slots at once (§2.2). The engine degrades gracefully: it
  falls back to WSOLA and reports this in telemetry.
- **Quality references:** SoundTouch (LGPL), Signalsmith Stretch (MIT) and Rubber Band (GPL) are
  used *only* in the host render tool for A/B comparison. The on-device algorithms are our own.

---

## 5. Click-free loop points

### 5.1 Sources of clicks

| Source | Fix |
|---|---|
| Seam: the end doesn't match the start | Enter the start from the audio that preceded it, the pre-roll (§5.2) |
| A loop bar drifting off its grid line (tempo, reset, phase shift) | Bar-locked playback moves the playhead just before a downbeat (§3.6, §5.2) |
| Hard cuts at capture edges | Pre-roll and read margins, cold-start fade (§5.3) |
| Jumps that can't be entered from before a downbeat (a mid-bar start, a line learned late; later stutter, slice, reverse) | The `crossfaded_seek` primitive (§5.4) |
| Gain steps (mute, punch, crossfader noise) | Smoothing everywhere (§5.5) |
| DC offset turning every edge into a thump | DC blocker before recording (§5.5) |
| Interpolators reading past the end of a loop | Reads through the margins (§5.3) |

### 5.2 Entering a downbeat: the pre-roll seam

Over the last **F** samples of the loop, the loop's own tail is crossfaded into the **audio that
came just before the loop start** (still on the tape, §3.4):

```
                 loop start                                   loop end
                     │                                            │
  … pre-roll ………… ───┼──────────── loop body ─────────── tail ────┤
     └──── F ────┘                                   └──── F ────┘
          │                                                 ▲
          └──────────── crossfaded into the tail ───────────┘
```

At the wrap, the output flows into the loop start exactly as the original audio did, because the
end has *become* the audio that preceded the start. **The downbeat transient at the start is
never touched**, so kicks stay sharp and grid alignment stays exact. (The alternative,
crossfading the head with the audio after the end, smears the downbeat and is not used.)

**The general rule:** every downbeat a loop plays is entered from the audio that preceded it in
the recording.

- **The wrap is one case.** The others use the same crossfade, also ending exactly on the line:
  - **a bar-locked move (§3.6):** from the current audio into the audio before the due bar;
  - **a new loop's first bar** (an early press, §3.4): from silence, or from the loop it
    replaces, into the new loop's pre-roll.
- **A large slowdown at the wrap is not an exception.** Suppose a bar-locked delay δ at the wrap
  is more than the pre-roll can absorb (50 ms − F, §5.3).
  - The delay is first taken up by a `crossfaded_seek` back by δ (§5.4), made F before the
    natural end, inside the last bar.
  - The last bar then plays out through the unchanged wrap seam into the pre-roll.

  The downbeat is entered from the pre-roll as always. The seek is not on a line, so it counts
  among §5.4's seeks, not as a third primitive.

The properties:

- **Non-destructive:** the seam is computed at read time. Seam length can change after capture,
  and half/double/reverse keep working.
- **Adaptive shape:** the correlation between the two signals being joined selects an
  **equal-gain** fade (correlated, the usual case, since it is the same music one bar apart) or
  an **equal-power** fade (uncorrelated), or a blend. This avoids the +3 dB bump an equal-power
  fade puts on correlated material.
  - The correlation is measured over the 50 ms before the downbeat, on both channels together.
    On mid alone it would be 0/0 for an anti-phase pair.
  - Zero energy counts as correlated: equal-gain.
- **Curves** come from our own polynomial or table, not libm (ADR 0005).
- **Length:** default **8 ms**, with a deep-menu override up to a 1/16 note for pads and bass. (A
  fade shorter than one cycle of a 40 Hz note can still thump.)
- **No zero-crossing snapping.** It breaks sample accuracy, and in stereo the channels cross zero
  at different times.

### 5.3 Margins

Every loop keeps at least **50 ms** of valid audio before its start (the **pre-roll**) and 50 ms
after its end (a **read margin**). Interpolators, stretchers and reverse playback read through
them and never past valid data.

- **The pre-roll is played.** It is what enters the first downbeat at each wrap (§5.2). It also
  absorbs a bar-locked slowdown at the wrap of up to 50 ms − F. A larger one is first taken up by
  a move inside the last bar (§5.2), and the wrap seam itself is unchanged.
- **The read margin is never played at native speed,** because it begins with the next bar's
  downbeat.
- **Too little pre-roll** (a capture just after power-up): the seam shortens to what exists.
  Below 2 ms, a starting value, the loop fades out and in instead.
- **A silent pre-roll** (a loop captured from the moment the music started): the seam is an 8 ms
  fade to silence before each first downbeat. It is pinned as a stated exception (§5.6).
- **The margins also bound the correlation windows (§5.4).**
  - The wrap seam's window reads the last 50 ms of pre-roll.
  - A seek made F before the natural end reads the outgoing stream 50 ms − F past the end
    (42 ms), inside the read margin, with F to spare.

  A correlation window longer than the margins would read past valid data, so the three
  lengths change together.

### 5.4 `crossfaded_seek`

Every playhead discontinuity uses one of two primitives, so there are exactly two places to get
this right:

- **Entering a downbeat (§5.2)**, for anything that lands on a known line: wraps, bar-locked
  moves, early-press starts and quantized launches.
- **`crossfaded_seek`**, for everything else:
  - a mid-bar start (a late press, §3.4);
  - a line learned after it passed (§3.6);
  - the first move of a large slowdown at the wrap (§5.2);
  - later, stutter, slice re-trigger and reverse.

  A second read head starts at the target position, and a crossfade hands over. It lasts 3 ms
  to start with, measured before it is pinned.

**One shape rule for every crossfade, seam or seek.** The correlation of the two streams being
joined chooses the shape, as for the seam (§5.2): equal-gain when they are correlated,
equal-power when they are not, or a blend.
- **The window.** An entry into a downbeat measures the 50 ms before the downbeat (§5.2). A seek
  measures the next 50 ms of both streams from the seek point. Both lie on the tape, because the
  margins are at least as long as the window (§5.3).
- **The cost** is a §2.2 row, measured in T5: up to 4,800 stereo frames read from SDRAM per
  crossfade.
- **Why seeks need it too.** A fixed equal-power seek would peak at +3 dB mid-fade on
  correlated audio, against §5.6's ±1 dB. Correlated seeks are common: the large-slowdown seek
  joins audio δ apart inside one bar, which is correlated on a pad or bass, and stutter and
  slice re-trigger do the same.
- **Starting from silence.** A seek from silence has zero energy on one side, which counts as
  correlated, so it fades in at equal gain.

### 5.5 Smoothing and DC

- Every gain, mute, record level, feedback level, punch-in and punch-out is ramped over 5–20 ms.
- Crossfader and CV inputs are filtered and slew-limited.
- Slice playback uses a 0.5 ms attack (keeping transients) and a few-ms release.
- Inputs pass through a **DC blocker** (about 5 Hz one-pole high-pass) *before* recording. P1
  puts it on the record path only. The live path stays a bit-exact passthrough until the mixer
  gives it the same blocker.
  - It is y[n] = x[n] − x[n−1] + r·y[n−1], with r = 1 − 2π × 5 Hz / rate. The coefficient uses
    only +, −, × and ÷, so every target computes the same bits (ADR 0005).
  - Measured at 48 kHz: −3.01 dB at 5 Hz, −0.26 dB at 20 Hz and +0.003 dB at 1 kHz. A full-scale
    offset is exactly zero after 52,934 samples (1.10 s).
  - Its state snaps to exactly zero below 2⁻⁵⁰ (−301 dBFS), and its input is flushed the same
    way. Fed silence after full scale, a 5 Hz one-pole goes subnormal after 2.78 s and stays
    there; with the snap it never does (ADR 0005).
  - A NaN or an infinity is flushed the same way, in the input and in the state. Unflushed, one
    stays in the state for ever, and every frame recorded after it is NaN. A Rack module
    upstream can send one, and finite input can overflow into one: the largest float minus its
    negative is infinite.

### 5.6 How it is verified

The **click metric** is the high-passed energy in a window around each seam or jump divided by
the same measure across the loop body. It should be ≈ 1.0.

Energy alone misses a dip: a 3 dB hole lowers the ratio and passes. So the **RMS envelope**
through each seam must also stay within ±1 dB of the body. ±1 dB is a starting value, to be
measured.

Both are tested against:

- a sine whose period does not divide the loop length
- a 40 Hz bass
- a DC-offset input
- white noise
- a kick exactly on the downbeat (the transient must survive)
- an anti-phase stereo pair

Each signal goes through every way a loop can enter a downbeat:
- the wrap;
- a bar-locked move;
- an early start;
- a replacement.

It also goes through `crossfaded_seek`, and through a wrap slowdown beyond 50 ms − F. That
slowdown adds a seek inside the last bar before the usual wrap seam (§5.2), and both crossfades
are held to the same bounds.

The stated exceptions:

- **a silent pre-roll** (§5.3): every seam is an 8 ms fade to silence;
- **an early press with under 2 ms of lead** (§3.4): its downbeat is faded in by
  `crossfaded_seek`, so the envelope dips;
- **a start from silence** (a late press with nothing playing): it is a fade-in, not a seam.

---

## 6. Signal flow and mixing

```
                         ┌───── DC block ─────┐
 IN L/R ───────┬────────►│                    ├──► tape (always recording, §2.3)
               │         └────────────────────┘
               │ live bus (P1: no DC block until the mixer, §5.5)
               ▼
  slot 1 ─┐                ┌─────────┐
  slot 2 ─┼─► loop bus ───►│ FX      ├──► loop ──┐
  slot 3 ─┤   (levels,     │ insert  │           ▼
  slot 4 ─┘    mutes)      └─────────┘     ┌────────────┐
               live ──────────────────────►│ crossfader │──► OUT L/R
                                           │ equal-power│
                                           └─────┬──────┘
                    capture source ◄── Input (dry) or Mix (what you hear)
                    cue bus (module only) ◄── pre-fader slot audition ──► CUE L/R
```

- **DC block:** in P1 it sits on the record path only; the live bus gets one with the
  mixer (§5.5).
- **Crossfader:** equal-power (sin/cos) between the live bus and the loop bus. It is CV
  controllable and slewed.
- **Quantized auto-fade:** one press fades across 1, 2 or 4 bars, landing exactly on a downbeat.
- **Capture source:** *Input* (dry) or *Mix* (resample the output, including loops and FX). Mix
  lets layers stack into a single slot.
- **Cue bus** (module only): pre-fader audition of any slot through a second codec on SAI2.

---

## 7. Loop slots and performance operations

- **4 slots.** Each has a state machine: `empty → capturing → playing ⇄ muted → empty`.
- **Operations, all quantized:**
  - **capture**: retro or forward
  - **launch / stop**
  - **overdub**: add the input
  - **replace**: punch in
  - **multiply**: extend by N repeats
  - **half / double** length
  - **reverse**
  - **clear**
  - **undo / redo**: one level to start
- **Zero-copy overdub with undo.** Overdub reads the slot's pages A and writes `A + input` into
  fresh pages B. The slot then points at B. Undo points back at A. Redo points at B again. No
  copying ever happens in the audio path.
- **Slot metadata:**
  - pages and window (start, length), and the N + 1 bar positions (§3.6)
  - recorded period
  - stretch mode
  - seam length and shape
  - level
  - mute state

---

## 8. Effects

All effects are tempo-aware (the tracker supplies the grid) and sit on the loop bus. They are
listed in build order:

| # | Effect | Notes | DaisySP block |
|---|---|---|---|
| 1 | **DJ filter** | One knob: LP ← center → HP; SVF | `Svf` (or our own) |
| 2 | **Beat repeat / stutter** | 1/4 to 1/32, quantized, via `crossfaded_seek` | — (ours) |
| 3 | **Reverse, varispeed, tape stop** | Ramped rate changes | — (ours) |
| 4 | **Tempo-synced stereo delay** | Ping-pong; feedback filter | `DelayLine` pattern, our own SDRAM buffer |
| 5 | **Granular freeze / cloud** | Grains read loop memory directly, so no extra RAM | `GranularPlayer` for reference |
| 6 | **Reverb** | Our own FDN (`ReverbSc` is LGPL, so it is avoided by default) | — |
| 7 | **Slice shuffle** | Re-order 16 slices, seeded so it is reproducible | — |

**Concurrency estimate** (§2.2 budget: about 7,000 usable cycles per frame):

- **Remaining for FX:** after the fixed cost and four slots (Tape about 500, all-WSOLA about 1,700),
  roughly **5,300–6,500 cycles** are left.
- **All seven effects as one global chain on the loop bus:** about 3,500–4,000 cycles, so they fit
  together.
- **Per slot (×4):** cheap effects (filter, stutter, varispeed, delay) fit on every slot. Reverb and
  granular are too heavy to run four times and become **shared send effects**.
- **Main risk:** SDRAM random-access stalls (granular, WSOLA search, delay), not raw CPU. Hot data
  is staged into internal SRAM where T5 measurements show it's needed.
- **Graceful degradation:** fewer grains, or HQ falling back to WSOLA, with load reported in
  telemetry.

**Working assumption:** a global FX chain, with cheap effects per slot and heavy effects as
sends. Final order and routing are confirmed in P3 (§15).

---

## 9. Controls and UI

### 9.1 Pod (development) mapping

| Control | Function |
|---|---|
| Pot 1 | **Crossfader**, live ↔ loop |
| Pot 2 | FX macro (current FX page) |
| Encoder turn | Capture length in bars (1, 2, 4, 8) |
| Encoder click | Cycle page (Loop / FX / Clock / Settings) |
| Encoder hold + turn | Edit the parameter on the current page |
| SW1 tap | **CAPTURE** (retro, last N bars) into the next free or selected slot |
| SW1 hold | Forward record (from the next bar) |
| SW2 | Quantized auto-fade (toggles direction) |
| SW1 + SW2 | Undo |
| LED 1 | Beat flash (bright on the downbeat). Color = clock source: green gate, blue MIDI, white internal, amber freewheel |
| LED 2 | Selected slot state: off empty, red capturing, green playing, dim green muted |
| MIDI (TRS or USB) | Every parameter and action through the control map (§11) |

### 9.2 Module panel concept (about 14–20 HP)

- **Big crossfader** (knob or slider, §15)
- **4 slot buttons** with RGB LEDs (launch/stop; hold to select)
- **CAPTURE** button and **length** encoder
- **FX** knobs (2–3) plus FX page button
- **Shift**

**Jacks:**

| Inputs | Outputs |
|---|---|
| IN L/R | OUT L/R |
| CLOCK | CUE L/R |
| RESET | CLOCK out / EOC (end of loop) |
| CAPTURE gate | |
| XFADE CV | |
| FX CV × 2 | |

### 9.3 Display or no display

**Rule: the module is fully playable without looking at a screen.** A display adds depth and
confidence, but nothing performance-critical depends on it.

| | No display | Display (128×64 OLED, SSD1306/SH1106) |
|---|---|---|
| State | RGB slot buttons, beat/clock LED, LED bar or ring for length and fade progress | Everything the LEDs show, plus waveforms, playheads, BPM, phase-lock indicator, FX pages |
| Deep settings | Shift layer, plus a USB editor (WebMIDI dev UI grown up; saved to flash) | On-device menu (USB editor still available) |
| Cost | — | About 5 pins (SPI), a few KB of flash, main-loop CPU only (about 30 fps), power filtering against OLED noise |

**How the code supports both:**

- A **UI model** (what the user should see) is kept separate from **renderers**: an LED renderer
  always, and an optional display renderer.
- The display renderer draws into a hardware-free 128×64 **framebuffer**:
  - **Rack** shows the same framebuffer on its panel, so both variants can be tried before
    hardware decisions.
  - **Tests** compare framebuffers against golden images.
  - **Device** sends the framebuffer to the OLED.
- The Pod's header exposes SPI, so a display can be trialled on the Pod.
- Panel variant vs. header-mounted option is decided in P4 (§15).

---

## 10. Software architecture

### 10.1 Layers

```
┌──────────────────────────────────────────────────────────────────┐
│ targets:  firmware/ (libDaisy)   rack/ (VCV Rack SDK)   tools/   │  ← C++ adapters (thin)
├──────────────────────────────────────────────────────────────────┤
│ engine/   clock_tracker  page_pool  tape  loop_player  mixer     │  ← pure C++20, no platform
│           fx/*  stretch/*  control_map  command_queue  telemetry │     headers, deterministic
├──────────────────────────────────────────────────────────────────┤
│ vendor/   libDaisy  DaisySP  CMSIS-DSP  (Rack SDK fetched)       │  ← never modified
└──────────────────────────────────────────────────────────────────┘
```

- **`engine/` knows nothing about hardware.** It takes buffers, clock events and commands, and
  produces audio and telemetry. The *same* code runs in tests, in the render tool, in Rack and on
  the Seed.
- **Engine entry point:**

  ```cpp
  void engine::process(const audio_block& block, std::span<const clock_event> events = {});
  const clock_report& engine::clock() const;
  ```

  - `audio_block` holds `std::span`s for the stereo in and out.
  - `events` are the block's clock and reset edges, timestamped from the block's first frame
    with sub-sample precision, in time order.
  - After each block, `clock()` reports the grid:
    - lock state, beat period and current bar
    - the grid pulses that fell in the block (offset, index, beat and bar flags), never closer
      than half a period. Exactly once with steady or jittery clocks, resets, tempo increases,
      a stopped clock and its return. During a sudden slowdown, predicted pulses can be reported
      before the slowdown is known (a pinned number of extras); every real pulse is still
      covered, by its own report or a prediction less than half a period before it.
    - `bar_restarted` when a reset makes an already-reported pulse bar 0.
    - Adapters use these for clock out and the lights.
  - The engine's sample clock never goes backwards; all timestamps are measured against it.
  - Commands (from UI, MIDI or tests) arrive through a **lock-free
    single-producer/single-consumer queue** and are applied at the start of the next block,
    quantized to the grid where relevant. For example, `reset_grid` acts like the reset input.
  - **Retro capture (P1, planned) adds:**
    - **a capture edge** among the block's events (the button or the gate, timed to the sample),
      carrying the bar count. Edges sampled on the audio thread, as in Rack, arrive this way. The
      command queue remains for UI and MIDI threads.
    - **`loop()`**, a per-block report: the slot's state, the last outcome (committed, shortened
      or refused, and why) and the loop's downbeats in the block.
    - **Split blocks.** The engine processes a block in segments. It splits at each clock and
      capture edge, and at each moment a capture's state can change (a fill-in committing).
      - Every decision uses only what is known at its moment.
      - An edge at the same moment as such a check goes first.

      Output is therefore identical at any block size, given the same edges and the same times
      at which commands are applied. Commands apply at block starts, so where a command takes
      effect does depend on the block size.
- **Memory:** the engine receives one memory region at construction, a
  `std::span<stereo_frame>` (SDRAM on device, a heap block on host), and carves it into pages
  (§2.3). `history()` reads back what the recorder has kept.
  - **Nothing is allocated after construction**, and the region is not touched before the first
    `process()`.
  - Without a region nothing is recorded, captures will be refused, and the engine otherwise
    behaves as before.
  - Members are declared so the pool outlives everything that holds its pages. The pool's
    destructor expects every count to be zero, and the recorder's gives its pages back.
- **Strong unit types:** `samples`, `beats`, `seconds` and `ratio` are distinct types, so mixing
  them does not compile.

### 10.2 Repository layout

```
engine/      pure C++ DSP and logic (shared by every target)
tests/       host unit, property, and golden tests
tools/       render CLI, dev UI, rig scripts
rack/        VCV Rack plugin (plugin.json, src/, res/)
firmware/    Daisy target (board abstractions, UI, main)
vendor/      pinned submodules (libDaisy, DaisySP, CMSIS-DSP)
docs/        this design, coding standard, ADRs, learning notes, vendor datasheets
```

### 10.3 Targets

| Target | What it is |
|---|---|
| **Host tests** | Native macOS/Linux build with sanitizers |
| **Render CLI** | WAV in + clock-edge file + timed command script → WAV out, plus metrics |
| **VCV Rack plugin** | The interactive test bed: real patching, clocks and controls. Rack processes one sample at a time, so the adapter **re-blocks** into device-sized blocks and block boundaries behave exactly as on hardware. |
| **Firmware** | Pod (Seed 1.x and Seed3), later the module board |

---

## 11. Development control surface

- **USB MIDI device** (libDaisy `MidiUsbTransport`, internal port). The Seed appears as a
  class-compliant MIDI device on the Mac.
- **Dev UI:** a single HTML page using WebMIDI (`tools/devui/`). It forwards any MIDI controller to
  the Seed and shows a live dashboard:
  - clock: BPM and phase error
  - slot states, playheads and waveform overview
  - levels
  - CPU cycles and overruns
- **USB host** (pins D29/D30/D0 plus 5 V): a class-compliant controller plugs straight into the
  Seed. This is a P2 stretch goal and a candidate module feature.
- **One control map** in `engine/` gives every parameter and action a CC or note. The map is
  identical on device, in Rack (through Rack's MIDI modules) and in the dev UI.
- **SysEx protocol** (versioned): device → host telemetry snapshots and events. Host → device test
  commands: reset, load scenario, self-test, **reboot to bootloader** (for automated flashing).

---

## 12. Testing strategy

Every change is verified as deep into the hardware as possible, automatically.

| Tier | Where | What it proves | When |
|---|---|---|---|
| **T0** | Mac / CI | All targets build with `-Werror`. clang-tidy, cppcheck and clang-format clean. Firmware flash and RAM budgets met. | Every commit |
| **T1** | Mac / CI | Engine unit tests under ASan/UBSan. A realtime guard traps any allocation inside `process()`. | Every commit |
| **T2** | Mac / CI | Property and fuzz tests: random clocks (jitter, drift, ramps, dropouts) and command sequences. Invariants: no NaN or denormals, bounded output, click metric ≈ 1, exact loop lengths. | Every commit (short), nightly (long) |
| **T3** | Mac / CI | Golden render regression with metrics. Intended changes are re-approved with `bless`. | Every commit |
| **T4** | Mac / CI | Rack adapter unit tests, and the plugin builds against the pinned SDK. Loading the plugin in Rack is checked manually, because headless Rack stalls on macOS Core Audio (ADR 0002). | Every commit |
| **T5** | Pod | The T1 suite cross-compiled and run on the M7, with results returned over the SWD probe (RTT). DWT cycle benchmarks have budgets. | Every commit (self-hosted runner) |
| **T6** | Pod + ES-9 | HIL audio: flash, drive over USB MIDI, play stimulus and clock from the ES-9, record output **and the clock on a spare input** (one timebase), analyze. Checks: latency, frequency response, noise, THD+N, capture alignment, 64-bar drift, seam clicks, overruns. | Every commit (quick), nightly (full) |
| **T7** | Pod | Soak: hours of randomized play. A watchdog and a crash record that survives reset report the cause on next boot. | Nightly |
| **T8** | Pod / module | Physical controls: MIDI injection at the control layer, a manual checklist per release, and a test-header fixture on the module PCB | Per hardware rev |

**Commands:**

| Command | Runs |
|---|---|
| `make test` | T0–T4 |
| `make test-device` | T5 |
| `make test-hil` | T6 |
| `make soak` | T7 |

**Hooks and CI:**

- A pre-commit hook runs format and T1.
- GitHub Actions runs T0–T4.
- The Mac with the Pod and ES-9 attached is a self-hosted runner for T5–T7.

**Rule:** every feature lands with tests at the deepest tier it can reach. Every phase in §14
states its **host-pass** and **device-pass** criteria.

**Determinism:** the engine uses seeded random numbers and fixed-point playheads, and never reads
the wall clock. Host renders are therefore bit-exact, and device output matches within a stated
tolerance.

Retro capture brings arithmetic into rendered audio, so three rules keep floating point identical
across compilers, CPUs and FPU modes (ADR 0005). Golden renders blessed on macOS (arm64) must then
match on Linux (x86-64) byte for byte:

- The engine is built with `-ffp-contract=off` on every target. No compiler fuses a multiply and
  an add on its own; `std::fma` does it explicitly where wanted.
- Rendered paths call no libm transcendental functions. Curves come from our own polynomial or
  table.
- **No engine arithmetic produces or consumes a subnormal.** Rack's engine thread flushes them to
  zero, while the host and the M7 keep them, so results would otherwise depend on the FPU mode.
  - Recursive state below 2⁻⁵⁰ (−301 dBFS) snaps to exactly zero.
  - Audio entering an arithmetic path, and every value passed between processing stages, is
    flushed the same way. The live path's copy stays bit-exact.
  - Within a stage, nothing multiplies more than two such values. A product of two accumulations
    is formed from their square roots: the seam correlation is normalised by √Σx² · √Σy², not
    √(Σx² · Σy²). At the floor, that product would be 3.6 × 10⁻⁵⁴, below even the smallest
    subnormal.

  This is also what makes T2's "no denormals" invariant true.

---

## 13. Hardware plan

### 13.1 Pod (development)

- libDaisy **v9.0.0** (pinned) detects the Seed3 and its TAC5242 codec, and adds 192 kHz support, so one firmware serves both. Seed 1.x is tested first because it's already in the Pod.
- **Additions on the header:**
  - a gate clock jack and BJT input (Fig 2.4) on D9 (CLOCK) and D10 (RESET)
  - the TRS MIDI in that already exists
- **Level caution:** Pod line inputs expect about 1 Vrms. ES-9 stimulus is capped in software, or
  goes through a passive attenuator, for Pod tests.

### 13.2 Module (P4)

| Area | Design |
|---|---|
| Power | ±12 V Eurorack → VIN using datasheet Fig 1.2 (reverse protection, filtering) |
| Audio I/O | Eurorack-level circuits, Figs 3.6 and 3.7 (100 k / 10 k scaling, 10 Vpp ↔ codec range) |
| CV in | Bipolar ±5 V → 0–3.3 V, Fig 4.3 (MCP6004, LM4040 reference) |
| Gate in | BJT circuit, Fig 2.4 |
| CV / clock out | DAC with gain stage, Fig 6.1 |
| Cue out | Second stereo codec on SAI2 (D24–D28) |
| Storage | microSD, Fig 5.1, routing rules in datasheet §SDMMC |
| Controls | Pots through CD4051 multiplexers (Fig 4.2); CVs direct to ADC pins |
| Test | Test header, output → input loopback points, factory self-test and ADC calibration mode |

Pin allocation against the Seed3 pin table is part of P4.

---

## 14. Roadmap

Each phase has **host-pass** criteria (T0–T4) and **device-pass** criteria (T5–T7).

### P0 — Foundations

**Host:**

- Repo, tooling, CI and pre-commit hook
- Engine skeleton with passthrough
- Tests, render tool, Rack plugin (passthrough) and firmware cross-compile (Pod, Seed 1.x)

**Rig:**

- On-device test runner over SWD/RTT
- Telemetry and bootloader-reboot command
- ES-9 HIL harness, with a passthrough latency and frequency-response test
- Self-hosted runner

**Device:**

- Pinned upstream libDaisy, with Seed3/TAC5242 support confirmed
- Passthrough on Seed 1.x and Seed3
- USB MIDI device and dev UI loopback

**Pass criteria:**

| Side | Criteria |
|---|---|
| Host | All targets build warning-free. Passthrough is bit-exact in tests and the render tool. The plugin loads in Rack. |
| Device | Passthrough measured on the rig. Latency recorded. Zero overruns over 1 h. |

### P1 — Core looper

Clock tracker (gate, MIDI, internal), retro capture, 2 slots, crossfader, seam crossfade, Tape
mode with drift correction, latency calibration.

**Progress:**

| Item | State |
|---|---|
| Clock tracker | Done: PRs #1, #4 |
| Engine and Rack clock wiring, clock out | Done: PR #2 |
| Retro capture, seam crossfade, bar-locked playback | Designed (v0.3). In four PRs: the tracker's edge times (done: PR #7); the paged tape, recorder, DC blocker and `-ffp-contract=off` (done: PR #10, host only; Rack records onto a tape from PR #13); next, capture and playback; then the Rack controls |
| 2 slots, crossfader | To do. Keep one capture in flight engine-wide (§3.4): the 187-page bound (§2.3) relies on it, and two in flight would need 4 × 47 + 46 = 234 pages |
| Tape mode and drift correction | To do |
| MIDI clock, internal tempo, tap tempo | To do |
| Latency calibration | Deferred (hardware) |

| Side | Criteria |
|---|---|
| Host | Each loop end on the sample nearest its true bar line, except where that line lies within 0.05 samples of a half sample, where the neighbouring sample is allowed (§3.4). No drift over 64 bars. Phase error < 1 sample at a steady clock and < 5 samples through a ±10% tempo ramp (**clock tracker: met**: < 0.01 steady, 0.6 ramp). Click metric ≤ 1.1, and the seam envelope within ±1 dB, on the full test-signal set (§5.6). |
| Device | Capture alignment vs. clock edge within ±1 sample at the jacks. No drift over 64 bars. |

### P2 — Performance

4 slots, overdub/replace/multiply, undo/redo, quantized auto-fade, resample capture, first loop
sets tempo with clock out, USB-host controllers (stretch goal).

### P3 — Effects and stretch

DJ filter, stutter, reverse/varispeed, delay, then WSOLA, then HQ phase vocoder, then Slice, then
granular and reverb. Each effect has CPU budgets enforced in T5.

### P4 — Module hardware

Schematic, PCB, panel, cue codec and SD save/load.

---

## 15. Open questions

1. **The page budget for P2 (§2.3).** At the longest loops, four slots plus undo need more pages
   than the pool holds. The options:
   - cap the loop length
   - refuse when the pool is full
   - shrink the FX share

   int16 storage would double the time, at a quality cost.
2. FX routing details. Working assumption in §8: global chain, cheap effects per slot, reverb and granular as sends.
3. Crossfader as a slider or a knob on the module.
4. Panel width (14 vs. 20 HP), and display vs. no display (§9.3), including panel variant vs. header option.
5. Whether USB-host controller support ships on the module.
6. Undo depth beyond one level (each level holds a loop's pages: up to 47, 11.75 MiB).
7. When to move the firmware to the QSPI bootloader layout. Deferred with the rest of the
   hardware work; `-Os` comes first if flash runs out (§2.3).
8. **Bar-locked sync points (§3.6).** P1 locks every bar. Locking every beat would make smaller,
   more frequent moves. To be measured in Rack.
9. **MIDI clock and the bar-line estimate (§3.4).** Jittery sources need their own tolerance,
   and smoothing.
10. **Rack's tape and sample-rate changes (§2.3).** Decided in PR #13: one tape per module,
    allocated when it is created and sized for 192 kHz, with the engine rebuilt over it at every
    rate. A rate change starts the history again, and will drop loops once they exist (§2.3).
    - Rack can change the rate on its audio thread: the Audio module calls
      `setSuggestedSampleRate` from `processInput`, and the plugin rebuilds its engine in
      `onSampleRateChange`. A tape's bytes depend on the rate.
    - Rack offers rates up to 768 kHz, where 210 pages take 840 MiB per instance.
    - Also considered:
      - allocating a new tape on the UI thread and handing it over, which leaves a gap with
        nothing recorded and needs a handover between threads;
      - reallocating at every change, which allocates on the audio thread;
      - sizing the tape for 768 kHz, so that every rate holds 210 pages: 840 MiB of address space
        per instance, the same resident memory at a given rate, but 840 MiB of Windows commit.
        Not taken, since nobody performs at 768 kHz; what the 192 kHz tape holds above that
        rate is in §2.3.

---

## 16. References

- `docs/vendor/Daisy_Seed3_datasheet.pdf` (v2.1): pinout, power, audio, ADC, DAC, SDMMC and MIDI
  circuits
- `docs/vendor/Pod_databrief-5-14-26.pdf`: Pod controls and pin mapping
- libDaisy (`MidiUsbTransport`, `USBHostHandle`, SDRAM `DSY_SDRAM_BSS`); DaisySP; CMSIS-DSP
- Prior art: tap-o-matic / OAM Time Machine (Daisy SDRAM buffer pattern)
- Time stretching: SoundTouch (WSOLA); Laroche & Dolson, *Improved phase vocoder time-scale
  modification of audio* (1999); Signalsmith Stretch; Rubber Band
- C++ Core Guidelines; MISRA C++:2023
