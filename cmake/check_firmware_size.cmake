# Firmware size budgets (tier T0). Fails the build when either is exceeded:
#
#   flash  the image in the STM32H750's internal flash, counted as the linker counts it: from the
#          start of flash to the end of the furthest load image, alignment padding included.
#   RAM    the variables in internal RAM: .data and .bss. Today they live in AXI SRAM, and the
#          stack has DTCM to itself. Under the QSPI bootloader layout (DESIGN.md §2.3) they move
#          into DTCM and share its 128 KiB with the stack, so the budget is DTCM less a stack
#          reserve. Within it, that move stays open. Counted the same way: from the first
#          variable to the end of the furthest, alignment padding included.
#
# Both come from one section table, `objdump -h`, which gives each section's size, addresses and
# flags.
#
# Inputs: OBJDUMP_TOOL, ELF, FLASH_ORIGIN and FLASH_REGION_BYTES (the flash region as the linker
# script declares it), FLASH_BYTES and RAM_BYTES (the budgets).

# A script run with cmake -P inherits no policies from the project. Without its own, CMake 3.x
# leaves CMP0057 unset and rejects IN_LIST below.
cmake_minimum_required(VERSION 3.25)

foreach(input IN ITEMS OBJDUMP_TOOL ELF FLASH_ORIGIN FLASH_REGION_BYTES FLASH_BYTES RAM_BYTES)
    if(NOT DEFINED ${input})
        message(FATAL_ERROR "check_firmware_size.cmake needs ${input}")
    endif()
endforeach()

# Load images outside the region (QSPI) take none of the internal flash.
math(EXPR flash_origin "${FLASH_ORIGIN}")
math(EXPR flash_region_end "${FLASH_ORIGIN} + ${FLASH_REGION_BYTES}")

# The variables, by where they live today. The linker adds .got and .got.plt after .data;
# libDaisy's .dtcmram_bss is for variables placed in DTCM explicitly.
set(sram_sections .data .got .got.plt .bss)
set(dtcm_sections .dtcmram_bss)

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
set(sram_start "")
set(sram_end 0)
set(dtcm_start "")
set(dtcm_end 0)
set(sections_seen "")
set(flags_next FALSE)

# Widens a span of addresses, kept as <group>_start and <group>_end, to cover [start, end).
macro(cover group start end)
    if("${${group}_start}" STREQUAL "" OR ${start} LESS ${group}_start)
        set(${group}_start ${start})
    endif()

    if(${end} GREATER ${group}_end)
        set(${group}_end ${end})
    endif()
endmacro()

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
    elseif(line MATCHES "^ *[0-9]+ ([^ ]+) +([0-9a-f]+) +([0-9a-f]+) +([0-9a-f]+) +[0-9a-f]+ +2[*][*][0-9]+$")
        set(section_name "${CMAKE_MATCH_1}")
        math(EXPR section_size "0x${CMAKE_MATCH_2}")
        math(EXPR address "0x${CMAKE_MATCH_3}")
        math(EXPR load_address "0x${CMAKE_MATCH_4}")
        math(EXPR address_end "${address} + ${section_size}")
        list(APPEND sections_seen "${section_name}")
        set(flags_next TRUE)

        if(section_name IN_LIST sram_sections)
            cover(sram ${address} ${address_end})
        elseif(section_name IN_LIST dtcm_sections)
            cover(dtcm ${address} ${address_end})
        endif()
    endif()
endforeach()

# Every image has .data and .bss. Without either, the table was not understood, and RAM would be
# understated.
if(NOT ".data" IN_LIST sections_seen OR NOT ".bss" IN_LIST sections_seen)
    message(FATAL_ERROR "could not read the section table of ${ELF}")
endif()

# Every image has a load image in flash. Without one, the region given does not match the link.
if(image_end EQUAL flash_origin)
    message(FATAL_ERROR "no load image in flash (FLASH_ORIGIN ${FLASH_ORIGIN}, FLASH_REGION_BYTES "
                        "${FLASH_REGION_BYTES}): do they match the linker script?")
endif()

math(EXPR flash_used "${image_end} - ${flash_origin}")
math(EXPR flash_percent "(${flash_used} * 100) / ${FLASH_BYTES}")

message(STATUS "firmware flash: ${flash_used} / ${FLASH_BYTES} bytes (${flash_percent}%)")

if(flash_used GREATER FLASH_BYTES)
    math(EXPR flash_over "${flash_used} - ${FLASH_BYTES}")
    message(FATAL_ERROR "firmware exceeds the flash budget (${FLASH_BYTES} bytes) by ${flash_over}")
endif()

set(ram_used 0)

foreach(group IN ITEMS sram dtcm)
    if(NOT "${${group}_start}" STREQUAL "")
        math(EXPR ram_used "${ram_used} + ${${group}_end} - ${${group}_start}")
    endif()
endforeach()

math(EXPR ram_percent "(${ram_used} * 100) / ${RAM_BYTES}")

message(STATUS "firmware RAM: ${ram_used} / ${RAM_BYTES} bytes (${ram_percent}%)")

if(ram_used GREATER RAM_BYTES)
    math(EXPR ram_over "${ram_used} - ${RAM_BYTES}")
    message(FATAL_ERROR "firmware exceeds the RAM budget (${RAM_BYTES} bytes) by ${ram_over}")
endif()
