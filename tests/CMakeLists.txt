# Host test suite (tiers T1–T3). See DESIGN.md §12.

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/vendor/doctest)

add_executable(previously_tests
    test_main.cpp
    realtime_guard.cpp
    test_units.cpp
    test_sample_time.cpp
    test_clock_tracker.cpp
    test_spsc_queue.cpp
    test_engine.cpp
    test_wav.cpp
    test_block_adapter.cpp
)

target_link_libraries(previously_tests PRIVATE
    previously::engine
    previously::host_support
    previously_wav
    doctest
)

# The Rack adapter is header-only and depends only on the engine, so it is tested here (T4).
target_include_directories(previously_tests PRIVATE ${PROJECT_SOURCE_DIR}/rack/src)

previously_apply_strict_options(previously_tests)

add_test(NAME unit COMMAND previously_tests)

# T3: golden render regression. Renders every stimulus in golden/stimulus and compares the result
# byte-for-byte with golden/expected. `cmake --build <dir> --target bless` approves new output.
set(golden_args
    -DRENDER=$<TARGET_FILE:previously_render>
    -DGOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/golden
    -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/golden_out
)

add_test(NAME golden COMMAND ${CMAKE_COMMAND} ${golden_args} -P ${CMAKE_CURRENT_SOURCE_DIR}/golden/run_golden.cmake)

add_custom_target(bless
    COMMAND ${CMAKE_COMMAND} ${golden_args} -DBLESS=ON -P ${CMAKE_CURRENT_SOURCE_DIR}/golden/run_golden.cmake
    DEPENDS previously_render
    COMMENT "Approving current render output as the new golden reference"
)
