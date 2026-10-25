# Checks T0's check of the firmware engine's flags without the ARM toolchain.
# cmake/check_firmware_engine_flags.cmake runs in a fresh cmake -P, as the firmware build runs it,
# against fixture compile commands. Inputs: CHECK_SCRIPT, WORK_DIR.

cmake_minimum_required(VERSION 3.25)  # script mode inherits no policies from the project

file(MAKE_DIRECTORY "${WORK_DIR}")

set(engine "/repo/engine/src")

# One compile command, as the firmware build writes it: GCC's options, then the file.
function(entry out file options)
    set(${out} "{\"directory\": \"/repo/build/firmware\", \"command\": \"arm-none-eabi-g++ -mthumb ${options} -c ${file}\", \"file\": \"${file}\"}" PARENT_SCOPE)
endfunction()

set(good "-ffp-contract=off -O3 -flto")

entry(clock   "${engine}/clock_tracker.cpp" "${good}")
entry(recorder "${engine}/recorder.cpp" "${good}")
entry(main    "/repo/firmware/src/main.cpp" "-O3 -flto -DNDEBUG")

function(write_fixture name)
    list(JOIN ARGN ", " entries)
    file(WRITE "${WORK_DIR}/${name}.json" "[${entries}]")
endfunction()

write_fixture(built "${clock}" "${recorder}" "${main}")

entry(no_lto "${engine}/recorder.cpp" "-ffp-contract=off -O3")
write_fixture(no_lto "${clock}" "${no_lto}")

entry(overridden "${engine}/recorder.cpp" "${good} -O0")
write_fixture(overridden "${clock}" "${overridden}")

entry(unoptimized "${engine}/recorder.cpp" "-ffp-contract=off -flto")
write_fixture(unoptimized "${clock}" "${unoptimized}")

entry(contracted "${engine}/recorder.cpp" "-O3 -flto")
write_fixture(contracted "${clock}" "${contracted}")

entry(ndebug "${engine}/recorder.cpp" "${good} -DNDEBUG")
write_fixture(ndebug "${clock}" "${ndebug}")

# Each option's last setting wins, as it does for the compiler.
entry(lto_undone "${engine}/recorder.cpp" "${good} -fno-lto")
write_fixture(lto_undone "${clock}" "${lto_undone}")

entry(lto_again "${engine}/recorder.cpp" "-ffp-contract=off -O3 -fno-lto -flto")
write_fixture(lto_again "${clock}" "${lto_again}")

entry(contract_fast "${engine}/recorder.cpp" "${good} -ffp-contract=fast")
write_fixture(contract_fast "${clock}" "${contract_fast}")

entry(contract_on "${engine}/recorder.cpp" "${good} -ffp-contract=on")
write_fixture(contract_on "${clock}" "${contract_on}")

entry(ndebug_undone "${engine}/recorder.cpp" "${good} -DNDEBUG -UNDEBUG")
write_fixture(ndebug_undone "${clock}" "${ndebug_undone}")

# The engine's sources found through a path that is not normalized, as the firmware's root is.
entry(unnormalized "/repo/firmware/../engine/src/recorder.cpp" "-ffp-contract=off -O3")
write_fixture(unnormalized "${clock}" "${unnormalized}")

write_fixture(no_engine "${main}")
write_fixture(empty)
file(WRITE "${WORK_DIR}/unreadable.json" "not json")

# Runs the check and requires its exit status (0 or not) and a line of its output.
function(expect name fixture should_pass expected_output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -DCOMPILE_COMMANDS=${WORK_DIR}/${fixture}.json
                -DENGINE_SOURCES=${engine}
                -DOPT_LEVEL=-O3
                -P "${CHECK_SCRIPT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE  output
    )

    if(should_pass AND NOT result EQUAL 0)
        message(SEND_ERROR "${name}: expected to pass, failed with ${result}:\n${output}")
    elseif(NOT should_pass AND result EQUAL 0)
        message(SEND_ERROR "${name}: expected to fail, passed:\n${output}")
    elseif(NOT output MATCHES "${expected_output}")
        message(SEND_ERROR "${name}: expected \"${expected_output}\" in:\n${output}")
    endif()
endfunction()

expect("the engine at the release level passes, and main.cpp is not checked" built TRUE
       "firmware engine: 2 sources at -O3 with -flto")
expect("without -flto fails" no_lto FALSE "recorder.cpp is compiled without LTO")
expect("a later -O wins, and fails" overridden FALSE "recorder.cpp is compiled at '-O0', not -O3")
expect("no -O at all fails" unoptimized FALSE "recorder.cpp is compiled at '', not -O3")
expect("without -ffp-contract=off fails" contracted FALSE
       "recorder.cpp's last -ffp-contract is '', not off")
expect("NDEBUG fails" ndebug FALSE "recorder.cpp is compiled with NDEBUG")
expect("a later -fno-lto fails" lto_undone FALSE "recorder.cpp is compiled without LTO")
expect("a later -flto passes" lto_again TRUE "firmware engine: 2 sources at -O3 with -flto")
expect("a later -ffp-contract=fast fails" contract_fast FALSE
       "recorder.cpp's last -ffp-contract is 'fast', not off")
expect("a later -ffp-contract=on fails" contract_on FALSE
       "recorder.cpp's last -ffp-contract is 'on', not off")
expect("a later -UNDEBUG passes" ndebug_undone TRUE "firmware engine: 2 sources at -O3 with -flto")
expect("an engine source on a path not normalized is checked too" unnormalized FALSE
       "recorder.cpp is compiled without LTO")
expect("commands without the engine fail" no_engine FALSE "compiles no source in")
expect("no commands at all fail" empty FALSE "compiles no source in")
expect("unreadable commands fail" unreadable FALSE "could not read")
