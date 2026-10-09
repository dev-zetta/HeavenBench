/*
 * HeavenBench — authored tessellation distance parameters
 */
#ifndef HEAVENBENCH_SRC_ENGINE_TESSELLATION_FADE_H_
#define HEAVENBENCH_SRC_ENGINE_TESSELLATION_FADE_H_

#include <algorithm>

namespace heaven {

struct TessellationFade {
    float threshold = 0.0f;
    float range = 0.0f;

    [[nodiscard]] float inverse_range() const noexcept {
        return range > 1e-5f ? 1.0f / range : 0.0f;
    }

    [[nodiscard]] bool enabled() const noexcept {
        return range > 1e-5f;
    }
};

// Unigine's render tessellation distance is a dimensionless quality multiplier,
// not a world-space far clip. It scales the authored fade span while leaving
// the material's full-detail threshold intact.
[[nodiscard]] inline TessellationFade tessellation_fade(
    float authored_threshold, float authored_range,
    float distance_multiplier) noexcept {
    const float multiplier = std::max(distance_multiplier, 0.0f);
    return {
        std::max(authored_threshold, 0.0f),
        std::max(authored_range, 0.0f) * multiplier,
    };
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_TESSELLATION_FADE_H_
