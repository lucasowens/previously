// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: what the tape's tests share. A signal and what the recorder should make of
// it, a feed that runs it through an engine, and pools that keep a failing test from aborting the
// run.

#pragma once

#include "previously/engine.hpp"
#include "previously/page_pool.hpp"
#include "previously/units.hpp"

#include <cstddef>
#include <vector>

namespace previously::test {

/// True when two frames hold the same bits.
[[nodiscard]] bool same_bits(const stereo_frame& a, const stereo_frame& b) noexcept;

/// Seeded stereo noise within ±1, the right channel offset by 0.5 for the DC blocker to remove,
/// and what the recorder should make of it: each channel through its own DC blocker.
struct stereo_noise {
    std::vector<float>        left;
    std::vector<float>        right;
    std::vector<stereo_frame> dc_blocked;

    stereo_noise(std::size_t frames, sample_rate rate);
};

/// The noise, fed to an engine block by block, and the engine's output.
struct engine_feed {
    stereo_noise       input;
    std::vector<float> out_left;
    std::vector<float> out_right;

    engine_feed(std::size_t frames, sample_rate rate);

    /// Feeds frames [from, to) in blocks of `block_frames`.
    void process(engine& target, std::size_t from, std::size_t to, std::size_t block_frames);
};

/// Checks, when a test ends, that every page of a pool has come back, then gives back any still
/// held. A page never given back then fails the test, rather than tripping the pool's destructor
/// contract, which aborts the run and hides every test after it. A failed REQUIRE unwinds past
/// the code that would give pages back; the audit gives them back then too, without a check.
class page_audit {
public:
    explicit page_audit(page_pool& pool) noexcept;
    ~page_audit();

    page_audit(const page_audit&)            = delete;
    page_audit& operator=(const page_audit&) = delete;
    page_audit(page_audit&&)                 = delete;
    page_audit& operator=(page_audit&&)      = delete;

private:
    page_pool* pool_;
    int        exceptions_at_start_;
};

/// A pool of small pages, the memory under it, and its audit. Declare anything that holds its
/// pages after it, so that holder goes first.
struct small_pool {
    static constexpr samples page_frames{16};

    std::vector<stereo_frame> memory;
    page_pool                 pool;
    page_audit                audit;

    explicit small_pool(std::size_t pages, std::size_t extra_frames = 0);
};

}  // namespace previously::test
