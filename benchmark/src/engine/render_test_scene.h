/* HeavenBench — small procedural scenes through the ordinary RHI paths. */
#ifndef HEAVENBENCH_SRC_ENGINE_RENDER_TEST_SCENE_H_
#define HEAVENBENCH_SRC_ENGINE_RENDER_TEST_SCENE_H_

#include "engine/asset_file_system.h"
#include "engine/scene_rhi.h"

#include <map>
#include <string_view>

namespace heaven {

struct RenderTestGround {
    std::size_t instance_index = 0;
    glm::vec2 local_min{-4.0f, -2.0f};
    glm::vec2 local_max{4.0f, 6.0f};
    float z = 0.0f;
};

struct RenderTestTexture {
    std::string path;
    SharedAssetBuffer bytes;
    TextureSemantic semantic = TextureSemantic::ColorSRGB;
};

struct RenderTestMaterial {
    std::string surface_name;
    GpuMaterial material;
    std::string diffuse_texture;
    std::string normal_texture;
    std::string specular_texture;
    std::string detail_diffuse_texture;
    std::string detail_normal_texture;
    std::string detail_specular_texture;
    std::string height_texture;
    std::string emission_texture;
};

struct RenderTestSkinnedObject {
    SkinnedMesh mesh;
    glm::mat4 transform{1.0f};
    float frames_per_second = 30.0f;
};

struct RenderTestSceneDefinition {
    std::string id;
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
    std::vector<RenderTestMaterial> materials;
    std::vector<RenderTestTexture> textures;
    std::vector<SceneLight> lights;
    std::vector<ParticleEmitter> particle_emitters;
    std::vector<RenderTestGround> grounds;
    std::vector<RenderTestSkinnedObject> skinned_objects;
    std::vector<ClothBody> cloths;
};

struct RenderTestScene {
    RhiScene scene;
    std::map<std::string, GpuMaterial, std::less<>> materials;
    std::vector<RenderTestGround> grounds;

    [[nodiscard]] GpuMaterial resolve_material(
        const std::string& mesh_path, const std::string& surface_name) const;
};

// Generate first, then mount before initializing a backend that borrows assets.
[[nodiscard]] Result<RenderTestSceneDefinition>
make_render_test_scene(std::string_view id);

[[nodiscard]] Result<void> mount_render_test_scene_assets(
    const RenderTestSceneDefinition& definition, AssetFileSystem& assets);

// Loads mounted DDS through IRhiBackend and uploads static/skinned meshes.
// The returned resolver records own no backend objects; destroy them with the
// scene before destroying the backend. Ground metadata is also registered via
// set_render_test_ground(), so foliage has real procedural source instances.
[[nodiscard]] Result<RenderTestScene> upload_render_test_scene(
    RenderTestSceneDefinition definition, IRhiBackend& backend,
    const AppConfig& config);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_RENDER_TEST_SCENE_H_
