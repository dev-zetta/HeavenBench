/*
 * HeavenBench — shared opt-in displacement policy for enhanced surfaces
 */
#ifndef HEAVENBENCH_SRC_ENGINE_SURFACE_DISPLACEMENT_POLICY_H_
#define HEAVENBENCH_SRC_ENGINE_SURFACE_DISPLACEMENT_POLICY_H_

#include "engine/path_resolver.h"
#include "engine/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace heaven {

inline constexpr uint32_t kSurfaceDisplacementPolicyVersion = 1;

enum class SurfaceDisplacementKind {
    MassiveFoliageTerrain,
    VillageGround,
};

// SurfaceDisplacementPolicy contains the one authoritative topology,
// displacement, cache, and texture choice for an enhanced surface.
struct SurfaceDisplacementPolicy {
    SurfaceDisplacementKind kind = SurfaceDisplacementKind::VillageGround;
    std::string height_path;
    std::string density_path;
    float tessellation_factor = 1.0f;
    float tessellation_length = 1.0f;
    float displacement_scale = 0.0f;
    float displacement_power = 2.0f;
    float displacement_distance = 10.0f;
    float displacement_threshold = 0.0f;
    float displacement_attenuation = 2.0f;
    float boundary_fade = 0.0f;
    // Reconstruct the low/mid-frequency shading normal from the displaced
    // fragment position, then layer the authored tangent-space normal over it.
    // This is intentionally policy-controlled rather than inferred from the
    // presence or name of a height texture.
    bool derive_displaced_normal = false;
    int max_subdivisions = 3;
    uint32_t policy_version = kSurfaceDisplacementPolicyVersion;
};

[[nodiscard]] bool is_village_ground_surface(
    std::string_view mesh_path,
    std::string_view surface_name,
    std::string_view material_name) noexcept;

[[nodiscard]] bool is_massive_foliage_terrain(
    std::string_view mesh_path,
    std::string_view surface_name) noexcept;

[[nodiscard]] std::optional<SurfaceDisplacementPolicy>
resolve_surface_displacement_policy(const AppConfig& config,
                                    const PathResolver& paths,
                                    std::string_view mesh_path,
                                    std::string_view surface_name,
                                    std::string_view material_name);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_SURFACE_DISPLACEMENT_POLICY_H_
