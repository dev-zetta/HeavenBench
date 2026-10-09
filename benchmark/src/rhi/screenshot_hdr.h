#ifndef HEAVENBENCH_SRC_RHI_SCREENSHOT_HDR_H_
#define HEAVENBENCH_SRC_RHI_SCREENSHOT_HDR_H_

#include "rhi/screenshot_ppm.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <utility>

namespace heaven {

[[nodiscard]] inline double screenshot_pq_to_nits(double pq) {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;
    const double powered = std::pow(std::clamp(pq, 0.0, 1.0), 1.0 / m2);
    return 10000.0 * std::pow(std::max(powered - c1, 0.0) /
        (c2 - c3 * powered), 1.0 / m1);
}

// Diagnostic preview only: decode PQ to absolute light, convert D65 Rec.2020
// to D65 sRGB, divide by paper white, apply x/(1+x), then the sRGB OETF. This
// deliberately does not claim to reproduce a physical HDR display's image.
[[nodiscard]] inline std::array<std::uint8_t, 3> screenshot_hdr_preview_pixel(
    std::span<const float, 3> pq, double paper_white_nits) {
    const double r = screenshot_pq_to_nits(pq[0]) / paper_white_nits;
    const double g = screenshot_pq_to_nits(pq[1]) / paper_white_nits;
    const double b = screenshot_pq_to_nits(pq[2]) / paper_white_nits;
    const std::array linear{
         1.6604910021084345 * r - 0.5876411387885495 * g - 0.0728498633198848 * b,
        -0.1245504745215907 * r + 1.1328998971259603 * g - 0.0083494226043695 * b,
        -0.0181507633549052 * r - 0.1005788980080074 * g + 1.1187296613629127 * b};
    std::array<std::uint8_t, 3> result{};
    for (std::size_t channel = 0; channel < result.size(); ++channel) {
        const double positive = std::max(linear[channel], 0.0);
        const double mapped = positive / (1.0 + positive);
        const double srgb = mapped <= 0.0031308 ? 12.92 * mapped
            : 1.055 * std::pow(mapped, 1.0 / 2.4) - 0.055;
        result[channel] = static_cast<std::uint8_t>(std::lround(std::clamp(srgb, 0.0, 1.0) * 255.0));
    }
    return result;
}

[[nodiscard]] inline std::filesystem::path screenshot_hdr_raw_path(
    const std::filesystem::path& preview) {
    return preview.parent_path() / (preview.stem().string() + ".pq.pfm");
}

// Raw floats retain the composed PQ signal before swap-chain quantization.
// PFM stores bottom-up RGB rows and records native endianness in its scale.
[[nodiscard]] inline Result<void> write_screenshot_hdr(
    const std::filesystem::path& preview_path, int width, int height,
    std::span<const float> rgba_pq, float paper_white_nits) {
    const auto required = screenshot_rgba_size(width, height);
    if (!required) return std::unexpected(required.error());
    if (rgba_pq.size() != *required || !std::isfinite(paper_white_nits) || paper_white_nits <= 0) {
        return std::unexpected(Error("Invalid HDR screenshot dimensions or paper white"));
    }
    for (const float value : rgba_pq) {
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
            return std::unexpected(Error("HDR screenshot contains invalid PQ readback values"));
        }
    }
    const auto w = static_cast<std::size_t>(width);
    std::vector<float> row(w * 3);
    std::vector<std::uint8_t> preview(*required, 255);
    const auto raw_path = screenshot_hdr_raw_path(preview_path);
    std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
    if (!raw) return std::unexpected(Error("Cannot open HDR screenshot: " + raw_path.string()));
    raw << "PF\n" << width << ' ' << height << '\n'
        << (std::endian::native == std::endian::little ? "-1.0\n" : "1.0\n");
    for (std::size_t row_index = 0; std::cmp_less(row_index, height); ++row_index) {
        const std::size_t y = static_cast<std::size_t>(height) - 1 - row_index;
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t source = (y * w + x) * 4;
            const auto pixel = rgba_pq.subspan(source, 3);
            const auto rgb = screenshot_hdr_preview_pixel(
                std::span<const float, 3>(pixel.data(), 3), paper_white_nits);
            for (std::size_t channel = 0; channel < 3; ++channel) {
                row[x * 3 + channel] = pixel[channel];
                preview[source + channel] = rgb[channel];
            }
        }
        raw.write(reinterpret_cast<const char*>(row.data()),
            static_cast<std::streamsize>(row.size() * sizeof(float)));
    }
    raw.close();
    if (!raw) return std::unexpected(Error("Cannot write complete HDR screenshot: " + raw_path.string()));
    return write_screenshot_ppm(preview_path, width, height, preview);
}

} // namespace heaven

#endif
