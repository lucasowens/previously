# Previously — developer entry points.
#
# Thin wrappers over CMake and tools/scripts, so the same commands work locally, in the
# pre-commit hook, and in CI. Run `make help` for the list.

RACK_DIR ?= $(CURDIR)/.deps/Rack-SDK

.PHONY: help setup hooks build quick seeds test lint format format-check tidy cppcheck \
        rack rack-install firmware bless clean

help:
	@echo "make setup         init submodules, fetch the Rack SDK, install git hooks"
	@echo "make test          the full host gate (T0-T4): build all targets, lint, run every test"
	@echo "make quick         build and run the host tests only (fast inner loop)"
	@echo "make seeds         the seeded properties, from an optimized build (part of make test)"
	@echo "make lint          format-check + clang-tidy + cppcheck"
	@echo "make format        reformat every project source in place"
	@echo "make rack          build the VCV Rack plugin (RACK_DIR=... to use a Rack source tree)"
	@echo "make rack-install  install the plugin into the Rack user folder"
	@echo "make firmware      cross-compile the Daisy firmware (size budget enforced)"
	@echo "make bless         approve current render output as the new golden reference"

setup: hooks
	git submodule update --init --recursive
	tools/scripts/fetch-rack-sdk.sh

hooks:
	git config core.hooksPath tools/hooks
	@echo "git hooks installed from tools/hooks"

build:
	cmake --preset dev
	cmake --build --preset dev

quick: build
	ctest --preset dev

# Seeded properties too slow for the sanitizers. The release preset's other tests are left out
# (issue #12).
seeds:
	cmake --preset release
	cmake --build --preset release --target previously_seed_tests
	ctest --preset release -L seeds --output-on-failure

test: format-check quick seeds tidy cppcheck rack firmware
	@echo "host gate passed: format, build, tests, seeded properties, clang-tidy, cppcheck, rack, firmware"

lint: format-check tidy cppcheck

format:
	tools/scripts/format.sh

format-check:
	tools/scripts/format.sh --check

tidy: build
	tools/scripts/tidy.sh build/dev

cppcheck: build
	tools/scripts/cppcheck.sh build/dev

rack:
	@test -d "$(RACK_DIR)" || tools/scripts/fetch-rack-sdk.sh
	cmake --preset rack
	cmake --build --preset rack
	$(MAKE) -C rack RACK_DIR="$(RACK_DIR)"

rack-install: rack
	$(MAKE) -C rack RACK_DIR="$(RACK_DIR)" install

firmware:
	cmake -S firmware -B build/firmware -G Ninja -DCMAKE_BUILD_TYPE=Release
	cmake --build build/firmware

bless: build
	cmake --build --preset dev --target bless

clean:
	rm -rf build
	$(MAKE) -C rack clean RACK_DIR="$(RACK_DIR)" || true
