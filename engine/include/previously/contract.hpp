// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — contract checks (preconditions and postconditions).
//
// Contracts state what must be true at a boundary. They are plain functions, not macros, in the
// style of the C++ Core Guidelines' Expects() and Ensures(): std::source_location supplies the
// file and line.
//
// Each target defines what a violation does. Host builds print and abort. Firmware records the
// failure for telemetry and halts. Contracts are compiled out when NDEBUG is defined, so every
// condition must be free of side effects.

#pragma once

#include <source_location>

namespace previously {

#ifdef NDEBUG
inline constexpr bool contracts_enabled = false;
#else
inline constexpr bool contracts_enabled = true;
#endif

/// Reports a failed contract. Defined once per target (host support library or firmware).
[[noreturn]] void contract_violation(const char* kind, std::source_location location) noexcept;

/// Precondition: what a function requires of its caller.
constexpr void expects(bool                 condition,
                       std::source_location location = std::source_location::current()) noexcept
{
    if constexpr (contracts_enabled) {
        if (!condition) {
            contract_violation("precondition", location);
        }
    }
}

/// Postcondition: what a function guarantees to its caller.
constexpr void ensures(bool                 condition,
                       std::source_location location = std::source_location::current()) noexcept
{
    if constexpr (contracts_enabled) {
        if (!condition) {
            contract_violation("postcondition", location);
        }
    }
}

}  // namespace previously
