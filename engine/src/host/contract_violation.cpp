// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — contract violation handler for host builds (tests, tools, Rack).

#include "previously/contract.hpp"

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace previously {

namespace {

void write_stderr(std::string_view text) noexcept
{
    std::fwrite(text.data(), 1, text.size(), stderr);
}

}  // namespace

void contract_violation(const char* kind, std::source_location location) noexcept
{
    // Formats "file:line: kind violated in function" without allocating or using varargs, so it
    // works even when the failure is an exhausted heap.
    std::array<char, 16> line_digits{};
    const auto [line_end, error] =
        std::to_chars(line_digits.data(), std::to_address(line_digits.end()), location.line());

    write_stderr(location.file_name());
    write_stderr(":");
    write_stderr(error == std::errc{} ? std::string_view{line_digits.data(), line_end} : "?");
    write_stderr(": ");
    write_stderr(kind);
    write_stderr(" violated in ");
    write_stderr(location.function_name());
    write_stderr("\n");

    std::abort();
}

}  // namespace previously
