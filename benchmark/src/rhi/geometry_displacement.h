/*
 * HeavenBench — effective geometry displacement shared by rendering and baking
 */
#ifndef HEAVENBENCH_SRC_RHI_GEOMETRY_DISPLACEMENT_H_
#define HEAVENBENCH_SRC_RHI_GEOMETRY_DISPLACEMENT_H_

#include "engine/tessellation_fade.h"
#include "rhi/rhi.h"

#include <algorithm>
#include <array>

namespace heaven {

struct GeometryDisplacementDescriptor {
    bool enabled = false;
    float amplitude = 0.0f;
    float power = 0.0f;
    TessellationFade fade;

    [[nodiscard]] std::array<float, 4> parameters() const noexcept {
        return {amplitude, power, fade.threshold,
                enabled ? fade.inverse_range() : 0.0f};
    }
};

// Keep the authored amplitude independent of the enable gate: existing color
// and depth routes retain it in their uniforms even when displacement is off.
[[nodiscard]] inline GeometryDisplacementDescriptor geometry_displacement(
    const GpuMaterial& material, bool height_available,
    float scale, float distance_multiplier, bool enabled = true) noexcept {
    const auto fade = tessellation_fade(
        material.tess_threshold, material.tess_distance, distance_multiplier);
    return {
        enabled && material.geometry_displacement && height_available &&
            scale > 0.0f && fade.enabled(),
        material.tess_scale * scale,
        std::max(material.tess_power, 1e-5f),
        fade,
    };
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_GEOMETRY_DISPLACEMENT_H_
