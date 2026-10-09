/*
 * HeavenBench — Rain feature implementation (skeleton)
 *
 * scope: the IRenderFeature scaffolding, resource init with
 * graceful failure, the toggle/destroy lifecycle
 * and the depth-pre-pass declaration. The build-once
 * streaks  and splashes  plus their per-frame
 * uniform pushes are wired in later tasks — build_rain / build_splashes
 * update are minimal stubs here that compile and do nothing harmful
 * while Rain_Intensity is 0.
 */
#include "rhi/features/rain_feature.h"
#include "rhi/render_test_scene_assets.h"
#include "engine/feature_assets.h"
#include "rhi/filament_bounds.h"
#include "rhi/filament_util.h"
#include "rhi/material_tuning_bindings.h"
#include "rhi/materials_data.h"
#include "engine/weather_shader_ref.h"
#include "engine/path_resolver.h"
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/TextureSampler.h>

#include <utils/EntityManager.h>

#include <math/vec2.h>
#include <math/vec3.h>
#include <math/vec4.h>
#include <math/mat4.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>

namespace heaven {

void RainFeature::init(RenderContext& ctx, const AppConfig& config) {
    auto* engine_ = ctx.engine;

    // Honour the feature toggle. Note: even when enabled,
    // rain only *renders* once Rain_Intensity > 0; intensity 0 (e.g. via
    // `--rain-intensity 0` or `--no-rain`) restores the clear Shipping_Look.
    // The default intensity is now medium (0.5).
    enabled_ = config.features.rain;

    // ── Streak base material (heaven_rain.filamat) ──────────────
    // On any material/texture failure we log the named resource, mark
    // the feature unavailable + disabled, build no renderables, and
    // return so the renderer keeps running without precipitation.
    rain_material_ = filament::Material::Builder()
        .package(k_heaven_rain_filamat, k_heaven_rain_filamat_size)
        .build(*engine_);
    if (!rain_material_) {
        std::printf("  Rain: material 'heaven_rain' failed to load — rain disabled\n");
        available_ = false;
        enabled_   = false;
        return;
    }

    // ── Splash base material (heaven_splash.filamat) ────────────
    splash_material_ = filament::Material::Builder()
        .package(k_heaven_splash_filamat, k_heaven_splash_filamat_size)
        .build(*engine_);
    if (!splash_material_) {
        std::printf("  Rain: material 'heaven_splash' failed to load — rain disabled\n");
        available_ = false;
        enabled_   = false;
        return;
    }

    rain_mi_ = rain_material_->createInstance();
    if (!rain_mi_) {
        std::printf("  Rain: rain material instance failed — rain disabled\n");
        available_ = false;
        enabled_ = false;
        return;
    }
    material_tuning::bind_rain(*rain_mi_);

    splash_mi_ = splash_material_->createInstance();
    if (!splash_mi_) {
        std::printf("  Rain: splash material instance failed — rain disabled\n");
        available_ = false;
        enabled_ = false;
        return;
    }
    material_tuning::bind_splash(*splash_mi_);

    // ── Precipitation / splash sprite textures ──────────────────
    // Loaded through the shared resource cache (IResourceProvider), so a
    // missing asset names the exact file in the diagnostic and the cache
    // retains ownership of the Filament textures.
    const std::string drop_path   =
        ctx.test_scene_assets ? "procedural rain-drop sprite" :
        feature_asset_path(ctx.path_resolver(), FeatureAsset::RainDrop);
    const std::string splash_path =
        ctx.test_scene_assets ? "procedural splash sprite" :
        feature_asset_path(ctx.path_resolver(), FeatureAsset::Glow);

    drop_tex_ = resolve_feature_texture(
        ctx, FeatureAsset::RainDrop, TextureSemantic::ColorSRGB);
    if (!drop_tex_) {
        std::printf("  Rain: texture '%s' failed to load — rain disabled\n",
                    drop_path.c_str());
        available_ = false;
        enabled_   = false;
        return;
    }

    splash_tex_ = resolve_feature_texture(
        ctx, FeatureAsset::Glow, TextureSemantic::ColorSRGB);
    if (!splash_tex_) {
        std::printf("  Rain: texture '%s' failed to load — rain disabled\n",
                    splash_path.c_str());
        available_ = false;
        enabled_   = false;
        return;
    }

    available_ = true;

    std::printf("  Rain: precipitation materials + textures loaded%s\n",
                enabled_ ? "" : " (disabled)");
}

void RainFeature::build_rain(RenderContext& ctx) {
    // Build one renderable up to the centralized kMaxStreaks capacity.
    // quads. The geometry is built exactly once here; every per-streak
    // world position, length and billboard orientation is computed in the
    // heaven_rain.mat vertex shader from the per-streak seed baked into CUSTOM0
    // plus a handful of uniforms — the CPU never moves a vertex after this
    // The template quad below is the same for every streak; the
    // shader replaces its world position outright (material.worldPosition).
    auto* engine_ = ctx.engine;

    constexpr int kStreaks    = kMaxStreaks;
    constexpr int vert_count  = kStreaks * 4;    // 4 verts / quad
    constexpr int idx_count   = kStreaks * 6;    // 2 tris / quad

    // Per-vertex attributes, matching heaven_rain.mat's `requires:[uv0, custom0]`:
    //   POSITION.xyz = unused base (world position computed in the shader)
    //   UV0.xy       = corner id in [0,1] (x across the streak, y along it)
    //   CUSTOM0.xyzw = (seed.x, seed.y, seed.z, streakIndex)
    //                  CUSTOM0.w carries the streak's SLOT INDEX i so the
    //                  vertex shader can cull with streakIndex < activeStreaks.
    auto positions =
        std::make_unique<filament::math::float3[]>(vert_count);
    auto uvs = std::make_unique<filament::math::float2[]>(vert_count);
    auto customs0 =
        std::make_unique<filament::math::float4[]>(vert_count);

    // Deterministic per-streak seeds, so a given slot always hashes to the
    // same cell/length: a fixed-seed PRNG fed into the hash3/hash1 the shader
    // (and weather_shader_ref) use. The seeds are bounded but otherwise
    // arbitrary — the volume + wrap math keeps every streak inside the
    // camera-centered cube for any seed.
    std::mt19937 rng(0x9E3779B9u);
    std::uniform_real_distribution<float> seed_dist(0.0f, 1000.0f);

    constexpr std::array<filament::math::float2, 4> corners = {{
        {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
    }};

    for (int s = 0; s < kStreaks; ++s) {
        const filament::math::float3 seed{
            seed_dist(rng), seed_dist(rng), seed_dist(rng)};
        const int v = s * 4;
        for (int c = 0; c < 4; ++c) {
            positions[v + c] = {0.0f, 0.0f, 0.0f};
            uvs[v + c]       = corners[c];
            customs0[v + c]  = filament::math::float4{
                seed.x, seed.y, seed.z, static_cast<float>(s)};
        }
    }

    // Index buffer (two triangles per quad).
    auto indices = std::make_unique<std::uint32_t[]>(idx_count);
    for (int s = 0; s < kStreaks; ++s) {
        const uint32_t v = static_cast<uint32_t>(s) * 4u;
        std::uint32_t* const o = &indices[static_cast<size_t>(s) * 6];
        o[0] = v + 0; o[1] = v + 1; o[2] = v + 2;
        o[3] = v + 0; o[4] = v + 2; o[5] = v + 3;
    }

    rain_vb_ = filament::VertexBuffer::Builder()
        .vertexCount(vert_count)
        .bufferCount(3)
        .attribute(filament::VertexAttribute::POSITION, 0,
                   filament::VertexBuffer::AttributeType::FLOAT3, 0, 12)
        .attribute(filament::VertexAttribute::UV0, 1,
                   filament::VertexBuffer::AttributeType::FLOAT2, 0, 8)
        .attribute(filament::VertexAttribute::CUSTOM0, 2,
                   filament::VertexBuffer::AttributeType::FLOAT4, 0, 16)
        .build(*engine_);

    rain_vb_->setBufferAt(*engine_, 0,
        filament::VertexBuffer::BufferDescriptor(
            positions.release(), vert_count * sizeof(filament::math::float3),
            [](void* b, size_t, void*) {
                delete[] static_cast<filament::math::float3*>(b);
            }));
    rain_vb_->setBufferAt(*engine_, 1,
        filament::VertexBuffer::BufferDescriptor(
            uvs.release(), vert_count * sizeof(filament::math::float2),
            [](void* b, size_t, void*) {
                delete[] static_cast<filament::math::float2*>(b);
            }));
    rain_vb_->setBufferAt(*engine_, 2,
        filament::VertexBuffer::BufferDescriptor(
            customs0.release(), vert_count * sizeof(filament::math::float4),
            [](void* b, size_t, void*) {
                delete[] static_cast<filament::math::float4*>(b);
            }));

    rain_ib_ = filament::IndexBuffer::Builder()
        .indexCount(idx_count)
        .bufferType(filament::IndexBuffer::IndexType::UINT)
        .build(*engine_);
    rain_ib_->setBuffer(*engine_,
        filament::IndexBuffer::BufferDescriptor(
            indices.release(), idx_count * sizeof(std::uint32_t),
            [](void* b, size_t, void*) {
                delete[] static_cast<uint32_t*>(b);
            }));

    // Static streak uniforms (the camera-tracking + animation uniforms are
    // pushed every frame in update()). volumeExtent / streakLenMin/Max mirror
    // the C++ constants in weather_shader_ref so the GPU and the CPU reference
    // agree byte-for-byte.
    rain_mi_->setParameter("time",          0.0f);
    rain_mi_->setParameter("rainIntensity", 0.0f);
    rain_mi_->setParameter("activeStreaks", 0);
    rain_mi_->setParameter("cameraPos",
        filament::math::float3{0.0f, 0.0f, 0.0f});
    rain_mi_->setParameter("volumeExtent",  weather_ref::kVolumeExtent);
    rain_mi_->setParameter("fallSpeed", render_tuning::rain.fall_speed);
    rain_mi_->setParameter("streakLenMin",  weather_ref::kStreakLenMin);
    rain_mi_->setParameter("streakLenMax",  weather_ref::kStreakLenMax);

    // The streak volume is camera-centered purely by the `cameraPos` uniform:
    // the vertex shader writes each streak's absolute world position as
    // `cameraPos + offset`, so a tight static AABB would cull as the camera
    // moves. Disable culling (the volume always surrounds the camera) and give
    // it a generous box anyway, per AGENTS.md guidance for very large
    // camera-tracking meshes.
    const float ext = weather_ref::kVolumeExtent;
    rain_entity_ = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  rain_vb_, rain_ib_, 0, idx_count)
        .material(0, rain_mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(6)  // transparent rain composites late, over opaque scene
        .boundingBox(box_from_min_max({-ext, -ext, -ext}, {ext, ext, ext}))
        .build(*engine_, rain_entity_);

    // Identity transform. Like the splash path, the streak vertex shader writes
    // an ABSOLUTE world position (`cameraPos` uniform + per-streak offset), so
    // this transform is never re-centered per frame — it exists only so the
    // renderable has a transform component.
    engine_->getTransformManager().create(rain_entity_);

    // Build-once instrumentation: bumped exactly here.
    ++streak_build_count_;

    std::printf("  Rain: built %d streak quads (one renderable)\n", kStreaks);
}

void RainFeature::build_splashes(RenderContext& ctx) {
    // Build the SINGLE fullscreen world-space quad for the screen-space splash
    // pass (Breath-of-the-Wild rain), mirroring GodraysFeature / VolfogFeature
    // exactly. The geometry is built once here; every per-pixel ripple is
    // computed in the heaven_splash.mat fragment shader from the sampled depth
    // pre-pass plus a handful of uniforms — the CPU never moves a vertex after
    // this. Only the per-frame uniforms and the quad's per-frame
    // placement in front of the camera change thereafter; that placement is the
    // same allowed pattern the godrays / volfog passes use (it does not
    // regenerate geometry).
    auto* engine_ = ctx.engine;

    // Shared-available semantics: if only the splash material/instance failed
    // to load while rain is otherwise available, skip building the splash
    // renderable so the falling streaks still render. Guarding here
    // keeps the streak path independent of the splash asset.
    if (!splash_material_ || !splash_mi_) {
        std::printf("  Rain: splash material unavailable — skipping splash build\n");
        return;
    }

    // Fullscreen quad geometry, identical shape to the godrays / volfog quad:
    // two triangles covering [-1,1] with UVs [0,1]. The CPU positions it just
    // inside the far plane in front of the camera each frame in update().
    struct QuadVertex {
        filament::math::float3 position;
        filament::math::float2 uv;
    };
    static constexpr std::array<QuadVertex, 4> quad_verts = {{
        {{-1, -1, 0}, {0, 0}},
        {{ 1, -1, 0}, {1, 0}},
        {{ 1,  1, 0}, {1, 1}},
        {{-1,  1, 0}, {0, 1}},
    }};
    static constexpr std::array<std::uint16_t, 6> quad_indices = {
        0, 1, 2, 0, 2, 3};

    splash_vb_ = filament::VertexBuffer::Builder()
        .vertexCount(4)
        .bufferCount(2)
        .attribute(filament::VertexAttribute::POSITION, 0,
                   filament::VertexBuffer::AttributeType::FLOAT3,
                   0, sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::UV0, 0,
                   filament::VertexBuffer::AttributeType::FLOAT2,
                   sizeof(filament::math::float3), sizeof(QuadVertex))
        .attribute(filament::VertexAttribute::TANGENTS, 1,
                   filament::VertexBuffer::AttributeType::SHORT4, 0, 0)
        .normalized(filament::VertexAttribute::TANGENTS)
        .build(*engine_);
    splash_vb_->setBufferAt(*engine_, 0,
        filament::VertexBuffer::BufferDescriptor(
            quad_verts.data(), sizeof(quad_verts)));

    static constexpr std::array<filament::math::short4, 4> dummy_tangents = {{
        {0, 0, 16383, 16383}, {0, 0, 16383, 16383},
        {0, 0, 16383, 16383}, {0, 0, 16383, 16383}
    }};
    splash_vb_->setBufferAt(*engine_, 1,
        filament::VertexBuffer::BufferDescriptor(
            dummy_tangents.data(), sizeof(dummy_tangents)));

    splash_ib_ = filament::IndexBuffer::Builder()
        .indexCount(6)
        .bufferType(filament::IndexBuffer::IndexType::USHORT)
        .build(*engine_);
    splash_ib_->setBuffer(*engine_,
        filament::IndexBuffer::BufferDescriptor(
            quad_indices.data(), sizeof(quad_indices)));

    // Static splash uniforms (the per-frame animation uniforms + depth binding
    // + reconstruction matrix are pushed every frame in update()). Ripple
    // tuning (spacing, period, ring width, colour) are shader CONSTANTS inside
    // heaven_splash.mat, so they are NOT uniforms here — only the actual
    // parameters get a starting value. The depth sampler starts on the shared
    // placeholder until the real depth pre-pass texture exists (bound in
    // update() outside any render pass —).
    filament::TextureSampler depth_sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    splash_mi_->setParameter("depthTexture",
        ctx.resources->placeholder_depth_texture(), depth_sampler);
    splash_mi_->setParameter("time",            0.0f);
    splash_mi_->setParameter("splashIntensity", 0.0f);
    // ABSOLUTE camera world position (Issue 1): used by the splash fragment
    // shader to recover an absolute world XY for the world-anchored ripple
    // tiling (worldAbs = worldPos + cameraPos). Initialised to the origin here
    // and pushed each frame in update() from frame.camera->position — mirrors
    // how the streak path seeds + updates its own cameraPos. Re-adding this
    // uniform does NOT re-introduce any per-frame geometry rebuild: the quad
    // still builds exactly once (splash_build_count == 1).
    splash_mi_->setParameter("cameraPos",
        filament::math::float3{0.0f, 0.0f, 0.0f});
    // Shared fall-law uniforms for the rain/splash UNIFICATION. The splash
    // retimes each ripple from the SAME lattice + fall law the streaks use, so
    // these MUST match the streak uniforms (weather_ref::kVolumeExtent and the
    // nominal kFallSpeed) byte-for-byte. Pushed again every frame in update().
    splash_mi_->setParameter("volumeExtent", weather_ref::kVolumeExtent);
    splash_mi_->setParameter("fallSpeed", render_tuning::rain.fall_speed);

    // Fullscreen quad: disable culling (it is repositioned in front of the
    // camera every frame, like godrays / volfog) and give it a generous box.
    // Transparent + doubleSided per the material; priority 5 so ground ripples
    // composite over the opaque scene and under the falling streaks (priority 6).
    splash_entity_ = utils::EntityManager::get().create();
    filament::RenderableManager::Builder(1)
        .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                  splash_vb_, splash_ib_)
        .material(0, splash_mi_)
        .castShadows(false)
        .receiveShadows(false)
        .culling(false)
        .priority(5)  // composite over opaque scene, under the falling streaks
        .boundingBox(uncullable_box())
        .build(*engine_, splash_entity_);

    // Explicit transform component — update() positions the quad in front of
    // the camera each frame (same as the godrays / volfog quads).
    engine_->getTransformManager().create(splash_entity_);

    // Build-once instrumentation: bumped exactly here.
    ++splash_build_count_;

    std::printf("  Rain: built splash fullscreen quad (one renderable)\n");
}

void RainFeature::release_shared_targets(RenderContext& ctx) {
    const filament::TextureSampler sampler(
        filament::TextureSampler::MinFilter::NEAREST,
        filament::TextureSampler::MagFilter::NEAREST,
        filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
    if (splash_mi_) splash_mi_->setParameter("depthTexture",
        ctx.resources->placeholder_depth_texture(), sampler);
    bound_depth_ = nullptr;
}

void RainFeature::update(RenderContext& ctx, const FrameContext& frame) {
    // Guard on a successful init  and the weather handle so we never
    // dereference a null FrameContext::weather.
    if (!available_) return;
    if (!frame.weather) return;

    // ── Build-once geometry on the first active frame ───────────
    // update() is only called while enabled (see update_features), so the
    // first call here is the first active frame. The streak / splash buffers
    // are created exactly once; afterwards only uniforms + the entity
    // transform change.
    if (!built_) {
        build_rain(ctx);
        build_splashes(ctx);
        built_ = true;
        in_scene_ = enabled_;
        if (enabled_) {
            if (rain_entity_)   ctx.scene->addEntity(rain_entity_);
            if (splash_entity_) ctx.scene->addEntity(splash_entity_);
        }
    }

    // Effective rain intensity drives every per-frame response; intensity 0
    // (e.g. via `--rain-intensity 0` or `--no-rain`) yields activeStreaks = 0
    // so nothing draws and the Shipping_Look is preserved. The default
    // intensity is now medium (0.5).
    const float intensity = frame.weather->effective_intensity();
    const int   active_streaks = static_cast<int>(
        std::lround(intensity * static_cast<float>(kMaxStreaks)));
    const glm::vec3 cam = frame.camera ? frame.camera->position : glm::vec3(0.0f);

    // ── Per-frame streak uniforms (no vertex is moved on the CPU) ──
    // time is the frame-rate-independent getUserTime() seconds. The
    // shader computes each streak's world position, fall + wrap, and length
    // from these uniforms and the baked CUSTOM0 seed (mirrors
    // weather_shader_ref::streak_world_position / streak_length).
    if (rain_mi_) {
        rain_mi_->setParameter("time",          frame.time);
        rain_mi_->setParameter("rainIntensity", intensity);
        rain_mi_->setParameter("activeStreaks", active_streaks);
        rain_mi_->setParameter("cameraPos",
            filament::math::float3{cam.x, cam.y, cam.z});
        rain_mi_->setParameter("volumeExtent",  weather_ref::kVolumeExtent);
        rain_mi_->setParameter(
            "fallSpeed", render_tuning::rain.fall_speed);
        rain_mi_->setParameter("streakLenMin",  weather_ref::kStreakLenMin);
        rain_mi_->setParameter("streakLenMax",  weather_ref::kStreakLenMax);
    }

    // The streak volume is placed purely by the `cameraPos` uniform above: the
    // vertex shader writes each streak's ABSOLUTE world position as
    // `cameraPos + offset` (mirrors weather_shader_ref::streak_world_position),
    // so the geometry already surrounds the camera at every world position.
    // The rain entity therefore keeps its IDENTITY transform and is never
    // re-centered per frame — exactly like the splash path, which also relies on
    // the `cameraPos` uniform alone. Re-centering the entity transform here too
    // would double-count `cameraPos` and drift the volume away from the camera
    // as it flies from the origin. This stays a uniform-only update — never a
    // rebuild or a transform move.

    // ── Per-frame splash uniforms + depth binding ─────────────
    // The splash renderable is only present when build_splashes succeeded;
    // guard on splash_mi_ so a splash-only asset failure (rain otherwise
    // available) skips this work gracefully (shared available_).

    // The splash pass is a SCREEN-SPACE fullscreen quad (Breath-of-the-Wild
    // rain): the fragment shader samples the shared depth pre-pass per pixel,
    // reconstructs world position + normal, ground-tests it, and tiles ripples
    // in world XY. This mirrors the godrays / volfog passes' depth binding and
    // per-frame quad placement, but the world reconstruction now uses Filament's
    // built-in getWorldFromClipMatrix() inside the shader (TRUE Z-up world),
    // instead of a CPU-built inverseViewProjection — see heaven_splash.mat.
    if (splash_mi_) {
        // Bind the shared depth pre-pass texture once it exists / on resize.
        // update() runs before the render passes (outside any beginRenderPass),
        // so this descriptor-set update never happens mid-render-pass — Vulkan
        // forbids that (same discipline as god rays / volumetric fog).
        if (frame.depth_texture && frame.depth_texture != bound_depth_) {
            filament::TextureSampler depth_sampler(
                filament::TextureSampler::MinFilter::NEAREST,
                filament::TextureSampler::MagFilter::NEAREST,
                filament::TextureSampler::WrapMode::CLAMP_TO_EDGE);
            splash_mi_->setParameter("depthTexture", frame.depth_texture, depth_sampler);
            bound_depth_ = frame.depth_texture;
        }

        // Camera view matrix — still needed below to place the fullscreen quad
        // in front of the camera. The world-position reconstruction itself no
        // longer needs a CPU-built matrix: the splash fragment shader uses
        // Filament's built-in getWorldFromClipMatrix(), which is guaranteed
        // consistent with how the scene + depth pre-pass were rendered (same
        // reversed-Z + clip convention) and yields TRUE Z-up world space — so
        // the old `inverseViewProjection` uniform (and its glm::perspective +
        // inverse construction, plus the `cameraPos` uniform) are gone.
        const float aspect = (frame.viewport_h > 0)
            ? static_cast<float>(frame.viewport_w) / static_cast<float>(frame.viewport_h)
            : 1.0f;
        const glm::mat4 view_mat = frame.camera
            ? frame.camera->view_matrix() : glm::mat4(1.0f);

        // time is the frame-rate-independent getUserTime() seconds. The
        // fragment shader retimes each world-XY ripple cell from the SHARED
        // lattice + fall law (drop_impact_time): a ripple animates over
        // [impactT, impactT + rippleDuration <= 2.0) where impactT is when the
        // drop that lands in that cell reaches the reconstructed ground height.
        // At intensity 0 the shader early-outs so nothing draws; the
        // active cell fraction scales with intensity so ripple density tracks
        // intensity.
        splash_mi_->setParameter("time",            frame.time);
        splash_mi_->setParameter("splashIntensity", intensity);
        // ABSOLUTE camera world position (Issue 1): the splash fragment shader
        // tiles ripples in absolute world XY (worldAbs = worldPos + cameraPos)
        // so the cell grid is world-locked and ripples stop sliding with the
        // camera. `cam` is frame.camera->position (engine Z-up world) — the same
        // value the streak path pushes as its cameraPos and uses successfully.
        splash_mi_->setParameter("cameraPos",
            filament::math::float3{cam.x, cam.y, cam.z});
        // Shared fall-law uniforms (rain/splash UNIFICATION). Identical to the
        // streak uniforms above so a ripple blooms exactly where + when a streak
        // lands: the splash derives the same lattice cell + fall law from the
        // reconstructed ground pixel that the streak shader does from its seed.
        splash_mi_->setParameter("volumeExtent", weather_ref::kVolumeExtent);
        splash_mi_->setParameter(
            "fallSpeed", render_tuning::rain.fall_speed);

        // Position the fullscreen quad just inside the far plane in front of the
        // camera so every screen pixel runs the fragment shader (same trick as
        // god rays / volfog). The fragment shader uses the depth buffer for the
        // actual world reconstruction; this placement only guarantees coverage.
        // Re-positioning the quad each frame is NOT a geometry rebuild — it is
        // the same allowed per-frame placement the godrays / volfog passes do.
        glm::vec3 cam_pos   = frame.camera ? frame.camera->position : glm::vec3(0.0f);
        glm::vec3 cam_fwd   = -glm::vec3(view_mat[0][2], view_mat[1][2], view_mat[2][2]);
        glm::vec3 cam_right =  glm::vec3(view_mat[0][0], view_mat[1][0], view_mat[2][0]);
        glm::vec3 cam_up    =  glm::vec3(view_mat[0][1], view_mat[1][1], view_mat[2][1]);

        float dist = render_tuning::fullscreen.splash_distance;
        float half_h = dist * std::tan(glm::radians(
            (frame.camera ? frame.camera->fov : Camera{}.fov) * 0.5f));
        float half_w = half_h * aspect;
        glm::vec3 center = cam_pos + cam_fwd * dist;

        glm::mat4 quad_xform(1.0f);
        quad_xform[0] = glm::vec4(cam_right * half_w, 0.0f);
        quad_xform[1] = glm::vec4(cam_up * half_h, 0.0f);
        quad_xform[2] = glm::vec4(cam_fwd, 0.0f);
        quad_xform[3] = glm::vec4(center, 1.0f);

        auto& tcm = ctx.engine->getTransformManager();
        auto ti = tcm.getInstance(splash_entity_);
        if (ti) {
            tcm.setTransform(ti, glm_to_filament(quad_xform));
        }
    }
}

void RainFeature::on_depth_prepass_begin(RenderContext& ctx) {
    // Pull BOTH quads out of the scene before the shared depth pre-pass
    // renders. The splash material samples that pass's own depth texture
    // (FrameContext::depth_texture); if the splash renderable were still
    // in the scene during the depth render, Filament would bind a
    // descriptor set whose sampler points at the depth attachment it is
    // writing this pass — a Vulkan read/write feedback hazard that warns
    // ("descriptor set... was set between begin/endRenderPass") and
    // segfaults the optimized build. The streak quad is pulled for the
    // same reason god rays / volfog pull theirs: it is a transparent
    // overlay that must not pollute the depth render. Mirrors
    // GodraysFeature / VolfogFeature exactly.
    if (!in_scene_) return;
    if (rain_entity_)   ctx.scene->remove(rain_entity_);
    if (splash_entity_) ctx.scene->remove(splash_entity_);
}

void RainFeature::on_depth_prepass_end(RenderContext& ctx) {
    // Re-add both quads before the main view renders. The depth-texture
    // binding on splash_mi_ was set in update() OUTSIDE any render pass,
    // so re-adding the renderables here touches no material parameter.
    if (!in_scene_) return;
    if (rain_entity_)   ctx.scene->addEntity(rain_entity_);
    if (splash_entity_) ctx.scene->addEntity(splash_entity_);
}

void RainFeature::on_enabled_changed(RenderContext& ctx, bool on) {
    // Add/remove ONLY this feature's renderables. Before
    // the build-once geometry exists, this is a no-op — build_rain /
    // build_splashes honour enabled_ when they run,
    // mirroring the ParticleFeature precedent.
    if (!built_) return;
    if (on && !in_scene_) {
        if (rain_entity_)   ctx.scene->addEntity(rain_entity_);
        if (splash_entity_) ctx.scene->addEntity(splash_entity_);
        in_scene_ = true;
    } else if (!on && in_scene_) {
        if (rain_entity_)   ctx.scene->remove(rain_entity_);
        if (splash_entity_) ctx.scene->remove(splash_entity_);
        in_scene_ = false;
    }
}

void RainFeature::destroy(RenderContext& ctx) {
    auto* engine_ = ctx.engine;

    // Entities (removed from the scene first, then destroyed). Both are
    // null until build_rain / build_splashes run.
    if (rain_entity_) {
        if (in_scene_) ctx.scene->remove(rain_entity_);
        engine_->destroy(rain_entity_);
        utils::EntityManager::get().destroy(rain_entity_);
        rain_entity_ = {};
    }
    if (splash_entity_) {
        if (in_scene_) ctx.scene->remove(splash_entity_);
        engine_->destroy(splash_entity_);
        utils::EntityManager::get().destroy(splash_entity_);
        splash_entity_ = {};
    }
    in_scene_ = false;

    // Material instances, vertex/index buffers, base materials.
    if (rain_mi_)   { engine_->destroy(rain_mi_);   rain_mi_ = nullptr; }
    if (splash_mi_) { engine_->destroy(splash_mi_); splash_mi_ = nullptr; }
    if (rain_vb_)   { engine_->destroy(rain_vb_);   rain_vb_ = nullptr; }
    if (rain_ib_)   { engine_->destroy(rain_ib_);   rain_ib_ = nullptr; }
    if (splash_vb_) { engine_->destroy(splash_vb_); splash_vb_ = nullptr; }
    if (splash_ib_) { engine_->destroy(splash_ib_); splash_ib_ = nullptr; }
    if (rain_material_)   { engine_->destroy(rain_material_);   rain_material_ = nullptr; }
    if (splash_material_) { engine_->destroy(splash_material_); splash_material_ = nullptr; }

    // drop_tex_ / splash_tex_ are owned by the resource cache.
    drop_tex_    = nullptr;
    splash_tex_  = nullptr;
    bound_depth_ = nullptr;
    built_       = false;
}

} // namespace heaven
