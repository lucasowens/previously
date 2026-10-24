// SPDX-License-Identifier: GPL-3.0-or-later
// Previously — tests: golden images of the screen.

#include "golden_image.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace previously::test {
namespace {

// Set by the build: where the golden images live, and where a mismatch leaves what was drawn.
[[nodiscard]] std::filesystem::path golden_directory()
{
    return PREVIOUSLY_DISPLAY_GOLDEN_DIR;
}

[[nodiscard]] std::filesystem::path output_directory()
{
    return PREVIOUSLY_DISPLAY_OUTPUT_DIR;
}

[[nodiscard]] bool bless_requested()
{
    const char* const value = std::getenv("PREVIOUSLY_BLESS");

    return value != nullptr && *value != '\0';
}

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};

    if (!file) {
        return std::nullopt;
    }

    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void write_file(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());

    std::ofstream file{path, std::ios::binary};

    file << text;

    REQUIRE_MESSAGE(file.good(), "could not write " << path);
}

/// Reads a plain PBM from the front: whitespace and comments, then numbers and pixels.
class pbm_reader {
public:
    explicit pbm_reader(std::string_view text) noexcept
        : text_{text}
    {
    }

    /// Skips whitespace, and comments, which run from '#' to the end of the line.
    void skip_space() noexcept
    {
        while (at_ < text_.size()) {
            if (text_[at_] == '#') {
                while (at_ < text_.size() && text_[at_] != '\n') {
                    ++at_;
                }
            }
            else if (is_space(text_[at_])) {
                ++at_;
            }
            else {
                return;
            }
        }
    }

    [[nodiscard]] bool take(std::string_view expected)
    {
        if (!text_.substr(at_).starts_with(expected)) {
            return false;
        }

        at_ += expected.size();

        return true;
    }

    [[nodiscard]] std::optional<std::size_t> number() noexcept
    {
        skip_space();

        const std::size_t start = at_;
        std::size_t       value = 0;

        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            value = (value * 10) + static_cast<std::size_t>(text_[at_] - '0');
            ++at_;
        }

        return at_ > start ? std::optional{value} : std::nullopt;
    }

    /// The next pixel: true for a lit one, or nothing if the next mark is not a pixel.
    [[nodiscard]] std::optional<bool> pixel() noexcept
    {
        while (at_ < text_.size() && is_space(text_[at_])) {
            ++at_;
        }

        if (at_ == text_.size() || (text_[at_] != '0' && text_[at_] != '1')) {
            return std::nullopt;
        }

        ++at_;

        return text_[at_ - 1] == '1';
    }

    [[nodiscard]] bool at_end() const noexcept
    {
        return at_ == text_.size();
    }

private:
    [[nodiscard]] static bool is_space(char character) noexcept
    {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    }

    std::string_view text_;
    std::size_t      at_{0};
};

/// How what was drawn differs from the golden image, for a failed check.
[[nodiscard]] std::string describe_mismatch(std::string_view                  name,
                                            const std::optional<std::string>& golden_text,
                                            const framebuffer&                drawn,
                                            const std::filesystem::path&      drawn_path)
{
    std::ostringstream report;

    report << name << ": ";

    if (!golden_text) {
        report << "there is no golden image yet";
    }
    else if (const std::optional<framebuffer> golden = from_pbm(*golden_text); !golden) {
        report << "the golden image is not a 128x64 plain PBM";
    }
    else {
        std::size_t                differing = 0;
        std::optional<std::size_t> first_x;
        std::size_t                first_y = 0;

        for (std::size_t y = 0; y < framebuffer::height; ++y) {
            for (std::size_t x = 0; x < framebuffer::width; ++x) {
                if (golden->lit(x, y) != drawn.lit(x, y)) {
                    if (!first_x) {
                        first_x = x;
                        first_y = y;
                    }

                    ++differing;
                }
            }
        }

        if (first_x) {
            report << differing << " pixels differ, the first at (" << *first_x << ", " << first_y
                   << ")";
        }
        else {
            report << "the pixels match, and the description or the file's layout differs";
        }
    }

    report << ". What was drawn is in " << drawn_path.string()
           << ". Once it is right, make bless approves it.";

    return report.str();
}

}  // namespace

std::string to_pbm(const framebuffer& screen, std::string_view description)
{
    std::string text = "P1\n# " + std::string{description} + "\n128 64\n";

    for (std::size_t y = 0; y < framebuffer::height; ++y) {
        for (std::size_t x = 0; x < framebuffer::width; ++x) {
            text += screen.lit(x, y) ? '1' : '0';
        }

        text += '\n';
    }

    return text;
}

std::optional<framebuffer> from_pbm(std::string_view text)
{
    pbm_reader reader{text};

    if (!reader.take("P1")) {
        return std::nullopt;
    }

    const std::optional<std::size_t> width  = reader.number();
    const std::optional<std::size_t> height = reader.number();

    if (width != framebuffer::width || height != framebuffer::height) {
        return std::nullopt;
    }

    framebuffer screen;

    for (std::size_t y = 0; y < framebuffer::height; ++y) {
        for (std::size_t x = 0; x < framebuffer::width; ++x) {
            const std::optional<bool> lit = reader.pixel();

            if (!lit) {
                return std::nullopt;
            }

            if (*lit) {
                screen.light(x, y);
            }
        }
    }

    reader.skip_space();

    return reader.at_end() ? std::optional{screen} : std::nullopt;
}

void check_golden_image(const framebuffer& screen,
                        std::string_view   name,
                        std::string_view   description)
{
    const std::filesystem::path golden_path = golden_directory() / (std::string{name} + ".pbm");
    const std::string           drawn_text  = to_pbm(screen, description);

    if (bless_requested()) {
        write_file(golden_path, drawn_text);
        MESSAGE("blessed " << golden_path.string());
        return;
    }

    const std::optional<std::string> golden_text = read_file(golden_path);
    const bool                       matches     = golden_text == drawn_text;
    const std::filesystem::path      drawn_path = output_directory() / (std::string{name} + ".pbm");

    if (!matches) {
        write_file(drawn_path, drawn_text);
    }

    CHECK_MESSAGE(matches, describe_mismatch(name, golden_text, screen, drawn_path));
}

}  // namespace previously::test
