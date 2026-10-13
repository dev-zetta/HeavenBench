/*
 * HeavenBench — Massive foliage and GPU-driven geometry feature
 *
 * Spec: advanced-rendering-features,.
 *
 * Modeled directly on GrassFeature: geometry is built ONCE (deferred to
 * the first frame that has the scene instance transforms), and animated
 * thereafter purely through the heaven_grass.mat vertex shader, driven by
 * a single per-frame `time` uniform sourced from FrameContext::time
 * (frame-rate-independent). The CPU never regenerates blade
 * geometry per frame (AGENTS.md hard rule).
 *
 * What it adds on top of the Shipping_Look grass baseline when enabled:
 *   - grass scaled by the foliage density multiplier via
 *     heaven::foliage_blade_count(...), capped at the configured maximum
 *
 *   - at least one new ground-clutter category (flowers) that is absent
 *     from the Shipping_Look — a distinct renderable category beyond the
 *     grass blades;
 *   - enhanced topology and camera-dependent height displacement on the
 *     existing floating-island ground surfaces. Scene loading owns this
 *     shared-pipeline enhancement; the feature does not duplicate terrain.
 *
 * Build-failure fallback: if the configured blade count cannot
 * be built (e.g. insufficient GPU resources), the feature falls back to
 * the Shipping_Look grass baseline, emits a diagnostic, and continues —
 * routed through std::expected per AGENTS.md.
 *
 * Disabled by default (config.features.massive_foliage); a flagless run is
 * Shipping_Look-equivalent.
 */
#ifndef HEAVENBENCH_SRC_RHI_FEATURES_MASSIVE_FOLIAGE_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_FEATURES_MASSIVE_FOLIAGE_FEATURE_H_

#include "rhi/render_feature.h"
#include "rhi/grass_depth_proxy.h"
#include "rhi/grass_patch_bounds.h"
#include "engine/grass_spatial_partition.h"

#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>

#include <utils/Entity.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace heaven {
struct TemporalGrassPatch;

class MassiveFoliageFeature : public IRenderFeature {
public:
    [[nodiscard]] const char* name() const override { return "massive-foliage"; }

    void init(RenderContext& ctx, const AppConfig& config) override;
    void update(RenderContext& ctx, const FrameContext& frame) override;
    void destroy(RenderContext& ctx);

protected:
    void on_enabled_changed(RenderContext& ctx, bool on) override;

    // Build-failure seam. Mirrors WetReflectionsFeature::probe_capability:
    // returns whether foliage geometry can be built at the requested blade count.
    // The default implementation always succeeds (real builds have ample GPU
    // headroom on the reference hardware); a test subclass overrides it to force
    // the fallback-to-baseline branch deterministically without a GPU dependency
    // `virtual`, so the class is intentionally not `final`.
    [[nodiscard]] virtual bool can_build_foliage(int requested_blade_count) const {
        return requested_blade_count > 0;
    }

public:
    // Number of times foliage geometry was actually generated. Like
    // GrassFeature::generation_count this is a one-time/deferred build (it must
    // stay at most 1 over a run); the per-frame discipline check reads it
    // through the backend's debug_build_counters() to prove the per-frame path
    // never regenerates foliage geometry (smoke test).
    [[nodiscard]] uint64_t generation_count() const { return generation_count_; }

    struct GeometryStats {
        size_t geometry_buffers = 0;
        size_t geometry_ranges = 0;
        size_t largest_range_vertices = 0;
        size_t patches = 0;
        size_t uploaded_vertices = 0;
        size_t instanced_vertices = 0;
        bool shared_bindings_valid = true;
    };
    [[nodiscard]] GeometryStats geometry_stats() const;
    void append_temporal_patches(std::vector<TemporalGrassPatch>& output) const;

private:
    // A renderable category. Grass and flowers share the heaven_grass vertex
    // animation but use distinct material instances so the flower clutter is a
    // visibly separate category absent from the Shipping_Look.
    enum class Category : uint8_t { Grass = 0, Flower = 1 };

    // One buffer owner per source mesh/category. Generation depends only on
    // that source's roots, never its instance transform. Every placement and
    // its depth proxy borrow these immutable buffers until feature teardown.
    struct FoliageGeometry {
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer* ib = nullptr;
        int vertex_count = 0;
        std::vector<GrassDrawRange> ranges;
    };

    // One renderable per spatial range and matching island instance, retaining
    // independent bounds and visibility while borrowing source-local geometry.
    struct FoliagePatch {
        int                     instance_index = -1;
        Category                category = Category::Grass;
        filament::VertexBuffer* vb = nullptr;
        filament::IndexBuffer*  ib = nullptr;
        utils::Entity           entity;
        int                     vertex_count = 0;
        std::uint32_t           index_offset = 0;
        bool                    lod_visible = true;
        GrassPatchBounds        bounds;
    };

    // Deferred one-time build, run on the first update() that has non-empty
    // instance/mesh data. Honours can_build_foliage(): on failure it logs the
    // diagnostic and falls back to the Shipping_Look grass baseline.
    void generate(RenderContext& ctx, const FrameContext& frame);

    // Build the scaled grass and flower clutter.
    // Returns an Error (std::expected) when the configured blade count cannot
    // be built so generate() can fall back to the baseline.
    Result<void> build_foliage(RenderContext& ctx, const FrameContext& frame);

    // Build only the Shipping_Look grass baseline (density 1.0, no flowers).
    // Used both as the fallback and as the body of the scaled grass
    // build.
    void build_grass(RenderContext& ctx, const FrameContext& frame,
                     int target_total_blades);

    // Build the flower ground-clutter category.
    void build_flowers(RenderContext& ctx, const FrameContext& frame,
                       int target_flower_count);

    // Allocate and upload one source/category's exact generated vertex stream.
    std::optional<size_t> upload_geometry(RenderContext& ctx,
        const std::vector<GrassRoot>& roots, Category category);

    // Build all of an island's spatial range renderables, sharing source
    // buffers and one category material instance.
    bool make_patch(RenderContext& ctx, const FrameContext& frame,
                    int instance_index, Category category,
                    size_t geometry_index,
                    filament::MaterialInstance& material_instance);

    filament::Material*         material_       = nullptr;  // heaven_grass
    filament::MaterialInstance* grass_mi_       = nullptr;  // shared by grass patches
    filament::MaterialInstance* flower_mi_      = nullptr;  // shared by flower patches
    const filament::Texture*    grass_diffuse_texture_ = nullptr; // borrowed
    const filament::Texture*    flower_diffuse_texture_ = nullptr; // borrowed
    std::vector<FoliagePatch>   patches_;
    std::vector<FoliageGeometry> geometries_;

    // Resolved quality knobs captured at init (the per-frame path never
    // re-reads them — geometry is built once).
    QualityKnobs                quality_{};

    bool                        generated_ = false;
    bool                        in_scene_  = false;
    uint64_t                    generation_count_ = 0;

    // Depth-only proxies of the foliage patches, written into the depth pre-pass
    // scene so the foliage occludes the screen-space depth consumers (clouds,
    // fog, god rays, SSGI, splash). Built lazily once the depth proxy scene is
    // available, sharing each patch's VB/IB (no extra geometry or per-frame
    // regeneration). Preserve the visible patch order, including flower cards.
    GrassDepthProxy             depth_proxy_;
    bool                        depth_proxy_built_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_FEATURES_MASSIVE_FOLIAGE_FEATURE_H_
