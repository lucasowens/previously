# Reads a memory region from a GNU linker script, so a check can take the region from the script
# the build links with instead of keeping its own copy of the numbers.
#
# A region is one line of the script's MEMORY block:
#
#     FLASH (RX) : ORIGIN = 0x08000000, LENGTH = 128K
#
# Origins and lengths are numbers, decimal or 0x hex, optionally with a K or M suffix, joined by +
# and -. That is everything libDaisy's scripts use (STM32H750IB_sram.lds has 512K - 32K). Anything
# else is not guessed at: the region reads as absent.

# Sets <origin_var> and <length_var> to the region's origin and length in bytes, or both to "" when
# the script has no region of that name, or its line is not understood.
function(read_memory_region script name origin_var length_var)
    set(${origin_var} "" PARENT_SCOPE)
    set(${length_var} "" PARENT_SCOPE)

    # The script without its comments: a commented-out line is not a region. A region's line can
    # only be in the MEMORY block, so the whole script can be searched.
    file(READ "${script}" text)
    string(REGEX REPLACE "/[*]([^*]|[*]+[^*/])*[*]+/" "" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")

    foreach(line IN LISTS lines)
        # The name, whole: RAM_D2 is not RAM.
        if(line MATCHES "^[ \t]*${name}[ \t]*(\\([^)]*\\))?[ \t]*:[ \t]*ORIGIN[ \t]*=([^,]+),[ \t]*LENGTH[ \t]*=(.+)$")
            set(origin_text "${CMAKE_MATCH_2}")
            set(length_text "${CMAKE_MATCH_3}")

            linker_number(origin "${origin_text}")
            linker_number(length "${length_text}")

            if(NOT "${origin}" STREQUAL "" AND NOT "${length}" STREQUAL "")
                set(${origin_var} ${origin} PARENT_SCOPE)
                set(${length_var} ${length} PARENT_SCOPE)
            endif()

            return()
        endif()
    endforeach()
endfunction()

# Sets <out_var> to the value of a linker expression of the form above, or "" if it is not one.
function(linker_number out_var expression)
    set(${out_var} "" PARENT_SCOPE)

    # Only numbers, suffixes, + and -: anything else (a symbol, a function, a ?:) is refused here,
    # before math() could reject it fatally.
    string(STRIP "${expression}" expression)

    if(NOT expression MATCHES "^(0x[0-9a-fA-F]+|[0-9]+[KM]?)([ \t]*[-+][ \t]*(0x[0-9a-fA-F]+|[0-9]+[KM]?))*$")
        return()
    endif()

    string(REGEX REPLACE "([0-9]+)K" "(\\1 * 1024)" expression "${expression}")
    string(REGEX REPLACE "([0-9]+)M" "(\\1 * 1048576)" expression "${expression}")
    math(EXPR value "${expression}")
    set(${out_var} ${value} PARENT_SCOPE)
endfunction()
