# Firmware size budget (tier T0). Fails the build when the flash image exceeds the budget.
# Inputs: SIZE_TOOL (arm-none-eabi-size), ELF, FLASH_BYTES.

execute_process(
    COMMAND "${SIZE_TOOL}" --format=berkeley "${ELF}"
    OUTPUT_VARIABLE size_output
    COMMAND_ERROR_IS_FATAL ANY
)

# Berkeley output: header line, then "text data bss dec hex filename".
string(REGEX MATCH "\n[ \t]*([0-9]+)[ \t]+([0-9]+)[ \t]+([0-9]+)" _ "${size_output}")

set(text_bytes "${CMAKE_MATCH_1}")
set(data_bytes "${CMAKE_MATCH_2}")
math(EXPR flash_used "${text_bytes} + ${data_bytes}")
math(EXPR flash_percent "(${flash_used} * 100) / ${FLASH_BYTES}")

message(STATUS "firmware flash: ${flash_used} / ${FLASH_BYTES} bytes (${flash_percent}%)")

if(flash_used GREATER FLASH_BYTES)
    message(FATAL_ERROR "firmware exceeds the flash budget by ${flash_used} - ${FLASH_BYTES} bytes")
endif()
