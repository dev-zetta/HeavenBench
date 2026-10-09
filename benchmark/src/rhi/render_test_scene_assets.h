/* HeavenBench — explicit procedural inputs for isolated render scenes. */
#ifndef HEAVENBENCH_SRC_RHI_RENDER_TEST_SCENE_ASSETS_H_
#define HEAVENBENCH_SRC_RHI_RENDER_TEST_SCENE_ASSETS_H_

#include "engine/feature_assets.h"
#include "engine/grass_cpu.h"
#include "engine/path_resolver.h"
#include "rhi/rhi.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace filament {
class Engine;
class Texture;
}

namespace heaven {

struct RenderContext;

// Roots are in the explicitly selected instance's local coordinates. The
// ordinary grass buffers, wind materials and depth proxies consume them.
struct RenderTestGrassPatch {
    std::size_t instance_index = 0;
    std::vector<GrassRoot> roots;
    std::vector<GrassRoot> flowers;
};

// Metre-scale inputs for the same cloud materials used by the Heaven scene.
// The near deck stays visible above a small procedural floor.
struct RenderTestAtmosphere {
    float cloud_plane_z = 10.0f;
    float cloud_texture_scale = 0.06f;
    float cloud_center_z = 12.0f;
    float cloud_thickness = 6.0f;
    float cloud_noise_scale = 0.08f;
    float cloud_extinction = 0.22f;
    float cloud_march_distance = 300.0f;
};

// The backend owns this alongside its resource store. Feature materials borrow
// its textures: destroy the features before destroying these assets or Engine.
// No extracted files, resolver names or texture fallback are used here.
class RenderTestSceneAssets {
public:
    [[nodiscard]] static Result<std::unique_ptr<RenderTestSceneAssets>> create(
        filament::Engine& engine);
    ~RenderTestSceneAssets();
    RenderTestSceneAssets(const RenderTestSceneAssets&) = delete;
    RenderTestSceneAssets& operator=(const RenderTestSceneAssets&) = delete;

    [[nodiscard]] filament::Texture* feature_texture(FeatureAsset asset) const;
    [[nodiscard]] filament::Texture* sky_texture(SkyAsset asset) const;
    // Linear RGBA16F gain 1 + .2 * dirt, including complete linear mip levels.
    // Bind with View::BloomOptions::dirtStrength = 1.
    [[nodiscard]] filament::Texture* lens_dirt_texture() const { return lens_dirt_; }
    [[nodiscard]] const RenderTestAtmosphere& atmosphere() const { return atmosphere_; }

    // Build deterministic, finite roots for an explicit rectangular ground.
    // This is setup-time work; no feature regenerates roots during update().
    [[nodiscard]] Result<void> set_ground_patch(
        std::size_t instance_index, glm::vec2 local_min, glm::vec2 local_max,
        float local_z = 0.0f, std::size_t blades = 1800,
        std::size_t flowers = 70);
    [[nodiscard]] std::span<const RenderTestGrassPatch> grass_patches() const {
        return grass_patches_;
    }

private:
    explicit RenderTestSceneAssets(filament::Engine& engine) : engine_(engine) {}
    [[nodiscard]] Result<void> init();
    filament::Engine& engine_;
    std::array<filament::Texture*, 11> feature_textures_{};
    std::array<filament::Texture*, 3> sky_textures_{};
    filament::Texture* lens_dirt_ = nullptr;
    std::vector<RenderTestGrassPatch> grass_patches_;
    RenderTestAtmosphere atmosphere_;
};

// Normal scenes retain their original shared-cache path. An explicit fixture
// context uses its typed procedural texture and throws if that input is absent.
[[nodiscard]] filament::Texture* resolve_feature_texture(
    RenderContext& ctx, FeatureAsset asset, TextureSemantic semantic);

} // namespace heaven
#endif
