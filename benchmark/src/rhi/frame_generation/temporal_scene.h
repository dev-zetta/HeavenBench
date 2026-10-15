/* Full-resolution project motion/depth proxies with accepted-frame history. */
#ifndef HEAVENBENCH_SRC_RHI_FRAME_GENERATION_TEMPORAL_SCENE_H_
#define HEAVENBENCH_SRC_RHI_FRAME_GENERATION_TEMPORAL_SCENE_H_

#include "rhi/backend_core.h"
#include "rhi/rhi.h"
#include "rhi/prepared_surface_culling.h"

#include <utils/Entity.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace filament {
class Material;
class MaterialInstance;
class RenderTarget;
class Skybox;
class Texture;
class View;
}

namespace heaven {
class Camera;
class MeshStore;
class TextureStore;

struct TemporalSceneCoverage {
    size_t proxy_primitives = 0;
    size_t skinned_primitives = 0;
    size_t cpu_deformed_primitives = 0;
    size_t glass_primitives = 0;
    size_t incomplete_history_primitives = 0;
    size_t missing_current_bones = 0;
    size_t missing_current_positions = 0;
};

// This pass borrows precisely the selected visible mesh buffers/index ranges.
// A separate proxy per placement permits previousModel without changing the
// production instance/bone layouts. Grass/particles are separate participants;
// the mesh coverage report alone never certifies complete-scene motion.
class TemporalScene {
public:
    explicit TemporalScene(const BackendCore& core) : core_(core) {}
    ~TemporalScene();
    TemporalScene(const TemporalScene&) = delete;
    TemporalScene& operator=(const TemporalScene&) = delete;

    bool ensure(int width, int height);
    bool build(MeshStore& meshes, TextureStore& textures,
               std::span<const Instance> instances,
               std::span<const GpuMesh> gpu_meshes,
               const MaterialResolver& resolve_material,
               const AppConfig& config,
               filament::Material* motion_material,
               filament::Material* masked_material);
    // Pass the main camera's unjittered rendering projection times its user
    // world view matrix, including Filament's infinite-far rendering policy.
    bool prepare(const Camera& camera, std::span<const Instance> instances,
                 const glm::mat4& current_clip_from_user_world);
    void commit_accepted();
    void invalidate_history() noexcept;
    void clear();

    bool set_bones(int mesh_index, std::span<const glm::mat4> matrices);
    bool set_positions(uint64_t surface_handle, std::span<const Vertex> vertices);

    [[nodiscard]] filament::View* view() const { return view_; }
    [[nodiscard]] filament::Scene* scene() const { return scene_; }
    // xy = current-to-previous normalized top-left UV delta; z = inverted
    // nonlinear device depth [0,1] (sky 0); w = valid temporal coverage.
    [[nodiscard]] filament::Texture* packed_texture() const { return packed_; }
    [[nodiscard]] const TemporalSceneCoverage& coverage() const { return coverage_; }
    [[nodiscard]] bool built() const { return built_; }
    [[nodiscard]] bool history_valid() const { return history_valid_; }
    [[nodiscard]] const glm::mat4& current_clip() const { return current_clip_; }
    [[nodiscard]] const glm::mat4& previous_clip() const {
        return history_valid_ ? previous_clip_ : current_clip_;
    }

private:
    struct Proxy {
        utils::Entity entity;
        filament::MaterialInstance* material = nullptr;
        size_t instance_index = 0;
        int mesh_index = -1;
        uint64_t surface_handle = 0;
        uint32_t bone_count = 0;
        bool skinned_source = false;
        bool prepared = false;
        bool base_culling = false;
        PreparedSurfaceCullingBounds culling_bounds{};
    };
    struct BoneHistory {
        std::vector<glm::mat4> current;
        std::vector<glm::mat4> previous;
        filament::Texture* texture = nullptr;
        uint32_t texture_height = 0;
    };
    struct PositionHistory {
        std::vector<glm::vec4> current;
        std::vector<glm::vec4> previous;
        filament::Texture* texture = nullptr;
        uint32_t texture_width = 0;
        uint32_t texture_height = 0;
    };
    bool upload_bone_history(BoneHistory& history);
    bool upload_position_history(PositionHistory& history);
    void destroy_targets();

    BackendCore core_;
    filament::Scene* scene_ = nullptr;
    filament::Skybox* skybox_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    utils::Entity camera_entity_;
    filament::RenderTarget* target_ = nullptr;
    filament::Texture* packed_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::Texture* neutral_ = nullptr;
    std::vector<Proxy> proxies_;
    std::unordered_map<int, BoneHistory> bones_;
    std::unordered_map<uint64_t, PositionHistory> positions_;
    std::vector<glm::mat4> current_models_;
    std::vector<glm::mat4> previous_models_;
    glm::mat4 current_clip_{1.0f};
    glm::mat4 previous_clip_{1.0f};
    glm::vec3 current_camera_{};
    glm::vec3 previous_camera_{};
    TemporalSceneCoverage coverage_;
    int width_ = 0;
    int height_ = 0;
    bool built_ = false;
    bool prepared_ = false;
    bool history_valid_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FRAME_GENERATION_TEMPORAL_SCENE_H_
