/* Shared backend reloads with real procedural geometry and feature borrowers. */
#include "smoke_cases.h"

#include "engine/camera.h"
#include "engine/render_test_scene.h"
#include "engine/render_test_scene_catalog.h"
#include "rhi/filament_backend.h"
#include "rhi/render_test_scene_assets.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <functional>
#include <optional>
#include <set>
#include <string_view>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_render_test_reload]";

// Call only after backend reload has destroyed the scene/depth borrowers.
// Assert the exact owner delta rather than a whole-engine baseline: resources
// belonging to the harness and freshly initialized features must stay alive.
[[nodiscard]] bool retire_scene_meshes(FilamentBackend& backend,
                                      std::optional<RenderTestScene>& scene) {
    if (!scene) return true;
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return false;
    std::set<std::uint64_t> handles;
    for (const auto& mesh : scene->scene.gpu_meshes)
        for (const auto& surface : mesh.surfaces)
            if (surface.handle) handles.insert(surface.handle);
    const auto vertices_before = engine->getVertexBufferCount();
    const auto indices_before = engine->getIndexBufferCount();
    for (const auto& mesh : scene->scene.gpu_meshes) backend.release_mesh(mesh);
    scene.reset();
    const auto vertices_after = engine->getVertexBufferCount();
    const auto indices_after = engine->getIndexBufferCount();
    const bool exact = vertices_before >= handles.size() && indices_before >= handles.size() &&
        vertices_after == vertices_before - handles.size() &&
        indices_after == indices_before - handles.size();
    if (!exact) {
        std::printf("%s FAIL: retiring %zu mesh surfaces changed VB %zu→%zu / IB %zu→%zu "
                    "instead of releasing only their owners\n", kTag, handles.size(),
                    vertices_before, vertices_after, indices_before, indices_after);
    }
    return exact;
}

// Declared after the generated asset owners so restoration always happens
// before their references expire, including when an upload or assertion fails.
class RestoreBackend {
public:
    RestoreBackend(FilamentBackend& backend, const AppConfig& config,
                   PathResolverRef paths, AssetFileSystemRef assets,
                   std::optional<RenderTestScene>& scene)
        : backend_(backend), config_(config), paths_(paths), assets_(assets), scene_(scene) {}

    ~RestoreBackend() { if (!attempted_) static_cast<void>(restore()); }

    [[nodiscard]] bool restore() noexcept {
        attempted_ = true;
        try {
            backend_.set_asset_context(paths_, assets_);
            const auto result = backend_.reconfigure_for_reload(config_);
            if (!result) {
                std::printf("%s FAIL: original configuration restore: %s\n",
                    kTag, result.error().message.c_str());
                return false;
            }
            // Successful teardown is the lifetime boundary. If restoration
            // failed, the local scene may still have borrowers and stays owned
            // by the backend until its final destruction.
            if (!retire_scene_meshes(backend_, scene_)) return false;
            auto& context = backend_.debug_render_context();
            if (context.engine) context.engine->flushAndWait();
            const auto same_reference = [](const auto& a, const auto& b) {
                return a.has_value() == b.has_value() &&
                    (!a || &a->get() == &b->get());
            };
            return same_reference(context.paths, paths_) &&
                   same_reference(context.assets, assets_) &&
                   (context.test_scene_assets != nullptr) ==
                       !config_.render_test_scene.empty();
        } catch (const std::exception& error) {
            std::printf("%s FAIL: restore exception: %s\n", kTag, error.what());
            return false;
        } catch (...) {
            std::printf("%s FAIL: unknown restore exception\n", kTag);
            return false;
        }
    }

private:
    FilamentBackend& backend_;
    const AppConfig& config_;
    PathResolverRef paths_;
    AssetFileSystemRef assets_;
    std::optional<RenderTestScene>& scene_;
    bool attempted_ = false;
};

[[nodiscard]] bool feature_enabled(const FilamentBackend& backend,
                                   const char* name) {
    const auto* feature = backend.debug_find_feature(name);
    return feature && feature->enabled();
}

struct ReloadScenario {
    std::string_view id;
    bool massive_foliage;
};
} // namespace

CaseResult run_render_test_reload_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (const auto initialized = backend.init_pipelines(ctx.config); !initialized) {
        std::printf("%s SKIP: pipeline initialization: %s\n", kTag,
                    initialized.error().message.c_str());
        return CaseResult::Skip;
    }
    const auto original_context = backend.debug_render_context();
    if (!original_context.engine || !original_context.view || ctx.width <= 0 ||
        ctx.height <= 0) {
        std::printf("%s FAIL: missing initialized context/native extent\n", kTag);
        return CaseResult::Fail;
    }

    std::optional<AssetFileSystem> assets;
    std::optional<PathResolver> paths;
    std::optional<RenderTestScene> scene;
    RestoreBackend restore(backend, ctx.config, original_context.paths,
                           original_context.assets, scene);
    bool ok = true;
    unsigned presented_total = 0;
    try {
        constexpr std::array scenarios{
            ReloadScenario{"foliage", false}, ReloadScenario{"foliage", true},
            ReloadScenario{"particles", false}, ReloadScenario{"atmosphere", false}};
        bool previous_haze = false;
        for (const auto& scenario : scenarios) {
            AppConfig config = ctx.config;
            if (!apply_render_test_scene_defaults(config, scenario.id)) {
                ok = false;
                break;
            }
            if (scenario.id == "foliage")
                config.features.massive_foliage = scenario.massive_foliage;

            auto definition = make_render_test_scene(scenario.id);
            if (!definition) {
                std::printf("%s FAIL: scene generation: %s\n", kTag,
                            definition.error().message.c_str());
                ok = false;
                break;
            }
            auto generated = AssetFileSystem::create_memory("render-test-reload");
            auto generated_paths = PathResolver::create("render-test-reload");
            if (!generated || !generated_paths) {
                std::printf("%s FAIL: cannot create generated-only asset namespace\n", kTag);
                ok = false;
                break;
            }
            if (const auto mounted = mount_render_test_scene_assets(*definition, *generated);
                    !mounted) {
                std::printf("%s FAIL: DDS mount: %s\n", kTag,
                            mounted.error().message.c_str());
                ok = false;
                break;
            }
            // Keep the previous namespace alive through borrower retirement.
            // Repeated DDS names are mounted only in the fresh namespace; the
            // AssetFileSystem duplicate-mount rejection remains unchanged.
            if (const auto reloaded = backend.reconfigure_for_reload(config); !reloaded) {
                std::printf("%s FAIL: procedural reload: %s\n", kTag,
                            reloaded.error().message.c_str());
                ok = false;
                break;
            }
            if (!retire_scene_meshes(backend, scene)) {
                ok = false;
                break;
            }
            assets.emplace(std::move(*generated));
            paths.emplace(std::move(*generated_paths));
            backend.set_asset_context(std::cref(*paths), std::cref(*assets));
            auto& render_context = backend.debug_render_context();
            // The View survives reload, so its procedural bloom sampler and
            // the previous heat-haze capture target must be retired explicitly.
            if (render_context.engine != original_context.engine ||
                render_context.view != original_context.view ||
                !render_context.test_scene_assets ||
                render_context.view->getBloomOptions().dirt != nullptr ||
                (previous_haze && !render_context.output_target &&
                    render_context.view->getRenderTarget() != nullptr)) {
                std::printf("%s FAIL: reload retained a retired View borrower\n", kTag);
                ok = false;
                break;
            }
            auto uploaded = upload_render_test_scene(std::move(*definition), backend, config);
            if (!uploaded) {
                std::printf("%s FAIL: procedural upload: %s\n", kTag,
                            uploaded.error().message.c_str());
                ok = false;
                break;
            }
            scene.emplace(std::move(*uploaded));
            backend.set_particle_emitters(scene->scene.particle_emitters);
            backend.set_scene_lights(scene->scene.lights);
            if (scene->scene.instances.empty() || scene->scene.gpu_meshes.empty() ||
                scene->materials.empty() ||
                (scenario.id == "particles" && scene->scene.particle_emitters.empty()) ||
                ((scenario.id == "particles" || scenario.id == "atmosphere") &&
                    scene->scene.lights.empty())) {
                std::printf("%s FAIL: generated scene lacks actual feature inputs\n", kTag);
                ok = false;
                break;
            }

            const auto catalog = render_test_scene_catalog();
            const auto info = std::ranges::find(catalog, scenario.id, &RenderTestSceneInfo::id);
            Camera camera;
            camera.position = {info->camera_pose[0], info->camera_pose[1], info->camera_pose[2]};
            camera.yaw = info->camera_pose[3]; camera.pitch = info->camera_pose[4];
            camera.fov = info->fov;
            const auto lighting = lighting_for_time_of_day(config.time_of_day);
            WeatherState weather(config.weather);
            TonemapParams tonemap;
            MaterialResolver resolver = [&](const std::string& mesh, const std::string& surface) {
                return scene->resolve_material(mesh, surface);
            };
            const auto before = backend.debug_build_counters();
            unsigned presented = 0;
            for (unsigned attempt = 0; attempt < 32 && presented < 2; ++attempt) {
                const auto frame = backend.render_frame(scene->scene.instances, {},
                    scene->scene.gpu_meshes, resolver, camera, lighting, weather,
                    tonemap, ctx.width, ctx.height);
                if (frame.presented) ++presented;
                else render_context.engine->flushAndWait();
            }
            render_context.engine->flushAndWait();
            presented_total += presented;
            const auto after = backend.debug_build_counters();
            ok &= presented == 2 && render_context.scene->getRenderableCount() > 0 &&
                after.scene_builds == before.scene_builds + 1 &&
                render_context.view->getBloomOptions().dirt ==
                    render_context.test_scene_assets->lens_dirt_texture();
            if (scenario.id == "foliage") {
                ok &= render_context.test_scene_assets->grass_patches().size() == 1 &&
                    (scenario.massive_foliage ? after.foliage_generations == 1
                                             : after.grass_generations == 1) &&
                    feature_enabled(backend, scenario.massive_foliage ? "massive-foliage" : "grass");
            } else if (scenario.id == "particles") {
                ok &= feature_enabled(backend, "particles") &&
                    backend.debug_depth_texture() != nullptr &&
                    backend.debug_find_feature("particles")->needs_depth_prepass();
            } else {
                ok &= feature_enabled(backend, "sky") &&
                    feature_enabled(backend, "clouds") &&
                    feature_enabled(backend, "volumetric-clouds") &&
                    feature_enabled(backend, "god_rays") &&
                    feature_enabled(backend, "volumetric_fog") &&
                    backend.debug_depth_texture() != nullptr;
            }
            std::printf("%s %.*s%s: %zu objects, %zu emitters, %zu lights, %u presented frames\n",
                kTag, static_cast<int>(scenario.id.size()), scenario.id.data(),
                scenario.id == "foliage" ? (scenario.massive_foliage ? " massive" : " grass") : "",
                scene->scene.instances.size(), scene->scene.particle_emitters.size(),
                scene->scene.lights.size(), presented);
            previous_haze = scenario.id == "particles";
            if (!ok) break;
        }
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        ok = false;
    }
    // Restore while every local namespace and resolver is still alive. This
    // also retires the last scene's materials, targets and procedural inputs.
    ok = restore.restore() && ok;
    if (!ok) return CaseResult::Fail;
    std::printf("%s PASS: four real scene reloads / %u presented frames, feature and View "
                "borrower teardown, exact retired mesh VB/IB owner deltas, "
                "original asset context/configuration restored; "
                "no pixel-appearance claim\n", kTag, presented_total);
    return CaseResult::Pass;
}
} // namespace heaven::smoke
