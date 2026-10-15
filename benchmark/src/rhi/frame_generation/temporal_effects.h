#ifndef HEAVEN_TEMPORAL_EFFECTS_H
#define HEAVEN_TEMPORAL_EFFECTS_H

#include "rhi/backend_core.h"
#include "engine/camera.h"
#include <filament/TextureSampler.h>
#include <utils/Entity.h>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace filament {
class VertexBuffer;
class IndexBuffer;
class MaterialInstance;
class Texture;
class View;
}

namespace heaven {

enum class TemporalEffectKind : uint8_t {
    Smoke, Flame, Glow, Firefly, Rain, LensFlare, CloudPlane, VolumetricCloud
};

struct TemporalEffectTexture {
    const char* name = nullptr;
    filament::Texture* texture = nullptr;
    filament::TextureSampler sampler;
};

// Feature-owned geometry and source parameters, borrowed only while its
// renderer/runtime is alive. Destroy temporal borrowers before feature owners.
struct TemporalEffectDraw {
    uint64_t identity = 0;
    TemporalEffectKind kind = TemporalEffectKind::Smoke;
    utils::Entity source_entity;
    filament::VertexBuffer* vertices = nullptr;
    filament::IndexBuffer* indices = nullptr;
    filament::MaterialInstance* source = nullptr;
    std::array<TemporalEffectTexture, 3> textures{};
};

struct TemporalEffectCoverage {
    uint64_t draws = 0;
    uint64_t unavailable_draws = 0;
    uint64_t new_or_changed_draws = 0;
};

// Optional full-resolution contributor layer. RG is signed top-left UV motion,
// B reversed depth, A coverage (alpha or bounded additive intensity): positive
// with accepted correspondence, negative
// for a new/discontinuous contributor, zero absent. The output resolver keeps
// the nearest coverage >= 0.5 scene layer; translucent contributors never masquerade as fully
// opaque quads. SDKs receive the resolved dense depth/motion, not this layer.
class TemporalEffects {
public:
    explicit TemporalEffects(const BackendCore& core);
    ~TemporalEffects();
    TemporalEffects(const TemporalEffects&) = delete;
    TemporalEffects& operator=(const TemporalEffects&) = delete;
    bool ensure(int width, int height);
    bool prepare(std::span<const TemporalEffectDraw> draws, const Camera& camera,
        float time, const glm::mat4& current_clip_from_user_world);
    void commit_accepted();
    void invalidate_history();
    filament::View* view() const noexcept;
    filament::Texture* packed_texture() const noexcept;
    const TemporalEffectCoverage& coverage() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace heaven

#endif // HEAVEN_TEMPORAL_EFFECTS_H
