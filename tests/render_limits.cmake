# Checks that previously_render validates --block against the engine's limit for the input's
# sample rate. Inputs: RENDER (previously_render path), WORK_DIR.

file(MAKE_DIRECTORY "${WORK_DIR}")
set(input "${WORK_DIR}/sine_8k.wav")

execute_process(
    COMMAND "${RENDER}" generate sine "${input}" --frames 4000 --rate 8000
    COMMAND_ERROR_IS_FATAL ANY
)

# At 8 kHz the engine accepts at most 512-frame blocks.
execute_process(
    COMMAND "${RENDER}" process "${input}" "${WORK_DIR}/too_big.wav" --block 2048
    RESULT_VARIABLE rejected
    ERROR_VARIABLE  message
)

if(NOT rejected EQUAL 2 OR NOT message MATCHES "at most 512 at 8000 Hz")
    message(FATAL_ERROR "expected exit 2 with a block-size message, got ${rejected}: ${message}")
endif()

execute_process(
    COMMAND "${RENDER}" process "${input}" "${WORK_DIR}/fits.wav" --block 512
    COMMAND_ERROR_IS_FATAL ANY
)
