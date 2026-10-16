# Checks T0's firmware budgets without the ARM toolchain. cmake/check_firmware_size.cmake runs in
# a fresh cmake -P, as the firmware build runs it, against a stand-in for arm-none-eabi-size that
# prints a fixture. Inputs: CHECK_SCRIPT, WORK_DIR.

cmake_minimum_required(VERSION 3.25)  # script mode inherits no policies from the project

file(MAKE_DIRECTORY "${WORK_DIR}")

# The stand-in prints the fixture for the format asked for. Its last argument, the "ELF", is a
# fixture directory.
set(fake_size "${WORK_DIR}/size")
file(WRITE "${fake_size}" [=[#!/bin/sh
for elf; do :; done
if [ "$1" = "-A" ]; then cat "$elf/sysv.txt"; else cat "$elf/berkeley.txt"; fi
]=])
file(CHMOD "${fake_size}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

# A real image, this firmware early in PR #7, as arm-none-eabi-size printed it (tabs shown as
# spaces). Flash is text + data = 98,388 bytes. RAM is .data, .got, .got.plt and .bss = 80,396
# bytes; .sram1_bss lives in RAM_D2 and does not count.
set(berkeley [=[
   text    data     bss     dec     hex filename
  95084    3304   94744  193132   2f26c previously.elf
]=])

set(sysv [=[
previously.elf  :
section                 size         addr
.isr_vector              664    134217728
.text                  95076    134218400
.ARM.extab                 0    134313476
.exidx                     8    134313476
.ARM.attributes           46            0
.init_array               12    134313484
.fini_array                4    134313496
.data                   2584    603979776
.got                      28    603982360
.got.plt                  12    603982388
.bss                   77772    603982400
.dtcmram_bss               0    536870912
.sram1_bss             16960    805306368
.sdram_bss                 0   3221225472
.backup_sram              12    947912704
.heap                      0    604060172
.reserved_for_stack        0    604060172
.comment                  69            0
Total                 197408
]=])

function(write_fixture name berkeley_text sysv_text)
    file(WRITE "${WORK_DIR}/${name}/berkeley.txt" "${berkeley_text}")
    file(WRITE "${WORK_DIR}/${name}/sysv.txt" "${sysv_text}")
endfunction()

write_fixture(image "${berkeley}" "${sysv}")

# Variables placed in DTCM explicitly count too.
string(REPLACE ".dtcmram_bss               0" ".dtcmram_bss            1024" sysv_dtcm "${sysv}")
write_fixture(dtcm "${berkeley}" "${sysv_dtcm}")

# A section table without .data or .bss was not understood.
write_fixture(unreadable "${berkeley}" "section size addr\n.text 95076 134218400\n")

# Runs the check and requires its exit status (0 or not) and a line of its output.
function(expect name fixture flash_bytes ram_bytes should_pass expected_output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -DSIZE_TOOL=${fake_size}
                -DELF=${WORK_DIR}/${fixture}
                -DFLASH_BYTES=${flash_bytes}
                -DRAM_BYTES=${ram_bytes}
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

expect("both budgets met exactly" image 98388 80396 TRUE
       "firmware flash: 98388 / 98388 bytes.*firmware RAM: 80396 / 80396 bytes")
expect("RAM one byte over" image 131072 80395 FALSE
       "exceeds the RAM budget \\(80395 bytes\\) by 1")
expect("flash one byte over" image 98387 114688 FALSE
       "exceeds the flash budget \\(98387 bytes\\) by 1")
expect("variables in DTCM count" dtcm 131072 114688 TRUE
       "firmware RAM: 81420 / 114688 bytes")
expect("an unreadable section table fails" unreadable 131072 114688 FALSE
       "could not read the section sizes")
