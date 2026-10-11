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
  - **transients, not just end states**
- **Mutation-check new logic.** Remove or invert each mechanism one at a time and confirm a test
  fails. Verify the mutation actually applied and built: `-Werror` turns unused variables into
  build failures, and a failed build silently reruns the old binary. Four rounds so far, recorded
  in `docs/learning/clock-tracking.md` §7. The recurring lesson: tests checking only end states
  miss bugs whose damage is a one-pulse transient (a predicted time, a label, a period during a
  transition).
- **Measure before choosing.** Gains, thresholds and transition behaviour are chosen from
  measurements (scratch probes against the engine library), then pinned by tests with the
  numbers in comments.
- **Docs must state guarantees exactly as the code meets them.** Overclaiming has been the most
  common review finding.

## Code

- C++20 embedded subset: no exceptions, no RTTI, no heap after init, bounded loops,
  `float` (never `double`) in the engine, strong unit types. Tests may use `double` as a
  reference.
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

## Current state (after PRs #1–#4)

- **Done:**
  - `sample_time` (Q47.16) and `clock_tracker`: an alpha-beta tracker with least-squares
    acquisition; freewheeling; resets; outlier verdicts (hiccup, phase shift, tempo change)
  - Engine clock wiring with `clock_report` (each pulse reported once, labels as they will be)
  - Rack CLOCK, RESET and CLOCK OUT jacks and lights. Verified in VCV Rack 2 Pro by the user.
- **Known, pinned limitations:**
  - A halving whose confirming pulse is more than a quarter period late keeps 2 filled-in pulses
  - Displacements over half a period can repeat or skip an index in reports (still covered)
- **Deferred by the user:** bootloader/QSPI and other hardware work. Don't propose it unless
  asked. If the firmware flash budget (73% now) trips, try `-Os` first and report the trade-off.
- **Next:** retro capture (DESIGN §3.4, §5), host- and Rack-first.
