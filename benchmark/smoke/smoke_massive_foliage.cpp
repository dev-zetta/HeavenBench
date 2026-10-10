/*
 * HeavenBench — massive-foliage build-once / build-failure-fallback /
 *                     disabled-parity smoke case (renderer-linked, GATED)
 *
 * (and  for
 * the registration cardinality, though the primary reqs are 8.4/8.5/8.7): the
 * MassiveFoliageFeature  is a build-once geometry feature modeled on
 * GrassFeature. It is registered EXACTLY ONCE in init_features, is
 * DISABLED by default so a flagless run is Shipping_Look-equivalent grass +
 * ground detail, builds its foliage geometry ONCE on the first active
 * frame and never regenerates it on subsequent frames (surfaced through
 * generation_count() / debug_build_counters().foliage_generations), and on a
 * build failure falls back to the Shipping_Look grass baseline with a
 * diagnostic and continues (via the protected can_build_foliage seam).
 *
 * This case combines two complementary harness patterns:
 *
 *   HALF A — against the LIVE backend (mirrors smoke_volumetric_clouds.cpp /
 *   smoke_ssgi.cpp): the GraphicsApi::Filament guard + static_cast<
 *   FilamentBackend&>, the init_pipelines SKIP, and the debug_count_features /
 *   debug_find_feature cardinality + enabled-default assertions, plus the
 *   debug_build_counters().foliage_generations discipline check.
 *     (1) REGISTRATION: debug_count_features("massive-foliage") == 1 with
 *         debug_find_feature non-null — registered exactly once.
 *     (2) DISABLED PARITY: the feature is disabled by default
 *         (enabled() == false). Rendering several empty-scene frames leaves
 *         debug_build_counters().foliage_generations at 0 — the disabled
 *         feature generates NO foliage geometry, so it adds none of its
 *         renderables and the grass + ground detail stay Shipping_Look-
 *         equivalent. The genuine ≤1-of-255 pixel parity at each benchmark
 *         camera requires separate full-scene reference captures; this case
 *         asserts the observable "builds nothing while disabled" precondition.
 *
 *   NOTE ON THE DEFERRED BUILD: MassiveFoliageFeature's build is DEFERRED to
 *   the first update() with non-empty instance/mesh data (it needs the scene
 *   instance transforms to place foliage on the floating islands, the
 *   GrassFeature model). The bare 64x64 harness renders an EMPTY scene, so the
 *   registered feature never reaches its build on the live backend — which is
 *   exactly why the live half asserts the disabled-state "foliage_generations
 *   stays 0" contract and the build-once / fallback behaviours are exercised
 *   HERMETICALLY below, where a synthetic non-empty frame can drive generate()
 *   deterministically. The production scene path exercises the registered
 *   feature's build-once via debug_build_counters().foliage_generations.
 *
 *   HALF B — HERMETIC (mirrors smoke_wet_reflections_capability.cpp /
 *   smoke_glass_refraction.cpp): a stand-alone NOOP Filament engine + scene +
 *   TextureStore, isolated from the shared backend. A MassiveFoliageFeature is
 *   init'd against it with a bogus data_dir (so generate_grass_roots / load_mi08
 *   fail fast and produce no patches — the embedded grass material still builds
 *   from k_heaven_grass_filamat, independent of data_dir) and DISABLED (so the
 *   build adds nothing to the NOOP scene). A synthetic non-empty FrameContext
 *   then drives the deferred build:
 *     (3) BUILD-ONCE: the first active frame runs generate() exactly once
 *         (generation_count() reaches 1); subsequent frames take the per-frame
 *         path and never regenerate (generation_count() stays 1) — the
 *         GrassFeature build-once discipline, the foliage analogue of
 *         smoke_rain_build_once.cpp's debug_build_counters assertion.
 *     (4) BUILD-FAILURE FALLBACK: a ForcedBuildFailure subclass overrides
 *         the protected can_build_foliage() seam to return false (the
 *         WetReflectionsFeature::probe_capability pattern). Driving an active
 *         frame, the case asserts the seam WAS consulted (proving it is wired
 *         into the build path) and returned false, yet the feature still
 *         completed a single generation (generation_count() == 1) and stayed
 *         usable (a second active frame does not rebuild) — i.e. build_foliage()
 *         returned the error and generate() fell back to the Shipping_Look grass
 *         baseline build (emitting the diagnostic, which the harness does
 *         not capture — covered by code inspection) rather than aborting. A
 *         default-seam control proves can_build_foliage() returns true on the
 *         success path, so the fallback is a genuine branch difference.
 *
 *   HERMETIC RATIONALE: MassiveFoliageFeature builds real filament::Material /
 *   MaterialInstance objects from the embedded compiled package and resolves
 *   textures through a real TextureStore. The headless RapidCheck target
 *   (heaven_benchmark_tests) links NEITHER Filament NOR SDL, so it cannot build
 *   a Material; this renderer-linked target can — matching the gated GPU
 *   smoke-test strategy. NOOP needs no GPU/display and isolates the assertion
 *   from the shared backend.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines,
 * or the NOOP engine / embedded grass material) fails, tolerating a
 * headless/driver-less environment that passed the gate.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "rhi/render_feature.h"
#include "rhi/features/massive_foliage_feature.h"
#include "rhi/features/grass_feature.h"
#include "rhi/render_test_scene_assets.h"
#include "rhi/texture_store.h"
#include "engine/types.h"
#include "engine/camera.h"
#include "engine/path_resolver.h"
#include "engine/feature_assets.h"
#include "rhi/filament_util.h"

#include "rhi/materials_data.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/Material.h>
#include <filament/TransformManager.h>
#include <filament/Camera.h>
#include <filament/Renderer.h>
#include <filament/SwapChain.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/Platform.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_massive_foliage]";

constexpr const char* kFeature = "massive-foliage";

// Test-only subclass that forces the build-failure seam to report the
// configured blade count as UN-buildable, regardless of the real (always
// ample) GPU headroom. This is the whole point of the protected virtual seam:
// it lets the fallback-to-baseline branch be exercised deterministically
// without a GPU dependency — exactly the WetReflectionsFeature::probe_capability
// pattern. It also RECORDS that the seam was consulted (and the requested
// count) so the case can prove the seam is genuinely wired into the build path.
class ForcedBuildFailureFoliage final : public MassiveFoliageFeature {
public:
    mutable bool seam_called = false;
    mutable int  last_requested = -1;
protected:
    bool can_build_foliage(int requested_blade_count) const override {
        seam_called    = true;
        last_requested = requested_blade_count;
        return false; // force the build to fail ⇒ fallback to baseline
    }
};

// A subclass that records the seam result on the SUCCESS path (default
// behaviour) so the case can prove the fallback above is a real branch
// difference: the production seam returns true for a positive blade count.
class ObservingFoliage final : public MassiveFoliageFeature {
public:
    mutable bool seam_called = false;
    mutable bool seam_result = false;
protected:
    bool can_build_foliage(int requested_blade_count) const override {
        seam_called = true;
        seam_result = MassiveFoliageFeature::can_build_foliage(requested_blade_count);
        return seam_result;
    }
};

// Minimal IResourceProvider backed by a TextureStore — the narrow slice the
// feature's texture binding needs. Missing DDS files resolve to the shared 1x1
// placeholders (the feature falls back to them for every unassigned map), so a
// bogus data_dir produces valid placeholder bindings rather than failures.
class StoreResourceProvider final : public IResourceProvider {
public:
    explicit StoreResourceProvider(TextureStore& store) : store_(store) {}
    uint64_t load_texture(
        const std::string& path, TextureSemantic semantic) override {
        return store_.load(path, semantic);
    }
    filament::Texture* get_filament_texture(uint64_t handle) override { return store_.get(handle); }
    filament::Texture* placeholder_texture() override { return store_.placeholder(); }
    filament::Texture* placeholder_normal_texture() override { return store_.placeholder_normal(); }
    filament::Texture* placeholder_depth_texture() override { return store_.placeholder_depth(); }
private:
    TextureStore& store_;
};

// Render exactly one frame with an EMPTY scene against the live backend. With
// no instances/meshes the registered (disabled) foliage feature never reaches
// its deferred build — exactly the disabled-state precondition Half A asserts.
void render_one_empty_frame(IRhiBackend& backend, const Camera& camera,
                            const LightingParams& lighting,
                            const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    WeatherState weather;
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

// Build a synthetic non-empty FrameContext that drives the feature's deferred
// build. The single instance + mesh make frame.instances/meshes non-empty (the
// only precondition update() checks before calling generate()); the surface
// name deliberately does NOT contain any island id marker, so even if the bogus
// data_dir yielded roots no patch would match — keeping the build side-effect
// free while still exercising the full generate()/build_foliage() path.
struct SyntheticFrame {
    std::vector<Instance> instances;
    std::vector<GpuMesh>  meshes;
    LightingParams        lighting;

    SyntheticFrame() {
        GpuMesh m;
        m.name = "synthetic_terrain";
        m.path = "synthetic";
        GpuSurface s;
        s.name = "synthetic_ground";
        m.surfaces.push_back(s);
        meshes.push_back(std::move(m));
        instances.push_back(Instance{0, glm::mat4(1.0f)});
    }

    FrameContext make(float time) const {
        FrameContext f;
        f.lighting   = &lighting;
        f.time       = time;
        f.viewport_w = 64;
        f.viewport_h = 64;
        f.instances  = std::span<const Instance>(instances.data(), instances.size());
        f.meshes     = std::span<const GpuMesh>(meshes.data(), meshes.size());
        return f;
    }
};

// Run a foliage feature through one deferred build + several per-frame updates
// on a hermetic NOOP engine, returning the generation_count() trajectory. The
// feature is init'd DISABLED with a bogus data_dir so it adds nothing to the
// scene and loads no real assets, while still running generate() exactly once.
struct BuildTrajectory {
    uint64_t after_frame_1 = 0;
    uint64_t final_count   = 0;
};

template <typename FeatureT>
BuildTrajectory drive_deferred_build(FeatureT& feature, RenderContext& rctx,
                                     const SyntheticFrame& sf, int frames) {
    AppConfig cfg;
    cfg.data_dir = "/heaven-smoke-nonexistent"; // assets fail fast ⇒ no patches
    cfg.features.massive_foliage = false;        // disabled ⇒ no scene mutation
    feature.init(rctx, cfg);

    BuildTrajectory t;
    // Frame 1: the deferred build runs here (generate() increments the counter
    // once, then build_foliage()/fallback runs producing no patches).
    {
        FrameContext f = sf.make(0.0f);
        feature.update(rctx, f);
    }
    t.after_frame_1 = feature.generation_count();

    // Frames 2..N: the pure per-frame path. generate() must NOT run again.
    for (int i = 1; i < frames; ++i) {
        FrameContext f = sf.make(static_cast<float>(i) * 0.016f);
        feature.update(rctx, f);
    }
    t.final_count = feature.generation_count();
    return t;
}

// A real upward-facing triangle exercises the production mi08 loader and root
// sampler. The three source meshes differ only in their authored surface name.
std::vector<uint8_t> foliage_source_mesh(std::string_view surface) {
    std::vector<uint8_t> bytes;
    const auto u16 = [&](uint16_t value) {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    };
    const auto u32 = [&](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>(value >> shift));
    };
    const auto f32 = [&](float value) { u32(std::bit_cast<uint32_t>(value)); };
    bytes.insert(bytes.end(), {'m', 'i', '0', '8'});
    for (const float value : {-1.0f, -1.0f, -1.0f, 2.0f, 2.0f,
                              1.0f, 0.0f, 0.0f, 0.0f, 3.0f}) f32(value);
    u32(1);
    u32(static_cast<uint32_t>(surface.size() + 1));
    bytes.insert(bytes.end(), surface.begin(), surface.end());
    bytes.push_back(0);
    bytes.insert(bytes.end(), 40, 0);
    u32(3);
    for (const glm::vec3 position : {glm::vec3(0, 0, 0),
                                    glm::vec3(2, 0, 0), glm::vec3(0, 2, 0)}) {
        f32(position.x); f32(position.y); f32(position.z);
        u16(32767); u16(32767); u16(65535);
    }
    u32(3);
    for (int i = 0; i < 6; ++i) f32(0.0f);
    u32(0);
    u32(1);
    u16(0); u16(1); u16(2);
    return bytes;
}

bool verify_shared_foliage_geometry(RenderContext& context) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        std::format("heaven-foliage-sharing-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        fs::path root;
        ~Cleanup() { std::error_code error; fs::remove_all(root, error); }
    } cleanup{root};

    constexpr std::array sources{FeatureAsset::GrassIsland1Mesh,
                                 FeatureAsset::GrassIsland2Mesh,
                                 FeatureAsset::GrassIsland3Mesh};
    SyntheticFrame sf;
    sf.instances.clear();
    sf.meshes.clear();
    for (size_t index = 0; index < sources.size(); ++index) {
        const fs::path path = root / feature_asset_info(sources[index]).relative_path;
        fs::create_directories(path.parent_path());
        const auto bytes = foliage_source_mesh(index == 0 ? "grass" : "ground");
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.close();
        if (!stream) return false;

        GpuMesh mesh;
        GpuSurface surface;
        surface.name = std::format("island{}_fixture", index + 1);
        mesh.surfaces.push_back(std::move(surface));
        sf.meshes.push_back(std::move(mesh));
        for (int copy = 0; copy < 2; ++copy) {
            glm::mat4 transform(1.0f);
            transform[3].x = static_cast<float>(sf.instances.size()) * 2.0f;
            sf.instances.push_back(Instance{static_cast<int>(index), transform});
        }
    }

    auto paths = PathResolver::create(root.string());
    if (!paths) return false;
    RenderContext rctx = context;
    rctx.paths = std::cref(*paths);
    AppConfig config;
    config.data_dir = root.string();
    config.features.massive_foliage = true;
    config.quality.foliage_blade_baseline = 20000;
    config.quality.foliage_blade_max = 20000;
    config.quality.foliage_density = 1.0f;
    Camera camera;
    camera.position = glm::vec3(0.0f);
    FrameContext frame = sf.make(0.0f);
    frame.camera = &camera;

    auto& engine = *rctx.engine;
    const size_t vertex_buffers_before = engine.getVertexBufferCount();
    const size_t index_buffers_before = engine.getIndexBufferCount();
    const size_t entities_before = rctx.scene->getEntityCount();
    filament::Scene* depth_scene = engine.createScene();
    MassiveFoliageFeature feature;
    feature.init(rctx, config);
    feature.update(rctx, frame);

    // 20000 roots become 420000 vertices, plus forty crossed flower cards
    // (480 vertices). The largest source splits at 8192 complete blades.
    // Seven source ranges across two placements borrow six buffer pairs;
    // every placement retains all 420480 vertices despite the new draw split.
    constexpr size_t uploaded_vertices = 420480;
    constexpr size_t range_patches = 14;
    const auto allocations_match = [&](size_t visible_patches) {
        const auto stats = feature.geometry_stats();
        return stats.geometry_buffers == 6 && stats.geometry_ranges == 7 &&
               stats.largest_range_vertices == size_t{8192} * GRASS_VERTS_PER_BLADE &&
               stats.patches == range_patches &&
               stats.uploaded_vertices == uploaded_vertices &&
               stats.instanced_vertices == uploaded_vertices * 2 &&
               stats.shared_bindings_valid && feature.generation_count() == 1 &&
               engine.getVertexBufferCount() == vertex_buffers_before + 6 &&
               engine.getIndexBufferCount() == index_buffers_before + 6 &&
               rctx.scene->getEntityCount() == entities_before + visible_patches;
    };
    bool ok = allocations_match(range_patches);
    if (!ok) std::printf("%s FAIL: foliage source/category buffers were duplicated or geometry changed\n", kTag);

    // Adding all depth proxies must reuse the existing buffers. No geometry
    // allocation is allowed when the depth pass appears on a later frame.
    frame.depth_proxy_scene = depth_scene;
    feature.update(rctx, frame);
    if (!allocations_match(range_patches) || depth_scene->getRenderableCount() != range_patches) {
        std::printf("%s FAIL: depth proxies did not reuse all visible foliage buffers\n", kTag);
        ok = false;
    }

    // Each placement has a grass and flower entity in both scenes. Moving an
    // instance must update those transforms without changing shared ownership.
    const auto transforms_match = [&](const filament::Scene& scene) {
        std::array<size_t, 6> matches{};
        bool all_match = true;
        auto& transforms = engine.getTransformManager();
        scene.forEach([&](utils::Entity entity) {
            const auto transform = transforms.getInstance(entity);
            bool found = false;
            if (transform) {
                for (size_t i = 0; i < sf.instances.size(); ++i) {
                    if (transforms.getTransform(transform) ==
                        glm_to_filament(sf.instances[i].transform)) {
                        ++matches[i];
                        found = true;
                    }
                }
            }
            all_match = all_match && found;
        });
        for (size_t i = 0; i < matches.size(); ++i)
            all_match = all_match && matches[i] == (i < 2 ? size_t{3} : size_t{2});
        return all_match;
    };
    sf.instances[1].transform[3].z = 3.0f;
    for (int tick = 1; tick <= 3; ++tick) {
        frame.time = static_cast<float>(tick) * 0.016f;
        feature.update(rctx, frame);
    }
    if (!allocations_match(range_patches) || !transforms_match(*rctx.scene) ||
        !transforms_match(*depth_scene)) {
        std::printf("%s FAIL: foliage sharing lost placement transforms or rebuilt geometry\n", kTag);
        ok = false;
    }

    // LOD and explicit visibility toggles must agree for both scenes, without
    // bringing distant patches back or leaving invisible depth occluders.
    const auto visibility_matches = [&](size_t visible_patches) {
        return allocations_match(visible_patches) &&
               depth_scene->getRenderableCount() == visible_patches;
    };
    camera.position = glm::vec3(10000.0f);
    feature.update(rctx, frame);
    bool visibility_ok = visibility_matches(0);
    feature.set_enabled(rctx, false);
    visibility_ok = visibility_matches(0) && visibility_ok;
    feature.set_enabled(rctx, true);
    visibility_ok = visibility_matches(0) && visibility_ok;
    camera.position = glm::vec3(0.0f);
    feature.update(rctx, frame);
    visibility_ok = visibility_matches(range_patches) && visibility_ok;
    feature.set_enabled(rctx, false);
    visibility_ok = visibility_matches(0) && visibility_ok;
    feature.set_enabled(rctx, true);
    visibility_ok = visibility_matches(range_patches) && visibility_ok;
    if (!visibility_ok) {
        std::printf("%s FAIL: foliage LOD/toggles disagreed with depth visibility or rebuilt geometry\n", kTag);
        ok = false;
    }

    feature.destroy(rctx);
    feature.destroy(rctx);
    const auto destroyed = feature.geometry_stats();
    if (engine.getVertexBufferCount() != vertex_buffers_before ||
        engine.getIndexBufferCount() != index_buffers_before ||
        rctx.scene->getEntityCount() != entities_before ||
        depth_scene->getEntityCount() != 0 || destroyed.geometry_buffers != 0 ||
        destroyed.geometry_ranges != 0 || destroyed.largest_range_vertices != 0 ||
        destroyed.patches != 0 || destroyed.uploaded_vertices != 0 ||
        destroyed.instanced_vertices != 0 || !destroyed.shared_bindings_valid) {
        std::printf("%s FAIL: foliage teardown did not release shared geometry exactly once\n", kTag);
        ok = false;
    }
    engine.destroy(depth_scene);
    return ok;
}

template<class Feature>
bool verify_native_grass_range_culling(RenderContext context, const char* label) {
    struct Fixture {
        RenderContext context;
        Feature feature;
        std::unique_ptr<RenderTestSceneAssets> assets;
        filament::Scene* depth_scene = nullptr;
        filament::SwapChain* swapchain = nullptr;
        utils::Entity camera_entity;
        std::shared_ptr<std::atomic_uint64_t> commands =
            std::make_shared<std::atomic_uint64_t>(0);
        ~Fixture() {
            auto& engine = *context.engine;
            engine.flushAndWait();
            if (auto* platform = engine.getPlatform())
                platform->setDebugUpdateStatFunc(filament::backend::Platform::DebugUpdateStatFunc{});
            feature.destroy(context);
            if (context.view) {
                context.view->setScene(nullptr);
                context.view->setCamera(nullptr);
            }
            engine.destroy(context.view);
            engine.destroy(context.scene);
            engine.destroy(depth_scene);
            if (camera_entity) {
                engine.destroyCameraComponent(camera_entity);
                utils::EntityManager::get().destroy(camera_entity);
            }
            engine.destroy(context.renderer);
            engine.destroy(swapchain);
        }
    } fixture;
    fixture.context = context;
    // Never inherit the application's borrowers into this isolated fixture.
    fixture.context.scene = nullptr;
    fixture.context.view = nullptr;
    fixture.context.renderer = nullptr;
    fixture.context.camera = nullptr;
    auto& engine = *context.engine;
    try {
        auto assets = RenderTestSceneAssets::create(engine);
        if (!assets) throw std::runtime_error(assets.error().message);
        fixture.assets = std::move(*assets);
        constexpr std::size_t blades = 16384;
        if (!fixture.assets->set_ground_patch(0, {-100, -1}, {100, 1}, 0, blades, 0))
            throw std::runtime_error("native grass roots could not be built");
        auto& isolated = fixture.context;
        isolated.test_scene_assets = fixture.assets.get();
        isolated.scene = engine.createScene();
        fixture.depth_scene = engine.createScene();
        isolated.view = engine.createView();
        isolated.renderer = engine.createRenderer();
        fixture.swapchain = engine.createSwapChain(64, 64);
        fixture.camera_entity = utils::EntityManager::get().create();
        isolated.camera = engine.createCamera(fixture.camera_entity);
        if (!isolated.scene || !fixture.depth_scene || !isolated.view || !isolated.renderer ||
                !fixture.swapchain || !isolated.camera)
            throw std::runtime_error("native grass culling objects could not be built");
        isolated.renderer->setDisplayInfo({.refreshRate = 0});
        isolated.renderer->setFrameRateOptions({.interval = 0});
        isolated.camera->setProjection(filament::Camera::Projection::ORTHO, -20, 20, -5, 5, 0.1, 30);
        isolated.view->setScene(isolated.scene);
        isolated.view->setCamera(isolated.camera);
        isolated.view->setViewport({0, 0, 64, 64});
        isolated.view->setPostProcessingEnabled(false);
        isolated.view->setShadowingEnabled(false);
        isolated.view->setAntiAliasing(filament::AntiAliasing::NONE);
        isolated.view->setMultiSampleAntiAliasingOptions({.enabled = false});
        isolated.view->setAmbientOcclusionOptions({.enabled = false});
        auto* platform = engine.getPlatform();
        if (!platform) throw std::runtime_error("native grass platform unavailable");
        platform->setDebugUpdateStatFunc([counter = fixture.commands](const char* key,
                std::uint64_t value, const utils::CString&) {
            if (std::string_view(key) == "filament.renderer.render_pass.command_count")
                counter->fetch_add(value, std::memory_order_relaxed);
        });
        AppConfig config;
        config.features.grass = true;
        config.features.massive_foliage = true;
        fixture.feature.init(isolated, config);
        SyntheticFrame source;
        source.instances.resize(1);
        source.instances[0] = Instance{0, glm::mat4(1)};
        Camera camera;
        FrameContext frame = source.make(0);
        frame.camera = &camera;
        frame.depth_proxy_scene = fixture.depth_scene;
        fixture.feature.update(isolated, frame);
        if (isolated.scene->getRenderableCount() != 2 ||
                fixture.depth_scene->getRenderableCount() != 2)
            throw std::runtime_error("native grass did not create matching spatial ranges");
        const auto draw = [&] {
            if (!isolated.renderer->beginFrame(fixture.swapchain))
                throw std::runtime_error("native grass frame was rejected");
            isolated.renderer->render(isolated.view);
            isolated.renderer->endFrame();
            engine.flushAndWait();
        };
        const auto measure = [&](filament::Scene* scene, bool culling, double x) {
            isolated.view->setScene(scene);
            isolated.view->setFrustumCullingEnabled(culling);
            isolated.camera->lookAt({x, 0, 10}, {x, 0, 0}, {0, 1, 0});
            draw();
            fixture.commands->store(0, std::memory_order_relaxed);
            draw();
            return std::pair{isolated.view->getVisibleRenderableCount(),
                             fixture.commands->load(std::memory_order_relaxed)};
        };
        for (auto* scene : {isolated.scene, fixture.depth_scene}) {
            const auto all = measure(scene, false, 75);
            const auto half = measure(scene, true, 75);
            const auto away = measure(scene, true, 300);
            std::printf("%s %s %s owner_vertices=%zu all_ranges=%d / commands=%llu "
                        "visible_ranges=%d / draw_indices=%zu / commands=%llu "
                        "away_ranges=%d / commands=%llu\n", kTag, label,
                        scene == isolated.scene ? "color" : "depth",
                        blades * GRASS_VERTS_PER_BLADE, all.first,
                        static_cast<unsigned long long>(all.second), half.first,
                        blades / 2 * GRASS_VERTS_PER_BLADE,
                        static_cast<unsigned long long>(half.second), away.first,
                        static_cast<unsigned long long>(away.second));
            if (all.first != 2 || !all.second || half.first != 1 || !half.second ||
                    half.second >= all.second || away.first != 0 || away.second != 0)
                throw std::runtime_error("native grass did not cull off-camera draw ranges");
        }
        return fixture.feature.generation_count() == 1;
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s spatial culling: %s\n", kTag, label, error.what());
        return false;
    }
}

} // namespace

CaseResult run_massive_foliage_case(const CaseContext& ctx) {
    bool ok = true;

    // ════════════════════════════════════════════════════════════
    // HALF A — registration + disabled parity against the LIVE backend
    // ════════════════════════════════════════════════════════════

    // The debug_* accessors are FilamentBackend-specific test instrumentation.
    // The harness always constructs a FilamentBackend (it is the sole
    // IRhiBackend), so this is safe; api() lets us bail gracefully if that ever
    // changes rather than risking a bad cast — mirroring the sibling cases.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: case needs the Filament backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the render pipeline up so init_features() has run and the registered
    // MassiveFoliageFeature exists. A failure here means the environment can't
    // drive the backend (no usable GPU) — treat it as a SKIP so the gated test
    // still passes. init_pipelines is idempotent.
    if (auto r = backend.init_pipelines(ctx.config); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    // (1) Registration  — asset-independent. Existence first (clear
    // diagnostic), then cardinality (catches both missing and duplicate).
    const IRenderFeature* mf = backend.debug_find_feature(kFeature);
    if (!mf) {
        std::printf("%s FAIL: no feature named \"%s\" is registered — the "
                    "massive-foliage feature must be registered exactly once in "
                    "init_features (R9.4)\n", kTag, kFeature);
        return CaseResult::Fail;
    }
    const int count = backend.debug_count_features(kFeature);
    if (count != 1) {
        std::printf("%s FAIL: render_features_ contains %d features named "
                    "\"%s\" (expected exactly 1 — R9.4)\n", kTag, count, kFeature);
        ok = false;
    }

    // (2) Disabled parity: the feature ships ENABLED by default (the
    // full advanced-rendering look ships on; use --no-massive-foliage to
    // disable). The disabled-state Shipping_Look parity is still asserted below
    // by explicitly disabling it.
    if (!mf->enabled()) {
        std::printf("%s FAIL: \"%s\" is disabled by default — it must default "
                    "enabled (use --no-massive-foliage to disable)\n",
                    kTag, kFeature);
        ok = false;
    }

    // (2 cont.) With the feature explicitly DISABLED and an empty scene, render
    // several frames and confirm the disabled feature generates NO foliage
    // geometry: debug_build_counters().foliage_generations stays 0 across
    // frames. A disabled feature that builds nothing adds none of its
    // renderables — the observable precondition for Shipping_Look-equivalent
    // output (full-scene pixel parity requires separate accepted-reference
    // captures and comparison). This also exercises the build-once counter's wiring
    // through the backend (debug_build_counters().foliage_generations sources
    // the registered feature's generation_count()). Restored to its enabled
    // default afterward so subsequent cases see the default feature set.
    {
        backend.debug_set_feature_enabled(kFeature, false);

        Camera camera;
        LightingParams lighting = lighting_for_time_of_day(ctx.config.time_of_day);
        TonemapParams  tonemap;
        const int w = ctx.width;
        const int h = ctx.height;
        constexpr int kFrames = 4;

        for (int i = 0; i < kFrames; ++i)
            render_one_empty_frame(backend, camera, lighting, tonemap, w, h);

        const IRhiBackend::BackendBuildCounters c = backend.debug_build_counters();
        if (c.foliage_generations != 0) {
            std::printf("%s FAIL: debug_build_counters().foliage_generations is "
                        "%llu after %d empty-scene frames with \"%s\" disabled "
                        "(expected 0 — a disabled feature must build no foliage "
                        "so the grass + ground detail stay Shipping_Look-"
                        "equivalent, R8.5)\n", kTag,
                        static_cast<unsigned long long>(
                            c.foliage_generations),
                        kFrames, kFeature);
            ok = false;
        }

        // Restore the enabled default for subsequent cases.
        backend.debug_set_feature_enabled(kFeature, true);
    }

    // These are real native draw-command checks, separate from the NOOP
    // ownership/placement fixture below. Full-density buffers stay present;
    // only off-frustum color/depth ranges cease submitting draw commands.
    ok = verify_native_grass_range_culling<GrassFeature>(
             backend.debug_render_context(), "regular") && ok;
    ok = verify_native_grass_range_culling<MassiveFoliageFeature>(
             backend.debug_render_context(), "massive") && ok;

    // ════════════════════════════════════════════════════════════
    // HALF B — build-once + build-failure fallback (HERMETIC NOOP)
    // ════════════════════════════════════════════════════════════

    // Stand up an isolated NOOP engine + scene + TextureStore so driving a
    // synthetic non-empty frame (which reaches the deferred build) does not
    // perturb the shared backend.
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine "
                    "(Half A assertions already evaluated)\n", kTag);
        return ok ? CaseResult::Skip : CaseResult::Fail;
    }

    // Probe that the embedded grass material builds on this engine. The feature
    // dereferences its material instance on the build/fallback path, so if NOOP
    // cannot build it we SKIP Half B rather than risk a null deref — the gated
    // GPU/display environment always can.
    {
        const filament::Material* probe = filament::Material::Builder()
            .package(k_heaven_grass_filamat, k_heaven_grass_filamat_size)
            .build(*engine);
        if (!probe) {
            std::printf("%s SKIP: embedded grass material did not build on NOOP "
                        "(Half A assertions already evaluated)\n", kTag);
            filament::Engine::destroy(&engine);
            return ok ? CaseResult::Skip : CaseResult::Fail;
        }
        engine->destroy(probe);
    }

    // Scope every engine-owned user object (scene, TextureStore placeholders,
    // the feature GPU resources) inside this block so their destructors run
    // BEFORE filament::Engine::destroy() below — destroying a Filament object
    // after its engine is a double-free panic (the discipline the sibling
    // smoke_glass_refraction.cpp follows for its TextureStore).
    {
        filament::Scene* scene = engine->createScene();
        TextureStore textures(*engine);
        textures.create_placeholders();
        StoreResourceProvider resources(textures);

        RenderContext rctx{};
        rctx.engine    = engine;
        rctx.scene     = scene;
        rctx.resources = &resources;

        // The feature resolves its texture/mesh paths through the backend-owned
        // PathResolver borrowed via RenderContext::paths (the production backend
        // populates it in build_render_context()). This hermetic harness builds
        // its own RenderContext, so it must supply the resolver too. Build it
        // from the same bogus data_dir drive_deferred_build configures, so every
        // resolved asset path points at a nonexistent tree and the feature falls
        // back to the shared 1x1 placeholders (no real assets loaded), exactly
        // the intended hermetic behaviour. The non-empty literal never fails
        // construction, so the value is always present here; the
        // resolver outlives every rctx use in this scope.
        auto resolver = PathResolver::create("/heaven-smoke-nonexistent");
        if (resolver) rctx.paths = std::cref(resolver.value());

        const SyntheticFrame sf;
        constexpr int kBuildFrames = 5;

        // (3) BUILD-ONCE  — the success path. Default seam ⇒ generate()
        // runs exactly once on the first active frame and never again.
        {
            ObservingFoliage feature;
            const BuildTrajectory t =
                drive_deferred_build(feature, rctx, sf, kBuildFrames);

            if (t.after_frame_1 != 1 || t.final_count != 1) {
                std::printf("%s FAIL: generation_count() expected 1, got %llu "
                            "after frame 1, %llu final over %d frames — foliage "
                            "geometry must build once on the first active frame "
                            "and never regenerate (R8.4)\n", kTag,
                            static_cast<unsigned long long>(t.after_frame_1),
                            static_cast<unsigned long long>(t.final_count),
                            kBuildFrames);
                ok = false;
            }
            // Positive control: the production seam returns true for the scaled
            // blade count, proving the forced-false fallback below is a genuine
            // branch difference and not "the seam always fails".
            if (!feature.seam_called || !feature.seam_result) {
                std::printf("%s FAIL: on the success path can_build_foliage() "
                            "was not consulted or did not return true (called=%d "
                            "result=%d) — the build-failure fallback would be "
                            "vacuous\n", kTag,
                            static_cast<int>(feature.seam_called),
                            static_cast<int>(feature.seam_result));
                ok = false;
            }
            feature.destroy(rctx);
        }

        // (4) BUILD-FAILURE FALLBACK  — forced-false seam. generate()
        // still completes a single generation (the fallback-to-baseline build),
        // the seam was consulted and returned false, and a subsequent frame does
        // not rebuild.
        {
            ForcedBuildFailureFoliage feature;
            const BuildTrajectory t =
                drive_deferred_build(feature, rctx, sf, kBuildFrames);

            if (!feature.seam_called) {
                std::printf("%s FAIL: can_build_foliage() was never consulted on "
                            "the build path — the R8.7 build-failure seam is not "
                            "wired in (no fallback could ever trigger)\n", kTag);
                ok = false;
            }
            if (feature.last_requested <= 0) {
                std::printf("%s FAIL: can_build_foliage() was consulted with a "
                            "non-positive requested blade count (%d) — the scaled "
                            "blade count should drive the seam\n",
                            kTag, feature.last_requested);
                ok = false;
            }
            // Build attempted exactly once (generate() ran, fell back to the
            // baseline, and did not abort), and never regenerated thereafter.
            if (t.after_frame_1 != 1 || t.final_count != 1) {
                std::printf("%s FAIL: generation_count() expected 1 after a "
                            "forced build failure, got %llu after frame 1, %llu "
                            "final — the feature must fall back to the "
                            "Shipping_Look grass baseline and continue (R8.7), "
                            "building once\n", kTag,
                            static_cast<unsigned long long>(t.after_frame_1),
                            static_cast<unsigned long long>(t.final_count));
                ok = false;
            }
            feature.destroy(rctx);
        }

        if (!verify_shared_foliage_geometry(rctx)) ok = false;

        // Destroy the scene before the TextureStore destructor (block exit) and
        // before the engine teardown below.
        engine->destroy(scene);
    }

    // Tear down the hermetic engine now that every engine-owned user object has
    // been destroyed.
    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s massive-foliage contract honored: exactly one \"%s\" feature "
                "registered (R9.4), enabled by default with "
                "foliage_generations==0 across empty frames while explicitly "
                "disabled (Shipping_Look-equivalent grass + ground detail, R8.5); "
                "hermetic deferred build ran once (generation_count 1→1, R8.4); "
                "forced build-failure seam "
                "fell back to the grass baseline and continued, building once "
                "(R8.7). The genuine benchmark-camera pixel parity and the "
                "registered feature's production build-once still require "
                "separate full-scene captures and runtime verification\n", kTag, kFeature);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
