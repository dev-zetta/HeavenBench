/*
 * HeavenBench — Procedural grass feature
 *
 * Per-island instanced grass patches. Generation is **deferred to the
 * first frame** because it needs the scene's instance transforms to
 * position grass on the floating islands. Once built, the geometry is
 * persistent — wind animation runs entirely in the vertex shader of
 * heaven_grass.mat, driven by a single per-frame `time` uniform. The
 * CPU never regenerates blade geometry per frame (doing so killed FPS
 * in the past).
 *
 * Migrated verbatim out of FilamentBackend::init_grass / update_grass
 * and the deferred-generation block in render_frame().
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_GRASS_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_GRASS_FEATURE_H_

#include "rhi/render_feature.h"
#include "rhi/grass_depth_proxy.h"
#include "rhi/grass_patch_bounds.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>

#include <utils/Entity.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace heaven {

class GrassFeature final : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "grass"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void destroy(RenderContext& ctx);

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

private:
    // Buffers outlive every range renderable and depth borrower. Each existing
    // island allocation retains its immutable generated vertex stream.
    struct GrassGeometry {
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
    };

    // One bounded spatial range per floating-island instance. All attributes
    // remain in their original mesh-local vertex stream.
    struct GrassPatch {
        std::size_t             instance_index = 0;
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer*  ib = nullptr;
        utils::Entity           entity;
        std::uint32_t           vertex_count = 0;
        std::uint32_t           index_offset = 0;
        bool                    lod_visible = true;
        GrassPatchBounds        bounds;
    };

    // Build all patches from the island meshes. Runs once on the first
    // update() that has non-empty instance/mesh data.
    void generate(RenderContext& ctx, const FrameContext& frame);

public:
    // Number of times grass blades were actually generated. Generation is a
    // one-time/deferred build (it must stay at most 1 over a run); the
    // per-frame discipline check  reads this through the backend's
    // debug_build_counters() to prove the per-frame path never regenerates
    // grass geometry.
    [[nodiscard]] uint64_t generation_count() const { return generation_count_; }

private:
    filament::Material*         material_ = nullptr;
    filament::MaterialInstance* mi_       = nullptr;   // shared across patches
    std::vector<GrassPatch>     patches_;
    std::vector<GrassGeometry>  geometries_;
    bool                        generated_ = false;
    bool                        in_scene_  = false;
    uint64_t                    generation_count_ = 0;

    // Depth-only proxies of the blades, written into the depth pre-pass scene
    // so the grass occludes the screen-space depth consumers (clouds, fog, god
    // rays, SSGI, splash). Built lazily once the depth proxy scene is available
    // (it shares each patch's VB/IB). depth_proxy_built_ guards the one-time
    // build; the proxies are then transform/time-synced per frame like the
    // visible patches.
    GrassDepthProxy             depth_proxy_;
    bool                        depth_proxy_built_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_GRASS_FEATURE_H_
