// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: the flush-to-zero mode, for each CPU the tests run on.

#include "flush_to_zero_mode.hpp"

#ifdef __aarch64__
#include <arm_acle.h>
#elif defined(__x86_64__)
#include <xmmintrin.h>
#else
#error "flush_to_zero_mode needs this CPU's flush-to-zero bits"
#endif

namespace previously::test {
namespace {

#ifdef __aarch64__

constexpr std::uint64_t fpcr_flush_to_zero = std::uint64_t{1} << 24U;  // FPCR.FZ

std::uint64_t read_mode() noexcept
{
    return __arm_rsr64("fpcr");
}

void write_mode(std::uint64_t mode) noexcept
{
    __arm_wsr64("fpcr", mode);
}

std::uint64_t with_flush_to_zero(std::uint64_t mode) noexcept
{
    return mode | fpcr_flush_to_zero;
}

#else

constexpr std::uint64_t mxcsr_flush_to_zero      = 0x8000U;  // FTZ: subnormal results become zero
constexpr std::uint64_t mxcsr_denormals_are_zero = 0x0040U;  // DAZ: subnormal inputs read as zero

std::uint64_t read_mode() noexcept
{
    return _mm_getcsr();
}

void write_mode(std::uint64_t mode) noexcept
{
    _mm_setcsr(static_cast<unsigned int>(mode));
}

std::uint64_t with_flush_to_zero(std::uint64_t mode) noexcept
{
    return mode | mxcsr_flush_to_zero | mxcsr_denormals_are_zero;
}

#endif

}  // namespace

flush_to_zero_mode::flush_to_zero_mode() noexcept
    : saved_{read_mode()}
{
    write_mode(with_flush_to_zero(saved_));
}

flush_to_zero_mode::~flush_to_zero_mode()
{
    write_mode(saved_);
}

}  // namespace previously::test
