#ifndef HEAVENBENCH_SRC_RHI_SCREENSHOT_PPM_H_
#define HEAVENBENCH_SRC_RHI_SCREENSHOT_PPM_H_

#include "engine/types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace heaven {

[[nodiscard]] inline Result<std::size_t> screenshot_rgba_size(int width, int height) {
    if (width <= 0 || height <= 0) {
        return std::unexpected(Error("Screenshot dimensions must be positive"));
    }
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / 4 / h) {
        return std::unexpected(Error("Screenshot dimensions overflow the readback buffer"));
    }
    return w * h * 4;
}

// Filament normalizes readPixels to top-down rows on every backend. Preserve
// those rows and the displayed SDR bytes exactly; PPM only discards alpha.
[[nodiscard]] inline Result<void> write_screenshot_ppm(
    const std::filesystem::path& path, int width, int height,
    std::span<const std::uint8_t> rgba) {
    const auto required = screenshot_rgba_size(width, height);
    if (!required) return std::unexpected(required.error());
    if (rgba.size() != *required) {
        return std::unexpected(Error("Screenshot readback byte count does not match its dimensions"));
    }
    std::vector<char> row(static_cast<std::size_t>(width) * 3);
    if (row.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return std::unexpected(Error("Screenshot row exceeds the output stream limit"));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(Error(std::format("Cannot open screenshot output: {}", path.string())));
    }
    output << "P6\n" << width << ' ' << height << "\n255\n";
    const auto w = static_cast<std::size_t>(width);
    for (std::size_t y = 0; std::cmp_less(y, height); ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const std::size_t source = (y * w + x) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                row[x * 3 + channel] = static_cast<char>(rgba[source + channel]);
            }
        }
        output.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    output.close();
    if (!output) {
        return std::unexpected(Error(std::format("Cannot write complete screenshot: {}", path.string())));
    }
    return {};
}

} // namespace heaven

#endif
