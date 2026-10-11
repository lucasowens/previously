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

**Paged tape** (P1, planned; ADR 0004). All loop audio lives in one pool of fixed-size **pages**.
Audio is never copied; loops hold pages:

- **A page** holds 2¹⁵ stereo frames: 0.68 s at 48 kHz, 256 KiB of float32. The size is a power of
  two, so a position inside a page is a shift and a mask. A modulo by a run-time length would be a
  software 64-bit division on the M7.
- **The recorder** writes the input into pages without pause. It takes a free page whenever the
  current one fills, and gives a page back once it is older than the history and no loop holds it.
- **A loop** holds the ids of the pages its window touches, with a reference count on each. When
  the loop goes, its pages go back to the pool.

| SDRAM | Size | Role |
|---|---|---|
| 210 pages | 52.5 MiB | Audio: the history, plus loops |
| Remaining | 11.5 MiB | FX (delay up to 10 s stereo = 3.8 MB), stretch scratch |

**History.** The history is the newest 46 pages. Once the recorder has filled them, 45 are
complete: **30.7 s**. A capture of N bars reaches back N bars from its end line, and the end line
can be up to a bar before the press (§3.4). The history therefore covers a capture whenever
N + 1 bars plus the pre-roll (50 ms, §5.3) fit in 30.7 s. In 4/4:

| Bars | Covered at any tempo of at least |
|---|---|
| 1 | 16 BPM |
| 2 | 24 BPM |
| 4 | 40 BPM |
| 8 | 71 BPM |

**Loops.** The 164 pages beyond the history (112 s) hold loops:

- A loop is never longer than the history; a capture that would be is shortened. With its
  margins and up to two partial pages, a loop therefore touches at most 47 pages.
- A capture is admitted only if every loop's pages plus the history still fit. The recorder
  therefore never runs out, and the history is never interrupted, however many loops exist.
- P1 needs at most 3 × 47 + 46 = 187 pages (two slots, one of them being replaced). P2's four
  slots and undo need a policy (§15).
- On the host and in Rack the pool is sized for the sample rate. Pages are counted in frames, so
  at 96 kHz a page lasts 0.34 s and the history needs twice as many.
- The engine never touches the pool before its first `process()`. On the Daisy, SDRAM is
  configured inside `hardware.Init()`, after static constructors have run.
- These numbers become `static_assert`s with the implementation.

**Precision rule:** absolute time is **never stored in a float**. A float has a 24-bit mantissa,
so it stops resolving individual samples after 2²⁴ samples (about 350 s at 48 kHz). Positions are
`int64_t` sample counts, and fractional positions are fixed-point (Q32.32). §3.6 covers this.

**Flash.** The STM32H750 has only **128 KB of internal flash**, and the firmware is filling it:

| Stage | Flash used | Mostly |
|---|---|---|
| P0 passthrough | 61% | libDaisy and newlib |
| With the clock wiring and hiccup handling (P1) | 73% (95 KB) | About 11 KB of engine and tracker code at `-O3` with LTO |

**Hardware work is deferred** (decided after PR #2): development stays host- and Rack-first. The
build fails if the flash budget is exceeded (T0). When it does:

1. Build the firmware for size (`-Os`). This is hardware-free, typically recovers 20–30%, and the
   speed cost is measured and reported.
2. Then move to libDaisy's **bootloader** layout: the program lives in the 8 MB QSPI flash and
   runs from SRAM.

**Internal SRAM.** Fixed engine state lives in internal SRAM, not SDRAM:

| Region | Where it lives |
|---|---|
| AXI SRAM, 512 KB | Default under the current linker script |
| DTCM, 128 KB | Shared with `.data` and the stack under the QSPI bootloader layout; `.bss` is already about 52 KB there |

Budget items so far:

| Item | Size | Note |
|---|---|---|
| Clock tracker history | 8 bytes × capacity | The owner provides it: 256 entries (2 KB) cover 16 bars at 4 PPQN; 1,536 (12 KB) at 24 PPQN |
| Clock tracker edge times (P1, planned) | 8 bytes × capacity | Each pulse's raw edge, for capture (§3.4): another 12 KB at 1,536 entries |
| Loop page tables (P1, planned) | 2 bytes per page | At most 47 pages per loop |

The firmware owner places these buffers explicitly. Today T0 checks only flash. Adding an SRAM
budget check from the linker's memory report is an open item, due before the engine state grows
past a few KB.

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
  - A history of filtered pulse times, in storage provided by the owner, answers questions
    about the past; extrapolation answers questions about the future.
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
  - two equal long gaps mean a slowed clock: the fill-ins are retired
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
  - **edge times**, for capture. Next to each filtered time, the history keeps the pulse's raw
    edge; a filled-in pulse keeps its predicted time. The filtered times serve the future, and
    capture estimates the past from the edges (§3.4). The change only adds; nothing existing
    changes.

### 3.4 Retrospective capture (the headline feature)

*Designed for P1; implementation in progress (§14).*

The input is **always being recorded** onto the paged tape (§2.3). A capture copies nothing and
never needs arming. It chooses a window of whole bars, pins the window's pages, and plays them.

#### Which bars

- **The press.** A CAPTURE press asks for the last **N bars** (1, 2, 4 or 8). It comes from the
  button or the gate input:
  - edges with sample-accurate timestamps (Rack's button and gate, the device's gate input)
    arrive with the block, like clock edges;
  - presses from a UI or MIDI thread arrive as commands at the start of a block (§10.1).
- **The end line** is the next bar line if it is at most ¼ beat away (an early press, "on the
  one"). Otherwise it is the most recent bar line.
  - ¼ beat is a starting value, to be tried in Rack.
- **The window** is `[end − N bars, end)`. Each edge of the window sits at a bar line's estimated
  time (below) plus L_in (§3.5), rounded to the nearest sample (halves up).
- **Around the window:** before it lies the **pre-roll**, at least 50 ms (§5.3). After it there
  is a 50 ms read margin.
- **One labelling.** While a reset is pending, bars are numbered as they will be once it applies
  (§3.3). A pending capture is planned again whenever the labels change.
- **Freewheeling.** While the clock is stopped, fill-ins count as edges, so a capture cuts on the
  freewheeling grid.

#### Where a bar line is

The tracker's history holds *filtered* pulse times, which serve prediction. The past is better
estimated from the raw **edges** on both sides of a line (§3.3). Each bar line is estimated from
the 8 edges on either side of it:

1. **Fit each side.** A repeated-median line goes through each side's 8 edges. A repeated median
   ignores up to 3 bad edges of 8.
2. **Keep an edge that agrees.** If the line's own edge lies within 1 sample of either side's
   line, it stays. It is an ordinary pulse, or the start of a phase shift or tempo change, which
   the side after it confirms.
3. **Repair an edge that doesn't.** Correct both lines for the curvature their slopes reveal. On
   a ramp both lines bend the same way, by a fixed multiple of the curvature. If the corrected
   lines then agree within 1 sample, the clock came back to its grid, and the line lies at their
   mean. This repairs hiccups and bursts.
4. **Otherwise keep the edge.**

Measured on the gate clock at 4 PPQN and 120 BPM. Errors are in samples, the worst over a line
and the 3 pulses either side:

| Case | History | Edge | Estimate |
|---|---|---|---|
| One pulse off by 0.2 period (tracked as jitter) | 1,080 | 1,200 | 0.01 |
| One pulse off by 0.45 period | 2,700 | 2,700 | 0.01 |
| A burst of 2 to 4 off-grid pulses, at or around the line | 1,800 | 1,800 | 0.05 |
| A phase shift of 0.2 period, at or 2 pulses before the line | 120 | 0.01 | 0.01 |
| Tempo × 1.1 from the line | 60 | 0.01 | 0.01 |
| Tempo ramps from −30% to +50%, linear or exponential | 0.64 (10% over 8 bars) | 0 | 0 |

- **When a loop is final.** The estimate needs the 8 pulses after a line, so a loop's end, and
  so its length, is final 8 pulses after its end line: 2 beats at 4 PPQN. If the clock stops,
  fill-ins arrive from 2.5 periods later (§3.3), so a loop always becomes final.
- **Short history.** A line with fewer than 8 edges on a side keeps its edge. That happens only
  when the clock has just started.
- **MIDI clock is not yet measured.** Its jitter would pass through unsmoothed. MIDI's tolerance
  and smoothing are chosen when MIDI clock is wired.

#### When the loop starts

- **Early press:** the new loop's first bar starts exactly on the end line, entered from its own
  pre-roll like every downbeat (§5.2). Its start was settled long before.
  - Its length becomes final before its first wrap whenever a bar has more than 8 pulses (4 PPQN
    in 4/4 has 16).
  - With fewer pulses per bar, the first wrap uses the raw end edge.
- **Late press:** the loop starts at once, mid-bar, where it would be had it started on the end
  line. `crossfaded_seek` fades it in (§5.4).
- **Replacing a playing loop** uses the same two moves: an early press hands over on the line,
  and a late press hands over with a seek.
- The loop then plays bar-locked (§3.6).

#### Pinning and limits

- **Pinning.** A press pins every page it may need at once, so the start can't age out of the
  history while the end waits to become final.
- **Shortened captures.** A capture is shortened to the largest of 1, 2, 4 or 8 bars that the
  history and the clock both cover. This happens just after power-up, or when the clock started
  recently.
- **Refused captures.** A capture is refused when not even one bar fits, when there is no tempo
  yet, or when the pool can't take its pages (§2.3). The loop report says which.
- **The bar count** is taken at the press.
- **A new press:**
  - an early press waiting for its line is superseded by the new one;
  - a press during an entry crossfade waits for that crossfade to finish;
  - otherwise the new capture replaces the playing loop as above.
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
  - **Which bar is due.** At grid bar *g*, loop bar `(g − c) mod N` is due, where *c* is the end
    bar modulo N. The loop's first bar therefore plays in the bar right after the bars it
    captured, and the loop keeps its place in an N-bar phrase across resets.
  - **At each line,** the playhead is compared with where the due bar starts. Within 1 sample it
    plays on. Otherwise it moves by the difference, just before whichever comes first: the line,
    or the natural downbeat. The move is a crossfade from the audio that preceded that downbeat
    (§5.2).
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
the recording. The wrap is one case. The others use the same crossfade, also ending exactly on
the line:

- **A bar-locked move (§3.6):** from the current audio into the audio before the due bar.
- **A new loop's first bar** (an early press, §3.4): from silence, or from the loop it replaces,
  into the new loop's pre-roll.

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
  absorbs a bar-locked slowdown at the wrap of up to 50 ms − F. A larger slowdown repeats the end
  of the loop's own last bar instead.
- **The read margin is never played at native speed,** because it begins with the next bar's
  downbeat.
- **Too little pre-roll** (a capture just after power-up): the seam shortens to what exists.
  Below 2 ms, a starting value, the loop fades out and in instead.
- **A silent pre-roll** (a loop captured from the moment the music started): the seam is an 8 ms
  fade to silence before each first downbeat. It is pinned as a stated exception (§5.6).

### 5.4 `crossfaded_seek`

Every playhead discontinuity uses one of two primitives, so there are exactly two places to get
this right:

- **Entering a downbeat (§5.2)**, for anything that lands on a known line: wraps, bar-locked
  moves, early-press starts and quantized launches.
- **`crossfaded_seek`**, for everything else: a mid-bar start (a late press, §3.4), a line
  learned after it passed (§3.6), and later stutter, slice re-trigger and reverse. A second read
  head starts at the target position, and an equal-power crossfade hands over. It lasts 3 ms to
  start with, measured before it is pinned.

### 5.5 Smoothing and DC

- Every gain, mute, record level, feedback level, punch-in and punch-out is ramped over 5–20 ms.
- Crossfader and CV inputs are filtered and slew-limited.
- Slice playback uses a 0.5 ms attack (keeping transients) and a few-ms release.
- Inputs pass through a **DC blocker** (about 5 Hz one-pole high-pass) *before* recording. P1
  puts it on the record path only. The live path stays a bit-exact passthrough until the mixer
  gives it the same blocker.

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

Each signal goes through every way a loop can enter a downbeat: the wrap, a bar-locked move, an
early start and a replacement. It also goes through `crossfaded_seek`. The one stated exception
is a silent pre-roll (§5.3).

---

## 6. Signal flow and mixing

```
                ┌──────────── DC block ────────────┐
 IN L/R ───────►│                                  ├──► tape (always recording, §2.3)
                └───────────────┬──────────────────┘
                                │ live bus
                                ▼
  slot 1 ─┐                ┌─────────┐
  slot 2 ─┼─► loop bus ───►│ FX      ├──► loop ──┐
  slot 3 ─┤   (levels,     │ insert  │           ▼
  slot 4 ─┘    mutes)      └─────────┘     ┌────────────┐
                                live ─────►│ crossfader │──► OUT L/R
                                           │ equal-power│
                                           └─────┬──────┘
                    capture source ◄── Input (dry) or Mix (what you hear)
                    cue bus (module only) ◄── pre-fader slot audition ──► CUE L/R
```

- **DC block:** in P1 it sits on the record path only (§5.5).
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
- **Memory (P1, planned):** the engine receives one memory region (SDRAM on device, a heap block
  on host) at construction and carves it into pages (§2.3).
  - **Nothing is allocated after construction**, and the region is not touched before the first
    `process()`.
  - Without a region, captures are refused and the engine behaves as it does today.
  - Members are declared so the pool outlives every loop that holds pages. The pool's destructor
    expects every count to be zero.
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

Retro capture brings arithmetic into rendered audio, so two rules keep floating point identical
across compilers and CPUs (ADR 0005). Golden renders blessed on macOS (arm64) must then match on
Linux (x86-64) byte for byte:

- The engine is built with `-ffp-contract=off` on every target. No compiler fuses a multiply and
  an add on its own; `std::fma` does it explicitly where wanted.
- Rendered paths call no libm transcendental functions. Curves come from our own polynomial or
  table.

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
| Retro capture, seam crossfade, bar-locked playback | Designed (v0.3). Next, in three PRs: the paged tape, recorder, DC blocker and tracker edge times; capture and playback; the Rack controls |
| 2 slots, crossfader | To do |
| Tape mode and drift correction | To do |
| MIDI clock, internal tempo, tap tempo | To do |
| Latency calibration | Deferred (hardware) |

| Side | Criteria |
|---|---|
| Host | Loop length error = 0 samples over 64 bars. Phase error < 1 sample at a steady clock and < 5 samples through a ±10% tempo ramp (**clock tracker: met**: < 0.01 steady, 0.6 ramp). Click metric ≤ 1.1, and the seam envelope within ±1 dB, on the full test-signal set (§5.6). |
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
