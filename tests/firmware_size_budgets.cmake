# Checks T0's firmware budgets without the ARM toolchain. cmake/check_firmware_size.cmake runs in
# a fresh cmake -P, as the firmware build runs it, against a stand-in for arm-none-eabi-objdump
# that prints a fixture. Inputs: CHECK_SCRIPT, WORK_DIR.

cmake_minimum_required(VERSION 3.25)  # script mode inherits no policies from the project

file(MAKE_DIRECTORY "${WORK_DIR}")

# The stand-in prints the fixture's section table. Its last argument, the "ELF", is a fixture
# directory.
set(fake_objdump "${WORK_DIR}/objdump")
file(WRITE "${fake_objdump}" [=[#!/bin/sh
for elf; do :; done
cat "$elf/sections.txt"
]=])
file(CHMOD "${fake_objdump}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)

# A real image, this firmware just after PR #7, as `arm-none-eabi-objdump -h` printed it.
#
# - Flash, as the linker counts it: from 0x08000000 to the end of .got.plt's load image at
#   0x080180ac, 98,476 bytes. That includes 8 bytes of padding before .text, which is aligned to
#   16: the contents alone are 98,468.
# - .bss also has a load address in flash, but no LOAD flag, so it takes no flash.
# - RAM: .data, .got, .got.plt and .bss, which abut, 80,404 bytes. .sram1_bss lives in RAM_D2 and
#   does not count.
set(sections [=[
previously.elf:     file format elf32-littlearm

Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .isr_vector   00000298  08000000  08000000  00001000  2**2
                  CONTENTS, ALLOC, LOAD, DATA
  1 .text         000173b4  080002a0  080002a0  000012a0  2**4
                  CONTENTS, ALLOC, LOAD, READONLY, CODE
  2 .ARM.extab    00000000  08017654  08017654  00019a40  2**0
                  CONTENTS
  3 .exidx        00000008  08017654  08017654  00018654  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, DATA
  4 .ARM.attributes 0000002e  00000000  00000000  00019a40  2**0
                  CONTENTS, READONLY
  5 .init_array   0000000c  0801765c  0801765c  0001865c  2**2
                  CONTENTS, ALLOC, LOAD, DATA
  6 .fini_array   00000004  08017668  08017668  00018668  2**2
                  CONTENTS, ALLOC, LOAD, DATA
  7 .data         00000a18  24000000  0801766c  00019000  2**3
                  CONTENTS, ALLOC, LOAD, DATA
  8 .got          0000001c  24000a18  08018084  00019a18  2**2
                  CONTENTS, ALLOC, LOAD, DATA
  9 .got.plt      0000000c  24000a34  080180a0  00019a34  2**2
                  CONTENTS, ALLOC, LOAD, DATA
 10 .bss          00012fd4  24000a40  080180ac  00019a40  2**3
                  ALLOC
 11 .dtcmram_bss  00000000  20000000  20000000  00019a6e  2**0
                  CONTENTS
 12 .sram1_bss    00004240  30000000  30000000  0001a000  2**2
                  ALLOC
 13 .sdram_bss    00000000  c0000000  c0000000  00019a6e  2**0
                  CONTENTS
 14 .backup_sram  0000000c  38800000  38800000  0001a000  2**2
                  ALLOC
 15 .qspiflash_text 00000000  90000000  90000000  00019a6e  2**0
                  CONTENTS
 16 .qspiflash_data 00000000  90000000  90000000  00019a6e  2**0
                  CONTENTS
 17 .qspiflash_bss 00000000  90000000  90000000  00019a6e  2**0
                  CONTENTS
 18 .heap         00000000  24013a14  24013a14  00019a6e  2**0
                  CONTENTS
 19 .reserved_for_stack 00000000  24013a14  24013a14  00019a6e  2**0
                  CONTENTS
 20 DISCARD       00000720  24013a14  24013a14  00019a70  2**2
                  CONTENTS, READONLY
 21 .comment      00000045  00000000  00000000  0001a190  2**0
                  CONTENTS, READONLY
 22 .debug_frame  0000074c  00000000  00000000  0001a1d8  2**2
                  CONTENTS, READONLY, DEBUGGING, OCTETS
 23 .debug_line_str 000001d5  00000000  00000000  0001a924  2**0
                  CONTENTS, READONLY, DEBUGGING, OCTETS
]=])

function(write_fixture name table)
    file(WRITE "${WORK_DIR}/${name}/sections.txt" "${table}")
endfunction()

# The image's table with one change. A change that finds nothing to change would leave the case
# testing the plain image, so it fails instead.
function(write_variant name from to)
    string(REPLACE "${from}" "${to}" table "${sections}")

    if(table STREQUAL sections)
        message(FATAL_ERROR "fixture ${name}: \"${from}\" not found")
    endif()

    write_fixture(${name} "${table}")
endfunction()

write_fixture(image "${sections}")

# Variables placed in DTCM explicitly count towards RAM.
write_variant(dtcm ".dtcmram_bss  00000000" ".dtcmram_bss  00000400")

# A load image in the QSPI flash takes none of the internal flash.
write_variant(qspi [=[
 15 .qspiflash_text 00000000  90000000  90000000  00019a6e  2**0
                  CONTENTS]=] [=[
 15 .qspiflash_text 00000100  90000000  90000000  00019a6e  2**0
                  CONTENTS, ALLOC, LOAD, READONLY, CODE]=])

# The table lists sections in header order, not address order, so the image ends at the furthest
# load image, not the last one listed. This one fills the padding before .text exactly.
write_variant(out_of_order [=[
 23 .debug_line_str 000001d5  00000000  00000000  0001a924  2**0
                  CONTENTS, READONLY, DEBUGGING, OCTETS]=] [=[
 23 .debug_line_str 000001d5  00000000  00000000  0001a924  2**0
                  CONTENTS, READONLY, DEBUGGING, OCTETS
 24 .early_rodata 00000008  08000298  08000298  0001ab00  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, DATA]=])

# Padding between the variables takes RAM too. Here .got.plt ends at 0x24000a44 and .bss, aligned
# to 8, starts at 0x24000a48: the variables span 80,412 bytes, and their contents sum to 80,408.
write_variant(ram_padding [=[
  9 .got.plt      0000000c  24000a34  080180a0  00019a34  2**2
                  CONTENTS, ALLOC, LOAD, DATA
 10 .bss          00012fd4  24000a40  080180ac  00019a40  2**3]=] [=[
  9 .got.plt      00000010  24000a34  080180a0  00019a34  2**2
                  CONTENTS, ALLOC, LOAD, DATA
 10 .bss          00012fd4  24000a48  080180b0  00019a48  2**3]=])

# The variables' span runs from the lowest to the highest address, whatever the table order: here
# .data, the lowest, is listed last.
set(data_entry [=[
  7 .data         00000a18  24000000  0801766c  00019000  2**3
                  CONTENTS, ALLOC, LOAD, DATA
]=])
string(REPLACE "${data_entry}" "" without_data "${sections}")

if(without_data STREQUAL sections)
    message(FATAL_ERROR "fixture ram_out_of_order: .data's entry not found")
endif()

write_fixture(ram_out_of_order "${without_data}\n${data_entry}")

# A table whose flags are not understood has no load image in flash, which would read as an empty
# image.
write_variant(no_load_images " LOAD," " LOADED,")

# A table missing .data, or missing .bss, would understate RAM. Each is checked on its own.
write_variant(no_data ".data         00000a18" ".ddata        00000a18")
write_variant(no_bss ".bss          00012fd4" ".bsss         00012fd4")

# A table with no sections at all was not understood.
write_fixture(unreadable "Sections:\nIdx Name          Size      VMA       LMA       File off  Algn\n")

# Runs the check and requires its exit status (0 or not) and a line of its output. CMake wraps a
# long error message, so a pattern spanning words allows any whitespace between them.
function(expect name fixture flash_bytes ram_bytes should_pass expected_output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -DOBJDUMP_TOOL=${fake_objdump}
                -DELF=${WORK_DIR}/${fixture}
                -DFLASH_ORIGIN=0x08000000
                -DFLASH_REGION_BYTES=131072
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

expect("both budgets met exactly" image 98476 80404 TRUE
       "firmware flash: 98476 / 98476 bytes.*firmware RAM: 80404 / 80404 bytes")
expect("flash one byte over" image 98475 114688 FALSE
       "exceeds the flash budget \\(98475 bytes\\) by 1")
expect("padding takes flash too" image 98468 114688 FALSE
       "exceeds the flash budget \\(98468 bytes\\) by 8")
expect("RAM one byte over" image 131072 80403 FALSE
       "exceeds the RAM budget \\(80403 bytes\\) by 1")
expect("padding between the variables takes RAM too" ram_padding 131072 80411 FALSE
       "exceeds the RAM budget \\(80411 bytes\\) by 1")
expect("the variables span from the lowest address to the highest" ram_out_of_order 131072 80404
       TRUE "firmware RAM: 80404 / 80404 bytes")
expect("variables in DTCM count" dtcm 131072 114688 TRUE
       "firmware RAM: 81428 / 114688 bytes")
expect("a load image in QSPI takes no internal flash" qspi 98476 114688 TRUE
       "firmware flash: 98476 / 98476 bytes")
expect("the furthest load image ends the image" out_of_order 98476 114688 TRUE
       "firmware flash: 98476 / 98476 bytes")
expect("an unreadable section table fails" unreadable 131072 114688 FALSE
       "could not read the section table")
expect("a table with no load image in flash fails, naming the bounds" no_load_images 131072 114688
       FALSE "no load image in flash \\(FLASH_ORIGIN 0x08000000,[ \n]+FLASH_REGION_BYTES[ \n]+131072\\)")
expect("a table without .data fails" no_data 131072 114688 FALSE
       "could not read the section table")
expect("a table without .bss fails" no_bss 131072 114688 FALSE
       "could not read the section table")

# An input left out is named, rather than surfacing as an arithmetic error.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -DOBJDUMP_TOOL=${fake_objdump}
            -DELF=${WORK_DIR}/image
            -DFLASH_REGION_BYTES=131072
            -DFLASH_BYTES=131072
            -DRAM_BYTES=114688
            -P "${CHECK_SCRIPT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE  output
)

if(result EQUAL 0 OR NOT output MATCHES "needs FLASH_ORIGIN")
    message(SEND_ERROR "a missing input: expected \"needs FLASH_ORIGIN\", got ${result}:\n${output}")
endif()
