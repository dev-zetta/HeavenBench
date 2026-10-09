/* HeavenBench — clean bloom plus bounded, linear lens-dirt modulation. */
#ifndef HEAVENBENCH_SRC_RHI_BLOOM_DIRT_H_
#define HEAVENBENCH_SRC_RHI_BLOOM_DIRT_H_

#include "engine/types.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace heaven {

// Filament multiplies the entire bloom term by its dirt sampler. A black
// source therefore needs unit gain; the authored dirt adds at most 20%.
// This data is linear HDR, never an sRGB/UNORM upload (gain exceeds one).
[[nodiscard]] inline float bloom_dirt_gain_from_srgb_byte(std::uint8_t byte) noexcept {
    const float srgb = static_cast<float>(byte) / 255.0f;
    const float linear = srgb <= 0.04045f ? srgb / 12.92f
        : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
    return 1.0f + 0.2f * linear;
}

[[nodiscard]] inline Result<std::vector<float>> make_bloom_dirt_gain_rgba(
        std::span<const std::uint8_t> bytes, unsigned channels) {
    if ((channels != 3 && channels != 4) || bytes.empty() ||
        bytes.size() % channels != 0 ||
        bytes.size() / channels > std::numeric_limits<std::size_t>::max() / 4) {
        return std::unexpected(Error{"invalid lens-dirt RGB/RGBA layout"});
    }
    const auto pixels = bytes.size() / channels;
    std::vector<float> gain(pixels * 4, 1.0f);
    for (std::size_t i = 0; i < pixels; ++i) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            gain[i * 4 + channel] =
                bloom_dirt_gain_from_srgb_byte(bytes[i * channels + channel]);
        }
    }
    return gain;
}

} // namespace heaven
#endif
