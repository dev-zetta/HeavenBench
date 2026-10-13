/* Accepted-frame wind motion for the actual persistent foliage ranges. */
#ifndef HEAVENBENCH_SRC_RHI_FRAME_GENERATION_TEMPORAL_GRASS_H_
#define HEAVENBENCH_SRC_RHI_FRAME_GENERATION_TEMPORAL_GRASS_H_

#include "engine/types.h"
#include "rhi/backend_core.h"

#include <filament/Box.h>
#include <utils/Entity.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace filament {
class IndexBuffer;
class Material;
class MaterialInstance;
class Texture;
class VertexBuffer;
}

namespace heaven {

// Read-only view of the source range. Buffers/textures remain feature-owned;
// clear TemporalGrass before destroying features or the shared temporal scene.
struct TemporalGrassPatch {
    utils::Entity source_entity;
    filament::VertexBuffer* vertices = nullptr;
    filament::IndexBuffer* indices = nullptr;
    const filament::Texture* diffuse = nullptr;
    size_t instance_index = 0;
    uint32_t index_offset = 0;
    uint32_t index_count = 0;
    filament::Box bounds;
    glm::vec4 diffuse_uv{1.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 wind_tuning{};
    bool culling = false;
    bool visible = false;
};

struct TemporalGrassCoverage {
    size_t ranges = 0;
    size_t visible_ranges = 0;
    size_t incomplete_history_ranges = 0;
    size_t unavailable_ranges = 0;
};

// Adds proxies to TemporalScene's scene, sharing its full-resolution target
// and z test. Does not create another clear/view or regenerate blade geometry.
class TemporalGrass {
public:
    TemporalGrass(const BackendCore& core, filament::Scene* scene,
                  filament::Material* material)
        : core_(core), scene_(scene), material_(material) {}
    ~TemporalGrass();
    TemporalGrass(const TemporalGrass&) = delete;
    TemporalGrass& operator=(const TemporalGrass&) = delete;

    bool prepare(std::span<const TemporalGrassPatch> patches,
                 std::span<const Instance> instances, float time,
                 const glm::mat4& clip_from_user_world);
    void commit_accepted();
    void invalidate_history() noexcept;
    void clear();
    [[nodiscard]] const TemporalGrassCoverage& coverage() const { return coverage_; }

private:
    struct Proxy {
        TemporalGrassPatch source;
        utils::Entity entity;
        filament::MaterialInstance* material = nullptr;
        glm::mat4 current_model{1.0f};
        glm::mat4 previous_model{1.0f};
        bool visible = false;
        bool previous_visible = false;
    };
    bool rebuild(std::span<const TemporalGrassPatch> patches);
    [[nodiscard]] bool matching_ranges(std::span<const TemporalGrassPatch> patches) const;

    BackendCore core_;
    filament::Scene* scene_ = nullptr;  // borrowed from TemporalScene
    filament::Material* material_ = nullptr; // borrowed, outlives every MI
    std::vector<Proxy> proxies_;
    glm::mat4 current_clip_{1.0f};
    glm::mat4 previous_clip_{1.0f};
    glm::mat3 current_wind_to_user_{1.0f};
    glm::mat3 previous_wind_to_user_{1.0f};
    float current_time_ = 0.0f;
    float previous_time_ = 0.0f;
    bool prepared_ = false;
    bool history_valid_ = false;
    TemporalGrassCoverage coverage_;
};

} // namespace heaven
#endif // HEAVENBENCH_SRC_RHI_FRAME_GENERATION_TEMPORAL_GRASS_H_
