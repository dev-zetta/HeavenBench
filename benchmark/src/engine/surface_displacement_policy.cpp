/*
 * HeavenBench — shared opt-in displacement policy for enhanced surfaces
 */
#include "engine/surface_displacement_policy.h"

#include "engine/ground_relief_generator.h"

#include <algorithm>

namespace heaven {
namespace {

bool path_ends_with(std::string_view path, std::string_view suffix) noexcept {
    return path.size() >= suffix.size() &&
           path.substr(path.size() - suffix.size()) == suffix;
}

} // namespace

bool is_village_ground_surface(std::string_view mesh_path,
                               std::string_view surface_name,
                               std::string_view material_name) noexcept {
    if (material_name != "ruins_ground" && material_name != "fort_ground") {
        return false;
    }
    return (path_ends_with(mesh_path,
                          "heaven/meshes/buildings/fort.mesh") &&
            surface_name == "fort_ground") ||
           (path_ends_with(mesh_path,
                          "heaven/meshes/ruins/island1_ruins.mesh") &&
            surface_name == "ruins_ground");
}

bool is_massive_foliage_terrain(std::string_view mesh_path,
                                std::string_view surface_name) noexcept {
    if (surface_name.find("_ground") == std::string_view::npos) return false;
    return mesh_path.find("fly_island1.mesh") != std::string_view::npos ||
           mesh_path.find("fly_island2.mesh") != std::string_view::npos ||
           mesh_path.find("fly_island3.mesh") != std::string_view::npos;
}

std::optional<SurfaceDisplacementPolicy>
resolve_surface_displacement_policy(const AppConfig& config,
                                    const PathResolver& paths,
                                    std::string_view mesh_path,
                                    std::string_view surface_name,
                                    std::string_view material_name) {
    if (config.features.ground_relief &&
        is_village_ground_surface(mesh_path, surface_name, material_name)) {
        return SurfaceDisplacementPolicy{
            .kind = SurfaceDisplacementKind::VillageGround,
            .height_path = paths.relative_path(kGroundHeightRelativePath),
            .density_path = {},
            .tessellation_factor = 16.0f,
            .tessellation_length = 0.15f,
            .displacement_scale = 0.03f,
            .displacement_power = 2.0f,
            .displacement_distance = 18.0f,
            .displacement_threshold = 2.0f,
            .displacement_attenuation = 2.0f,
            .boundary_fade = 0.30f,
            .derive_displaced_normal = true,
            .max_subdivisions = 5,
        };
    }

    if (config.features.massive_foliage &&
        is_massive_foliage_terrain(mesh_path, surface_name)) {
        const float foliage_density =
            std::max(config.quality.foliage_density, 1.0f);
        const float factor = 2.0f * foliage_density;
        return SurfaceDisplacementPolicy{
            .kind = SurfaceDisplacementKind::MassiveFoliageTerrain,
            .height_path = paths.texture_path(
                TextureCategory::Environment, "rocks_h.dds"),
            .density_path = {},
            .tessellation_factor = factor,
            .tessellation_length = 0.2f,
            .displacement_scale = 0.2f,
            .displacement_power = 2.0f,
            .displacement_distance = 10.0f,
            .displacement_threshold = 0.0f,
            .displacement_attenuation = 2.0f,
            .boundary_fade = 0.0f,
            .derive_displaced_normal = false,
            .max_subdivisions = factor > 2.0f ? 3 : 2,
        };
    }
    return std::nullopt;
}

} // namespace heaven
