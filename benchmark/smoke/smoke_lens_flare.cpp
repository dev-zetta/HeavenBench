// Real feature/material readback: translating camera and light together must
// preserve the flare, even when Filament moves its shader-world origin.
#include "smoke_cases.h"

#include "rhi/features/lens_flare_feature.h"
#include "rhi/filament_backend.h"
#include "rhi/scene_light_spawner.h"
#include "rhi/filament_util.h"
#include "rhi/material_instance_cache.h"
#include "rhi/texture_store.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <utility>

namespace heaven::smoke {
namespace {

constexpr const char* kTag = "[smoke_lens_flare]";
constexpr std::uint32_t kExtent = 64;
using Pixels = std::array<std::uint8_t, static_cast<std::size_t>(kExtent) * kExtent * 4>;

// A uniform white sprite isolates geometry and distance fading from installed
// assets. Shared white/zero-depth textures retain the backend's ownership.
class FlareResources final : public IResourceProvider {
public:
    explicit FlareResources(IResourceProvider& source) : source_(source) {}
    std::uint64_t load_texture(const std::string& /*path*/, TextureSemantic /*semantic*/) override { return 1; }
    filament::Texture* get_filament_texture(std::uint64_t /*handle*/) override {
        return source_.placeholder_texture();
    }
    filament::Texture* placeholder_texture() override { return source_.placeholder_texture(); }
    filament::Texture* placeholder_normal_texture() override {
        return source_.placeholder_normal_texture();
    }
    filament::Texture* placeholder_depth_texture() override {
        return source_.placeholder_depth_texture();
    }
private:
    IResourceProvider& source_;
};

class FlareFixture {
public:
    explicit FlareFixture(RenderContext context)
        : context_(context), engine_(*context.engine), resources_(*context.resources) {
        context_.resources = &resources_;
        context_.renderer = engine_.createRenderer();
        context_.renderer->setDisplayInfo({.refreshRate = 0.0f});
        context_.renderer->setFrameRateOptions({.interval = 0});
        swapchain_ = engine_.createSwapChain(kExtent, kExtent);
        camera_entity_ = utils::EntityManager::get().create();
        context_.camera = engine_.createCamera(camera_entity_);
        context_.camera->setProjection(60.0, 1.0, 0.1, 200.0);
        context_.scene = engine_.createScene();
        sky_ = filament::Skybox::Builder().color({0, 0, 0, 1}).build(engine_);
        context_.scene->setSkybox(sky_);
        // Filament documents these texture usage flags as composable bits.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        texture_ = filament::Texture::Builder().width(kExtent).height(kExtent).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, texture_).build(engine_);
        context_.view = engine_.createView();
        context_.view->setScene(context_.scene);
        context_.view->setCamera(context_.camera);
        context_.view->setRenderTarget(target_);
        context_.view->setViewport({0, 0, kExtent, kExtent});
        context_.view->setPostProcessingEnabled(false);
        context_.view->setShadowingEnabled(false);
        light_entity_ = utils::EntityManager::get().create();
        filament::LightManager::Builder(filament::LightManager::Type::POINT)
            .intensityCandela(100.0f).build(engine_, light_entity_);
        occluder_depth_ = filament::Texture::Builder().width(1).height(1).levels(1)
            .format(filament::Texture::InternalFormat::R32F).build(engine_);
        auto* depth = new float(1.0f);
        occluder_depth_->setImage(engine_, 0, filament::backend::PixelBufferDescriptor(
            depth, sizeof(float), filament::backend::PixelDataFormat::R,
            filament::backend::PixelDataType::FLOAT,
            [](void* buffer, std::size_t, void*) { delete static_cast<float*>(buffer); }));
    }

    ~FlareFixture() {
        if (flare_) flare_->destroy(context_);
        engine_.destroy(light_entity_);
        utils::EntityManager::get().destroy(light_entity_);
        engine_.destroy(occluder_depth_);
        context_.view->setRenderTarget(nullptr);
        context_.scene->setSkybox(nullptr);
        engine_.destroy(context_.view);
        engine_.destroy(context_.scene);
        engine_.destroy(sky_);
        engine_.destroy(target_);
        engine_.destroy(texture_);
        engine_.destroyCameraComponent(camera_entity_);
        utils::EntityManager::get().destroy(camera_entity_);
        engine_.destroy(context_.renderer);
        engine_.destroy(swapchain_);
    }

    FlareFixture(const FlareFixture&) = delete;
    FlareFixture& operator=(const FlareFixture&) = delete;

    bool capture(glm::vec3 offset, float distance, Pixels& pixels,
                 float angle_degrees = 0.0f, float power = 1.0f,
                 bool occluded = false, bool reuse = false) {
        if (flare_ && !reuse) { flare_->destroy(context_); flare_.reset(); }
        // Skybox contributes a scene renderable; measure only the new flare.
        const std::size_t baseline_renderables = context_.scene->getRenderableCount();
        AppConfig config;
        config.features.lens_flares = true;
        const bool created = !flare_;
        if (created) {
            flare_ = std::make_unique<LensFlareFeature>();
            flare_->init(context_, config);
        }
        Camera camera;
        camera.position = offset;
        camera.yaw = 0.0f;
        camera.pitch = -90.0f;
        context_.camera->lookAt({offset.x, offset.y, offset.z},
                               {offset.x, offset.y, offset.z - 1.0f}, {0, 1, 0});
        SceneLightInfo light;
        light.authored = true;
        light.flare_source = true; // this fixture explicitly supplies a visible bulb
        const float angle = glm::radians(angle_degrees);
        light.world_pos = offset + glm::vec3(std::sin(angle), 0, -std::cos(angle)) * distance;
        light.entity = light_entity_;
        light.nominal_intensity = 100.0f;
        light.intensity = light.nominal_intensity * power;
        auto& manager = engine_.getLightManager();
        const auto component = manager.getInstance(light_entity_);
        manager.setPosition(component, {light.world_pos.x, light.world_pos.y, light.world_pos.z});
        manager.setIntensityCandela(component, light.intensity);
        light.color = glm::vec3(1.0f);
        FrameContext frame;
        frame.camera = &camera;
        frame.scene_lights = std::span<const SceneLightInfo>(&light, 1);
        frame.depth_texture = occluded ? occluder_depth_ :
            context_.resources->placeholder_depth_texture();
        flare_->update(context_, frame);
        const std::size_t actual_renderables = context_.scene->getRenderableCount();
        if (actual_renderables != baseline_renderables + (created ? 1 : 0)) {
            std::printf("%s FAIL: expected one added feature renderable (%zu -> %zu)\n",
                        kTag, baseline_renderables, actual_renderables);
            return false;
        }
        if (!context_.renderer->beginFrame(swapchain_)) return false;
        context_.renderer->render(context_.view);
        std::atomic_bool completed{false};
        context_.renderer->readPixels(target_, 0, 0, kExtent, kExtent,
            filament::backend::PixelBufferDescriptor(pixels.data(), sizeof(pixels),
                filament::backend::PixelDataFormat::RGBA,
                filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &completed));
        context_.renderer->endFrame();
        engine_.flushAndWait();
        return completed.load();
    }

private:
    RenderContext context_;
    filament::Engine& engine_;
    FlareResources resources_;
    std::unique_ptr<LensFlareFeature> flare_;
    utils::Entity camera_entity_;
    utils::Entity light_entity_;
    filament::Texture* occluder_depth_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Skybox* sky_ = nullptr;
    filament::Texture* texture_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
};

bool check_emission_sources(filament::Engine& engine) {
    auto* scene = engine.createScene();
    const BackendCore core{.engine = &engine, .scene = scene};
    SceneLightSpawner spawner;
    GpuMesh mesh;
    mesh.path = "synthetic_lamp_sources.mesh";
    const std::array names{"lamp", "lamp_glass", "authored_panel",
                           "uniform_panel", "disabled_panel", "missing_map"};
    for (std::size_t i = 0; i < names.size(); ++i) {
        GpuSurface surface;
        surface.name = names[i];
        surface.index_count = 3;
        surface.bbox_min = surface.bbox_max = glm::vec3(static_cast<float>(i) * 10, 0, 0);
        mesh.surfaces.push_back(std::move(surface));
    }
    const std::array meshes{std::move(mesh)};
    const std::array instances{Instance{0, glm::mat4(1.0f)}};
    spawner.spawn_emission(core, instances, meshes,
        [](const std::string& /*mesh_path*/, const std::string& surface) {
            GpuMaterial material;
            // A default or explicitly disabled map remains bound for assets
            // and shader sampling, but must not create a synthetic point light.
            material.emission = surface == "missing_map" ? 0 : 1;
            material.emission_light_source = surface == "authored_panel" ||
                surface == "uniform_panel" || surface == "missing_map";
            return material;
        });
    // Only lamp_glass (day/night semantic exception), explicitly active custom
    // emission, and explicitly active uniform emission are eligible. In
    // particular the bare metal `lamp` must not produce a second nearby flare.
    const auto lights = spawner.light_infos();
    bool ok = lights.size() == 3 && scene->getLightCount() == 3;
    if (ok) {
        ok = lights[0].local_pos.x == 10.0f &&
             lights[1].local_pos.x > 20.0f && lights[1].local_pos.x < 21.0f &&
             lights[2].local_pos.x > 30.0f && lights[2].local_pos.x < 31.0f;
    }
    if (!ok) {
        std::printf("%s FAIL: synthetic source selection produced %zu lights\n", kTag,
                    lights.size());
    }
    LightingParams lighting;
    lighting.time_of_day = 12.0f;
    spawner.update_operating_state(core, lighting);
    ok = ok && spawner.light_infos()[0].intensity == 0 &&
        spawner.light_infos()[0].nominal_intensity > 0 &&
        spawner.light_infos()[1].intensity > 0 && spawner.light_infos()[2].intensity > 0;
    lighting.time_of_day = 21.0f;
    spawner.update_operating_state(core, lighting);
    ok = ok && spawner.light_infos()[0].intensity == spawner.light_infos()[0].nominal_intensity;
    spawner.destroy(core);
    ok = ok && scene->getLightCount() == 0;

    // Main publishes island offsets before the first rendered scene. Creation
    // must consume that cached value even if fixed-time captures never change
    // it again, and descriptors must agree with the actual LightManager source.
    SceneLight authored;
    authored.world_pos = {1, 2, 3};
    authored.floating_group = 6;
    authored.shadow_eligible = false;
    const std::array offsets{std::pair{6, 0.75f}};
    spawner.update_floating(core, offsets);
    spawner.set_pending_authored(std::span<const SceneLight>(&authored, 1));
    spawner.spawn_authored(core, true);
    auto& manager = engine.getLightManager();
    const auto check_position = [&](float expected) {
        const auto infos = spawner.light_infos();
        if (infos.size() != 1 || infos[0].shadow_eligible || infos[0].flare_source)
            return false;
        const auto component = manager.getInstance(infos[0].entity);
        return component && manager.getPosition(component).z == expected &&
            infos[0].world_pos.z == expected;
    };
    ok = ok && check_position(3.75f);
    spawner.update_floating(core, offsets);
    ok = ok && check_position(3.75f);
    const std::array changed_offsets{std::pair{6, 0.25f}};
    spawner.update_floating(core, changed_offsets);
    ok = ok && check_position(3.25f);
    spawner.destroy(core);
    ok = ok && scene->getLightCount() == 0 && spawner.light_infos().empty() &&
        spawner.light_entities().empty();
    if (!ok) std::printf("%s FAIL: floating source position/policy mismatch\n", kTag);
    engine.destroy(scene);
    return ok;
}

bool check_lamp_operating_state(filament::Engine& engine) {
    auto* scene = engine.createScene();
    const BackendCore core{.engine = &engine, .scene = scene};
    SceneLightSpawner spawner;
    GpuMesh mesh;
    mesh.path = "lamp_operating_sources.mesh";
    for (const auto* name : {"lamp_glass", "torch_glass"}) {
        GpuSurface surface;
        surface.name = name;
        surface.index_count = 3;
        const float x = surface.name == "lamp_glass" ? 0.0f : 10.0f;
        surface.bbox_min = {x - .2f, -.2f, -.2f};
        surface.bbox_max = {x + .2f, .2f, .2f};
        mesh.surfaces.push_back(std::move(surface));
    }
    const std::array meshes{std::move(mesh)};
    const std::array instances{Instance{0, glm::mat4(1)}};
    spawner.spawn_emission(core, instances, meshes,
        [](const std::string&, const std::string&) { return GpuMaterial{}; });
    std::array<SceneLight, 2> authored;
    authored[0].kind = SceneLightKind::Spot;
    authored[0].world_pos = {0, 0, .1f}; // actual bounded bulb association
    authored[1].world_pos = {20, 0, 0}; // unrelated lighting-only accent
    spawner.set_pending_authored(authored);
    spawner.spawn_authored(core, true);
    bool ok = spawner.light_infos().size() == 4;
    LightingParams lighting;
    lighting.time_of_day = 12;
    spawner.update_operating_state(core, lighting);
    if (ok) {
        const auto infos = spawner.light_infos();
        ok = infos[0].intensity == 0 && infos[1].intensity > 0 &&
            infos[2].intensity == 0 && infos[3].intensity > 0;
    }
    lighting.time_of_day = 21;
    spawner.update_operating_state(core, lighting);
    for (const auto& info : spawner.light_infos()) {
        ok = ok && info.intensity == info.nominal_intensity && info.intensity > 0;
    }
    // An explicit external power-off remains authoritative while the shared
    // operating gain is unchanged; flares read the live source, not a timer.
    if (spawner.light_infos().size() == 4) {
        auto& manager = engine.getLightManager();
        manager.setIntensityCandela(manager.getInstance(spawner.light_infos()[2].entity), 0);
        spawner.update_operating_state(core, lighting);
        ok = ok && spawner.light_infos()[2].intensity == 0;
    }
    spawner.destroy(core);
    ok = ok && scene->getLightCount() == 0;
    engine.destroy(scene);
    if (!ok) std::printf("%s FAIL: lamp/authored/torch shared operating state\n", kTag);
    return ok;
}

bool check_lamp_material_state(filament::Engine& engine) {
    TextureStore textures(engine);
    textures.create_placeholders();
    MaterialInstanceCache materials(engine, textures);
    if (!materials.load_base_materials()) return false;
    GpuMaterial material;
    material.emission_scale = 4.0f;
    materials.update_lamp_emission(21);
    auto* lamp = materials.get_or_create("fixture.mesh", "lamp_glass", material);
    auto* window = materials.get_or_create("fixture.mesh", "house_window_glass", material);
    if (!lamp || !window) return false;
    bool ok = lamp->getParameter<float>("emissionScale") == 4.0f &&
        window->getParameter<float>("emissionScale") == 4.0f;
    materials.update_lamp_emission(12);
    ok = ok && lamp->getParameter<float>("emissionScale") == 0 &&
        window->getParameter<float>("emissionScale") == 4.0f;
    auto* second = materials.get_or_create("second.mesh", "lantern_glass", material);
    ok = ok && second && second->getParameter<float>("emissionScale") == 0;
    materials.clear_instances();
    materials.update_lamp_emission(12);
    lamp = materials.get_or_create("fixture.mesh", "lamp_glass", material);
    ok = ok && lamp && lamp->getParameter<float>("emissionScale") == 0;
    materials.update_lamp_emission(21);
    ok = ok && lamp && lamp->getParameter<float>("emissionScale") == 4.0f;
    if (!ok) std::printf("%s FAIL: actual lamp material night/day/rebuild state\n", kTag);
    return ok;
}

std::uint64_t rgb_sum(const Pixels& pixels) {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        sum += pixels[i] + pixels[i + 1] + pixels[i + 2];
    }
    return sum;
}

} // namespace

CaseResult run_lens_flare_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Fail;
    const auto context = backend.debug_render_context();
    if (!context.engine || !context.resources || !context.paths) return CaseResult::Fail;
    if (!check_emission_sources(*context.engine)) return CaseResult::Fail;
    if (!check_lamp_operating_state(*context.engine) ||
        !check_lamp_material_state(*context.engine)) return CaseResult::Fail;
    FlareFixture fixture(context);
    Pixels reference{};
    if (!fixture.capture(glm::vec3(0), 5.0f, reference) || rgb_sum(reference) < 1000) {
        std::printf("%s FAIL: close flare must illuminate the black target\n", kTag);
        return CaseResult::Fail;
    }
    // These offsets move the old mismatched-space distance beyond its 55m fade
    // limit despite an unchanged 5m camera/light separation.
    constexpr std::array offsets{glm::vec3(150, -75, 20), glm::vec3(-120, 80, -30)};
    for (const auto offset : offsets) {
        Pixels translated{};
        if (!fixture.capture(offset, 5.0f, translated)) return CaseResult::Fail;
        for (std::size_t i = 0; i < reference.size(); ++i) {
            const int delta = static_cast<int>(reference[i]) - static_cast<int>(translated[i]);
            if (delta < -2 || delta > 2) {
                std::printf("%s FAIL: camera/light translation changed pixel %zu (%u -> %u)\n",
                            kTag, i, static_cast<unsigned>(reference[i]),
                            static_cast<unsigned>(translated[i]));
                return CaseResult::Fail;
            }
        }
        Pixels distant{};
        if (!fixture.capture(offset, 60.0f, distant) || rgb_sum(distant) != 0) {
            std::printf("%s FAIL: physically distant flare did not fade out\n", kTag);
            return CaseResult::Fail;
        }
    }
    Pixels off_axis{}, off_power{}, daytime{}, occluded{}, restored{};
    if (!fixture.capture({}, 5.0f, off_axis, 25.0f) || rgb_sum(off_axis) != 0 ||
        !fixture.capture({}, 5.0f, off_power, 0.0f, 0.0f, false, true) ||
        rgb_sum(off_power) != 0 ||
        !fixture.capture({}, 5.0f, daytime, 0.0f, night_luminaire_gain(12), false, true) ||
        rgb_sum(daytime) != 0 ||
        !fixture.capture({}, 5.0f, occluded, 0.0f, 1.0f, true, true) ||
        rgb_sum(occluded) != 0 ||
        !fixture.capture({}, 5.0f, restored, 0.0f, night_luminaire_gain(21), false, true) ||
        rgb_sum(restored) < 1000) {
        std::printf("%s FAIL: off-axis/off-power/day/occlusion or night recovery\n", kTag);
        return CaseResult::Fail;
    }
    std::printf("%s PASS: translation, distance, day/night, live power, optical axis and depth controls\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
