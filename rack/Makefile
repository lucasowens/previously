# Previously — VCV Rack plugin.
#
# The engine is NOT compiled here. Rack's build adds -funsafe-math-optimizations, which would make
# the plugin's DSP differ from the tests and the firmware. Instead the engine is built by CMake
# (preset "rack") with the project's own flags and linked in as a static library.
#
#   make -C rack RACK_DIR=~/Source/Rack          build against a local Rack source tree
#   make -C rack RACK_DIR=../.deps/Rack-SDK      build against the fetched SDK (CI)
#   make -C rack install                         install into the Rack user plugins folder

RACK_DIR ?= ../.deps/Rack-SDK

ENGINE_BUILD := ../build/rack/engine

# Rack's headers are third-party code: include them as system headers so our warnings stay ours.
FLAGS += -isystem $(RACK_DIR)/include -isystem $(RACK_DIR)/dep/include
FLAGS += -I../engine/include
FLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wdouble-promotion -Wold-style-cast -Werror
SOURCES += $(wildcard src/*.cpp)

LDFLAGS += $(ENGINE_BUILD)/libpreviously_engine.a $(ENGINE_BUILD)/libpreviously_host_support.a

# The licence lives at the repo root; Rack's packager only accepts paths inside this directory.
DISTRIBUTABLES += res LICENSE

include $(RACK_DIR)/plugin.mk

# Rack's compile.mk appends -std=c++11; the last -std wins, so ours must come after it.
CXXFLAGS += -std=c++20

LICENSE: ../LICENSE
	cp $< $@

dist: LICENSE

# Relink whenever the engine library changes, or the plugin could carry a stale engine.
$(TARGET): $(ENGINE_BUILD)/libpreviously_engine.a $(ENGINE_BUILD)/libpreviously_host_support.a
