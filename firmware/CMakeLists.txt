# Previously firmware: cross-compiled for the Daisy Seed (STM32H750) with libDaisy.
#
#   cmake -S firmware -B build/firmware -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/firmware

cmake_minimum_required(VERSION 3.25)

set(previously_root "${CMAKE_CURRENT_LIST_DIR}/..")

# libDaisy's toolchain discovery: finds arm-none-eabi-gcc on PATH and selects the matching
# toolchain file. It must run before project(), exactly as libDaisy's own build does.
include("${previously_root}/vendor/libDaisy/cmake/toolchains/autodetect.cmake")

project(previously_firmware LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${previously_root}/cmake")
include(previously_options)

add_subdirectory("${previously_root}/vendor/libDaisy" libDaisy)
add_subdirectory("${previously_root}/engine" engine)

# libDaisy and its dependencies are third-party: consumers see their headers as system headers,
# so our strict warnings apply to our code only.
get_target_property(daisy_dependencies daisy LINK_LIBRARIES)
foreach(dependency IN LISTS daisy_dependencies ITEMS daisy)
    set_target_properties(${dependency} PROPERTIES SYSTEM ON)
endforeach()

set(FIRMWARE_NAME    previously)
set(FIRMWARE_SOURCES
    src/main.cpp
    src/contract_violation.cpp
)

include(DaisyProject)

# DaisyProject.cmake sets C++14; the project standard is C++20.
set_target_properties(${FIRMWARE_NAME} PROPERTIES CXX_STANDARD 20 CXX_EXTENSIONS OFF)

target_link_libraries(${FIRMWARE_NAME} PRIVATE previously::engine)

previously_apply_strict_options(${FIRMWARE_NAME})
previously_apply_embedded_subset(${FIRMWARE_NAME})

# T0: fail the build if the image outgrows the STM32H750's 128 KiB internal flash, or the
# variables in internal RAM outgrow 112 KiB: DTCM's 128 KiB less 16 KiB for the stack.
#
# The stack reserve is about 9 times the measured need. GCC's -fstack-usage report, before LTO,
# summed along the deepest call chains, gives about 1.8 KB once audio runs and about 1 KB at
# start-up. The 1.8 KB is main()'s frame plus the audio interrupt: the FPU's exception frame, the
# DMA handler, libDaisy's callback, the engine's process() and report_clock(), and the tracker's
# history search.
add_custom_command(TARGET ${FIRMWARE_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            -DSIZE_TOOL=${TOOLCHAIN_BIN_DIR}/arm-none-eabi-size
            -DELF=$<TARGET_FILE:${FIRMWARE_NAME}>
            -DFLASH_BYTES=131072
            -DRAM_BYTES=114688
            -P "${previously_root}/cmake/check_firmware_size.cmake"
    VERBATIM
)
