/*
 * HeavenBench — Massive Foliage enhanced-terrain policy
 *
 * The feature adds detail to the existing floating-island ground surfaces.
 * It never creates a second overlay mesh: scene loading consumes these
 * parameters through the shared CPU-topology/GPU-displacement pipeline.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_MASSIVE_FOLIAGE_TERRAIN_H_
#define HEAVENBENCH_SRC_ENGINE_MASSIVE_FOLIAGE_TERRAIN_H_

#include "engine/surface_displacement_policy.h"

namespace heaven {

struct MassiveFoliageTerrainParams {
    // At the default global Extreme multiplier (1.5), factor 2 requests two
    // dyadic refinement levels. The already-detailed island ground therefore
    // grows by at most 16x rather than the old overlay's unconditional 1024x.
    float tessellation_factor = 2.0f;
    float tessellation_length = 0.2f;
    float displacement_scale = 0.2f;
    float displacement_power = 2.0f;
    float displacement_distance = 10.0f;
    float displacement_threshold = 0.0f;
    float displacement_attenuation = 2.0f;
    int max_subdivisions = 2;
};

[[nodiscard]] inline MassiveFoliageTerrainParams
massive_foliage_terrain_params(float foliage_density) noexcept {
    MassiveFoliageTerrainParams out;
    const float density = std::max(foliage_density, 1.0f);
    out.tessellation_factor *= density;
    if (out.tessellation_factor > 2.0f) out.max_subdivisions = 3;
    return out;
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_MASSIVE_FOLIAGE_TERRAIN_H_
