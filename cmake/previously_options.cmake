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
