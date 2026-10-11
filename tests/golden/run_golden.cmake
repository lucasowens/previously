# Golden render regression (tier T3).
#
# For every stimulus/<name>.wav: render it through previously_render, then compare the output
# byte-for-byte with expected/<name>.wav. With -DBLESS=ON, copy the output over the expected file
# instead (run this only after listening to and approving the change).
#
# Inputs: RENDER (previously_render path), GOLDEN_DIR (this directory), OUTPUT_DIR, optional BLESS.

file(GLOB stimuli "${GOLDEN_DIR}/stimulus/*.wav")

if(NOT stimuli)
    message(FATAL_ERROR "no golden stimuli found in ${GOLDEN_DIR}/stimulus")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")

set(failures 0)

foreach(stimulus IN LISTS stimuli)
    get_filename_component(name "${stimulus}" NAME)

    set(actual   "${OUTPUT_DIR}/${name}")
    set(expected "${GOLDEN_DIR}/expected/${name}")

    execute_process(
        COMMAND "${RENDER}" process "${stimulus}" "${actual}"
        RESULT_VARIABLE render_result
    )

    if(NOT render_result EQUAL 0)
        message(SEND_ERROR "render failed for ${name}")
        math(EXPR failures "${failures} + 1")
        continue()
    endif()

    if(BLESS)
        file(COPY_FILE "${actual}" "${expected}")
        message(STATUS "blessed ${name}")
        continue()
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E compare_files "${actual}" "${expected}"
        RESULT_VARIABLE compare_result
    )

    if(compare_result EQUAL 0)
        message(STATUS "match    ${name}")
    else()
        message(SEND_ERROR "MISMATCH ${name} (output: ${actual})")
        math(EXPR failures "${failures} + 1")
    endif()
endforeach()

if(failures GREATER 0)
    message(FATAL_ERROR "${failures} golden case(s) failed")
endif()
