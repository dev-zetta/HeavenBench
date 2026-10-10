/*
 * HeavenBench — glass refraction screen-data-fallback /
 *                     disabled-parity smoke case (renderer-linked, GATED)
 *
 * the screen-space refraction feature
 * routes glass-flagged surfaces through the refractive
 * `heaven_glass` material ONLY when the refraction feature is enabled AND the
 * screen-space scene color it samples is available this frame. This case
 * locks down the two parity branches that keep the Shipping_Look:
 *
 *   -  (screen-data fallback): WHEN the screen-space scene color needed
 *     for refraction is UNAVAILABLE for a frame, glass surfaces fall back to
 *     the opaque `heaven_lit` material for that frame and a diagnostic is
 *     emitted — even with the feature enabled.
 *   -  (disabled parity): WHILE the refraction feature is DISABLED,
 *     glass surfaces render with their opaque Shipping_Look appearance,
 *     pixel-matching the never-enabled path.
 *
 * WHY THIS LIVES IN THE SMOKE HARNESS (not heaven_benchmark_tests):
 * MaterialInstanceCache builds real filament::Material / MaterialInstance
 * objects from the embedded compiled material packages and resolves textures
 * through a real TextureStore. The headless RapidCheck target
 * (heaven_benchmark_tests) deliberately links NEITHER Filament NOR SDL, so it
 * cannot build a Material. This target links the full renderer, so it can —
 * matching the "GPU smoke tests (gated)" testing strategy.
 *
 * HOW WE "FORCE THE SCENE COLOR UNAVAILABLE" deterministically without a GPU
 * dependency: MaterialInstanceCache exposes the decision through a tiny
 * protected virtual seam, `refraction_scene_color_available()`, whose default
 * is the production feature-level check (FEATURE_LEVEL_1). This case subclasses
 * the cache and overrides the seam to return false, exercising the real
 * fallback branch in get_or_create() — exactly the
 * WetReflectionsFeature::probe_capability pattern in
 * smoke_wet_reflections_capability.cpp. A sibling subclass forces the seam true
 * as a positive control, proving the opaque fallback is a genuine difference
 * (refraction routing CAN reach the glass material) and not "glass is always
 * opaque".
 *
 * HERMETIC ENGINE: the case stands up its OWN no-op (NOOP) Filament engine +
 * TextureStore + MaterialInstanceCache, so the assertion does not depend on (or
 * perturb) the shared backend the harness brought up. NOOP needs no
 * GPU/display. It SKIPs (exit 0 via the harness) if it cannot create its NOOP
 * engine or load the base materials.
 *
 * To distinguish a glass vs a lit MaterialInstance we read the base material
 * name back through MaterialInstance::getMaterial()->getName() ("heaven_glass"
 * vs "heaven_lit").
 *
 * Native coverage additionally uses the shared hardware engine and production
 * heaven_glass with generated DDS inputs: no-light night/day/restored/oblique
 * emission, and reflection-only red/blue IBL with zero diffuse irradiance.
 * A FLOAT32 target measures these signals without the lens-flare feature.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 */

#include "smoke_cases.h"

#include "dds_loader_cpu.h"
#include "rhi/filament_backend.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
#include "rhi/screenshot_ppm.h"
#include "rhi/texture_store.h"
#include "rhi/rhi.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_glass_refraction]";

// Test-only subclass that forces the screen-space-scene-color seam to
// report UNAVAILABLE, regardless of the engine's real feature level. This is
// the whole point of the protected virtual seam: it lets the fallback branch
// be exercised on hardware that would otherwise always be available.
class ForcedSceneColorUnavailable final : public MaterialInstanceCache {
public:
    using MaterialInstanceCache::MaterialInstanceCache;
protected:
    bool refraction_scene_color_available() const override { return false; }
};

// Positive-control subclass: forces the seam AVAILABLE so glass routing can
// reach the refractive material. Proves the opaque fallback above is a real
// branch difference, not "glass is always opaque".
class ForcedSceneColorAvailable final : public MaterialInstanceCache {
public:
    using MaterialInstanceCache::MaterialInstanceCache;
protected:
    bool refraction_scene_color_available() const override { return true; }
};

// A glass-flagged GpuMaterial (the xml0 classifier sets `glass` on windows /
// airship glass / lab equipment —). two_sided/alpha_ref stay at their
// opaque defaults so the fallback resolves to the standard `heaven_lit` base
// material (not the two-sided variant).
GpuMaterial make_glass_material() {
    GpuMaterial m;
    m.glass = true;
    return m;
}

// True iff the instance's base material has the given name.
bool material_is(const filament::MaterialInstance* mi, const char* name) {
    if (!mi) return false;
    const char* mat_name = mi->getMaterial()->getName();
    return mat_name != nullptr && std::strcmp(mat_name, name) == 0;
}

constexpr std::uint32_t kSize = 128;
using Pixels = std::vector<float>;
using Float3 = filament::math::float3;

SharedAssetBuffer white_dds() {
    dds_detail::Header header{};
    header.size = sizeof(header);
    header.flags = dds_detail::DDSD_PITCH;
    header.width = header.height = header.mipMapCount = 1;
    header.pitchOrLinearSize = 4;
    header.pixelFormat.size = sizeof(header.pixelFormat);
    header.pixelFormat.flags = dds_detail::DDPF_RGB | dds_detail::DDPF_ALPHAPIXELS;
    header.pixelFormat.rgbBitCount = 32;
    header.pixelFormat.rBitMask = 0xff;
    header.pixelFormat.gBitMask = 0xff00;
    header.pixelFormat.bBitMask = 0xff0000;
    header.pixelFormat.aBitMask = 0xff000000;
    auto bytes = std::make_shared<AssetBuffer>(4 + sizeof(header) + 4, 255);
    const std::uint32_t magic = dds_detail::DDS_MAGIC;
    std::memcpy(bytes->data(), &magic, sizeof(magic));
    std::memcpy(bytes->data() + sizeof(magic), &header, sizeof(header));
    return bytes;
}

// Production material routing and samplers are exercised against generated
// input bytes. There is no flare, light entity, visible sky or background object
// in this fixture. Emission and reflected illumination are varied separately.
class GlassLightingFixture {
public:
    explicit GlassLightingFixture(filament::Engine& engine)
        : engine_(engine), meshes_(engine) {}
    GlassLightingFixture(const GlassLightingFixture&) = delete;
    GlassLightingFixture& operator=(const GlassLightingFixture&) = delete;
    ~GlassLightingFixture() {
        engine_.flushAndWait();
        if (view_) {
            view_->setRenderTarget(nullptr);
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
        }
        if (scene_) {
            scene_->setIndirectLight(nullptr);
            if (glass_) scene_->remove(glass_);
        }
        if (glass_) {
            engine_.destroy(glass_);
            utils::EntityManager::get().destroy(glass_);
        }
        // Renderables borrow both instances and buffers; retire them first.
        cache_.reset();
        meshes_.release(mesh_);
        textures_.reset();
        engine_.destroy(indirect_);
        for (auto* cube : cubemaps_) engine_.destroy(cube);
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

    void init() {
        auto assets = AssetFileSystem::create_memory("glass-lighting-smoke");
        if (!assets) throw std::runtime_error(assets.error().message);
        assets_ = std::move(*assets);
        if (!assets_.mount_memory("white.dds", white_dds()))
            throw std::runtime_error("generated glass DDS mount failed");
        textures_ = std::make_unique<TextureStore>(engine_, std::cref(assets_));
        textures_->create_placeholders();
        cache_ = std::make_unique<MaterialInstanceCache>(engine_, *textures_);
        if (auto loaded = cache_->load_base_materials(); !loaded)
            throw std::runtime_error(loaded.error().message);
        cache_->set_refraction_enabled(true);
        GpuMaterial lamp = make_glass_material();
        lamp.diffuse = textures_->load("white.dds", TextureSemantic::ColorSRGB);
        lamp.emission_scale = 4;
        lamp.emission_color[0] = 1;
        lamp.emission_color[1] = 0.6f;
        lamp.emission_color[2] = 0.2f;
        if (!lamp.diffuse) throw std::runtime_error("generated glass DDS upload failed");
        // First-frame night state, daytime cache insertion and reload all use
        // the same production owner. No stale pointer may survive clear_instances.
        cache_->update_lamp_emission(21);
        material_ = cache_->get_or_create("lamp.mesh", "lamp_glass", lamp);
        require(material_is(material_, "heaven_glass"), "native lamp was not refractive");
        // Filament stores bool uniforms as uint32_t and exports that getter.
        require(material_->getParameter<std::uint32_t>("hasEmissive") == 1 &&
            material_->getParameter<float>("emissionScale") == 4,
            "first-frame night lamp lost its diffuse-fallback emission");
        cache_->update_lamp_emission(12);
        require(material_->getParameter<float>("emissionScale") == 0,
            "refractive lamp emission did not turn off during day");
        auto* second = cache_->get_or_create("second.mesh", "lantern_glass", lamp);
        require(second && second->getParameter<float>("emissionScale") == 0,
            "new refractive lamp ignored the current daytime state");
        auto* window = cache_->get_or_create("window.mesh", "airship_glass", lamp);
        require(window && window->getParameter<std::uint32_t>("hasEmissive") == 0,
            "non-lamp refractive glass gained synthetic emission");
        auto authored = lamp;
        authored.emission = lamp.diffuse;
        auto* unmarked = cache_->get_or_create("unmarked.mesh", "lab_glass", authored);
        require(unmarked && unmarked->getParameter<std::uint32_t>("hasEmissive") == 0,
            "unmarked refractive glass emitted from a bound default core map");
        authored.emission_light_source = true;
        auto* authored_glass = cache_->get_or_create("authored.mesh", "lab_glass", authored);
        require(authored_glass && authored_glass->getParameter<std::uint32_t>("hasEmissive") == 1 &&
            authored_glass->getParameter<float>("emissionScale") == 4,
            "authored non-lamp glass emission was suppressed by the lamp schedule");
        cache_->clear_instances();
        cache_->update_lamp_emission(12);
        material_ = cache_->get_or_create("lamp.mesh", "lamp_glass", lamp);
        require(material_ && material_->getParameter<float>("emissionScale") == 0,
            "refractive reload lost the current daytime state");
        cache_->update_lamp_emission(21);
        require(material_->getParameter<float>("emissionScale") == 4 &&
            material_->getParameter<float>("transmission") > 0 &&
            material_->getParameter<float>("ior") > 1,
            "restored lamp lost emission or the refractive physical material");

        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        if (!renderer_ || !swapchain_ || !scene_ || !view_ || !camera_)
            throw std::runtime_error("glass fixture object creation failed");
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera_->setExposure(1, 1.0f / 60, 100);
        camera_->setProjection(filament::Camera::Projection::ORTHO, -1.5, 1.5, -1.5, 1.5, 0.1, 20);
        camera_->lookAt({0, 0, 4}, {0, 0, 0}, {0, 1, 0});
        // Filament permits combining these documented texture usage flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).usage(usage).build(engine_);
        depth_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        if (!color_ || !depth_) throw std::runtime_error("glass capture texture creation failed");
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        if (!target_) throw std::runtime_error("glass capture target creation failed");
        view_->setScene(scene_);
        view_->setCamera(camera_);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);

        Surface surface;
        surface.name = "lamp_glass";
        constexpr std::array<glm::vec2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
        for (const auto p : corners) {
            Vertex vertex{};
            vertex.px = p.x;
            vertex.py = p.y;
            vertex.nz = 1;
            vertex.u = vertex.u1 = (p.x + 1) * 0.5f;
            vertex.v = vertex.v1 = (p.y + 1) * 0.5f;
            vertex.tx = vertex.tw = 1;
            surface.vertices.push_back(vertex);
        }
        surface.indices = {0, 1, 2, 0, 2, 3};
        Mesh mesh;
        mesh.name = mesh.path = "glass-lighting-quad";
        mesh.surfaces.push_back(std::move(surface));
        mesh_ = meshes_.upload(mesh);
        require(mesh_.surfaces.size() == 1, "glass fixture mesh upload failed");
        const auto buffers = meshes_.find(mesh_.surfaces.front().handle);
        require(buffers.vb && buffers.ib, "glass fixture buffer lookup failed");
        glass_ = utils::EntityManager::get().create();
        require(filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                buffers.vb, buffers.ib, 0, mesh_.surfaces.front().index_count)
            .material(0, material_).boundingBox({{0, 0, 0}, {1, 1, 0.001f}})
            .culling(true).castShadows(false).receiveShadows(false).build(engine_, glass_) ==
                filament::RenderableManager::Builder::Success,
            "glass fixture renderable creation failed");
        scene_->addEntity(glass_);
        cubemaps_[0] = cube({1, 0, 0});
        cubemaps_[1] = cube({0, 0, 1});
    }

    Pixels capture(float hour, int reflection = -1, bool oblique = false) {
        cache_->update_lamp_emission(hour);
        camera_->lookAt(oblique ? filament::math::double3{1.5, 0, 4} :
            filament::math::double3{0, 0, 4}, {0, 0, 0}, {0, 1, 0});
        scene_->setIndirectLight(nullptr);
        engine_.destroy(indirect_);
        indirect_ = nullptr;
        if (reflection >= 0) {
            // Zero diffuse irradiance, no visible sky and a black transmission
            // background isolate environment reflection from additive emission.
            const std::array<Float3, 9> zero_sh{};
            indirect_ = filament::IndirectLight::Builder().irradiance(3, zero_sh.data())
                .reflections(cubemaps_.at(static_cast<std::size_t>(reflection)))
                .intensity(200).build(engine_);
            require(indirect_ != nullptr, "reflection-only IBL creation failed");
            scene_->setIndirectLight(indirect_);
        }
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
        require(complete.load(), "native glass FLOAT readback incomplete");
        return pixels;
    }

private:
    static void require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
    filament::Texture* cube(Float3 color) {
        constexpr std::uint32_t side = 64;
        // Filament permits combining these documented texture usage flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::SAMPLEABLE) |
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::UPLOADABLE) |
            std::to_underlying(filament::Texture::Usage::GEN_MIPMAPPABLE));
        auto* texture = filament::Texture::Builder().width(side).height(side).levels(7)
            .sampler(filament::Texture::Sampler::SAMPLER_CUBEMAP)
            .format(filament::Texture::InternalFormat::RGBA16F).usage(usage).build(engine_);
        require(texture != nullptr, "reflection cubemap creation failed");
        // Mirror the production sky probe's level-zero FLOAT upload and GPU
        // mip generation. Constant radiance is already convolved for every
        // roughness. Tiny FLOAT-to-HALF staging rows are unsuitable for the
        // Vulkan driver path and must not corrupt this independent oracle.
        constexpr std::size_t count = static_cast<std::size_t>(side) * side * 6 * 4;
        auto pixels = std::make_unique<float[]>(count);
        for (std::size_t i = 0; i < count; i += 4) {
            pixels[i] = color.x;
            pixels[i + 1] = color.y;
            pixels[i + 2] = color.z;
            pixels[i + 3] = 1;
        }
        texture->setImage(engine_, 0, 0, 0, 0, side, side, 6,
            filament::backend::PixelBufferDescriptor(pixels.release(), count * sizeof(float),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::FLOAT,
                [](void* buffer, std::size_t, void*) { delete[] static_cast<float*>(buffer); }));
        texture->generateMipmaps(engine_);
        return texture;
    }
    void draw() {
        for (int retry = 0; retry < 12; ++retry) {
            if (!renderer_->beginFrame(swapchain_)) continue;
            renderer_->render(view_);
            renderer_->endFrame();
            engine_.flushAndWait();
            return;
        }
        throw std::runtime_error("native glass frame repeatedly rejected");
    }

    filament::Engine& engine_;
    AssetFileSystem assets_;
    std::unique_ptr<TextureStore> textures_;
    std::unique_ptr<MaterialInstanceCache> cache_;
    MeshStore meshes_;
    GpuMesh mesh_;
    filament::MaterialInstance* material_ = nullptr;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    filament::IndirectLight* indirect_ = nullptr;
    std::array<filament::Texture*, 2> cubemaps_{};
    utils::Entity camera_entity_, glass_;
};

std::array<double, 3> mean_glass(const Pixels& pixels) {
    std::array<double, 3> mean{};
    constexpr std::uint32_t lo = 40, hi = 88;
    for (std::uint32_t y = lo; y < hi; ++y) {
        for (std::uint32_t x = lo; x < hi; ++x) {
            const auto index = (static_cast<std::size_t>(y) * kSize + x) * 4;
            for (std::size_t c = 0; c < mean.size(); ++c) {
                const auto value = pixels[index + c];
                if (!std::isfinite(value) || value < 0)
                    throw std::runtime_error("nonfinite or negative glass radiance");
                mean[c] += value;
            }
        }
    }
    for (auto& value : mean) value /= static_cast<double>((hi - lo) * (hi - lo));
    return mean;
}

bool save_glass(const char* name, const Pixels& pixels) {
    const char* output = std::getenv("HEAVEN_SMOKE_GLASS_OUTPUT_DIR");
    if (!output || !*output) return true;
    const auto path = std::filesystem::path(output) / (std::string(name) + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    std::vector<std::uint8_t> bytes(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(std::lround(std::clamp(pixels[i], 0.0f, 1.0f) * 255.0f));
    return write_screenshot_ppm(path, kSize, kSize, bytes).has_value();
}

bool check_native_lighting(const CaseContext& ctx) {
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return false;
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return false;
    const auto owners = [&] {
        return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
            engine->getViewCount(), engine->getTextureCount(), engine->getMaterialCount(),
            engine->getSceneCount(), engine->getRenderTargetCount(), engine->getSwapChainCount(),
            engine->getIndirectLightCount()};
    };
    bool valid = true;
    try {
        // First use can create Engine-owned lazy refraction programs. Warm them
        // in a completely retired fixture before checking borrower cleanup.
        {
            GlassLightingFixture warmup(*engine);
            warmup.init();
            static_cast<void>(warmup.capture(12, 0));
        }
        engine->flushAndWait();
        const auto baseline = owners();
        {
            GlassLightingFixture fixture(*engine);
            fixture.init();
            const auto night = fixture.capture(21);
            const auto day = fixture.capture(12);
            const auto restored = fixture.capture(21);
            const auto oblique = fixture.capture(21, -1, true);
            const auto red = fixture.capture(12, 0);
            const auto blue = fixture.capture(12, 1);
            const auto dark_return = fixture.capture(12);
            const auto n = mean_glass(night), d = mean_glass(day), r = mean_glass(restored),
                o = mean_glass(oblique), a = mean_glass(red), b = mean_glass(blue),
                z = mean_glass(dark_return);
            bool stable = true;
            for (std::size_t channel = 0; channel < n.size(); ++channel) {
                stable &= std::abs(r[channel] - n[channel]) <=
                    std::max(1e-7, std::abs(n[channel]) * 0.001);
                stable &= std::abs(o[channel] - n[channel]) <=
                    std::max(1e-7, std::abs(n[channel]) * 0.001);
            }
            const bool emission = n[0] > 1e-5 && n[0] > 2 * n[2] &&
                d[0] + d[1] + d[2] < 1e-6 && z[0] + z[1] + z[2] < 1e-6 && stable;
            const bool reflection = a[0] > 1e-5 && a[0] > 10 * a[2] &&
                b[2] > 1e-5 && b[2] > 10 * b[0];
            valid &= emission && reflection;
            std::printf("%s native emission nightRGB=(%.7g,%.7g,%.7g) day=%.7g "
                "returnDark=%.7g restored/oblique=%s: %s; reflection-only "
                "redRGB=(%.7g,%.7g,%.7g) blueRGB=(%.7g,%.7g,%.7g): %s\n",
                kTag, n[0], n[1], n[2], d[0] + d[1] + d[2], z[0] + z[1] + z[2],
                stable ? "PASS" : "FAIL", emission ? "PASS" : "FAIL",
                a[0], a[1], a[2], b[0], b[1], b[2], reflection ? "PASS" : "FAIL");
            valid &= save_glass("night-emission", night) && save_glass("day-unlit", day) &&
                save_glass("restored-emission", restored) && save_glass("oblique-emission", oblique) &&
                save_glass("red-reflection", red) && save_glass("blue-reflection", blue) &&
                save_glass("returned-dark", dark_return);
        }
        engine->flushAndWait();
        if (owners() != baseline) {
            std::printf("%s FAIL: native fixture resources did not return to baseline\n", kTag);
            valid = false;
        }
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        valid = false;
    }
    return valid;
}

} // namespace

CaseResult run_glass_refraction_case(const CaseContext& ctx) {
    // Hermetic NOOP engine: no GPU/display needed, isolated from the shared
    // backend. If even NOOP can't be created, SKIP (exit 0 via the harness).
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine\n", kTag);
        return CaseResult::Skip;
    }

    bool ok = true;

    // Shared TextureStore (placeholders only — no DDS on disk needed; the cache
    // falls back to the 1x1 placeholders for every unassigned map).
    {
        TextureStore textures(*engine);
        textures.create_placeholders();

        const GpuMaterial glass_mat = make_glass_material();

        // ──: screen color UNAVAILABLE + feature ENABLED ⇒ opaque ──
        // With refraction enabled but the scene color forced unavailable, a
        // glass surface must fall back to the opaque heaven_lit material for
        // that build and the cache must emit ONE diagnostic. (The harness does
        // not capture stdout, so the single-diagnostic behaviour is covered by
        // code inspection / the latch in the cache; here we assert the
        // observable opaque-fallback outcome.)
        {
            ForcedSceneColorUnavailable cache(*engine, textures);
            if (auto r = cache.load_base_materials(); !r) {
                std::printf("%s SKIP: base materials failed to build on NOOP "
                            "(%s)\n", kTag, r.error().message.c_str());
                ok = false; // fall through to teardown, report SKIP below
            } else {
                cache.set_refraction_enabled(true);
                auto* mi = cache.get_or_create("airship.mesh", "airship_glass", glass_mat);
                if (material_is(mi, "heaven_glass")) {
                    std::printf("%s FAIL: glass routed to refractive material "
                                "while scene color is unavailable (expected "
                                "opaque heaven_lit fallback, Req 5.5)\n", kTag);
                    ok = false;
                } else if (!material_is(mi, "heaven_lit")) {
                    std::printf("%s FAIL: unavailable-scene-color glass did not "
                                "resolve to heaven_lit (got '%s')\n", kTag,
                                mi ? mi->getMaterial()->getName() : "<null>");
                    ok = false;
                }
            }
        }

        // ──: feature DISABLED ⇒ opaque (Shipping_Look parity) ──
        // With refraction disabled the glass flag is ignored entirely (the
        // availability seam is never consulted) and the surface resolves to the
        // opaque heaven_lit material, pixel-matching the never-enabled path.
        {
            // Disabled means the seam doesn't matter; use the production cache
            // (refraction_enabled_ defaults false) so this mirrors a flagless
            // run exactly.
            MaterialInstanceCache cache(*engine, textures);
            if (auto r = cache.load_base_materials(); !r) {
                std::printf("%s SKIP: base materials failed to build on NOOP "
                            "(%s)\n", kTag, r.error().message.c_str());
                ok = false;
            } else {
                // refraction disabled (default)
                auto* mi = cache.get_or_create("airship.mesh", "airship_glass", glass_mat);
                if (!material_is(mi, "heaven_lit")) {
                    std::printf("%s FAIL: disabled-refraction glass did not "
                                "resolve to opaque heaven_lit (got '%s', "
                                "Req 5.4)\n", kTag,
                                mi ? mi->getMaterial()->getName() : "<null>");
                    ok = false;
                }
            }
        }

        // ── Positive control: ENABLED + scene color AVAILABLE ⇒ refractive ──
        // Proves the opaque fallbacks above are genuine branch differences: the
        // refraction routing CAN reach the heaven_glass material when both the
        // feature is on and the scene color is available.
        {
            ForcedSceneColorAvailable cache(*engine, textures);
            if (auto r = cache.load_base_materials(); !r) {
                std::printf("%s SKIP: base materials failed to build on NOOP "
                            "(%s)\n", kTag, r.error().message.c_str());
                ok = false;
            } else {
                cache.set_refraction_enabled(true);
                auto* mi = cache.get_or_create("airship.mesh", "airship_glass", glass_mat);
                if (!material_is(mi, "heaven_glass")) {
                    std::printf("%s FAIL: enabled+available glass did not route "
                                "to the refractive heaven_glass material (got "
                                "'%s') — the unavailable/disabled fallbacks "
                                "would be vacuous\n", kTag,
                                mi ? mi->getMaterial()->getName() : "<null>");
                    ok = false;
                }
            }
        }
        // TextureStore destroyed here (before the engine), then the caches'
        // destructors already ran at their block scope exits.
    }

    filament::Engine::destroy(&engine);

    if (!ok || !check_native_lighting(ctx)) return CaseResult::Fail;

    std::printf("%s scene-color-unavailable ⇒ opaque heaven_lit (Req 5.5); "
                "refraction-disabled ⇒ opaque heaven_lit (Req 5.4); "
                "enabled+available ⇒ refractive heaven_glass (control)\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
