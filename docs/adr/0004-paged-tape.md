# 0004 — Loop memory is a paged tape

- **Status:** Accepted
- **Date:** 2026-10-11

## Context

DESIGN v0.2 kept loop audio in six 24 s buffers. Recording went into one of them as a ring. A
capture made that buffer the slot's, and recording moved on to a fresh one.

Reviewing the retro-capture design before any code was written found three problems:

- **History restarts at every capture.** The fresh buffer starts empty, so a second capture within
  N bars of the first has too little audio. The headline promise is "at any moment".
- **Keeping the history continuous costs buffers.** Letting a loop span two buffers fixes the
  history, but each live loop can then hold two whole buffers. Four slots and an undo state could
  need 12 buffers of 6.
- **The ring position is a division.** `s mod C`, with C a run-time length, compiles to a software
  64-bit division (`__aeabi_ldivmod`) on the Cortex-M7, on every sample.

## Decision

- Loop memory is one pool of **pages**, each 2¹⁵ stereo frames (0.68 s, 256 KiB at 48 kHz).
- The **recorder** writes without pause, taking a free page whenever the current one fills. The
  newest pages are the history.
- A **loop** holds the ids of the pages its window touches, with a reference count on each. A page
  returns to the pool when it is older than the history and no loop holds it.
- A capture is admitted only while every loop's pages plus the history fit the pool.
- A position inside a page is a shift and a mask.

## Consequences

- The history is never interrupted, however many captures happen.
- Memory use is the sum of what is actually held, not of whole buffers. A loop wastes at most two
  partial pages (1.4 s).
- The budget is a sum that tests can check with `static_assert`, rather than a proof about which
  buffer holds what.
- Every loop read goes through a page table: one lookup per page crossing, every 0.68 s.
- Overdub and undo (P2) write into fresh pages instead of a spare buffer.
