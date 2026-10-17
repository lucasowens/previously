# Previously — notes for Claude sessions

**Infinitedecay Previously** is a clock-locked stereo Eurorack looper on the Daisy Seed3, with
retrospective capture and live/loop crossfading. Repo: `lucasowens/previously` (public, GPLv3).
The local folder is still called `DaisyLooper`; the name doesn't matter to the build.

Read [docs/DESIGN.md](docs/DESIGN.md) (spec and roadmap, §14 has the P1 progress table) and
[docs/CODING_STANDARD.md](docs/CODING_STANDARD.md) before changing code.

## Commands

| Command | What it does |
|---|---|
| `make quick` | Build and run host tests (ASan + UBSan) |
| `make test` | The full gate: format check, tests, clang-tidy, cppcheck, Rack plugin, firmware |
| `make format` | clang-format everything (LLVM 23 is pinned; the scripts refuse other versions) |
| `make rack-install` | Build and install the plugin into the user's Rack folder. Rack must be restarted to load it. |
| `make bless` | Approve new golden renders. Only after the change is understood. |

Test binary: `build/dev/tests/previously_tests` (doctest; `-tc="name*"` filters). The pre-commit
hook runs the format check and the host tests.

## Workflow

- **Every change goes through a branch and a PR.** CI must be green (macOS gate, Linux GCC 14,
  Linux Clang 18). Commit messages are Conventional Commits with the Co-Authored-By trailer.
  Never commit or push to `main` directly.
- **The user reviews PRs adversarially, with reproductions.** Reply to every review comment
  saying what changed and which test covers it. Where a finding is wrong, say so with evidence.
- **Before calling anything ready, attack its behaviour:**
  - values just either side of every threshold
  - both orderings of events
  - extreme ratios, and missing calls
  - both ends of a buffer: a wrapped history's oldest entries, not only its newest
  - **transients, not just end states**
- **Mutation-check new logic.** Remove or invert each mechanism one at a time and confirm a test
  fails. Verify the mutation actually applied and built: `-Werror` turns unused variables into
  build failures, and a failed build silently reruns the old binary. Five rounds so far, recorded
  in `docs/learning/clock-tracking.md` §7. The recurring lesson: tests checking only end states
  miss bugs whose damage is a one-pulse transient (a predicted time, a label, a period during a
  transition). Round 5 added one: a bug that writes where nothing reads (struct padding) needs a
  `static_assert`, not a test.
- **Measure before choosing.** Gains, thresholds and transition behaviour are chosen from
  measurements (scratch probes against the engine library), then pinned by tests with the
  numbers in comments.
- **Docs must state guarantees exactly as the code meets them.** Overclaiming has been the most
  common review finding.

## Code

- C++20 throughout, with rules scoped as in CODING_STANDARD §3:
  - **Engine, tools and firmware:** no exceptions or RTTI; no heap after init in the engine;
    `float`, never `double`, in the engine; strong unit types.
  - **The realtime path** (`engine::process()` and everything it calls): no allocation, locks,
    I/O or recursion, and bounded loops only.
  - **Host tests** may use exceptions (doctest does) and `double` as a reference. Offline
    tools and tests are not under the realtime rules.
- **Style is enforced and matters to the user, who reads code like a book:**
  - Core Guidelines naming: `snake_case`, `trailing_underscore_` members
  - Stroustrup braces, 100 columns
  - aligned declarations, blank lines between logical steps
  - comments say *why*
  - one consistent style across all files
- Edits: clang-format reflows code, so text-substitution edits often miss. Read the formatted
  file, then edit.
- `engine/` is platform-free and shared by tests, the render tool, Rack and the firmware.
  `rack/src` and `firmware/src` are thin adapters, the only places vendor APIs (PascalCase)
  appear. `vendor/` is never modified.

## Current state (after PRs #1–#7)

- **Done:**
  - `sample_time` (Q47.16) and `clock_tracker`: an alpha-beta tracker with least-squares
    acquisition; freewheeling; resets; outlier verdicts (hiccup, phase shift, tempo change)
  - Engine clock wiring with `clock_report`. Reports are never closer than half a period and
    every real pulse is covered; exactly once only for displacements under half a period, and
    sudden slowdowns add pinned extras (`clock_events.hpp`, clock-tracking.md §8). Labels
    are as they will be once a pending reset applies.
  - Rack CLOCK, RESET and CLOCK OUT jacks and lights. Verified in VCV Rack 2 Pro by the user.
  - Retro capture's design (DESIGN §3.4, v0.3, PR #6).
  - Edge times (PR #7): the tracker's `clock_history` keeps each pulse's raw edge and a fill-in
    bit beside its filtered time, read with `edge(index)`. The firmware build also enforces a
    RAM budget (DESIGN §2.3).
- **Known, pinned limitations:**
  - A halving whose confirming pulse is more than a quarter period late keeps 2 filled-in pulses
  - Displacements over half a period can repeat or skip an index in reports (still covered)
  - After a slowed clock's retirement the history is shorter until new pulses overwrite the
    retired slots. From 768.5× on the engine's 1,536 pulses, a slowdown reads as a stop and
    restart, and its fill-ins stay
- **Deferred by the user:** bootloader/QSPI and other hardware work. Don't propose it unless
  asked. If the firmware flash budget (75% now) trips, try `-Os` first and report the trade-off.
  The RAM budget (70% now) keeps the bootloader layout open; if it trips, report what grew.
- **Next:** retro capture, host- and Rack-first, in three more PRs: the paged tape, recorder, DC
  blocker and `-ffp-contract=off` (DESIGN §2.3, §5.5, ADR 0005); capture and playback (§3.4,
  §5); the Rack controls. PR #6's closing review lists the tests each one owes.
