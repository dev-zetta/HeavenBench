/*
 * HeavenBench — extracted assets owned directly by render features
 *
 * Scene and material traversal discovers most runtime assets automatically,
 * but procedural render features also load a small fixed set of meshes and
 * textures directly. This inventory is the single source of truth shared by
 * those features and the asset packer, preventing release archives from
 * silently omitting assets that are not referenced by scene XML or materials.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_FEATURE_ASSETS_H_
#define HEAVENBENCH_SRC_ENGINE_FEATURE_ASSETS_H_

#include "engine/path_resolver.h"

#include <string>
#include <string_view>
#include <vector>

namespace heaven {

enum class FeatureAsset {
    GrassIsland1Mesh,
    GrassIsland2Mesh,
    GrassIsland3Mesh,
    GrassDiffuse,
    GrassIrregularity,
    GrassClutter,
    CloudLayer1,
    CloudLayer2,
    CloudMask,
    Glow,
    RainDrop,
};

struct FeatureAssetInfo {
    std::string_view relative_path;
    std::string_view referrer;
    std::string_view texture_role;
    std::string_view wrap;
};

[[nodiscard]] constexpr FeatureAssetInfo
feature_asset_info(FeatureAsset asset) noexcept {
    switch (asset) {
        case FeatureAsset::GrassIsland1Mesh:
            return {"heaven/meshes/environment/fly_island1.mesh", "<grass feature>", {}, {}};
        case FeatureAsset::GrassIsland2Mesh:
            return {"heaven/meshes/environment/fly_island2.mesh", "<grass feature>", {}, {}};
        case FeatureAsset::GrassIsland3Mesh:
            return {"heaven/meshes/environment/fly_island3.mesh", "<grass feature>", {}, {}};
        case FeatureAsset::GrassDiffuse:
            return {"heaven/textures/environment/grass_d.dds", "<grass feature>", "diffuse", "repeat"};
        case FeatureAsset::GrassIrregularity:
            return {"heaven/textures/environment/grass_irregularity_d.dds", "<grass feature>", "diffuse", "repeat"};
        case FeatureAsset::GrassClutter:
            return {"heaven/textures/environment/grass_variation_d.dds", "<massive foliage feature>", "diffuse", "repeat"};
        case FeatureAsset::CloudLayer1:
            return {"heaven/textures/environment/clouds1_d.dds", "<cloud feature>", "diffuse", "repeat"};
        case FeatureAsset::CloudLayer2:
            return {"heaven/textures/environment/clouds2_d.dds", "<cloud feature>", "diffuse", "repeat"};
        case FeatureAsset::CloudMask:
            return {"heaven/textures/environment/clouds_mask0_d.dds", "<cloud feature>", "data", "repeat"};
        case FeatureAsset::Glow:
            return {"heaven/textures/sfx/fire_glow_dr.dds", "<glow features>", "emissive", "clamp"};
        case FeatureAsset::RainDrop:
            return {"heaven/textures/sfx/spark_d.dds", "<rain feature>", "diffuse", "clamp"};
    }
    return {};
}

[[nodiscard]] inline std::string
feature_asset_path(const PathResolver& paths, FeatureAsset asset) {
    return paths.relative_path(feature_asset_info(asset).relative_path);
}

[[nodiscard]] inline std::vector<FeatureAsset>
enabled_feature_assets(const AppConfig& config) {
    std::vector<FeatureAsset> result;
    result.reserve(10);

    if (config.features.grass || config.features.massive_foliage) {
        result.insert(result.end(), {
            FeatureAsset::GrassIsland1Mesh,
            FeatureAsset::GrassIsland2Mesh,
            FeatureAsset::GrassIsland3Mesh,
            FeatureAsset::GrassDiffuse,
            FeatureAsset::GrassIrregularity,
        });
    }
    if (config.features.massive_foliage) {
        result.push_back(FeatureAsset::GrassClutter);
    }
    if (config.features.clouds) {
        result.insert(result.end(), {
            FeatureAsset::CloudLayer1,
            FeatureAsset::CloudLayer2,
            FeatureAsset::CloudMask,
        });
    }
    if (config.features.particles || config.features.lens_flares ||
        config.features.rain) {
        result.push_back(FeatureAsset::Glow);
    }
    if (config.features.rain) {
        result.push_back(FeatureAsset::RainDrop);
    }
    return result;
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_FEATURE_ASSETS_H_
