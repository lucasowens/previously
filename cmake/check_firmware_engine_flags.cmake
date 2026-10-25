# Firmware engine flags (tier T0). Fails the build unless the engine is compiled for the firmware as
# libDaisy compiles the firmware's own sources (issue #11). Every engine source's compile command
# must have:
#
#   - the release optimization (OPT_LEVEL) as its last -O option, since a later one would win;
#   - -flto, so the engine is optimized across files with the rest of the image;
#   - -ffp-contract=off, the floating point every target shares (ADR 0005);
#   - no NDEBUG, so the engine's contracts stay on in the firmware.
#
# The commands come from the compile_commands.json the build writes, so they are the commands the
# build runs, with every option from the toolchain, the directory and the target.
#
# Inputs: COMPILE_COMMANDS (the compile_commands.json), ENGINE_SOURCES (the engine's source
# directory), OPT_LEVEL (the release optimization, such as -O3).

# A script run with cmake -P inherits no policies from the project.
cmake_minimum_required(VERSION 3.25)

foreach(input IN ITEMS COMPILE_COMMANDS ENGINE_SOURCES OPT_LEVEL)
    if(NOT DEFINED ${input})
        message(FATAL_ERROR "check_firmware_engine_flags.cmake needs ${input}")
    endif()
endforeach()

file(READ "${COMPILE_COMMANDS}" commands)
string(JSON entries ERROR_VARIABLE unreadable LENGTH "${commands}")

if(unreadable)
    message(FATAL_ERROR "could not read ${COMPILE_COMMANDS}: ${unreadable}")
endif()

set(checked  0)
set(failures "")

if(entries GREATER 0)
    math(EXPR last_entry "${entries} - 1")

    foreach(index RANGE ${last_entry})
        string(JSON file GET "${commands}" ${index} file)
        cmake_path(IS_PREFIX ENGINE_SOURCES "${file}" NORMALIZE in_engine)

        if(NOT in_engine)
            continue()
        endif()

        string(JSON command GET "${commands}" ${index} command)
        separate_arguments(arguments UNIX_COMMAND "${command}")

        set(level    "")
        set(lto      OFF)
        set(contract OFF)
        set(ndebug   OFF)

        foreach(argument IN LISTS arguments)
            if(argument MATCHES "^-O")
                set(level "${argument}")
            elseif(argument MATCHES "^-flto(=.*)?$")
                set(lto ON)
            elseif(argument STREQUAL "-ffp-contract=off")
                set(contract ON)
            elseif(argument MATCHES "^-DNDEBUG(=.*)?$")
                set(ndebug ON)
            endif()
        endforeach()

        cmake_path(GET file FILENAME source)

        if(NOT level STREQUAL OPT_LEVEL)
            list(APPEND failures "${source} is compiled at '${level}', not ${OPT_LEVEL}")
        endif()

        if(NOT lto)
            list(APPEND failures "${source} is compiled without -flto")
        endif()

        if(NOT contract)
            list(APPEND failures "${source} is compiled without -ffp-contract=off")
        endif()

        if(ndebug)
            list(APPEND failures "${source} is compiled with NDEBUG, which turns its contracts off")
        endif()

        math(EXPR checked "${checked} + 1")
    endforeach()
endif()

if(checked EQUAL 0)
    message(FATAL_ERROR "${COMPILE_COMMANDS} compiles no source in ${ENGINE_SOURCES}")
endif()

if(failures)
    list(JOIN failures "\n  " report)
    message(FATAL_ERROR "the firmware's engine is not compiled for release:\n  ${report}")
endif()

message(STATUS "firmware engine: ${checked} sources at ${OPT_LEVEL} with -flto")
