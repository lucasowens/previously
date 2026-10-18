# Host test suite (tiers T1–T3). See DESIGN.md §12.

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/vendor/doctest)

add_executable(previously_tests
    test_main.cpp
    realtime_guard.cpp
    flush_to_zero_mode.cpp
    test_units.cpp
    test_sample_time.cpp
    test_clock_tracker.cpp
    test_spsc_queue.cpp
    test_dc_blocker.cpp
    test_page_pool.cpp
    test_recorder.cpp
    test_engine.cpp
    test_engine_clock.cpp
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

# The render tool refuses a block size the engine cannot take at the file's sample rate, with a
# clear message instead of an engine contract violation.
add_test(NAME render_rejects_oversized_block
    COMMAND ${CMAKE_COMMAND}
            -DRENDER=$<TARGET_FILE:previously_render>
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/render_limits
            -P ${CMAKE_CURRENT_SOURCE_DIR}/render_limits.cmake
)

# T0's firmware budgets, checked on every host without the ARM toolchain.
add_test(NAME firmware_size_budgets
    COMMAND ${CMAKE_COMMAND}
            -DCHECK_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/check_firmware_size.cmake
            -DLINKER_SCRIPT=${PROJECT_SOURCE_DIR}/vendor/libDaisy/core/STM32H750IB_flash.lds
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/firmware_size
            -P ${CMAKE_CURRENT_SOURCE_DIR}/firmware_size_budgets.cmake
)

# The budgets' memory regions, read from the linker script.
add_test(NAME linker_memory_region
    COMMAND ${CMAKE_COMMAND}
            -DMODULE=${PROJECT_SOURCE_DIR}/cmake/linker_memory_region.cmake
            -DLIBDAISY_CORE=${PROJECT_SOURCE_DIR}/vendor/libDaisy/core
            -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/linker_memory_region
            -P ${CMAKE_CURRENT_SOURCE_DIR}/linker_memory_region.cmake
)
