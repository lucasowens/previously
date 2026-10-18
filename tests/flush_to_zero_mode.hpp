// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: runs code with subnormals flushed to zero, as Rack's engine thread does.
//
// Rack's rack::system::resetFpuFlags() sets FPCR.FZ on arm64, and MXCSR's FTZ and DAZ on x86-64,
// so subnormal inputs and results become zero. The host tests otherwise run with the IEEE default,
// which keeps them. ADR 0005 promises the engine computes the same bits either way, and a test
// checks it by running the same input under both modes.

#pragma once

#include <cstdint>

namespace previously::test {

/// While one is alive, this thread's FPU flushes subnormals to zero, with the bits Rack sets. The
/// previous mode comes back when it is destroyed.
class flush_to_zero_mode {
public:
    flush_to_zero_mode() noexcept;
    ~flush_to_zero_mode();

    flush_to_zero_mode(const flush_to_zero_mode&)            = delete;
    flush_to_zero_mode& operator=(const flush_to_zero_mode&) = delete;
    flush_to_zero_mode(flush_to_zero_mode&&)                 = delete;
    flush_to_zero_mode& operator=(flush_to_zero_mode&&)      = delete;

private:
    std::uint64_t saved_;
};

}  // namespace previously::test
