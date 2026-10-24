// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: golden images of the screen (DESIGN.md §9.3).
//
// A golden image is a plain PBM file (P1): a comment saying what it shows, its size, then a line
// of 128 digits for each row of pixels, 1 where a pixel is lit. A diff of one shows which rows
// changed, and the 1s draw the picture. With PREVIOUSLY_BLESS set in the environment, the tests
// write their images instead of comparing them: `make bless`, once the change is understood.

#pragma once

#include "previously/framebuffer.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace previously::test {

/// `screen` as a plain PBM, with `description` as its comment.
[[nodiscard]] std::string to_pbm(const framebuffer& screen, std::string_view description);

/// The pixels of a plain PBM the size of the screen, or nothing if `text` is not one.
[[nodiscard]] std::optional<framebuffer> from_pbm(std::string_view text);

/// Checks `screen` against the golden image `name`, which shows `description`. On a mismatch it
/// writes what was drawn into the build's output and says how many pixels differ, and where the
/// first is. With PREVIOUSLY_BLESS set, it writes the golden image instead.
void check_golden_image(const framebuffer& screen,
                        std::string_view   name,
                        std::string_view   description);

}  // namespace previously::test
