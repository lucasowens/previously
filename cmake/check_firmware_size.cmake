# Firmware size budgets (tier T0). Fails the build when either is exceeded:
#
#   flash  the image in the STM32H750's internal flash.
#   RAM    the variables in internal RAM: .data and .bss. Today they live in AXI SRAM, and the
#          stack has DTCM to itself. Under the QSPI bootloader layout (DESIGN.md §2.3) they move
#          into DTCM and share its 128 KiB with the stack, so the budget is DTCM less a stack
#          reserve. Within it, that move stays open.
#
# Inputs: SIZE_TOOL (arm-none-eabi-size), ELF, FLASH_BYTES, RAM_BYTES.

# A script run with cmake -P inherits no policies from the project. Without its own, CMake 3.x
# leaves CMP0057 unset and rejects IN_LIST below.
cmake_minimum_required(VERSION 3.25)

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
    math(EXPR flash_over "${flash_used} - ${FLASH_BYTES}")
    message(FATAL_ERROR "firmware exceeds the flash budget (${FLASH_BYTES} bytes) by ${flash_over}")
endif()

# The sections the bootloader layout places in DTCM. The linker adds .got and .got.plt after
# .data; libDaisy's .dtcmram_bss is for variables placed in DTCM explicitly.
set(ram_sections .data .got .got.plt .bss .dtcmram_bss)

execute_process(
    COMMAND "${SIZE_TOOL}" -A -d "${ELF}"
    OUTPUT_VARIABLE sections_output
    COMMAND_ERROR_IS_FATAL ANY
)

# System V output: one "name size address" line per section, in decimal.
string(REPLACE "\n" ";" section_lines "${sections_output}")
set(ram_used 0)
set(ram_sections_seen "")

foreach(line IN LISTS section_lines)
    if(line MATCHES "^([.][A-Za-z0-9_.]+)[ \t]+([0-9]+)[ \t]+[0-9]+")
        set(section_name "${CMAKE_MATCH_1}")

        if(section_name IN_LIST ram_sections)
            math(EXPR ram_used "${ram_used} + ${CMAKE_MATCH_2}")
            list(APPEND ram_sections_seen "${section_name}")
        endif()
    endif()
endforeach()

# Every image has .data and .bss. Without them the output was not understood, and a sum of zero
# would pass silently.
if(NOT ".data" IN_LIST ram_sections_seen OR NOT ".bss" IN_LIST ram_sections_seen)
    message(FATAL_ERROR "could not read the section sizes of ${ELF}")
endif()

math(EXPR ram_percent "(${ram_used} * 100) / ${RAM_BYTES}")

message(STATUS "firmware RAM: ${ram_used} / ${RAM_BYTES} bytes (${ram_percent}%)")

if(ram_used GREATER RAM_BYTES)
    math(EXPR ram_over "${ram_used} - ${RAM_BYTES}")
    message(FATAL_ERROR "firmware exceeds the RAM budget (${RAM_BYTES} bytes) by ${ram_over}")
endif()
