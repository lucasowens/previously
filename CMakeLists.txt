# Previously — host build: engine, tests, and tools.
# The firmware has its own cross-compiling project in firmware/; the Rack plugin links the
# engine library built here (preset "rack").

cmake_minimum_required(VERSION 3.25)

project(previously
    VERSION      0.1.0
    DESCRIPTION  "Clock-locked stereo looper for Eurorack (Daisy Seed3)"
    LANGUAGES    CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(previously_options)

option(PREVIOUSLY_BUILD_TESTS "Build the host test suite"        ON)
option(PREVIOUSLY_BUILD_TOOLS "Build the host tools (render CLI)" ON)
option(PREVIOUSLY_SANITIZE    "Build with ASan and UBSan"         OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(PREVIOUSLY_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(engine)

if(PREVIOUSLY_BUILD_TOOLS)
    add_subdirectory(tools/render)
endif()

if(PREVIOUSLY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
