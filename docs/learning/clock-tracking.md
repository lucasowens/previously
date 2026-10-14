# Clock tracking: from noisy pulses to a sample-accurate grid

How `clock_tracker` works, and why it's built the way it is.

**Code:**
- [`sample_time.hpp`](../../engine/include/previously/sample_time.hpp)
- [`clock_tracker.hpp`](../../engine/include/previously/clock_tracker.hpp)
- [`clock_tracker.cpp`](../../engine/src/clock_tracker.cpp)
- [`test_clock_tracker.cpp`](../../tests/test_clock_tracker.cpp)

---

## 1. What problem are we solving?

A Eurorack clock is a stream of short pulses on a gate input, typically 4 per beat (16th notes).
MIDI clock is 24 messages per beat. From that stream, Previously must answer:

- **Past:** "when did the bar start, 4 bars ago?" This is what retrospective capture needs: the
  loop window is cut from the always-recording buffer at that moment.
- **Future:** "when does the next bar start?" Quantized launches, fades and stutters need this.

**"Sample-accurate" means the answers are right to well under one sample** (about 21 µs at
48 kHz). Get it wrong and every loop seam drifts, flams, or clicks.

---

## 2. Representing time: why not a float?

A `float` has 24 bits of mantissa. It can count exactly up to 2²⁴ = 16,777,216, which at 48 kHz is
**349 seconds**. After that, adding 1 to a float timestamp sometimes does nothing at all. A live
set runs for hours.

`double` would last longer, but the Cortex-M7's FPU is single-precision. Every `double` operation
becomes a slow software routine, and we ban them on device.

**The solution is fixed point: an integer that counts fractions.** `sample_time` stores a 64-bit
integer of *ticks*, with 65,536 (2¹⁶) ticks per sample. That's called Q47.16: 47 bits of whole
samples, 16 bits of fraction.

| Property | Value |
|---|---|
| Resolution | 1/65,536 sample ≈ 0.3 nanoseconds |
| Range | 2⁴⁷ samples ≈ 93 years at 48 kHz |
| Addition and subtraction | Exact integer arithmetic. Adding a period a million times equals multiplying it by a million, to the tick (tested). |

If you know C#, this is exactly why `TimeSpan` and `DateTime` store a `long` count of 100 ns ticks
instead of a `double` number of seconds.

**Floats are still used, but only for *small differences*:** an error of a few samples, or a gain
applied to it. The result is rounded straight back into ticks. Small numbers are where floats are
precise.

---

## 3. The grid is integers

Every pulse gets an **index**: 0, 1, 2, ... A bar is `index / pulses_per_bar` (16 at 4 PPQN in
4/4). The grid itself is exact integer arithmetic. Only the *time attached to each index* is an
estimate.

Two details make this robust:

- **Floor division.** Before a reset, pulses have negative indices (-1, -2, ...). C++'s `/` rounds
  toward zero, which would put pulse -1 in bar 0. `floor_divide` rounds toward minus infinity, so
  pulse -1 is the last pulse of bar -1. A test exists for exactly this, written after a planted
  bug showed it was missing (§7).
- **Relabelling instead of moving.** History is stored by arrival *sequence*, and the public index
  is `sequence - index_offset_`. A reset just changes the offset: O(1), no data moved.

---

## 4. Filtering: the alpha-beta tracker

Pulse timestamps are noisy:

| Source | Jitter |
|---|---|
| Eurorack gate | About 0.005 samples (interrupt latency) |
| MIDI clock | Up to about 1 ms (≈ 48 samples), because a MIDI byte takes 320 µs on the wire and senders are sloppy |

We want a smooth, *predictable* grid without losing real tempo changes.

The tracker keeps two numbers, the predicted time of the next pulse and the current period. When a
pulse arrives:

```
error     = actual - predicted        how wrong was the prediction?
corrected = predicted + α·error       believe the measurement by a fraction α
period    = period + β·error          believe a tempo change by a fraction β
predicted = corrected + period        predict the next pulse
```

- **α (alpha)** says *how much to trust each new edge*. α = 1 means the corrected time is exactly
  the measured time. α = 0.1 means "mostly keep my prediction, nudge it 10% toward the measurement".
- **β (beta)** says *how much to trust a tempo change*. If pulses keep arriving early, the period
  shrinks.

This is the textbook α-β filter (radar tracking). It's also equivalent to the second-order
**DLL (delay-locked loop)** that JACK uses to smooth audio-driver timing. See Fons Adriaensen,
*Using a DLL to filter time* (2005).

### Choosing β: critical damping

α and β can't be chosen independently. Pick β too large for a given α and the loop overshoots and
rings after every tempo change, like a car with worn shock absorbers. The **Benedict–Bordner**
relation β = α² / (2 − α) gives critical damping: the fastest settling without overshoot. So the
config exposes only α (`phase_gain`), and β is derived.

### Starting fast: least-squares gains

With only a handful of pulses, steady gains would waste data. A low α means the very first,
possibly noisy, interval sticks around for many beats. So for the *k*-th pulse after acquisition
the tracker uses the gains that make it an exact **least-squares line fit** through all k points:

```
α_k = 2(2k − 1) / (k(k + 1))        β_k = 6 / (k(k + 1))
```

| Pulse k | What happens |
|---|---|
| k = 2 | α = β = 1: the line through two points, an exact tempo after two pulses |
| k = 3 | α ≈ 0.83, β = 0.5: averaging begins |

The tracker uses whichever gains are larger, least-squares or steady, so it hands over smoothly.
The test "MIDI tempo is accurate within the first beat" checks this across 20 random jitter seeds,
and fails if the least-squares schedule is removed.

---

## 5. The central trade-off: smoothing vs. following

**There is no free lunch.** A filter that ignores noise also ignores real change for a while.
Under a steady tempo *ramp* (the period changing a little every pulse) an α-β tracker settles into
a constant **lag**.

Measured with this code (gate clock, 4 PPQN; ramp = 10% over 8 bars):

| α | Lag on ramp | Gate jitter, RMS gain | MIDI-size jitter (±48 samples, RMS 27.7) |
|---|---|---|---|
| 1.0 | 0.0 samples | ×1.00 | 27.7 |
| **0.9** | **0.6** | **×0.92** | 25.5 |
| 0.5 | 13.5 | ×0.65 | 17.9 |
| 0.3 | 60 | ×0.49 | 13.7 |
| 0.1 | **773 (16 ms)** | ×0.31 | 8.6 |

At α = 0.1 the timing looks beautifully smooth in a steady test, but during a tempo ramp the grid
trails by 16 ms, which is an audible flam. That is far worse than the 1 ms jitter it removes.

**So the presets are measured, not guessed:**

| Preset | α | Why |
|---|---|---|
| `clock_tracker_config::gate()` | 0.9 | Gate edges are already clean, so trust them. Ramp lag stays under a sample, and noise is not amplified (×0.92; theory gives 0.916). |
| `clock_tracker_config::midi()` | 0.2 | At 24 PPQN each pulse covers less of a ramp, so lag is smaller. Measured on a *jittery ramp*, α = 0.2 has the lowest total error: 10.8 samples RMS vs 27.7 raw, with 4 samples of ramp lag. α = 0.1 was worse overall (18.7) because its lag dominated. |

### Why "no amplification" is an RMS statement

My first jitter test asserted *max filtered error < max raw jitter*, and it failed (0.052 vs 0.05).
**The test was wrong, not the tracker.** A filtered edge is a weighted mix of several recent noisy
edges. On average (RMS) that mix is quieter, but occasionally several unlucky edges line up, and
the peak exceeds any single edge's worst case. The test now checks the right property: RMS not
increased, and peak within a sensible bound.

---

## 6. Robustness: only real edges measure tempo

The first version of this section described rules that looked right but broke under scenarios
I hadn't tried. A code review on PR #1 reproduced five of them:

- a tempo drop of 3× was never acquired
- a clock returning at a new phase reported a 40% wrong tempo for a beat
- an exact doubling lost pulses
- a stray edge deafened the tracker for seconds
- a reset delivered just before a slightly late edge missed it

They all traced back to one mistake: **filled-in freewheel pulses were being treated as
measurements.** The rules now follow a single principle:

> **Tempo is only ever measured between two real edges.** Filled-in pulses keep the grid counting;
> they never measure anything.

| Situation | Detection | Response |
|---|---|---|
| Double trigger (a bouncing gate) | Less than **4 ms** after the previous real pulse | Ignored. Absolute time, not "half a period", so an exact tempo doubling is never mistaken for bounce. |
| Stray first edge (patching a cable) | While acquiring, an interval over **4 s** | The stray edge is replaced by this one; acquisition restarts |
| Outlier | Prediction error over **¼ period** | Recorded where it really happened, but the tempo is kept and the grid keeps predicting from the old phase. The *next* pulse decides, by comparing intervals between real edges. |
| Hiccup (one late or early pulse) | The next pulse lands back on the old grid | Nothing changes: the tempo and phase were never disturbed (issue #3) |
| Phase shift | The next interval matches the **current period** (within ¼) | The clock carried on at the same tempo from a new phase: re-anchor the phase only. Checked before the tempo-change rule, so a shift of ¼–⅓ of a period isn't briefly mistaken for a new tempo. |
| Tempo change | The next interval matches the **outlier's** (within ¼) | Re-acquire from that interval. Three real edges agree, so the fit starts at 3 and the very next pulse already has hiccup immunity. The pulse count continues. |
| Provisional tempo | A tempo resting on one interval (fewer than 3 pulses in the fit) | An outlier re-acquires at once: the first interval is as likely to be wrong (a stray edge) |
| Clock stopped | No pulse for **2.5 periods** | **Freewheel:** fill in pulses at the last tempo. Each fill-in commits half a period after its slot, so a late real pulse still claims its slot. Judged against the current period even while an outlier is pending, so a hiccup just before a short stop doesn't change how the return is numbered. The known cost: a halving whose confirming pulse is more than ¼ late (intervals 2P then 2.6P, which genuinely don't agree) reads as a stop and keeps two filled-in pulses until the next reset. Pinned by a test. |
| Clock returns | A real pulse while freewheeling | **Re-anchor phase only:** the pulse may land anywhere in its slot, so the gap measures phase, not tempo. The *next* real pulse measures the tempo from two real edges. |
| Clock slowed by 2.5× or more | Two consecutive long gaps of the same length (within ¼) | It slowed rather than stopped: **retire the fill-ins**, re-record both real pulses, re-acquire. The count matches the real pulses. |
| Very long silence | More slots closed than the history holds | Skipped in O(1); at most `history_capacity()` fill-ins are written |
| Reset input | Within **2 ms** of a pulse's *raw* edge, delivered before or after it | That pulse becomes index 0. A reset delivered late (after later pulses) labels the first pulse at or after its timestamp, from the history. While freewheeling, the next fill-in can take the label. |

**Why 2.5 periods, not 2?** An exact tempo halving delivers its first pulse two periods after the
last. With the threshold at exactly 2P, jitter decided whether that pulse was "a tempo change" or
"a stopped clock plus a phantom". A margin wider than the quarter-period tolerance removes the
knife edge.

**Timing contract.** Callers deliver pulses in time order, and deliver each audio block's pulses
*before* calling `advance()` for the end of that block. `pulse(when)` also advances to `when`
itself, so the result never depends on how often `advance()` runs. A test pins this down.

**History storage belongs to the owner.** The history is a `clock_history<Capacity>` provided by
whoever owns the tracker, so the firmware decides which RAM it lives in. 16 bars needs 256 entries
at 4 PPQN but 1,536 at 24 PPQN. The engine keeps 1,536: 24 KB and 192 bytes.

**Two times per pulse.** Each entry holds the pulse's filtered time and its raw edge, plus one bit
saying whether it was filled in:

- **The filtered time** is what the grid runs on: predictions, clock out, `pulse_time()`. It is
  built from earlier pulses only, which is all a prediction can have.
- **The raw edge** is for retro capture. Asked where a bar line *was*, capture can also use the
  pulses that came after it. DESIGN.md §3.4 measured the difference: after a phase shift or a
  tempo change near a line, the filtered times are up to 120 samples off, and the edges 0.01.
- **A fill-in** has no real edge: its edge is the slot it was filled in at, and the bit marks it,
  so capture can leave fill-ins out of its jitter estimate.

The three arrays live in one `clock_history`, so their sizes cannot disagree: a mismatch would not
compile. The tracker sees them as spans.

**Where hiccup immunity does not apply.** It needs a confirmed tempo: three pulses in the fit.
Right after acquisition, the tempo rests on one interval, so any outlier re-acquires. After a
freewheel return, the returning pulse and the next two rebuild the fit, so immunity resumes
from the fourth pulse. Both follow from "re-anchor, then measure", and both are short.

All loops are bounded:

| Loop | Bound |
|---|---|
| History binary search | At most log₂(capacity) steps |
| Freewheel fill-ins per call | At most the history capacity |

This is the realtime rule from the coding standard (§3.3), and a test checks that tracking never
allocates.

---

## 7. How we know the tests are good: mutation testing by hand

A passing test suite only proves something if it would *fail* on wrong code. So each mechanism was
broken on purpose, one at a time, and the suite run against it.

**Round 1** (the first version):

| Planted bug | First result | Action |
|---|---|---|
| Tempo never updates (β = 0) | Caught | — |
| Bars use truncating `/` instead of `floor_divide` | **Survived** | Added "pulses before a reset belong to negative bars" |
| Least-squares acquisition removed | **Survived** | Added "MIDI tempo is accurate within the first beat" |
| Never re-acquires on a tempo jump | Caught | — |
| Never freewheels | Caught | — |
| Simultaneous reset ignored | Caught | — |

**Round 2** (after the review), restoring each *old* behaviour inside the new code:

| Old behaviour restored | First result | Action |
|---|---|---|
| Stop detected at exactly 2P | **Survived** | The slowed-clock rule repaired the count one pulse later, hiding the transient phantom. Added "a tempo halving is a tempo change from its very first pulse". |
| Returning pulse measures tempo | **Survived** | The wrong period lasted one pulse, then healed. The test now checks the period after the first and second returning pulses. |
| Double trigger = half a period | Caught | — |
| No acquisition ceiling | Caught | — |
| Slowed clock never retires fill-ins | Caught | — |
| 64-fill cap per call | Caught | — |
| Pending reset never matches a real edge | Caught | — |
| Late reset not looked up in history | Caught | — |
| No index range guard | Caught | — |
| Windows may round to zero samples | **Survived** | The test used 500 Hz, where the window is still 2 samples. Moved to 100 Hz. |
| Period floor in `lock_on_interval()` removed | **Survived** | Unreachable: every caller has already rejected shorter intervals. Replaced with a stated `expects()` precondition rather than kept as dead code. |
| Period floor in `track()` removed | Survived, **kept** | Unreachable with real input (a correction is at most ¼ period), but a zero period would divide by zero on the device. Labelled in the code as a defensive guard. |

**Lesson, twice over:** two of the round-2 survivors weren't missing behaviour, they were
*transients*. The end state was correct but the path was wrong for one pulse, and in follow mode a
one-pulse period error is an audible pitch jump. Tests must check the moments that matter, not just
the final state.

**Round 3** (the hiccup fix, issue #3). The same lesson, a third time:

| Planted bug | First result | Action |
|---|---|---|
| Prediction after an outlier restarts from the outlier, not the old grid | **Survived** | The end state was identical. A test now checks the *prediction* right after a hiccup. Strengthening it exposed a real bug: public extrapolation was anchored on the latest recorded pulse (the off-grid hiccup), not on the prediction, so reports and fill-ins could disagree by the hiccup's offset. Every extrapolation now anchors on the prediction. |
| Outlier interval not forgotten on return to the grid | **Survived** | A second identical hiccup went through two bogus re-locks that happened to land back on the right period. The test now checks the period **at every pulse**. |
| No least-squares acquisition | **Survived** (was caught before) | The new outlier handling also converges by pulse 24, so the end-of-beat check stopped separating them. Measured over 200 seeds, least squares matters most mid-beat (worst 8.4 vs mean 18 samples after 9 pulses); the test now checks there too. |
| Lookup ignores the gap before the prediction | **Survived** | Only an early hiccup leaves a gap of more than a period to the next prediction. A lookup inside that gap is now tested. |
| Outliers re-lock immediately (the old behaviour), no provisional tempo, extrapolation anchored on the latest pulse | Caught | — |

**Round 4** (the review of the hiccup fix, PR #4). The review found a one-pulse gap in immunity
after a confirmed tempo change, phase shifts taking three pulses, two permanent phantoms in a
compound case, and an overflow. Each fix got a test, then each fix was removed to check it:

| Fix removed | First result | Action |
|---|---|---|
| Phase-shift verdict | **Survived** | The end state and the period were identical; what the verdict changes is the *prediction* during the transition (clock out on the new phase after two pulses, not three). The test now checks the prediction after each shifted pulse. Fourth time the same lesson: check the moments that matter. |
| Fit of 3 after a confirmed change | Caught | — |
| Stop detection ignores the pending outlier | Caught | — (fix 3 was later reverted: the follow-up review showed it moved the inconsistency to the commoner case of a hiccup before a short stop, where the misnumbering grows with the hiccup; both cases are now pinned) |
| Overflow guard (UBSan) | Caught | — |
| "Same length" tolerance widened to a half | Caught | — |

**Round 5** (edge times, PR #7). Before the round, reading `edge()` found a lower bound that no
test could fail: an index more than 2⁴⁰ pulses old. Labels are a sequence minus an offset that is
never negative, so the conversion cannot overflow downwards, and the oldest-sequence check refuses
such an index anyway. The bound was removed, and `relabel()` now states the offset as a
precondition.

| Planted bug | First result | Action |
|---|---|---|
| Tracking stores the filtered time as the edge | Caught | — |
| A stray first edge replaces only the filtered time | Caught | — |
| Fill-ins not flagged | Caught | — |
| A flag is set but never cleared | Caught | Only where a slot is reused: after a retirement, and where stops wrap the history |
| The pulse returning from freewheel flagged as a fill-in | Caught | — |
| The pulse re-recorded at a retirement flagged as a fill-in | Caught | — |
| `edge()` ignores a pending reset | Caught | — |
| `edge()` has no upper bound | Caught | Also by UBSan: an absurd index overflowed on its way to a sequence |
| `edge()` has no lower bound | Caught | — |
| Every flag in one word; the flag read from the wrong bit | Caught | — |
| Flag words rounded down, not up | **Survived** | With 33 pulses the missing word fell in the struct's padding, so nothing read or wrote out of bounds. Now pinned at compile time: `static_assert`s on the word count, and on the engine's history size (24,768 bytes). |

**Lesson, a new one:** a test cannot see a bug that writes where nothing else reads. When a
property is arithmetic on sizes, prove it at compile time.

The review then found its mirror image: a read of memory nothing had written. Setting one flag
reads the rest of its word, so a history declared without `{}` read uninitialized words, which is
undefined behaviour. No result changed, since every recorded slot writes its own bit before it is
tested, and neither ASan nor UBSan reports such a read. The flags now start at zero however the
history is declared, and a test builds one over memory full of ones to show it.

---

## 8. Getting pulses in, and clock out

The tracker only sees timestamps. Two adapters produce and consume them.

### In: timing edges from sampled signals (the Rack adapter)

`edge_detector` is a Schmitt trigger using Rack's conventions: high at 1 V, re-armed only after
the signal falls to 0.1 V, so noise on a high gate never retriggers. Its timing rule rests on what
a sampled signal can actually tell you:

- **A digital step carries no sub-sample information.** The gate is low on one sample and high on
  the next. The module that produced it means "this sample", so the edge is timed at the sample
  where it is first seen. That also lines Rack clocks up with Rack audio, sample for sample.
- **An edge caught mid-rise does.** If the previous sample was already above 0.1 V (a slewed or
  filtered gate), the 1 V crossing is found by linear interpolation between the two samples.

Every timestamp is relative to the start of the audio block, as the engine requires. A mid-rise
edge can interpolate back across a block boundary, into a block that has already been processed.
It is placed at the new block's first frame instead: an error under one sample, and only for
slewed edges.

### Out: reporting each grid pulse

After each block the engine reports the grid pulses that fell inside it. The adapter turns them
into clock-out triggers and beat lights.

My first version promised "each pulse exactly once", and a review on PR #2 showed where that
failed. The guarantee is now stated exactly as the code meets it:

> **Reported pulses are never closer than half a period** (the period in force at the previous
> report). **Every real pulse is covered**: reported at its time, or stood in for by a prediction
> less than half a period before it. With steady or jittery clocks, hiccups and phase shifts of
> under half a period, resets, tempo increases, a stopped clock and its return, every grid pulse
> is reported **exactly once**, with its final index.

A pulse displaced by **more than half a period** is closer to its neighbour's slot than to its
own. It is still covered, but the labels can't come out exactly once. 0.6 of a period late:
index 20 is reported twice (the prediction, then the real pulse) and index 21 never. 0.6 early:
index 20 is never reported. Both are pinned. At exactly half a period (only reachable with
synthetic edges) a phase shift reports two indices twice.

**How it works:**

- **Hysteresis by time.** A grid pulse is reported once it lies at least half a period after the
  last pulse reported. "At least" matters: a pulse exactly on the boundary used to be dropped.
- **Why not by index.** Indices get relabelled: a reset turns pulse 37 into pulse 0, and a slowed
  clock retires fill-ins that were already reported.
- **The half period is the one in force at the last report,** not the current one. When the clock
  slows, the new, longer period would otherwise hide the first real slow pulse inside the window,
  and a real pulse would get no trigger.
- **Labels come from the tracker.** `clock_tracker::pulses_between()` returns recorded pulses then
  predicted ones, already labelled the way they *will* be. A prediction at or after a pending
  reset is already numbered 0, so a reset while the clock is stopped is reported as bar 0 when it
  happens.
- **`bar_restarted`.** A reset can arrive after its pulse was reported (a late edge whose
  prediction already went out). Reports can't be taken back, so the report raises `bar_restarted`
  instead, and the bar light uses it. It isn't raised when that pulse was already reported as 0.

**The one inherent exception: sudden slowdowns.** The grid predicts at the old tempo, and a
prediction goes out before anyone can know the real pulse will be late. Every real pulse is still
reported, at its time or covered by a prediction less than half an old period before it, and
reports follow the real pulses one for one once the slowdown is confirmed. The extra predicted
reports during the transition are pinned by a test:

| Slowdown | Extra reports | Why |
|---|---|---|
| 1.5×, 2×, 2.2× | 2 | One pulse can't tell a slowdown from a hiccup (issue #3), so the tempo changes at the second slow pulse; old-grid predictions go out until then |
| 2.5× | 4 | As above, over a longer gap |
| 3× | 4 | The clock looks stopped for two gaps, so predictions keep clock out running until the second real pulse confirms the slowdown |
| 4× | 6 | As for 3×, over longer gaps |

Those numbers are the price of hiccup immunity. Before issue #3 was fixed they were 1, 1, 1 and 2,
but a single pulse a third of a period late, then back on the grid, cost an extra report and two
bogus tempo changes. Now a hiccup changes nothing and is reported exactly once. A sudden
deliberate slowdown is rare and audibly a transition anyway; hiccups from a jittery source can
happen at any time.

The alternative rule (the current period) produces fewer extras, but it gives some *real*
pulses no trigger at all. A downbeat from the master clock with no clock out is the worse
failure.

The adapter then plays each reported pulse as a 1 ms trigger **one block later, at the same
offset**: the same one-block latency the audio has, so clock out stays aligned with the sound.

---

## Further reading

- Fons Adriaensen, *Using a DLL to filter time* (2005). The DLL view of the same filter, as used in
  JACK.
- T. R. Benedict and G. W. Bordner, *Synthesis of an optimal set of radar track-while-scan
  smoothing equations* (1962). The origin of β = α²/(2−α).
- Paul Kalata, *The tracking index* (1984). Steady-state α-β behaviour, including the lag and noise
  gains measured above.
