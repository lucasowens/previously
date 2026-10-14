# 0004 — Loop memory is a paged tape

- **Status:** Accepted
- **Date:** 2026-10-11

## Context

DESIGN v0.2 kept loop audio in six 24 s buffers. Recording went into one of them as a ring. A
capture made that buffer the slot's, and recording moved on to a fresh one.

Reviewing the retro-capture design before any code was written found two problems:

- **History restarts at every capture.** The fresh buffer starts empty, so a second capture within
  N bars of the first has too little audio. The headline promise is "at any moment".
- **Keeping the history continuous costs buffers.** Letting a loop span two buffers fixes the
  history, but each live loop can then hold two whole buffers. Four slots and an undo state could
  need 12 buffers of 6.

## Decision

- Loop memory is one pool of **pages**. A page is the smallest power-of-two number of stereo
  frames that lasts at least 0.6827 s: 2¹⁵ frames (256 KiB) at 44.1 and 48 kHz, and more at
  higher rates. Page counts then do not depend on the sample rate.
- The **recorder** writes without pause, taking a free page whenever the current one fills. The
  newest pages are the history.
- A **loop** holds the ids of the pages its window touches, with a reference count on each. A page
  returns to the pool when it is older than the history and no loop holds it.
- A capture is admitted only while every loop's pages plus the history fit the pool.

## Consequences

- The history is never interrupted, however many captures happen.
- Memory use is the sum of what is actually held, not of whole buffers. A loop wastes at most two
  partial pages (1.4 s at 48 kHz).
- The budget is a sum that tests can check with `static_assert`, rather than a proof about which
  buffer holds what. The same counts hold at every sample rate; only the bytes per page change.
- Every loop read goes through a page table. A sequential read needs one lookup per page
  crossing, every 0.68 s. A seam reads two streams, and P3's random reads (granular, the WSOLA
  search) need a lookup each.
- A position inside a page is a shift and a mask.
- Overdub and undo (P2) write into fresh pages instead of a spare buffer.
