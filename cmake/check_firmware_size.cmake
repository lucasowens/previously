# Firmware size budgets (tier T0). Fails the build when either is exceeded:
#
#   flash  the image in the STM32H750's internal flash, counted as the linker counts it: from the
#          start of flash to the end of the last load image, alignment padding included.
#   RAM    the variables in internal RAM: .data and .bss. Today they live in AXI SRAM, and the
#          stack has DTCM to itself. Under the QSPI bootloader layout (DESIGN.md §2.3) they move
#          into DTCM and share its 128 KiB with the stack, so the budget is DTCM less a stack
#          reserve. Within it, that move stays open.
#
# Both come from one section table, `objdump -h`, which gives each section's size, load address
# and flags.
#
# Inputs: OBJDUMP_TOOL (arm-none-eabi-objdump), ELF, FLASH_BYTES, RAM_BYTES.

# A script run with cmake -P inherits no policies from the project. Without its own, CMake 3.x
# leaves CMP0057 unset and rejects IN_LIST below.
cmake_minimum_required(VERSION 3.25)

# The internal flash, as libDaisy's linker script declares it. The linker refuses an image that
# does not fit, so no load image lies beyond it, and none elsewhere (QSPI) counts.
math(EXPR flash_origin "0x08000000")
math(EXPR flash_region_end "0x08000000 + (128 * 1024)")

# The sections the bootloader layout places in DTCM. The linker adds .got and .got.plt after
# .data; libDaisy's .dtcmram_bss is for variables placed in DTCM explicitly.
set(ram_sections .data .got .got.plt .bss .dtcmram_bss)

execute_process(
    COMMAND "${OBJDUMP_TOOL}" -h "${ELF}"
    OUTPUT_VARIABLE section_table
    COMMAND_ERROR_IS_FATAL ANY
)

# Two lines per section: "index name size VMA LMA file-offset alignment", in hex, then its flags.
# A load image is a section flagged LOAD; its load address (LMA) is where it sits in flash. .bss
# has a load address too, but no LOAD flag, and takes no flash.
string(REPLACE "\n" ";" table_lines "${section_table}")
set(image_end ${flash_origin})
set(ram_used 0)
set(sections_seen "")
set(flags_next FALSE)

foreach(line IN LISTS table_lines)
    if(flags_next)
        set(flags_next FALSE)

        if(line MATCHES "[ ,]LOAD(,|$)"
           AND load_address GREATER_EQUAL flash_origin
           AND load_address LESS flash_region_end)
            math(EXPR load_end "${load_address} + ${section_size}")

            if(load_end GREATER image_end)
                set(image_end ${load_end})
            endif()
        endif()
    elseif(line MATCHES "^ *[0-9]+ ([^ ]+) +([0-9a-f]+) +[0-9a-f]+ +([0-9a-f]+) +[0-9a-f]+ +2[*][*][0-9]+$")
        set(section_name "${CMAKE_MATCH_1}")
        math(EXPR section_size "0x${CMAKE_MATCH_2}")
        math(EXPR load_address "0x${CMAKE_MATCH_3}")
        list(APPEND sections_seen "${section_name}")
        set(flags_next TRUE)

        if(section_name IN_LIST ram_sections)
            math(EXPR ram_used "${ram_used} + ${section_size}")
        endif()
    endif()
endforeach()

# Every image has .data, .bss and a load image in flash. Without them the table was not
# understood, and a total of zero would pass silently.
if(NOT ".data" IN_LIST sections_seen
   OR NOT ".bss" IN_LIST sections_seen
   OR image_end EQUAL flash_origin)
    message(FATAL_ERROR "could not read the section table of ${ELF}")
endif()

math(EXPR flash_used "${image_end} - ${flash_origin}")
math(EXPR flash_percent "(${flash_used} * 100) / ${FLASH_BYTES}")

message(STATUS "firmware flash: ${flash_used} / ${FLASH_BYTES} bytes (${flash_percent}%)")

if(flash_used GREATER FLASH_BYTES)
    math(EXPR flash_over "${flash_used} - ${FLASH_BYTES}")
    message(FATAL_ERROR "firmware exceeds the flash budget (${FLASH_BYTES} bytes) by ${flash_over}")
endif()

math(EXPR ram_percent "(${ram_used} * 100) / ${RAM_BYTES}")

message(STATUS "firmware RAM: ${ram_used} / ${RAM_BYTES} bytes (${ram_percent}%)")

if(ram_used GREATER RAM_BYTES)
    math(EXPR ram_over "${ram_used} - ${RAM_BYTES}")
    message(FATAL_ERROR "firmware exceeds the RAM budget (${RAM_BYTES} bytes) by ${ram_over}")
endif()
