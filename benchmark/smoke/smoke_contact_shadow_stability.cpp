// Real material/GPU regression: front-lit receivers must remain free of
// contact-shadow acne, back-lit curved receivers must not leak light between
// triangles, and a nearby occluder must retain its contact shadow. No original assets or external reference image are needed.
#include "smoke_cases.h"
#include "rhi/filament_backend.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
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
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr std::uint32_t kSize = 256;
using Pixels = std::vector<std::uint8_t>;
enum class Projection { Perspective, OffAxis, Orthographic };

class ContactFixture {
public:
    explicit ContactFixture(filament::Engine& engine, bool smooth_normals, bool grazing_light,
                            Projection projection, bool curved = false, bool steep = false)
        : steep_(steep), smooth_normals_(smooth_normals), engine_(engine), textures_(engine), materials_(engine, textures_), meshes_(engine) {
        textures_.create_placeholders();
        if (!materials_.load_base_materials()) throw std::runtime_error("Contact fixture materials failed");
        renderer_ = engine_.createRenderer();
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        camera_entity_ = utils::EntityManager::get().create();
        auto* camera = engine_.createCamera(camera_entity_);
        if (projection == Projection::Orthographic) {
            camera->setProjection(filament::Camera::Projection::ORTHO,
                                  -2.2, 2.2, -2.2, 2.2, 0.1, 50.0);
        } else {
            camera->setProjection(40.0, 1.0, 0.1, 50.0);
            if (projection == Projection::OffAxis) camera->setShift({0.1, -0.08});
        }
        camera->lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
        camera->setExposure(16.0f, 1.0f / 125.0f, 100.0f);
        scene_ = engine_.createScene();
        // Filament documents these usage flags as composable bits.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        texture_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, texture_).build(engine_);
        view_ = engine_.createView();
        view_->setScene(scene_);
        view_->setCamera(camera);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        filament::AmbientOcclusionOptions ao;
        ao.enabled = false;
        ao.resolution = 1.0f;
        view_->setAmbientOcclusionOptions(ao);
        filament::TemporalAntiAliasingOptions taa;
        taa.enabled = true;
        view_->setTemporalAntiAliasingOptions(taa);
        view_->setDithering(filament::Dithering::NONE);
        // Cover the entire measurement region even for the shifted projection;
        // a finite receiver edge would measure silhouette filtering as acne.
        add_quad(steep ? 20.0f : 4.0f, 0.0f, !grazing_light, curved);
        add_quad(0.3f, 0.12f, false);
        // Keep a shadow map active without hiding the grazing receiver under
        // its own conventional shadow. The separate caster is behind it.
        if (grazing_light) add_quad(0.1f, -2.0f, true);
        light_ = utils::EntityManager::get().create();
        filament::LightManager::Builder(filament::LightManager::Type::SUN)
            .color({1, 1, 1}).intensity(100000.0f)
            .direction(steep ? filament::math::float3{-0.1f, -1.0f, -0.05f}
                : filament::math::float3{-0.8f, -0.1f, grazing_light ? -0.3f : -1.0f})
            .castShadows(true).build(engine_, light_);
        scene_->addEntity(light_);
        set_contact(true);
    }

    ~ContactFixture() {
        view_->setRenderTarget(nullptr);
        engine_.destroy(view_);
        for (auto entity : entities_) {
            scene_->remove(entity);
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        engine_.destroy(light_);
        utils::EntityManager::get().destroy(light_);
        engine_.destroy(scene_);
        engine_.destroy(target_);
        engine_.destroy(texture_);
        engine_.destroyCameraComponent(camera_entity_);
        utils::EntityManager::get().destroy(camera_entity_);
        engine_.destroy(renderer_);
        engine_.destroy(swapchain_);
    }
    ContactFixture(const ContactFixture&) = delete;
    ContactFixture& operator=(const ContactFixture&) = delete;

    void set_contact(bool enabled) {
        auto& lights = engine_.getLightManager();
        const auto instance = lights.getInstance(light_);
        auto options = lights.getShadowOptions(instance);
        options.screenSpaceContactShadows = enabled;
        options.stepCount = 32;
        options.maxShadowDistance = 0.3f;
        lights.setShadowOptions(instance, options);
    }

    bool frame(Pixels* pixels = nullptr) {
        if (!renderer_->beginFrame(swapchain_)) return false;
        renderer_->render(view_);
        std::atomic_bool complete{false};
        if (pixels) {
            pixels->resize(static_cast<std::size_t>(kSize) * kSize * 4);
            renderer_->readPixels(target_, 0, 0, kSize, kSize,
                filament::backend::PixelBufferDescriptor(pixels->data(), pixels->size(),
                    filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                    [](void*, std::size_t, void* user) {
                        static_cast<std::atomic_bool*>(user)->store(true);
                    }, &complete));
        }
        renderer_->endFrame();
        engine_.flushAndWait();
        return !pixels || complete.load();
    }

private:
    void add_quad(float extent, float lift, bool shadow_caster, bool curve_receiver = false) {
        Mesh mesh;
        Surface surface;
        // Smoothed vertex normals need not match the actual triangle plane.
        const auto normal = smooth_normals_ ? glm::vec3(0, 0, 1)
            : glm::normalize(steep_ ? glm::vec3(0, -3, 1) : glm::vec3(-0.4f, -0.7f, 1.0f));
        const unsigned segments = curve_receiver ? 16 : 1;
        for (unsigned y = 0; y <= segments; ++y) {
            for (unsigned x = 0; x <= segments; ++x) {
                Vertex vertex{};
                vertex.u = float(x) / float(segments);
                vertex.v = float(y) / float(segments);
                vertex.px = (vertex.u * 2.0f - 1.0f) * extent;
                vertex.py = (vertex.v * 2.0f - 1.0f) * extent;
                vertex.pz = -6.0f + (steep_ ? 3.0f * vertex.py
                    : 0.4f * vertex.px + 0.7f * vertex.py) + lift;
                // Convex, slightly twisted patches have neighboring noncoplanar
                // triangles, as on the rounded stones that exposed the regression.
                if (curve_receiver) {
                    vertex.pz += -0.06f * (vertex.px * vertex.px + vertex.py * vertex.py)
                        + 0.04f * vertex.px * vertex.py;
                }
                vertex.nx = normal.x; vertex.ny = normal.y; vertex.nz = normal.z;
                surface.vertices.push_back(vertex);
            }
        }
        for (unsigned y = 0; y < segments; ++y) {
            for (unsigned x = 0; x < segments; ++x) {
                const auto a = y * (segments + 1) + x;
                const auto b = a + 1;
                const auto c = b + segments + 1;
                const auto d = a + segments + 1;
                surface.indices.insert(surface.indices.end(), {a, b, c, a, c, d});
            }
        }
        mesh.surfaces.push_back(std::move(surface));
        const auto gpu = meshes_.upload(mesh);
        const auto buffers = meshes_.find(gpu.surfaces.front().handle);
        const auto entity = utils::EntityManager::get().create();
        auto* material = materials_.get_or_create("contact-fixture", "receiver", GpuMaterial{});
        // The raised occluder casts only contact shadows. A separate caster
        // ensures that Filament uploads the contact-shadow uniforms.
        filament::RenderableManager::Builder(1)
            .boundingBox({{0, 0, -6}, {24, 24, 80}}).culling(false)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, buffers.vb, buffers.ib)
            .material(0, material).receiveShadows(true).castShadows(shadow_caster)
            .screenSpaceContactShadows(true).build(engine_, entity);
        entities_.push_back(entity);
        scene_->addEntity(entity);
    }

    bool steep_;
    bool smooth_normals_;
    filament::Engine& engine_;
    TextureStore textures_;
    MaterialInstanceCache materials_;
    MeshStore meshes_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Texture* texture_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    utils::Entity camera_entity_, light_;
    std::vector<utils::Entity> entities_;
};
} // namespace

CaseResult run_contact_shadow_stability_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return CaseResult::Fail;
    bool passed = true;
    // Smooth / normal-mapped lighting can admit light from below the actual
    // surface. Those grazing rays are geometrically occluded: the receiver must
    // darken consistently, including across noncoplanar triangle boundaries.
    // Front-lit fixtures instead require no receiver acne and a raised contact
    // shadow. Testing only a flat receiver failed to catch triangular light leaks.
    // The steep plane exposes t=0 intersections that a dithered first sample
    // can skip completely by stepping beyond the finite depth slab.
    for (const auto projection : {Projection::Perspective, Projection::OffAxis, Projection::Orthographic}) {
        for (const auto configuration : {std::array{false, false, false, false},
                std::array{true, false, false, false}, std::array{true, false, true, false},
                std::array{true, true, false, false}, std::array{true, true, true, false},
                std::array{true, true, false, true}}) {
            const auto [smooth_normals, grazing_light, curved, steep] = configuration;
            ContactFixture fixture(*engine, smooth_normals, grazing_light, projection, curved, steep);
            for (int frame = 0; frame < 64; ++frame) if (!fixture.frame()) return CaseResult::Fail;
            Pixels previous, current;
            if (!fixture.frame(&previous)) return CaseResult::Fail;
            double worst_delta = 0.0;
            for (int frame = 0; frame < 16; ++frame) {
                if (!fixture.frame(&current)) return CaseResult::Fail;
                std::uint64_t delta = 0;
                std::size_t samples = 0;
                // A static, uniformly lit receiver region away from the occluder.
                for (std::size_t y = 32; y < 96; ++y) for (std::size_t x = 32; x < 96; ++x) {
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const auto index = (y * kSize + x) * 4 + channel;
                        delta += static_cast<unsigned>(std::abs(int(current[index]) - int(previous[index])));
                        ++samples;
                    }
                }
                worst_delta = std::max(worst_delta, double(delta) / double(samples));
                previous.swap(current);
            }
            fixture.set_contact(false);
            for (int frame = 0; frame < 64; ++frame) if (!fixture.frame()) return CaseResult::Fail;
            if (!fixture.frame(&current)) return CaseResult::Fail;
            std::size_t shadow_pixels = 0;
            for (std::size_t y = 96; y < 160; ++y) for (std::size_t x = 96; x < 160; ++x) {
                const auto index = (y * kSize + x) * 4;
                if (int(current[index]) - int(previous[index]) > 8) ++shadow_pixels;
            }
            std::size_t receiver_shadow_pixels = 0;
            for (std::size_t y = 32; y < 96; ++y) for (std::size_t x = 32; x < 96; ++x) {
                const auto index = (y * kSize + x) * 4;
                if (int(current[index]) - int(previous[index]) > 8) ++receiver_shadow_pixels;
            }
            std::printf("[smoke_contact_shadow_stability] projection %d, smooth normals %d, grazing light %d, curved %d, steep %d, worst static RGB delta %.4f, contact pixels %zu, receiver shadow pixels %zu\n",
                        std::to_underlying(projection), smooth_normals, grazing_light, curved, steep, worst_delta, shadow_pixels, receiver_shadow_pixels);
            const bool correct_receiver = grazing_light
                ? receiver_shadow_pixels >= 4000 : receiver_shadow_pixels == 0;
            passed = passed && worst_delta < 1.0 && correct_receiver
                && (grazing_light || shadow_pixels >= 8);
        }
    }
    return passed ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
