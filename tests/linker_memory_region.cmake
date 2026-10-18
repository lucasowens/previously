# Checks cmake/linker_memory_region.cmake: reading a region from a linker script's MEMORY block,
# on libDaisy's own scripts and on the cases they do not show. Inputs: MODULE, LIBDAISY_CORE,
# WORK_DIR.

cmake_minimum_required(VERSION 3.25)  # script mode inherits no policies from the project

include("${MODULE}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# Requires one region's origin and length, in bytes ("" for both when there is no such region).
function(expect_region name script region expected_origin expected_length)
    read_memory_region("${script}" ${region} origin length)

    if(NOT "${origin}" STREQUAL "${expected_origin}" OR NOT "${length}" STREQUAL "${expected_length}")
        message(SEND_ERROR "${name}: expected origin \"${expected_origin}\" and length "
                           "\"${expected_length}\", read \"${origin}\" and \"${length}\"")
    endif()
endfunction()

set(flash_layout "${LIBDAISY_CORE}/STM32H750IB_flash.lds")
set(bootloader_layout "${LIBDAISY_CORE}/STM32H750IB_sram.lds")

# libDaisy's scripts, as the firmware links with them.
expect_region("flash, in K" "${flash_layout}" FLASH 134217728 131072)
expect_region("DTCM" "${flash_layout}" DTCMRAM 536870912 131072)
expect_region("SDRAM, in M" "${flash_layout}" SDRAM 3221225472 67108864)
expect_region("a length that is a difference" "${bootloader_layout}" SRAM 603979776 491520)
expect_region("QSPI, past the bootloader" "${bootloader_layout}" QSPIFLASH 2416181248 8126464)

# A name is matched whole: RAM_D2 is not RAM, and FLASH is not FLASH2.
expect_region("a name that is another's prefix" "${flash_layout}" RAM "" "")
expect_region("a region the script lacks" "${flash_layout}" FLASH2 "" "")

# A comment is not part of a line, and a region inside one is not a region.
file(WRITE "${WORK_DIR}/comments.lds" [=[
MEMORY
{
    FLASH (RX) : ORIGIN = 0x08000000, LENGTH = 128K /* internal */
    /*
    SRAM  (RWX) : ORIGIN = 0x00000000, LENGTH = 1K
    */
    SRAM  (RWX) : ORIGIN = 0x24000000, LENGTH = 512K
}
]=])
expect_region("a region beside comments" "${WORK_DIR}/comments.lds" FLASH 134217728 131072)
expect_region("a commented-out duplicate" "${WORK_DIR}/comments.lds" SRAM 603979776 524288)

# A region written in syntax the reader does not know is not guessed at.
file(WRITE "${WORK_DIR}/unknown.lds" [=[
MEMORY
{
    FLASH (RX) : ORIGIN = DEFINED(boot) ? 0x08020000 : 0x08000000, LENGTH = 128K
}
]=])
expect_region("syntax it does not know" "${WORK_DIR}/unknown.lds" FLASH "" "")
