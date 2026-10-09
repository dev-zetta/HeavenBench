/*
 * HeavenBench — renderer-owned extracted asset inventory tests
 *
 * Verifies that the packer-facing inventory follows effective feature toggles,
 * contains no duplicates, and resolves through the same PathResolver join used
 * by runtime feature loaders.
 */
#include "engine/feature_assets.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool unit_feature_assets() {
    heaven::AppConfig config;
    const std::vector defaults = heaven::enabled_feature_assets(config);
    const std::set<heaven::FeatureAsset> unique(defaults.begin(), defaults.end());

    bool ok = defaults.size() == 8 && unique.size() == defaults.size() &&
        unique.contains(heaven::FeatureAsset::GrassIsland1Mesh) &&
        unique.contains(heaven::FeatureAsset::GrassIsland2Mesh) &&
        unique.contains(heaven::FeatureAsset::GrassIsland3Mesh) &&
        unique.contains(heaven::FeatureAsset::GrassDiffuse) &&
        unique.contains(heaven::FeatureAsset::GrassIrregularity) &&
        unique.contains(heaven::FeatureAsset::GrassClutter) &&
        unique.contains(heaven::FeatureAsset::Glow) &&
        unique.contains(heaven::FeatureAsset::RainDrop);

    config.features.grass = false;
    config.features.massive_foliage = false;
    config.features.particles = false;
    config.features.lens_flares = false;
    config.features.rain = false;
    ok = ok && heaven::enabled_feature_assets(config).empty();

    config.features.clouds = true;
    const std::vector clouds = heaven::enabled_feature_assets(config);
    ok = ok && clouds == std::vector{
        heaven::FeatureAsset::CloudLayer1,
        heaven::FeatureAsset::CloudLayer2,
        heaven::FeatureAsset::CloudMask,
    };

    auto paths = heaven::PathResolver::create("assets.zip");
    ok = ok && paths && heaven::feature_asset_path(
        *paths, heaven::FeatureAsset::GrassDiffuse) ==
        "assets.zip/heaven/textures/environment/grass_d.dds";

    if (!ok) std::printf("[unit_feature_assets] FAILED\n");
    return ok;
}

const heaven::testing::Register registered(unit_feature_assets);

} // namespace
