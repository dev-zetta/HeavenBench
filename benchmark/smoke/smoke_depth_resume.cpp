// Native depth silhouettes must catch up after a one-off move while depth is idle.
#include "smoke_cases.h"
#include "engine/render_test_scene_catalog.h"
#include "rhi/filament_backend.h"
#include "rhi/render_feature.h"

#include <filament/Engine.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Texture.h>
#include <backend/PixelBufferDescriptor.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <exception>
#include <span>
#include <stdexcept>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_depth_resume]";

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

Mesh card_mesh() {
    Mesh mesh;
    mesh.name = mesh.path = "depth_resume_card";
    Surface surface;
    surface.name = "card";
    constexpr std::array<glm::vec2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (const auto corner : corners) {
        Vertex vertex{};
        vertex.px = corner.x;
        vertex.pz = corner.y;
        vertex.ny = -1;
        vertex.u = (corner.x + 1) * 0.5f;
        vertex.v = (corner.y + 1) * 0.5f;
        vertex.tx = 1;
        vertex.tw = 1;
        surface.vertices.push_back(vertex);
    }
    surface.indices = {0, 1, 2, 0, 2, 3};
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

std::vector<float> read_depth(FilamentBackend& backend) {
    auto& context = backend.debug_render_context();
    const auto* texture = backend.debug_depth_texture();
    require(texture != nullptr, "active rain did not produce scene depth");
    // This temporary target only borrows the current shared color texture. It
    // is destroyed before any backend reload/resize can retire that texture.
    auto* target = filament::RenderTarget::Builder()
        .texture(filament::RenderTarget::AttachmentPoint::COLOR0,
                 const_cast<filament::Texture*>(texture))
        .build(*context.engine);
    require(target != nullptr, "cannot attach shared depth for readback");
    std::vector<float> pixels(static_cast<std::size_t>(texture->getWidth()) * texture->getHeight());
    std::atomic_bool complete{false};
    context.renderer->readPixels(target, 0, 0, texture->getWidth(), texture->getHeight(),
        filament::backend::PixelBufferDescriptor(pixels.data(), pixels.size() * sizeof(float),
            filament::backend::PixelDataFormat::R, filament::backend::PixelDataType::FLOAT,
            [](void*, std::size_t, void* user) {
                static_cast<std::atomic_bool*>(user)->store(true);
            }, &complete));
    context.engine->flushAndWait();
    context.engine->destroy(target);
    require(complete.load(), "shared depth readback incomplete");
    require(std::ranges::all_of(pixels, [](float value) { return std::isfinite(value); }),
        "shared depth contains non-finite values");
    return pixels;
}

std::size_t different_pixels(const std::vector<float>& left, const std::vector<float>& right) {
    require(left.size() == right.size(), "depth extent changed during comparison");
    std::size_t count = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (std::abs(left[i] - right[i]) > 1e-4f) ++count;
    return count;
}

class RestoreBackend {
public:
    RestoreBackend(FilamentBackend& backend, const AppConfig& config,
                   std::vector<GpuMesh>& meshes)
        : backend_(backend), config_(config), meshes_(meshes) {}
    ~RestoreBackend() {
        if (!attempted_) static_cast<void>(restore());
    }
    bool restore() noexcept {
        attempted_ = true;
        try {
            if (!backend_.reconfigure_for_reload(config_)) return false;
            // Clear all scene/proxy borrowers before releasing their buffers.
            for (const auto& mesh : meshes_) backend_.release_mesh(mesh);
            meshes_.clear();
            backend_.debug_render_context().engine->flushAndWait();
            return true;
        } catch (...) { return false; }
    }
private:
    FilamentBackend& backend_;
    const AppConfig& config_;
    std::vector<GpuMesh>& meshes_;
    bool attempted_ = false;
};
} // namespace

CaseResult run_depth_resume_case(const CaseContext& ctx) {
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Fail;
    std::vector<GpuMesh> meshes;
    RestoreBackend restore(backend, ctx.config, meshes);
    bool ok = true;
    try {
        for (const bool instanced : {false, true}) {
            AppConfig config = ctx.config;
            require(apply_render_test_scene_defaults(config, "rain"), "rain fixture unavailable");
            config.features.gpu_instancing = instanced;
            config.features.taa = config.features.reflections = config.features.wet_reflections = false;
            config.tessellation.enabled = false;
            require(backend.reconfigure_for_reload(config).has_value(), "fixture reload failed");
            for (const auto& mesh : meshes) backend.release_mesh(mesh);
            meshes.clear();
            auto& context = backend.debug_render_context();
            require(!instanced || context.engine->getMaxAutomaticInstances() >= 2,
                "native engine cannot exercise instanced depth");
            meshes.push_back(backend.upload_mesh(card_mesh()));
            require(!meshes.front().surfaces.empty(), "card upload failed");
            MaterialResolver resolver = [](const std::string&, const std::string&) {
                GpuMaterial material;
                material.two_sided = true;
                return material;
            };
            std::vector<Instance> instances;
            std::vector<int> dirty;
            const int count = instanced ? 2 : 1;
            for (int index = 0; index < count; ++index) {
                const float x = instanced ? 2.5f * static_cast<float>(index) : 0.0f;
                instances.push_back({0, glm::translate(glm::mat4(1), glm::vec3(x, 0, -1.5f))});
                dirty.push_back(index);
            }
            Camera camera;
            camera.position = {0, -12, 0};
            camera.yaw = 90;
            camera.pitch = 0;
            const auto lighting = lighting_for_time_of_day(12);
            TonemapParams tonemap;
            WeatherState wet, dry;
            wet.set_intensity(0.8f);
            const auto buffers_before = context.engine->getInstanceBufferCount();
            const auto submit = [&](const WeatherState& weather, std::span<const int> changed) {
                bool presented = false;
                for (int retry = 0; retry < 32 && !presented; ++retry) {
                    presented = backend.render_frame(instances, changed, meshes, resolver,
                        camera, lighting, weather, tonemap, ctx.width, ctx.height).presented;
                    context.engine->flushAndWait();
                }
                require(presented, "native frame repeatedly rejected");
            };
            submit(wet, dirty);
            const auto buffer_delta = context.engine->getInstanceBufferCount() - buffers_before;
            require(buffer_delta == (instanced ? 2u : 0u),
                "fixture did not construct the expected main/depth instance buffers");
            const auto initial = read_depth(backend);
            const auto before = backend.debug_build_counters();
            const auto* rain = backend.debug_find_feature("rain");
            require(rain && rain->enabled() && !rain->needs_depth_prepass(dry),
                "fixture did not isolate rain's inactive depth interval");
            for (const auto& feature : {"god_rays", "volumetric_fog", "particles",
                                       "lens_flares", "volumetric-clouds", "ssgi"}) {
                const auto* other = backend.debug_find_feature(feature);
                require(!other || !other->enabled(), "another consumer supplied idle depth");
            }
            for (auto& instance : instances) instance.transform[3].z += 3;
            submit(dry, dirty);  // A one-off movement/reset while no consumer needs depth.
            submit(dry, {});     // Main clears its dirty list once the objects stop.
            const auto idle = read_depth(backend);
            submit(wet, {});     // Reactivation must catch up without a new dirty event.
            const auto resumed = read_depth(backend);
            submit(wet, dirty);  // Explicit current-pose reference, independently observable.
            const auto reference = read_depth(backend);
            const auto after = backend.debug_build_counters();
            const auto moved_pixels = different_pixels(initial, reference);
            const auto resume_errors = different_pixels(resumed, reference);
            const auto idle_errors = different_pixels(initial, idle);
            std::printf("%s %s: %zu instance buffers, moved=%zu, resume/reference=%zu, idle/initial=%zu pixels\n",
                kTag, instanced ? "instanced" : "singleton", buffer_delta,
                moved_pixels, resume_errors, idle_errors);
            require(moved_pixels >= 8, "depth movement oracle was vacuous");
            require(idle_errors == 0, "idle rain updated the retained depth field");
            if (resume_errors != 0) {
                std::printf("%s FAIL: %s resumed depth retained an earlier instance pose\n",
                    kTag, instanced ? "instanced" : "singleton");
                ok = false;
            }
            require(after.scene_builds == before.scene_builds &&
                after.rain_streak_builds == before.rain_streak_builds &&
                after.splash_builds == before.splash_builds,
                "dry/resume rebuilt persistent scene or rain geometry");
        }
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        ok = false;
    }
    ok = restore.restore() && ok;
    return ok ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
