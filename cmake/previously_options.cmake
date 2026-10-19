# Shared compiler options for every Previously target (host, Rack, firmware).
# One definition, so every build of the engine sees identical rules.

include_guard(GLOBAL)

# Strict warnings, all fatal. See docs/CODING_STANDARD.md §4.
set(previously_warning_flags
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wdouble-promotion
    -Wnull-dereference
    -Wold-style-cast
    -Wcast-align
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wimplicit-fallthrough
    -Wformat=2
    -Werror
)

# Applies the project language standard and warnings to a target we own.
function(previously_apply_strict_options target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    target_compile_options(${target} PRIVATE ${previously_warning_flags})

    # Clang's -Wshadow skips base-class members; GCC's does not. -Wshadow-all closes the gap so a
    # local Clang build catches everything the GCC CI job would.
    target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wshadow-all>)
endfunction()

# Applies the embedded subset: no exceptions, no RTTI. Used by the engine and the tools,
# so host builds of the engine behave exactly like the firmware build.
function(previously_apply_embedded_subset target)
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
endfunction()

# Every multiply and add rounds on its own, so the engine computes the same bits on every target
# (ADR 0005). Clang and GCC otherwise fuse them where the CPU has an FMA instruction: arm64 and the
# Cortex-M7 do, and baseline x86-64 does not. PUBLIC, so engine code inlined into a consumer
# follows the same rule. The Rack plugin compiles its own files with Rack's flags, which is why
# rendered arithmetic lives in the engine's .cpp files (CODING_STANDARD §3.13).
function(previously_apply_float_rules target)
    target_compile_options(${target} PUBLIC -ffp-contract=off)
endfunction()
