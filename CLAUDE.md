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
| `make bless` | Approve new golden renders and screen images. Only after the change is understood. |

Test binary: `build/dev/tests/previously_tests` (doctest; `-tc="name*"` filters). The pre-commit
hook runs the format check and the host tests. The binary turns on ASan's alloc-dealloc check
itself (`tests/sanitizer_options.cpp`), which macOS leaves off by default, so the check holds
however the binary is run.

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
  build failures, and a failed build silently reruns the old binary. Six rounds so far: five in
  `docs/learning/clock-tracking.md` §7, and the tape's in PR #10. The recurring lesson: tests
  checking only end states miss bugs whose damage is a one-pulse transient (a predicted time, a
  label, a period during a transition). Round 5 added one: a bug that writes where nothing reads
  (struct padding) needs a `static_assert`, not a test. Round 6 added two:
  - A contract that aborts ends the run, which hides every later test. The tape's tests audit
    their pools (`page_audit` in `tests/tape_fixtures.hpp`), so a page left held or a failed
    REQUIRE fails that one test instead. A leak inside the engine's own pool still aborts: rerun
    those mutations filtered to the recorder's tests, or on the `release` preset, where contracts
    compile out.
  - Read the whole test output, not its tail, to know which tests caught a mutation.
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

## Current state (after PRs #1–#16)

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
    RAM budget (DESIGN §2.3). PR #9 counts flash and RAM as the linker does.
  - The paged tape (PR #10): `page_pool`, the `recorder` with its 46-page history, the DC
    blocker, the audio floor (2^-50) and `-ffp-contract=off` (ADR 0005, CODING_STANDARD §3.13).
    The engine takes an optional tape span. The firmware passes none.
  - Rack's tape (PR #13, `rack/src/tape.hpp`): one 210 MiB tape per module, allocated when it is
    created and left unwritten; the engine is rebuilt over it at every rate (DESIGN §15 Q10).
  - The display's model (PR #14, DESIGN §9.3): the recorder keeps each bucket's peaks (1/32 page,
    as 3 dB levels, `peak_level.hpp`), and `engine::ui()` is rebuilt at every bucket: the clock,
    and a 128-column strip showing the last 4 whole bars and the bar in progress, or up to the
    newest 30 s before the grid has a tempo.
  - The screen (PR #15, DESIGN §9.3): `draw()` turns a model into a 128×64 framebuffer in the
    SSD1306's page order (`display.hpp`, `framebuffer.hpp`), with our own 5×7 font (`font.hpp`).
    Its golden images are plain PBM files in `tests/golden/display/`, which the unit tests
    compare and `make bless` rewrites.
  - Rack's screen (PR #16, DESIGN §9.3): a 1.54" OLED's 35.05 × 17.51 mm on the panel. The module
    hands each rebuilt model to the UI thread through a `triple_buffer` (engine), kept by the
    module so it outlives engine rebuilds (`rack/src/screen.hpp`); the widget draws it on the
    light layer. The module draws the panel's labels in Rack's Nunito Bold, since Rack's SVG
    renderer draws no text. Not yet checked by the user in Rack.
- **Known, pinned limitations:**
  - A halving whose confirming pulse is more than a quarter period late keeps 2 filled-in pulses
  - Displacements over half a period can repeat or skip an index in reports (still covered)
  - After a slowed clock's retirement the history is shorter until new pulses overwrite the
    retired slots. From 768.5× on the engine's 1,536 pulses, a slowdown reads as a stop and
    restart, and its fill-ins stay
  - Rack's tape holds fewer pages above 192 kHz (DESIGN §2.3): 105 at 352.8 and 384 kHz, where a
    full-length loop cannot be replaced, and 52 at 705.6 and 768 kHz, where loops get 6 pages
    and one bar needs 74.7 BPM. A module's resident memory follows the highest rate it has run
    at: 184 MiB at 768 kHz. A rate change starts the history again
- **Deferred by the user:** bootloader/QSPI and other hardware work. Don't propose it unless
  asked. The RAM budget (73% now) keeps the bootloader layout open; if it trips, report what
  grew.
- **The engine is built at `-O0` in the firmware** (DESIGN §2.3, issue #11): flash is 88% now,
  61% with the engine optimized. If the flash budget trips, fix that first, then try `-Os` and
  report the trade-off.
- **Next:** capture and playback (§3.4, §5), then the Rack controls. PR #6's closing review lists
  the tests each owes, and DESIGN §2.3 adds one for Rack's 52-page tape; the crossfade-bits test
  and the floor test through the seam correlation moved to capture and playback.
