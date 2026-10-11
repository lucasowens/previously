# Previously

**Infinitedecay Previously** is a clock-locked stereo looper for Eurorack, built on the
**Daisy Seed3**.

It sits at the end of the chain and is always listening. Press CAPTURE and it grabs the last N
bars, aligned to the clock to the sample. Then crossfade from the live signal to the loop, rebuild
the patch underneath, and capture again: transitions, layering and performance effects.

> **Status:** P0 foundations. The engine is a sample-exact passthrough; the toolchain, tests, Rack
> plugin and firmware build are all in place. See the [roadmap](docs/DESIGN.md#14-roadmap).

## Documents

| Document | What it covers |
|---|---|
| [DESIGN.md](docs/DESIGN.md) | The complete design: sync, capture, tempo, seams, FX, controls, hardware, testing, roadmap |
| [CODING_STANDARD.md](docs/CODING_STANDARD.md) | How the code is written, and why |
| [docs/adr/](docs/adr/README.md) | Architecture decision records |
| [docs/learning/](docs/learning/README.md) | Notes on the ideas behind the code |

## Layout

```
engine/      the looper engine: pure C++20, no platform headers, shared by every target
tests/       host tests: unit, golden render regression, Rack adapter
tools/       render CLI (WAV → engine → WAV) and developer scripts
rack/        VCV Rack plugin (development test bed)
firmware/    Daisy firmware (Pod today, Eurorack module later)
vendor/      pinned third-party code (libDaisy, DaisySP, doctest); never modified
docs/        design, standards, decisions, learning notes, vendor datasheets
```

## Getting started (macOS)

**Install the tools:**

```sh
brew install cmake ninja llvm cppcheck jq zstd
```

You also need the [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
15.2 (`arm-none-eabi-gcc` on `PATH`).

**Set up and check everything:**

```sh
make setup        # submodules, Rack SDK, git hooks
make test         # the full host gate: format, build, tests, clang-tidy, cppcheck, Rack, firmware
```

**Day to day:**

| Command | What it does |
|---|---|
| `make quick` | Build and run the host tests (sanitizers on) |
| `make format` | Format everything |
| `make rack-install` | Build and install the Rack plugin into your Rack user folder |
| `make firmware` | Cross-compile the Daisy firmware, with the flash budget checked |
| `make bless` | Approve new golden render output (only after listening to it) |

Use `make rack RACK_DIR=~/Source/Rack` to build against a Rack source tree instead of the SDK.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
