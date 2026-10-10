// Native submission and pixel oracles for production instance-group culling.
// The public Platform statistic reports executed RenderPass command spans,
// after visibility and automatic merging; scene membership alone cannot do so.
#include "smoke_cases.h"
#include "rhi/filament_backend.h"
#include "rhi/scene_builder.h"
#include "rhi/screenshot_ppm.h"

#include <filament/Box.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Frustum.h>
#include <filament/LightManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/PixelBufferDescriptor.h>
#include <backend/Platform.h>
#include <utils/EntityManager.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_culling]";
constexpr std::uint32_t kSize = 192;
using Pixels = std::vector<std::uint8_t>;

struct SubmissionCounter {
    std::atomic<std::uint64_t> commands{0};
};

struct Capture {
    Pixels pixels;
    std::uint64_t commands = 0;
    std::int32_t visible_renderables = -1;
};

Mesh quad_mesh(const char* name, float half_extent) {
    Mesh mesh;
    mesh.name = name;
    mesh.path = name;
    Surface surface;
    surface.name = "culling_quad";
    constexpr std::array<glm::vec2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (const auto corner : corners) {
        Vertex vertex{};
        vertex.px = corner.x * half_extent;
        vertex.py = corner.y * half_extent;
        vertex.nz = 1;
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

Mesh cube_mesh(const char* name, float half_extent) {
    Mesh mesh;
    mesh.name = name;
    mesh.path = name;
    Surface surface;
    surface.name = "culling_cube";
    struct Face {
        glm::vec3 normal;
        glm::vec3 tangent;
        glm::vec3 bitangent;
    };
    constexpr std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
        {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
        {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}
    }};
    constexpr std::array<glm::vec2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    surface.vertices.reserve(faces.size() * corners.size());
    surface.indices.reserve(faces.size() * 6);
    for (const auto& face : faces) {
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (const auto corner : corners) {
            const auto position = half_extent *
                (face.normal + corner.x * face.tangent + corner.y * face.bitangent);
            Vertex vertex{};
            vertex.px = position.x;
            vertex.py = position.y;
            vertex.pz = position.z;
            vertex.nx = face.normal.x;
            vertex.ny = face.normal.y;
            vertex.nz = face.normal.z;
            vertex.u = (corner.x + 1) * 0.5f;
            vertex.v = (corner.y + 1) * 0.5f;
            vertex.tx = face.tangent.x;
            vertex.ty = face.tangent.y;
            vertex.tz = face.tangent.z;
            vertex.tw = 1;
            surface.vertices.push_back(vertex);
        }
        for (const std::uint32_t local : {0u, 1u, 2u, 0u, 2u, 3u}) {
            surface.indices.push_back(first + local);
        }
    }
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

Instance placement(int mesh, float x, float z = 0) {
    return {mesh, glm::translate(glm::mat4(1), glm::vec3(x, 0, z))};
}

std::size_t bright_pixels(const Pixels& pixels) {
    std::size_t result = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] > 20 || pixels[i + 1] > 20 || pixels[i + 2] > 20) ++result;
    }
    return result;
}

bool save_capture(std::string_view name, const Pixels& pixels) {
    const char* directory = std::getenv("HEAVEN_SMOKE_CULLING_OUTPUT_DIR");
    if (!directory || !*directory) return true;
    const auto path = std::filesystem::path(directory) / (std::string(name) + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    return write_screenshot_ppm(path.string(), kSize, kSize, pixels).has_value();
}

class CullingFixture {
public:
    explicit CullingFixture(filament::Engine& engine)
        : engine_(engine), textures_(engine), materials_(engine, textures_), meshes_(engine) {}
    CullingFixture(const CullingFixture&) = delete;
    CullingFixture& operator=(const CullingFixture&) = delete;
    ~CullingFixture() {
        engine_.flushAndWait();
        if (auto* platform = engine_.getPlatform()) {
            platform->setDebugUpdateStatFunc(filament::backend::Platform::DebugUpdateStatFunc{});
        }
        builder_.clear(core_);
        if (view_) {
            view_->setRenderTarget(nullptr);
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
        }
        if (scene_ && light_) scene_->remove(light_);
        if (light_) {
            engine_.destroy(light_);
            utils::EntityManager::get().destroy(light_);
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

    void init() {
        // Set the core engine before any fallible initialization, so even an
        // early failure can safely clear an empty SceneBuilder during teardown.
        core_.engine = &engine_;
        textures_.create_placeholders();
        if (!materials_.load_base_materials()) throw std::runtime_error("material load failed");
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        if (!renderer_ || !swapchain_ || !scene_ || !view_ || !camera_) {
            throw std::runtime_error("native fixture object creation failed");
        }
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera_->setProjection(filament::Camera::Projection::ORTHO, -2, 2, -2, 2, 0.1, 100);
        camera_->lookAt({0, 0, 10}, {0, 0, 0}, {0, 1, 0});
        camera_->setExposure(16, 1.0f / 125, 100);
        // Filament permits combining its documented texture usage bits.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(usage).build(engine_);
        depth_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        if (!color_ || !depth_ || !target_) throw std::runtime_error("capture target creation failed");
        view_->setScene(scene_);
        view_->setCamera(camera_);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        core_ = {&engine_, scene_, view_, renderer_, camera_};
        light_ = utils::EntityManager::get().create();
        filament::LightManager::ShadowOptions shadows;
        shadows.mapSize = 1024;
        shadows.shadowCascades = 1;
        shadows.screenSpaceContactShadows = false;
        // Diagnostic control: use stable orthogonal fitting and remove the
        // receiver offsets without changing the blocker or pixel oracle.
        shadows.stable = true;
        shadows.lispsm = false;
        shadows.normalBias = 0;
        shadows.constantBias = 0;
        if (filament::LightManager::Builder(filament::LightManager::Type::DIRECTIONAL)
                .color({1, 1, 1}).intensity(50000).direction({0, 0, -1})
                .castShadows(true).shadowOptions(shadows).build(engine_, light_) !=
                filament::LightManager::Builder::Success) {
            throw std::runtime_error("fixture light creation failed");
        }
        scene_->addEntity(light_);
        auto* platform = engine_.getPlatform();
        if (!platform) throw std::runtime_error("native platform unavailable");
        platform->setDebugUpdateStatFunc([counter = counter_](const char* key,
                std::uint64_t value, const utils::CString&) {
            if (std::string_view(key) == "filament.renderer.render_pass.command_count") {
                counter->commands.fetch_add(value, std::memory_order_relaxed);
            }
        });
    }

    void build(std::vector<Instance> instances, bool shadow_fixture = false) {
        builder_.clear(core_);
        for (const auto& mesh : gpu_meshes_) meshes_.release(mesh);
        gpu_meshes_.clear();
        instances_ = std::move(instances);
        gpu_meshes_.push_back(meshes_.upload(quad_mesh("culling-receiver", shadow_fixture ? 1.5f : 0.5f)));
        if (shadow_fixture) gpu_meshes_.push_back(meshes_.upload(cube_mesh("culling-caster", 0.5f)));
        const MaterialResolver resolve = [shadow_fixture](const std::string& path, const std::string&) {
            GpuMaterial material;
            material.casts_shadow = shadow_fixture && path == "culling-caster";
            return material;
        };
        builder_.build(core_, meshes_, materials_, instances_, gpu_meshes_, resolve, false, true);
        if (!builder_.built() || scene_->getRenderableCount() == 0) {
            throw std::runtime_error("production scene builder produced no renderables");
        }
    }

    void move(std::size_t index, float x, float z = 0) {
        instances_.at(index).transform = placement(instances_.at(index).mesh_index, x, z).transform;
        const std::array<int, 1> dirty{static_cast<int>(index)};
        builder_.update_transforms(core_, instances_, dirty);
    }

    [[nodiscard]] std::size_t renderable_count() const { return scene_->getRenderableCount(); }

    Capture capture() {
        // Compilation/framegraph warmup is excluded. Drain before resetting
        // the callback, then measure the commands from one completed frame.
        for (int i = 0; i < 3; ++i) draw();
        engine_.flushAndWait();
        counter_->commands.store(0, std::memory_order_relaxed);
        draw();
        Capture result;
        result.commands = counter_->commands.load(std::memory_order_relaxed);
        result.visible_renderables = view_->getVisibleRenderableCount();
        result.pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 0);
        std::atomic_bool complete{false};
        renderer_->readPixels(target_, 0, 0, kSize, kSize,
            filament::backend::PixelBufferDescriptor(result.pixels.data(), result.pixels.size(),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &complete));
        engine_.flushAndWait();
        if (!complete.load()) throw std::runtime_error("native readback did not complete");
        return result;
    }

    void shadows(bool enabled) {
        auto& lights = engine_.getLightManager();
        lights.setDirection(lights.getInstance(light_), {-1, 0, -1});
        view_->setShadowingEnabled(enabled);
    }

    [[nodiscard]] bool caster_outside_camera() const {
        // The caster's actual box and ordinary singleton margin remain fully
        // outside the camera. Its projected shadow falls at the receiver center.
        return !camera_->getFrustum().intersects(filament::Box{{4, 0, 4}, {1.5f, 1.5f, 1.5f}});
    }

private:
    void draw() {
        for (int attempt = 0; attempt < 12; ++attempt) {
            if (!renderer_->beginFrame(swapchain_)) continue;
            renderer_->render(view_);
            renderer_->endFrame();
            engine_.flushAndWait();
            return;
        }
        throw std::runtime_error("native frame was repeatedly rejected");
    }

    filament::Engine& engine_;
    TextureStore textures_;
    MaterialInstanceCache materials_;
    MeshStore meshes_;
    SceneBuilder builder_;
    BackendCore core_{};
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    utils::Entity camera_entity_, light_;
    std::vector<GpuMesh> gpu_meshes_;
    std::vector<Instance> instances_;
    std::shared_ptr<SubmissionCounter> counter_ = std::make_shared<SubmissionCounter>();
};

bool check_group_culling(CullingFixture& fixture) {
    // Distant clusters of the SAME mesh straddle the camera. One union box
    // intersects it despite every member being absent; spatial groups must not.
    fixture.build({placement(0, -60), placement(0, -61), placement(0, 60), placement(0, 61)});
    const auto built_count = fixture.renderable_count();
    const auto hidden = fixture.capture();
    fixture.move(0, 0);
    const auto moved = fixture.capture();
    fixture.move(0, -60);
    const auto hidden_again = fixture.capture();
    const auto moved_bright = bright_pixels(moved.pixels);
    std::printf("%s group hidden/moved/hidden commands=%llu/%llu/%llu; visible=%d/%d/%d; moved pixels=%zu\n",
        kTag, static_cast<unsigned long long>(hidden.commands),
        static_cast<unsigned long long>(moved.commands), static_cast<unsigned long long>(hidden_again.commands),
        hidden.visible_renderables, moved.visible_renderables, hidden_again.visible_renderables, moved_bright);
    return hidden.commands == 0 && hidden.visible_renderables == 0 && bright_pixels(hidden.pixels) == 0 &&
        moved.commands > 0 && moved.visible_renderables > 0 && moved_bright > 256 &&
        hidden_again.commands == 0 && hidden_again.visible_renderables == 0 &&
        bright_pixels(hidden_again.pixels) == 0 && fixture.renderable_count() == built_count &&
        save_capture("culling-hidden", hidden.pixels) && save_capture("culling-moved", moved.pixels);
}

bool check_shadow_caster(CullingFixture& fixture) {
    fixture.build({placement(0, 0), placement(1, 4, 4)}, true);
    if (!fixture.caster_outside_camera()) return false;
    fixture.shadows(false);
    const auto unshadowed = fixture.capture();
    fixture.shadows(true);
    const auto shadowed = fixture.capture();
    const bool saved = save_capture("culling-shadow-off", unshadowed.pixels) &&
        save_capture("culling-shadow-on", shadowed.pixels);
    std::size_t darker = 0;
    // Central receiver pixels are separated from all geometric silhouettes.
    // A diagonal directional ray from this square meets the off-camera cube.
    for (std::uint32_t y = kSize / 2 - 14; y < kSize / 2 + 14; ++y) {
        for (std::uint32_t x = kSize / 2 - 14; x < kSize / 2 + 14; ++x) {
            const auto i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            if (unshadowed.pixels[i] > shadowed.pixels[i] + 10) ++darker;
        }
    }
    std::printf("%s off-camera caster shadow-off/on commands=%llu/%llu; camera-visible=%d/%d; darker receiver pixels=%zu\n",
        kTag, static_cast<unsigned long long>(unshadowed.commands),
        static_cast<unsigned long long>(shadowed.commands), unshadowed.visible_renderables,
        shadowed.visible_renderables, darker);
    return unshadowed.visible_renderables == 1 && shadowed.visible_renderables == 1 &&
        shadowed.commands > unshadowed.commands && darker > 256 &&
        saved;
}
} // namespace

CaseResult run_culling_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Skip;
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return CaseResult::Fail;
    const auto owner_counts = [&] {
        return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
                          engine->getViewCount(), engine->getTextureCount()};
    };
    const auto baseline = owner_counts();
    bool ok = false;
    try {
        {
            CullingFixture fixture(*engine);
            fixture.init();
            ok = check_group_culling(fixture) && check_shadow_caster(fixture);
        }
        engine->flushAndWait();
        ok &= owner_counts() == baseline;
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        return CaseResult::Fail;
    }
    if (!ok) return CaseResult::Fail;
    std::printf("%s PASS: actual culled submissions, moved-member pixels, off-camera shadow and owner teardown\n", kTag);
    return CaseResult::Pass;
}
} // namespace heaven::smoke
