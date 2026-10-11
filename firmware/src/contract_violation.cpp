// SPDX-License-Identifier: GPL-3.0-or-later
// Previously firmware — contract violation handler.
//
// P0 behaviour: stop everything so the failure is obvious and a debugger can inspect it. P0 (rig)
// replaces this with a crash record that survives reset and is reported over telemetry on boot.

#include "previously/contract.hpp"

namespace previously {

void contract_violation(const char* /*kind*/, std::source_location /*location*/) noexcept
{
    // Disable interrupts (audio stops), then halt here for the debugger.
    asm volatile("cpsid i" ::: "memory");  // NOLINT(hicpp-no-assembler)

    while (true) {
        asm volatile("bkpt #0");  // NOLINT(hicpp-no-assembler)
    }
}

}  // namespace previously
