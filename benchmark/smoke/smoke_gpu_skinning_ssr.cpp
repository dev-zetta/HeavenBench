// Native regression for the special SSR material variant combined with SKN.
// Both production opaque and masked/A2C packages must execute as real skinned
// SSR draws, retain their visible color/alpha coverage and follow a bone move.
#include "smoke_cases.h"
#include "benchmark/microprofiler.h"
#include "dds_loader_cpu.h"
#include "rhi/filament_backend.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
#include "rhi/profile_bridge.h"
#include "rhi/screenshot_ppm.h"
#include "rhi/texture_store.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TransformManager.h>
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
constexpr const char* kTag = "[smoke_gpu_skinning_ssr]";
constexpr std::uint32_t kSize = 192;
using Pixels = std::vector<std::uint8_t>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

SharedAssetBuffer card_dds(bool masked) {
    constexpr std::uint32_t side = 32;
    dds_detail::Header header{};
    header.size = sizeof(header);
    header.flags = dds_detail::DDSD_PITCH;
    header.width = header.height = side;
    header.mipMapCount = 1;
    header.pitchOrLinearSize = side * 4;
    header.pixelFormat.size = sizeof(header.pixelFormat);
    header.pixelFormat.flags = dds_detail::DDPF_RGB | dds_detail::DDPF_ALPHAPIXELS;
    header.pixelFormat.rgbBitCount = 32;
    header.pixelFormat.rBitMask = 0xff;
    header.pixelFormat.gBitMask = 0xff00;
    header.pixelFormat.bBitMask = 0xff0000;
    header.pixelFormat.aBitMask = 0xff000000;
    auto bytes = std::make_shared<AssetBuffer>(4 + sizeof(header) + side * side * 4, 0);
    const std::uint32_t magic = dds_detail::DDS_MAGIC;
    std::memcpy(bytes->data(), &magic, sizeof(magic));
    std::memcpy(bytes->data() + sizeof(magic), &header, sizeof(header));
    for (std::uint32_t y = 0; y < side; ++y) {
        for (std::uint32_t x = 0; x < side; ++x) {
            const auto offset = 4 + sizeof(header) + (y * side + x) * 4;
            (*bytes)[offset] = masked ? 24 : 255;
            (*bytes)[offset + 1] = masked ? 255 : 24;
            (*bytes)[offset + 2] = 12;
            (*bytes)[offset + 3] = masked && x >= 12 && x < 20 ? 0 : 255;
        }
    }
    return bytes;
}

SkinnedMesh cards() {
    SkinnedMesh mesh;
    mesh.name = mesh.path = "native-ssr-skinned-cards";
    mesh.bones.push_back({"root", -1});
    mesh.num_frames = 2;
    BoneFrame rest;
    rest.position = {0, 0, 0, 1};
    rest.rotation = {1, 0, 0, 0};
    mesh.frames.assign(2, rest);
    constexpr std::array<glm::vec2, 4> corners{{{-.5f, -.75f}, {.5f, -.75f},
        {.5f, .75f}, {-.5f, .75f}}};
    for (const bool masked : {false, true}) {
        SkinnedSurface surface;
        surface.name = masked ? "masked-card" : "opaque-card";
        for (const auto p : corners) {
            SkinnedVertex vertex{};
            vertex.px = p.x + (masked ? .75f : -.75f);
            vertex.py = p.y;
            vertex.nz = 1;
            vertex.u = vertex.u1 = p.x + .5f;
            vertex.v = vertex.v1 = (p.y + .75f) / 1.5f;
            vertex.bone_indices[0] = 0;
            vertex.bone_weights[0] = 1;
            surface.vertices.push_back(vertex);
        }
        surface.indices = {0, 1, 2, 0, 2, 3};
        mesh.surfaces.push_back(std::move(surface));
    }
    return mesh;
}

struct Capture {
    Pixels pixels;
    std::uint64_t ssr_passes = 0;
};

class SkinningSsrFixture {
public:
    explicit SkinningSsrFixture(filament::Engine& engine) : engine_(engine), meshes_(engine) {}
    SkinningSsrFixture(const SkinningSsrFixture&) = delete;
    SkinningSsrFixture& operator=(const SkinningSsrFixture&) = delete;
    ~SkinningSsrFixture() {
        engine_.flushAndWait();
        bridge_.reset();
        if (view_) {
            view_->setRenderTarget(nullptr);
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
        }
        for (const auto entity : objects_) {
            if (scene_) scene_->remove(entity);
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        if (scene_ && light_) scene_->remove(light_);
        if (light_) {
            engine_.destroy(light_);
            utils::EntityManager::get().destroy(light_);
        }
        materials_.reset();
        meshes_.release(mesh_);
        textures_.reset();
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
        auto assets = AssetFileSystem::create_memory("skinning-ssr-smoke");
        require(assets.has_value(), "memory assets unavailable");
        assets_ = std::move(*assets);
        require(assets_.mount_memory("opaque.dds", card_dds(false)).has_value() &&
            assets_.mount_memory("masked.dds", card_dds(true)).has_value(), "DDS mount failed");
        textures_ = std::make_unique<TextureStore>(engine_, std::cref(assets_));
        textures_->create_placeholders();
        materials_ = std::make_unique<MaterialInstanceCache>(engine_, *textures_);
        require(materials_->load_base_materials().has_value(), "production materials unavailable");
        mesh_ = meshes_.upload_skinned(cards());
        require(mesh_.surfaces.size() == 2, "GPU-skinned card upload failed");
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        require(renderer_ && swapchain_ && scene_ && view_ && camera_, "native owners unavailable");
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera_->setExposure(16, 1.0f / 125, 100);
        camera_->setProjection(45, 1, .1, 20, filament::Camera::Fov::VERTICAL);
        camera_->lookAt({0, 0, 4}, {0, 0, 0}, {0, 1, 0});
        // Filament documents these combinable texture usage flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        depth_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        require(color_ && depth_, "capture textures unavailable");
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        require(target_, "capture target unavailable");
        view_->setScene(scene_);
        view_->setCamera(camera_);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        // SSR consumes real previous-frame color; disabling post-processing
        // would test only the option setter rather than its frame-graph pass.
        view_->setPostProcessingEnabled(true);
        view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        for (std::size_t i = 0; i < mesh_.surfaces.size(); ++i) {
            const auto& surface = mesh_.surfaces[i];
            const auto buffers = meshes_.find(surface.handle);
            const auto info = meshes_.skinned_info(surface.handle);
            require(buffers.vb && buffers.ib && info.skinned && info.bone_count == 1,
                "card did not retain production GPU bone attributes");
            GpuMaterial gpu_material;
            gpu_material.diffuse_semantic = TextureSemantic::ColorSRGB;
            gpu_material.diffuse = textures_->load(i ? "masked.dds" : "opaque.dds",
                TextureSemantic::ColorSRGB);
            gpu_material.alpha_ref = i ? .5f : 0;
            require(gpu_material.diffuse, "card texture unavailable");
            auto* material = materials_->get_or_create(mesh_.path, surface.name, gpu_material);
            require(material && material->getMaterial()->getBlendingMode() ==
                (i ? filament::BlendingMode::MASKED : filament::BlendingMode::OPAQUE),
                "opaque/masked production material routing failed");
            const auto entity = utils::EntityManager::get().create();
            objects_.push_back(entity);
            require(filament::RenderableManager::Builder(1)
                .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                    buffers.vb, buffers.ib, 0, surface.index_count)
                .material(0, material).skinning(1)
                .boundingBox({{i ? .75f : -.75f, 0, 0}, {.5f, .75f, .001f}})
                .culling(false).castShadows(false).receiveShadows(false).build(engine_, entity) ==
                    filament::RenderableManager::Builder::Success, "skinned renderable build failed");
            scene_->addEntity(entity);
        }
        light_ = utils::EntityManager::get().create();
        require(filament::LightManager::Builder(filament::LightManager::Type::DIRECTIONAL)
            .color({1, 1, 1}).intensity(50000).direction({-.3f, -.2f, -1})
            .castShadows(false).build(engine_, light_) == filament::LightManager::Builder::Success,
            "native light build failed");
        scene_->addEntity(light_);
        bridge_ = std::make_unique<ProfileBridge>(recorder_);
    }

    Capture capture(bool ssr, unsigned samples, bool moved) {
        view_->setScreenSpaceReflectionsOptions({.enabled = ssr});
        view_->setMultiSampleAntiAliasingOptions({.enabled = samples > 1,
            .sampleCount = static_cast<std::uint8_t>(samples)});
        auto& renderables = engine_.getRenderableManager();
        const auto bone = filament::math::mat4f::translation(
            moved ? filament::math::float3{.2f, .15f, 0} : filament::math::float3{});
        for (const auto object : objects_) renderables.setBones(renderables.getInstance(object), &bone, 1);
        recorder_.begin_capture(60'000'000'000ull);
        for (std::uint64_t frame = 1; frame <= 12; ++frame) {
            bridge_->update({frame, frame, 0, 0, 0, recorder_.session_id()});
            bool accepted = false;
            for (int retry = 0; retry < 12 && !accepted; ++retry) {
                if (!renderer_->beginFrame(swapchain_)) continue;
                renderer_->render(view_);
                renderer_->endFrame();
                engine_.flushAndWait();
                accepted = true;
            }
            require(accepted, "native SSR frame repeatedly rejected");
        }
        Capture capture;
        capture.pixels.resize(static_cast<std::size_t>(kSize) * kSize * 4);
        std::atomic_bool complete{false};
        renderer_->readPixels(target_, 0, 0, kSize, kSize,
            filament::backend::PixelBufferDescriptor(capture.pixels.data(), capture.pixels.size(),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &complete));
        engine_.flushAndWait();
        require(complete.load(), "native color readback incomplete");
        const auto snapshot = recorder_.snapshot();
        recorder_.set_active(false);
        for (const auto& sample : snapshot.gpu_samples) {
            const bool terminal = sample.status == profile::SampleStatus::Available ||
                sample.status == profile::SampleStatus::Unsupported;
            if (sample.name != "SSR Pass" || !terminal ||
                sample.context.view_id != reinterpret_cast<std::uintptr_t>(view_)) continue;
            // Both distinct materials must be submitted, not just a clear pass.
            require(sample.command_count == 2 && sample.primitive_count == 4,
                "SSR did not submit both opaque and masked GPU-skinned cards");
            ++capture.ssr_passes;
        }
        require(ssr ? capture.ssr_passes > 0 : capture.ssr_passes == 0,
            "SSR enabled/disabled pass control was not exercised");
        return capture;
    }

private:
    filament::Engine& engine_;
    AssetFileSystem assets_;
    std::unique_ptr<TextureStore> textures_;
    std::unique_ptr<MaterialInstanceCache> materials_;
    MeshStore meshes_;
    GpuMesh mesh_;
    profile::Recorder recorder_;
    std::unique_ptr<ProfileBridge> bridge_;
    std::vector<utils::Entity> objects_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    utils::Entity camera_entity_, light_;
};

std::array<double, 3> probe(const Pixels& pixels, double world_x, double world_y) {
    // Independent perspective projection for the fixed 45-degree camera.
    const double world_per_ndc = 4 * std::tan(3.14159265358979323846 / 8);
    const int px = static_cast<int>((world_x / world_per_ndc + 1) * kSize / 2);
    const int py = static_cast<int>((world_y / world_per_ndc + 1) * kSize / 2);
    std::array<double, 3> mean{};
    for (int y = py - 1; y <= py + 1; ++y) {
        for (int x = px - 1; x <= px + 1; ++x) {
            require(x >= 0 && y >= 0 && x < static_cast<int>(kSize) &&
                y < static_cast<int>(kSize), "pixel oracle outside target");
            const auto index = (static_cast<std::size_t>(y) * kSize + static_cast<std::size_t>(x)) * 4;
            for (std::size_t c = 0; c < mean.size(); ++c) mean[c] += pixels[index + c] / 9.0;
        }
    }
    return mean;
}

void check_pixels(const Capture& capture, bool moved) {
    const double dx = moved ? .2 : 0, dy = moved ? .15 : 0;
    const auto red = probe(capture.pixels, -.75 + dx, dy);
    const auto green = probe(capture.pixels, .45 + dx, dy);
    const auto hole = probe(capture.pixels, .75 + dx, dy);
    const auto background = probe(capture.pixels, 0, -.95);
    require(red[0] > 12 && red[0] > 2 * red[1] && red[0] > 2 * red[2],
        "opaque GPU-skinned color is absent or corrupt");
    require(green[1] > 12 && green[1] > 2 * green[0] && green[1] > 2 * green[2],
        "masked GPU-skinned color is absent or corrupt");
    require(*std::max_element(hole.begin(), hole.end()) <= 2 &&
        *std::max_element(background.begin(), background.end()) <= 2,
        "masked hole or clear background lost coverage");
    const auto old_edge = probe(capture.pixels, -1.15, 0);
    const auto new_edge = probe(capture.pixels, -.15, .15);
    require(moved ? old_edge[0] <= 2 && new_edge[0] > 12 :
        old_edge[0] > 12 && new_edge[0] <= 2,
        "bone translation did not move actual raster coverage");
}

void same_pixels(const Capture& actual, const Capture& reference, const char* label) {
    unsigned maximum = 0;
    for (std::size_t i = 0; i < actual.pixels.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) maximum = std::max(maximum,
            static_cast<unsigned>(std::abs(int(actual.pixels[i + c]) - int(reference.pixels[i + c]))));
    }
    std::printf("%s %s max RGB8 difference=%u\n", kTag, label, maximum);
    // There is no reflected source behind these front-facing cards. SSR must
    // preserve their directly lit appearance; allow only output quantization.
    require(maximum <= 3, "SSR/return control changed directly lit pixels");
}

void save_pixels(const Capture& capture, const std::string& name) {
    const char* directory = std::getenv("HEAVEN_SMOKE_GPU_SKINNING_SSR_OUTPUT_DIR");
    if (!directory || !*directory) return;
    const auto path = std::filesystem::path(directory) / (name + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    require(write_screenshot_ppm(path, kSize, kSize, capture.pixels).has_value(), "capture save failed");
}
} // namespace

CaseResult run_gpu_skinning_ssr_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Skip;
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return CaseResult::Skip;
    try {
        // Warm Engine-owned lazy post-process programs before the owner oracle.
        {
            SkinningSsrFixture warmup(*engine);
            warmup.init();
            static_cast<void>(warmup.capture(true, 4, false));
        }
        engine->flushAndWait();
        const auto owners = [&] {
            return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
                engine->getBufferObjectCount(), engine->getViewCount(), engine->getTextureCount(),
                engine->getMaterialCount(), engine->getSceneCount(), engine->getRenderTargetCount(),
                engine->getSwapChainCount(), engine->getSkinningBufferCount(),
                engine->getRenderableManager().getComponentCount(),
                engine->getTransformManager().getComponentCount(),
                engine->getLightManager().getComponentCount()};
        };
        const auto baseline = owners();
        {
            SkinningSsrFixture fixture(*engine);
            fixture.init();
            for (const unsigned samples : {1u, 4u}) {
                const auto off = fixture.capture(false, samples, false);
                const auto on = fixture.capture(true, samples, false);
                const auto moved = fixture.capture(true, samples, true);
                const auto moved_off = fixture.capture(false, samples, true);
                const auto restored = fixture.capture(true, samples, false);
                check_pixels(off, false);
                check_pixels(on, false);
                check_pixels(moved, true);
                check_pixels(moved_off, true);
                check_pixels(restored, false);
                same_pixels(on, off, "SSR-on vs off");
                same_pixels(moved, moved_off, "moved SSR-on vs off");
                same_pixels(restored, on, "restored bones/SSR");
                const auto suffix = std::to_string(samples) + "x";
                save_pixels(off, "off-" + suffix);
                save_pixels(on, "on-" + suffix);
                save_pixels(moved, "moved-" + suffix);
                save_pixels(restored, "restored-" + suffix);
                std::printf("%s MSAA=%u completed SSR passes=%llu opaque/masked/bones/return PASS\n",
                    kTag, samples, static_cast<unsigned long long>(on.ssr_passes));
            }
        }
        engine->flushAndWait();
        require(owners() == baseline, "native SSR/skinning owners did not return to baseline");
        return CaseResult::Pass;
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        return CaseResult::Fail;
    }
}
} // namespace heaven::smoke
