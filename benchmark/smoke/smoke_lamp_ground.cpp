// An authored lamp must illuminate the ground with the local-shadow feature
// enabled. Its source shadow=0 differs from a nearby eligible control light:
// the control's opaque blocker must still produce a real ground shadow.
#include "smoke_cases.h"
#include "engine/material_rhi.h"
#include "engine/scene_rhi.h"
#include "rhi/filament_backend.h"
#include "rhi/lighting_system.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
#include "rhi/scene_builder.h"
#include "rhi/scene_light_spawner.h"
#include "rhi/screenshot_ppm.h"
#include "rhi/texture_store.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/RenderableManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <math/mat4.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_lamp_ground]";
constexpr std::uint32_t kSize = 256;
constexpr std::string_view kLampNode = "heaven/nodes/lamp_post.node";
constexpr std::string_view kLightNode = "heaven/nodes/lamp_post_light.node";
constexpr filament::math::float3 kControlPosition{5, -1, 4};
using Pixels = std::vector<float>;

Mesh floor_mesh() {
    Mesh mesh;
    mesh.name = mesh.path = "lamp-ground-receiver";
    Surface surface;
    surface.name = "lamp-ground-receiver";
    for (const auto position : std::array<glm::vec3, 4>{{
             {-3.5f, -5, 0}, {7.5f, -5, 0}, {7.5f, 3, 0}, {-3.5f, 3, 0}}}) {
        Vertex vertex{};
        vertex.px = position.x; vertex.py = position.y;
        vertex.nz = 1; vertex.tx = 1; vertex.tw = 1;
        surface.vertices.push_back(vertex);
    }
    surface.indices = {0, 1, 2, 0, 2, 3};
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

Mesh control_cube() {
    struct Face { glm::vec3 n, t, b; };
    constexpr std::array<Face, 6> faces{{
        {{1,0,0}, {0,1,0}, {0,0,1}}, {{-1,0,0}, {0,-1,0}, {0,0,1}},
        {{0,1,0}, {-1,0,0}, {0,0,1}}, {{0,-1,0}, {1,0,0}, {0,0,1}},
        {{0,0,1}, {1,0,0}, {0,1,0}}, {{0,0,-1}, {-1,0,0}, {0,1,0}}}};
    constexpr std::array<glm::vec2, 4> corners{{{-1,-1}, {1,-1}, {1,1}, {-1,1}}};
    Mesh mesh;
    mesh.name = mesh.path = "eligible-light-blocker";
    Surface surface;
    surface.name = "eligible-light-blocker";
    for (const auto& face : faces) {
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (const auto corner : corners) {
            const auto position = 0.35f * (face.n + corner.x * face.t + corner.y * face.b);
            Vertex vertex{};
            vertex.px = position.x; vertex.py = position.y; vertex.pz = position.z;
            vertex.nx = face.n.x; vertex.ny = face.n.y; vertex.nz = face.n.z;
            vertex.tx = face.t.x; vertex.ty = face.t.y; vertex.tz = face.t.z; vertex.tw = 1;
            vertex.u = (corner.x + 1) * 0.5f; vertex.v = (corner.y + 1) * 0.5f;
            surface.vertices.push_back(vertex);
        }
        for (const auto corner : std::array<std::uint32_t, 6>{0,1,2,0,2,3})
            surface.indices.push_back(first + corner);
    }
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

// This adapter only gives the production CPU loaders/material resolver access
// to fixture-owned native resource stores. Rendering uses a real Renderer.
class LampFixture final : public IRhiBackend {
public:
    LampFixture(filament::Engine& engine, const RenderContext& context)
        : engine_(engine), context_(context), textures_(engine, context.assets),
          materials_(engine, textures_), meshes_(engine) {}
    LampFixture(const LampFixture&) = delete;
    LampFixture& operator=(const LampFixture&) = delete;
    ~LampFixture() override {
        engine_.flushAndWait();
        spawner_.destroy(core_);
        lighting_.reset();
        builder_.clear(core_);
        for (const auto entity : objects_) {
            if (scene_) scene_->remove(entity);
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        if (view_) {
            view_->setRenderTarget(nullptr);
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
        }
        engine_.destroy(view_);
        engine_.destroy(scene_);
        engine_.destroy(target_);
        engine_.destroy(depth_);
        engine_.destroy(color_);
        if (camera_entity_) {
            engine_.destroyCameraComponent(camera_entity_);
            utils::EntityManager::get().destroy(camera_entity_);
        }
        engine_.destroy(renderer_);
        engine_.destroy(swapchain_);
    }

    GraphicsApi api() const noexcept override { return GraphicsApi::Filament; }
    std::string device_name() const override { return "native lamp fixture resources"; }
    std::string api_version() const override { return {}; }
    RendererFeatureStats renderer_feature_stats() const override { return {}; }
    GpuMesh upload_mesh(const Mesh& mesh) override { return meshes_.upload(mesh); }
    GpuMesh upload_skinned(const SkinnedMesh& mesh) override { return meshes_.upload_skinned(mesh); }
    void release_mesh(const GpuMesh& mesh) override { meshes_.release(mesh); }
    std::uint64_t load_texture(const std::string& path, TextureSemantic semantic) override {
        return textures_.load(path, semantic);
    }
    Result<void> init_pipelines(const AppConfig& /*config*/) override { return {}; }
    FrameResult render_frame(std::span<const Instance> /*instances*/,
            std::span<const int> /*dirty*/, std::span<const GpuMesh> /*meshes*/,
            const MaterialResolver& /*resolve*/, const Camera& /*camera*/,
            const LightingParams& /*lighting*/, const WeatherState& /*weather*/,
            const TonemapParams& /*tonemap*/, int /*width*/, int /*height*/) override { return {}; }
    void present() override {}
    Result<void> update_mesh_vertices(std::uint64_t handle, const Vertex* vertices,
                                      std::uint32_t count) override {
        return meshes_.update_vertices(handle, vertices, count);
    }

    void init(const AppConfig& source) {
        if (!context_.paths) throw std::runtime_error("original asset resolver unavailable");
        const auto& paths = context_.paths->get();
        textures_.create_placeholders();
        textures_.set_texture_quality(TextureQuality::Original);
        if (!materials_.load_base_materials()) throw std::runtime_error("fixture materials unavailable");
        materials_.set_tessellation_config(1, 1, false);
        database_.build(paths, nullptr, context_.assets);
        AppConfig config = source;
        config.render_test_scene.clear();
        config.tessellation.enabled = false;
        config.features.gpu_skinning = false;
        config.node_file = paths.relative_path(std::string(kLampNode));
        auto lamp = load_scene_rhi(config, *this, nullptr, context_.assets);
        if (!lamp || lamp->instances.size() != 1 || lamp->gpu_meshes.size() != 1)
            throw std::runtime_error("authored lamp node failed or changed its one-mesh contract");
        lamp_ = std::move(*lamp);
        config.node_file = paths.relative_path(std::string(kLightNode));
        auto lights = load_scene_rhi(config, *this, nullptr, context_.assets);
        if (!lights || lights->lights.size() != 1 || lights->lights[0].shadow_eligible ||
                lights->lights[0].kind != SceneLightKind::Spot)
            throw std::runtime_error("authored lamp spot lost its source shadow=0 contract");
        auto authored = std::move(lights->lights);
        SceneLight control;
        control.name = "eligible-control-spot";
        control.kind = SceneLightKind::Spot;
        control.world_pos = control.local_pos = {kControlPosition.x, kControlPosition.y, kControlPosition.z};
        control.direction = {0, 0, -1};
        control.color = {1, 1, 1};
        control.multiplier = 2;
        control.radius = 6;
        control.inner_cone_rad = 35.0f * std::numbers::pi_v<float> / 180.0f;
        control.outer_cone_rad = 55.0f * std::numbers::pi_v<float> / 180.0f;
        control.shadow_eligible = true;
        authored.push_back(control);

        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        if (!renderer_ || !swapchain_ || !scene_ || !view_ || !camera_)
            throw std::runtime_error("fixture native objects unavailable");
        core_ = {.engine = &engine_, .scene = scene_, .view = view_,
                 .renderer = renderer_, .camera = camera_};
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera_->setExposure(2.8f, 1.0f / 30, 200);
        camera_->setProjection(filament::Camera::Projection::ORTHO, -6, 6, -6, 6, 0.1, 30);
        camera_->lookAt({2, -1, 15}, {2, -1, 0}, {0, 1, 0});
        // Filament permits combining its documented usage flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA16F).usage(usage).build(engine_);
        depth_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        if (!color_ || !depth_) throw std::runtime_error("capture textures unavailable");
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        if (!target_) throw std::runtime_error("capture target unavailable");
        view_->setScene(scene_); view_->setCamera(camera_); view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(true);
        view_->setShadowType(filament::ShadowType::PCF);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        const MaterialResolver resolve = [&](const std::string& mesh, const std::string& surface) {
            return database_.resolve(mesh, surface, paths, *this);
        };
        builder_.build(core_, meshes_, materials_, lamp_.instances, lamp_.gpu_meshes, resolve);
        add_object(floor_mesh(), {});
        blocker_ = add_object(control_cube(), {kControlPosition.x, kControlPosition.y, 2});
        spawner_.spawn_emission(core_, lamp_.instances, lamp_.gpu_meshes, resolve);
        spawner_.set_pending_authored(authored);
        spawner_.spawn_authored(core_, builder_.built());
        lighting_ = std::make_unique<LightingSystem>(core_);
        // Exercise production selection with the global feature enabled at all
        // times. The eligible control consumes the budget; lamps remain lit.
        lighting_->configure_point_light_shadows(spawner_.light_infos(), 1, 512, true);
        auto& manager = engine_.getLightManager();
        std::size_t lamp_lights = 0, eligible = 0;
        for (const auto& info : spawner_.light_infos()) {
            const auto light = manager.getInstance(info.entity);
            if (!light) throw std::runtime_error("spawned light has no native component");
            if (info.shadow_eligible) {
                ++eligible;
                control_ = info.entity;
                if (!manager.isShadowCaster(light))
                    throw std::runtime_error("enabled budget failed to select the eligible control");
            } else {
                ++lamp_lights;
                if (manager.isShadowCaster(light))
                    throw std::runtime_error("global budget promoted an enclosed/authored shadow=0 lamp");
            }
        }
        if (lamp_lights != 2 || eligible != 1)
            throw std::runtime_error("expected original emission point + original spot + eligible control");
        if (textures_.stats().active_quality != TextureQuality::Original || textures_.stats().srgb == 0)
            throw std::runtime_error("authored lamp textures did not use their original color path");
    }

    Pixels capture(bool lamp_on, bool control_on, bool blocker_casts) {
        auto& lights = engine_.getLightManager();
        for (const auto& info : spawner_.light_infos()) {
            const bool enabled = info.entity == control_ ? control_on : lamp_on;
            // The spawner descriptor reads LightManager::getIntensity(), which
            // returns candela for punctual lights; setIntensity takes lumens.
            // Preserve the authored source power rather than converting twice.
            lights.setIntensityCandela(lights.getInstance(info.entity), enabled ? info.intensity : 0);
        }
        auto& renderables = engine_.getRenderableManager();
        renderables.setCastShadows(renderables.getInstance(blocker_), blocker_casts);
        // Neither LightManager shadow eligibility nor the global feature is
        // disabled to obtain the unblocked control: only its blocker changes.
        for (int settle = 0; settle < 3; ++settle) draw();
        Pixels pixels(static_cast<std::size_t>(kSize) * kSize * 4);
        std::atomic_bool complete{false};
        renderer_->readPixels(target_, 0, 0, kSize, kSize,
            filament::backend::PixelBufferDescriptor(pixels.data(), pixels.size() * sizeof(float),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::FLOAT,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &complete));
        engine_.flushAndWait();
        if (!complete.load()) throw std::runtime_error("native FLOAT readback incomplete");
        return pixels;
    }

private:
    utils::Entity add_object(const Mesh& cpu, filament::math::float3 position) {
        const auto gpu = meshes_.upload(cpu);
        if (gpu.surfaces.size() != 1) throw std::runtime_error("control mesh upload failed");
        const auto buffers = meshes_.find(gpu.surfaces[0].handle);
        const auto entity = utils::EntityManager::get().create();
        objects_.push_back(entity);
        auto* material = materials_.get_or_create(cpu.path, cpu.surfaces[0].name, GpuMaterial{});
        if (!material || !buffers.vb || !buffers.ib) throw std::runtime_error("control material/buffers unavailable");
        const bool floor = cpu.path == "lamp-ground-receiver";
        const filament::Box box = floor ? filament::Box{{2,-1,0}, {5.5f,4,0.001f}}
                                      : filament::Box{{0,0,0}, {0.35f,0.35f,0.35f}};
        if (filament::RenderableManager::Builder(1).boundingBox(box)
                .material(0, material).geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                    buffers.vb, buffers.ib, 0, gpu.surfaces[0].index_count)
                .culling(false).castShadows(!floor).receiveShadows(true).build(engine_, entity) !=
                filament::RenderableManager::Builder::Success)
            throw std::runtime_error("control renderable creation failed");
        auto& transforms = engine_.getTransformManager();
        transforms.create(entity);
        transforms.setTransform(transforms.getInstance(entity), filament::math::mat4f::translation(position));
        scene_->addEntity(entity);
        return entity;
    }

    void draw() {
        for (int retry = 0; retry < 12; ++retry) {
            if (!renderer_->beginFrame(swapchain_)) continue;
            renderer_->render(view_); renderer_->endFrame();
            engine_.flushAndWait();
            return;
        }
        throw std::runtime_error("native frame repeatedly rejected");
    }

    filament::Engine& engine_;
    RenderContext context_;
    TextureStore textures_;
    MaterialInstanceCache materials_;
    MeshStore meshes_;
    RhiMaterialDatabase database_;
    RhiScene lamp_;
    SceneBuilder builder_;
    SceneLightSpawner spawner_;
    BackendCore core_{};
    std::unique_ptr<LightingSystem> lighting_;
    std::vector<utils::Entity> objects_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    utils::Entity camera_entity_, blocker_, control_;
};

struct GroundSample { double sum = 0; std::size_t count = 0; };

GroundSample annulus(const Pixels& pixels, double center_x, double inner, double outer) {
    GroundSample sample;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            // Independent orthographic ground-plane projection. The camera
            // sees unchanged cube pixels inside radius.35; the shadow witness
            // samples only ground outside that silhouette, at radius.45–.65.
            const double world_x = -4 + (static_cast<double>(x) + 0.5) * 12 / kSize;
            const double world_y = -7 + (static_cast<double>(y) + 0.5) * 12 / kSize;
            const double radius = std::hypot(world_x - center_x, world_y + 1);
            if (radius < inner || radius > outer) continue;
            const auto offset = (static_cast<std::size_t>(y) * kSize + x) * 4;
            for (std::size_t c = 0; c < 3; ++c) {
                if (!std::isfinite(pixels[offset + c]) || pixels[offset + c] < 0)
                    throw std::runtime_error("invalid native ground radiance");
                sample.sum += pixels[offset + c];
            }
            ++sample.count;
        }
    }
    return sample;
}

bool save_pixels(const char* name, const Pixels& pixels) {
    const char* output = std::getenv("HEAVEN_SMOKE_LAMP_GROUND_OUTPUT_DIR");
    if (!output || !*output) return true;
    const auto path = std::filesystem::path(output) / (std::string(name) + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    std::vector<std::uint8_t> bytes(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        if (!std::isfinite(pixels[i])) throw std::runtime_error("invalid capture pixel");
        bytes[i] = static_cast<std::uint8_t>(std::lround(std::clamp(pixels[i], 0.0f, 1.0f) * 255));
    }
    return write_screenshot_ppm(path, kSize, kSize, bytes).has_value();
}
} // namespace

CaseResult run_lamp_ground_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Fail;
    const auto context = backend.debug_render_context();
    auto* engine = context.engine;
    if (!engine) return CaseResult::Fail;
    const auto owners = [&] {
        return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
            engine->getViewCount(), engine->getTextureCount(), engine->getMaterialCount(),
            engine->getSceneCount(), engine->getRenderTargetCount(), engine->getSwapChainCount(),
            engine->getLightManager().getComponentCount(),
            engine->getTransformManager().getComponentCount()};
    };
    const auto baseline = owners();
    bool valid = true;
    try {
        LampFixture fixture(*engine, context);
        fixture.init(ctx.config);
        const auto dark = fixture.capture(false, false, true);
        const auto lamp = fixture.capture(true, false, true);
        const auto unblocked = fixture.capture(false, true, false);
        const auto blocked = fixture.capture(false, true, true);
        const auto a = annulus(dark, 0, 0.7, 1.3), b = annulus(lamp, 0, 0.7, 1.3),
            c = annulus(unblocked, 5, 0.45, 0.65), d = annulus(blocked, 5, 0.45, 0.65);
        const auto mean = [](GroundSample sample) { return sample.sum / static_cast<double>(sample.count); };
        valid = a.count > 256 && c.count > 128 && mean(b) > mean(a) + 0.005 &&
            mean(c) > 0.01 && mean(d) < 0.65 * mean(c);
        std::printf("%s actual lamp ground dark/lit=%.7g/%.7g (%zu pixels); eligible control "
            "unblocked/blocked=%.7g/%.7g (%zu pixels), global local shadows enabled: %s\n",
            kTag, mean(a), mean(b), a.count, mean(c), mean(d), c.count, valid ? "PASS" : "FAIL");
        valid &= save_pixels("ground-dark", dark) && save_pixels("authored-lamp-on", lamp) &&
            save_pixels("eligible-control-unblocked", unblocked) && save_pixels("eligible-control-blocked", blocked);
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        valid = false;
    }
    engine->flushAndWait();
    if (owners() != baseline) {
        std::printf("%s FAIL: fixture owners did not return to baseline\n", kTag);
        valid = false;
    }
    return valid ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
