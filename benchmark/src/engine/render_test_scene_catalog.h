/*
 * HeavenBench — procedural and authored visual test scene catalog
 */
#ifndef HEAVENBENCH_SRC_ENGINE_RENDER_TEST_SCENE_CATALOG_H_
#define HEAVENBENCH_SRC_ENGINE_RENDER_TEST_SCENE_CATALOG_H_

#include <array>
#include <span>
#include <string_view>

namespace heaven {

struct AppConfig;

struct RenderTestSceneInfo {
    std::string_view id;
    std::string_view title;
    std::string_view description;
    // World position XYZ, yaw and pitch in degrees, matching --camera-pose.
    std::array<float, 5> camera_pose;
    float fov = 50.0f;
    float time_of_day = 12.0f;
};

[[nodiscard]] std::span<const RenderTestSceneInfo> render_test_scene_catalog();

// Authored fixtures load these logical Heaven nodes through the normal asset
// filesystem and material database. Procedural and unknown IDs return no nodes.
[[nodiscard]] bool render_test_scene_uses_assets(std::string_view id);
[[nodiscard]] std::span<const std::string_view> render_test_scene_asset_nodes(
    std::string_view id);

// Fixture placement is a rigid parent translation; the source node's internal
// geometry, transforms, materials, animation and lights remain authored.
[[nodiscard]] std::array<float, 3> render_test_scene_asset_translation(
    std::string_view id, std::string_view node_path);

// Apply an isolated scene's rendering defaults. Explicit later CLI options
// may override them. An unknown ID returns false without modifying config.
[[nodiscard]] bool apply_render_test_scene_defaults(
    AppConfig& config, std::string_view id);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_RENDER_TEST_SCENE_CATALOG_H_
