#include "rhi/screenshot_ppm.h"
#include "rhi/screenshot_hdr.h"

#include <array>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_screenshot_ppm() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("heaven_screenshot_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{root};
    fs::create_directories(root);
    const fs::path output = root / "capture.ppm";
    // Different top/bottom and RGB channels catch accidental flipping or BGRA
    // interpretation. Alpha is intentionally unrelated to the visible colors.
    constexpr std::array<std::uint8_t, 16> rgba{
        255, 0, 1, 0, 2, 254, 3, 64,
        4, 5, 253, 128, 252, 251, 250, 255};
    constexpr std::array<std::uint8_t, 12> rgb{
        255, 0, 1, 2, 254, 3, 4, 5, 253, 252, 251, 250};
    const auto read_file = [&] {
        std::ifstream input(output, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "[unit_screenshot_ppm] %s\n", message);
            ok = false;
        }
    };
    check(heaven::write_screenshot_ppm(output, 2, 2, rgba).has_value(),
          "valid screenshot was rejected");
    std::string expected = "P6\n2 2\n255\n";
    for (const auto byte : rgb) expected.push_back(static_cast<char>(byte));
    check(read_file() == expected, "P6 header, top-down rows or RGB bytes changed");
    check(!heaven::write_screenshot_ppm(output, 2, 2, std::span(rgba).first(15)),
          "truncated readback was accepted");
    check(!heaven::write_screenshot_ppm(output, 1, 1, rgba),
          "excess readback was accepted");
    check(!heaven::write_screenshot_ppm(output, 0, 2, {}),
          "zero width was accepted");
    check(!heaven::screenshot_rgba_size(2, -1), "negative height was accepted");
    check(read_file() == expected, "invalid readback truncated an existing image");
    check(!heaven::write_screenshot_ppm(root, 2, 2, rgba),
          "directory output was accepted");
    check(!heaven::write_screenshot_ppm(root / "absent/frame.ppm", 2, 2, rgba),
          "missing output parent was accepted");

    // Independent ST.2084 luminance anchors exercise actual preview conversion,
    // not a round-trip through the matching forward implementation.
    constexpr float pq100 = 0.5080784215f;
    constexpr float pq400 = 0.6525785976f;
    check(std::abs(heaven::screenshot_pq_to_nits(pq100) - 100.0) < 0.001,
          "PQ 100-nit anchor decoded incorrectly");
    check(std::abs(heaven::screenshot_pq_to_nits(pq400) - 400.0) < 0.002,
          "PQ 400-nit anchor decoded incorrectly");
    const auto preview_pixel = [&](std::array<float, 3> pixel) {
        return heaven::screenshot_hdr_preview_pixel(pixel, 100.0);
    };
    check(preview_pixel({0, 0, 0}) == std::array<std::uint8_t, 3>{0, 0, 0},
          "PQ black was lifted");
    check(preview_pixel({pq100, pq100, pq100}) == std::array<std::uint8_t, 3>{188, 188, 188},
          "paper-white preview does not match Reinhard plus sRGB");
    check(preview_pixel({pq400, pq400, pq400}) == std::array<std::uint8_t, 3>{231, 231, 231},
          "HDR highlight preview does not match Reinhard plus sRGB");
    check(preview_pixel({pq100, 0, 0}) == std::array<std::uint8_t, 3>{207, 0, 0},
          "Rec.2020 red was not converted / gamut-clipped");
    check(preview_pixel({0, pq100, 0}) == std::array<std::uint8_t, 3>{0, 193, 0},
          "Rec.2020 green was not converted / gamut-clipped");
    check(preview_pixel({0, 0, pq100}) == std::array<std::uint8_t, 3>{0, 0, 192},
          "Rec.2020 blue was not converted / gamut-clipped");
    const std::array<float, 8> pq{pq100, pq100, pq100, 1, 0, 0, 0, 1};
    check(heaven::write_screenshot_hdr(output, 1, 2, pq, 100).has_value(),
          "valid HDR screenshot was rejected");
    const auto raw_path = heaven::screenshot_hdr_raw_path(output);
    check(raw_path == root / "capture.pq.pfm", "unexpected raw HDR sibling name");
    std::ifstream raw_input(raw_path, std::ios::binary);
    const std::string raw(std::istreambuf_iterator<char>(raw_input), {});
    const std::string header = std::endian::native == std::endian::little
        ? "PF\n1 2\n-1.0\n" : "PF\n1 2\n1.0\n";
    check(raw.starts_with(header) && raw.size() == header.size() + 6 * sizeof(float),
          "PFM dimensions, endianness or byte count changed");
    if (raw.size() == header.size() + 6 * sizeof(float)) {
        std::array<float, 6> stored{};
        std::memcpy(stored.data(), raw.data() + header.size(), sizeof(stored));
        check(stored == std::array<float, 6>{0, 0, 0, pq100, pq100, pq100},
              "raw PQ bytes or bottom-up PFM row order changed");
    }
    auto invalid = pq;
    invalid[0] = std::numeric_limits<float>::quiet_NaN();
    check(!heaven::write_screenshot_hdr(output, 1, 2, invalid, 100),
          "incomplete / NaN HDR readback was accepted");
    check(!heaven::write_screenshot_hdr(output, 1, 2, pq, 0),
          "zero HDR paper-white divisor was accepted");
    return ok;
}

const heaven::testing::Register g_register_screenshot_ppm{&unit_screenshot_ppm};

} // namespace
