/*
 * HeavenBench — Render feature framework
 *
 * The Filament backend used to be a single ~4600-line god-class that
 * owned every visual feature (grass, particles, god rays, volumetric
 * fog, clouds, sky, lens flares, heat haze, …) inline. This header
 * introduces a small framework that lets each feature live in its own
 * translation unit under rhi/features/ be toggled on/off at runtime,
 * and (later) expose its own tunables to a settings UI.
 *
 * Design:
 *   - RenderContext: long-lived shared Filament state + a resource
 *                      provider for texture loading. Built once by the
 *                      backend and handed to every feature.
 *   - FrameContext: per-frame inputs (camera, lighting, time,
 *                      viewport, shared depth pre-pass texture).
 *   - IRenderFeature: the unit of decomposition. Owns its own GPU
 *                      resources, adds/removes its renderables from the
 *                      shared scene, and participates in the depth
 *                      pre-pass when it samples scene depth.
 *
 * Features must NOT reach into FilamentBackend internals — everything
 * they need is on RenderContext / FrameContext. This keeps each feature
 * independently compilable and testable.
 */
#ifndef HEAVENBENCH_SRC_RHI_RENDER_FEATURE_H_
#define HEAVENBENCH_SRC_RHI_RENDER_FEATURE_H_

#include "rhi/rhi.h"
#include "engine/asset_file_system.h"
#include "engine/path_resolver.h"
#include "engine/types.h"
#include "engine/camera.h"

#include <utils/Entity.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <stdexcept>
#include <string>

namespace filament {
class Engine;
class Scene;
class View;
class Renderer;
class Camera;
class Texture;
class RenderTarget;
}

namespace heaven {

class RenderTestSceneAssets;
struct TemporalEffectDraw;

// ── Scene light info (read-only view for features) ──────────────

// The backend spawns and owns all scene point/spot lights (emission-
// derived + world-authored). Features that need to react to those
// lights — volumetric fog (scattering contributors) and lens flares
// (one halo per luminaire) — get this read-only descriptor per light.
// `entity` lives in the shared LightManager; features query live
// position/colour/falloff/intensity through it and must never create
// or destroy it.

// `world_pos` is the light's current world position (already updated
// for floating-island bob this frame). `local_pos` + `instance_index`
// let a feature recompute the world position itself when it needs to
// track an animated parent mesh (lens flares glued to a building).
// `floating_group` (>= 0) means the light bobs with that island group.
struct SceneLightInfo {
    utils::Entity entity;
    glm::vec3     world_pos{0.0f};
    glm::vec3     local_pos{0.0f};
    glm::vec3     color{1.0f};
    // Luminous intensity (candela), read from the LightManager. The
    // shadow-casting point/spot light feature  ranks candidates
    // highest-intensity-first up to the budget, so features need it alongside
    // the colour/position they already read.
    float         intensity = 0.0f;
    int           floating_group = -1;
    int           instance_index = -1;
    bool          authored = false;   // world-authored vs emission-derived
    // Illumination alone does not imply a visible bulb at the light origin.
    bool          flare_source = false;
    bool          shadow_eligible = true;
    // Ungated source candela, retained even while a night bulb is off.
    float         nominal_intensity = 0.0f;
    // One local atmospheric volume per synthetic enclosed bulb. Authored
    // lighting-only sources and open flames must not duplicate that glow.
    bool          local_volume_source = false;
};

// ── Resource provider ───────────────────────────────────────────

// Texture loading + the shared placeholder textures live on the
// backend (it owns the cache and the Filament engine). Features get
// at them through this narrow interface rather than the whole backend.
class IResourceProvider {
public:
    virtual ~IResourceProvider() = default;

    //Load a DDS texture from disk (cached by path). 0 on failure.
    [[nodiscard]] virtual uint64_t load_texture(
        const std::string& path,
        TextureSemantic semantic = TextureSemantic::ScalarLinear) = 0;

    //Resolve a handle from load_texture() to a Filament texture.
    [[nodiscard]] virtual filament::Texture* get_filament_texture(uint64_t handle) = 0;

    // Some authored feature textures are PNGs and keep their exact legacy
    // decoder path at Original quality. Let the owning resource store include
    // those externally uploaded textures in the same effective-tier report.
    virtual void record_external_original_texture(
        uint32_t /*width*/, uint32_t /*height*/, TextureSemantic /*semantic*/,
        bool /*complete_mip_chain*/) {}

    //Shared 1×1 white / flat-normal placeholders for missing maps.
    [[nodiscard]] virtual filament::Texture* placeholder_texture() = 0;
    [[nodiscard]] virtual filament::Texture* placeholder_normal_texture() = 0;

    //1×1 zero-reading placeholder for the scene-depth sampler. Consumers bind
    //this when the real Scene_Depth_Texture is absent; their shaders read its
    //non-positive red channel as "far / no geometry" and skip occlusion
    //(graceful fallback, spec). Distinct from placeholder_texture()
    //(white = 1.0), which as a depth value would falsely occlude everything.
    [[nodiscard]] virtual filament::Texture* placeholder_depth_texture() = 0;
};

// ── Shared render context (long-lived) ──────────────────────────
struct RenderContext {
    filament::Engine*   engine    = nullptr;
    filament::Scene*    scene     = nullptr;  // the main scene
    filament::View*     view      = nullptr;  // the main view
    filament::Renderer* renderer  = nullptr;
    filament::Camera*   camera    = nullptr;  // the main camera
    IResourceProvider*  resources = nullptr;

    // Explicit procedural inputs for an isolated render fixture. The backend
    // owns this; features borrow it and are destroyed before its textures.
    RenderTestSceneAssets* test_scene_assets = nullptr;

    // Optional references to the application-owned logical asset context.
    // They are populated before feature initialization and avoid nullable,
    // non-owning raw pointers in the ZIP-related path.
    PathResolverRef paths;
    AssetFileSystemRef assets;

    // Asset-consuming features require the resolver populated by the backend.
    // Keep the check at this boundary so incomplete contexts fail explicitly.
    [[nodiscard]] const PathResolver& path_resolver() const {
        if (!paths.has_value()) {
            throw std::logic_error("render feature context has no path resolver");
        }
        return paths->get();
    }

    // Final output render target for composite passes. Normally nullptr (the
    // swap chain). Under HDR display output the backend sets this to the linear
    // Rec.2020 fp16 HDR scene target so a feature's composite pass (heat haze)
    // lands there instead of the swap chain; the backend's HDR encode pass then
    // PQ-encodes that target to the swap chain. Features that render a composite
    // to the swap chain MUST honour this (fall back to nullptr when unset).
    filament::RenderTarget* output_target = nullptr;
};

// ── Per-frame context ───────────────────────────────────────────
struct FrameContext {
    const Camera*         camera     = nullptr;
    const LightingParams* lighting   = nullptr;
    // Read-only per-frame weather state (rain intensity + derived
    // responses), owned by the application layer and threaded through
    // exactly like `lighting`. Defaults null so features guard until
    // the backend wires it in render_frame.
    const WeatherState*   weather    = nullptr;
    float                 time       = 0.0f;  // frame-rate-independent seconds
    // Audit captures reuse this index when a swapchain frame is rejected, so
    // per-frame sampling phases remain tied to accepted simulation steps.
    std::optional<uint64_t> capture_frame;
    int                   viewport_w = 0;
    int                   viewport_h = 0;

    // Scene data spans (some features — e.g. volumetric fog — walk the
    // instance/mesh lists to find emissive contributors).
    std::span<const Instance> instances;
    std::span<const GpuMesh>  meshes;
    const MaterialResolver*   resolve_material = nullptr;

    // Shared scene-depth pre-pass texture (the Scene_Depth_Texture):
    // an F COLOR attachment holding linear eye-space distance in
    // metres (empty pixels read the far value, "no geometry"). Valid
    // only for features whose needs_depth_prepass() returns true and
    // only after the backend has rendered the depth view this frame.
    // Half-resolution.
    filament::Texture* depth_texture = nullptr;
    int                depth_w       = 0;
    int                depth_h       = 0;

    // The depth pre-pass proxy Scene (the one the depth view renders to produce
    // depth_texture). Procedural-grass features add depth-only proxy
    // renderables here so the blades are written into the scene-depth texture —
    // otherwise depth consumers (clouds, fog, god rays, SSGI, splashes) read the
    // terrain/sky behind the grass and composite in front of it. Null when the
    // depth pre-pass is not active this frame (no enabled consumer needs depth),
    // in which case there is nothing to occlude and features skip proxy build.
    filament::Scene* depth_proxy_scene = nullptr;

    // Previous frame's lit colour, for an inline screen-space lighting pass
    // (SSGI) to gather one-bounce indirect light from. Provided by the backend's
    // SceneColorCapture only when a feature requested scene colour this frame
    // (and not on the very first frame, when no history exists yet). Null
    // otherwise — features must fall back (e.g. to a neutral source) when null.
    filament::Texture* scene_color_texture = nullptr;
    // Projection/view that produced that exact history texture. Camera state
    // advances only with a successful history copy, never with a skipped frame.
    std::optional<glm::mat4> scene_color_view_proj;

    // Point/spot light entities the backend spawned for the scene
    // (emission-derived + world-authored). Volumetric fog walks this
    // list to pick the strongest scattering contributors near the
    // camera. The entities live in the shared LightManager; features
    // query position/colour/falloff/intensity through it and must not
    // create or destroy them.
    std::span<const utils::Entity> scene_light_entities;

    // Richer per-light descriptors for features that need more than
    // the entity (lens flares: colour classification + per-parent
    // tracking). Same lights as scene_light_entities, but with the
    // metadata the backend already tracks. Valid once the scene is
    // built and authored lights spawned.
    std::span<const SceneLightInfo> scene_lights;

    // Per-frame floating-island Z offsets: (group, z_offset) pairs.
    // Features whose renderables are anchored to a floating group
    // (lens flares, particle emitters) add (0,0,z) to follow the
    // island's sine-wave bob. Empty ⇒ nothing is floating this frame.
    std::span<const std::pair<int, float>> floating_offsets;
};

// ── Render feature interface ────────────────────────────────────
class IRenderFeature {
public:
    virtual ~IRenderFeature() = default;

    //Stable identifier for logs / settings UI.
    [[nodiscard]] virtual const char* name() const = 0;

    //One-time GPU resource setup. Called once from init_pipelines().
    //Implementations should tolerate missing assets gracefully.
    virtual void init(RenderContext& /*ctx*/, const AppConfig& /*config*/) {}

    //Per-frame uniform / transform update. Only called while enabled.
    virtual void update(RenderContext& /*ctx*/, const FrameContext& /*frame*/) {}

    // Export persistent geometry and current source parameters only when the
    // optional temporal runtime asks. The runtime owns accepted-frame history;
    // rejected application attempts must not advance feature correspondence.
    virtual void append_temporal_draws(std::vector<TemporalEffectDraw>& /*draws*/) const {}

    // Called for every feature, including disabled ones, before shared render
    // targets are resized. Replace borrowed samplers with stable placeholders
    // and clear cached identities: allocator address reuse is not identity.
    // Filament can prepare descriptors belonging to an inactive material too.
    virtual void release_shared_targets(RenderContext& /*ctx*/) {}

    //Runtime on/off. Implementations add/remove their renderables from
    //the scene in on_enabled_changed(); the default just flips the flag.
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void set_enabled(RenderContext& ctx, bool on) {
        if (on == enabled_) return;
        enabled_ = on;
        on_enabled_changed(ctx, on);
    }

    // ── Depth pre-pass participation ────────────────────────────

    // God rays, volumetric fog and heat haze sample the scene's depth.
    // The backend renders a depth-only view each frame; features that
    // draw screen-space quads in the MAIN scene must be pulled OUT for
    // that depth render (so their quads don't pollute depth) and put
    // back before the main view renders.

    //   needs_depth_prepass()  — "I sample the depth texture, so the
    //                            backend must run the pass this frame."
    //   on_depth_prepass_begin/end() — called for EVERY enabled feature
    //                            whenever the pass runs (regardless of
    //                            needs_depth_prepass), so a feature can
    //                            pull its quads out even if it only
    //                            *contributes* to depth without sampling
    //                            it (e.g. lens flares, whose alpha quads
    //                            would otherwise write bright depth
    //                            "voids"). Features that own no main-
    //                            scene quads leave these as no-ops.
    [[nodiscard]] virtual bool needs_depth_prepass() const { return false; }
    virtual void on_depth_prepass_begin(RenderContext& /*ctx*/) {}
    virtual void on_depth_prepass_end(RenderContext& /*ctx*/) {}

    // ── Scene-colour participation ──────────────────────────────

    // An inline screen-space lighting pass (SSGI) gathers from the rendered
    // scene colour. Because it composites within the main scene, it reads the
    // PREVIOUS frame's lit result. A feature returns true here (while enabled)
    // to ask the backend to capture the main view to an offscreen colour target
    // and expose last frame's copy via FrameContext::scene_color_texture. When
    // no enabled feature wants it, the main view renders straight to the swap
    // chain (Shipping_Look parity, no capture/blit overhead).
    [[nodiscard]] virtual bool wants_scene_color() const { return false; }

    // Composite pass for a scene-colour feature (SSGI). Called by the backend
    // AFTER the clean main view has been captured and blitted to `target`, so
    // the feature can draw its additive contribution OVER the displayed scene
    // WITHOUT that contribution being captured into the history it gathers from
    // next frame. This is what breaks the SSGI self-feedback loop: the captured
    // history is the pre-SSGI scene, and SSGI's bounce is applied to the display
    // only. `target` is the final render target (null swap chain on SDR, the HDR
    // scene target on HDR). No-op for features that don't gather scene colour.
    virtual void render_scene_color_composite(RenderContext& /*ctx*/,
                                              filament::RenderTarget* /*target*/) {}

    // ── Scene-capture / composite participation ─────────────────

    // Heat haze captures the rendered scene to an offscreen texture,
    // then runs a second "composite" view that blits it back and draws
    // refraction billboards on top. A feature that does this owns its
    // own capture render target + composite view; the backend just
    // sequences the passes around the main view render.

    // Flow each frame when wants_scene_capture() is true:
    //   1. backend calls begin_scene_capture(ctx, frame): the feature
    //      ensures its capture RT, points the MAIN view at it
    //      (ctx.view->setRenderTarget(...)), rebinds its materials and
    //      positions its composite/blit quad.
    //   2. main view renders into the feature's capture texture.
    //   3. backend calls render_composite(ctx) AFTER the main view; the
    //      feature renders its own view to the swap chain.
    // When no feature wants capture this frame, the backend points the
    // main view back at the swap chain itself.
    [[nodiscard]] virtual bool wants_scene_capture(const FrameContext& /*frame*/) const {
        return false;
    }
    virtual void begin_scene_capture(RenderContext& /*ctx*/,
                                     const FrameContext& /*frame*/) {}
    virtual void render_composite(RenderContext& /*ctx*/) {}

protected:
    virtual void on_enabled_changed(RenderContext& /*ctx*/, bool /*on*/) {}
    bool enabled_ = true;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_RENDER_FEATURE_H_
