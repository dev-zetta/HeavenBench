// Per-cascade work must fall without losing raster-visible shadow casters.
// The reference changes only RenderableManager's public culling flag. It uses
// the same binary, geometry and receiver materials. Separated populations stay
// camera-visible; the off-camera control also permits upstream receiver fitting
// to change, so its map projections are not independently held fixed.
#include "smoke_cases.h"
#include "benchmark/microprofiler.h"
#include "rhi/filament_backend.h"
#include "rhi/material_instance_cache.h"
#include "rhi/mesh_store.h"
#include "rhi/profile_bridge.h"
#include "rhi/screenshot_ppm.h"

#include <filament/Box.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Frustum.h>
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
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_cascade_shadow_culling]";
constexpr std::uint32_t kSize = 192;
constexpr std::uint64_t kSeparatedTriangles = std::uint64_t{6 + 8 + 10 + 12} * (12 + 2);
using Pixels = std::vector<std::uint8_t>;

Mesh plane_mesh() {
    Mesh mesh;
    mesh.name = mesh.path = "cascade-receiver";
    Surface surface;
    surface.name = "plane";
    constexpr std::array<glm::vec2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (const auto corner : corners) {
        Vertex v{};
        v.px = corner.x;
        v.py = corner.y;
        v.nz = 1;
        v.tx = v.tw = 1;
        v.u = (corner.x + 1) * 0.5f;
        v.v = (corner.y + 1) * 0.5f;
        surface.vertices.push_back(v);
    }
    surface.indices = {0, 1, 2, 0, 2, 3};
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

Mesh cube_mesh() {
    Mesh mesh;
    mesh.name = mesh.path = "cascade-caster";
    Surface surface;
    surface.name = "cube";
    struct Face { glm::vec3 n, t, b; };
    constexpr std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
        {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}},
        {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}
    }};
    constexpr std::array<glm::vec2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    for (const auto& face : faces) {
        const auto first = static_cast<std::uint32_t>(surface.vertices.size());
        for (const auto corner : corners) {
            const auto p = 0.5f * (face.n + corner.x * face.t + corner.y * face.b);
            Vertex v{};
            v.px = p.x; v.py = p.y; v.pz = p.z;
            v.nx = face.n.x; v.ny = face.n.y; v.nz = face.n.z;
            v.tx = face.t.x; v.ty = face.t.y; v.tz = face.t.z; v.tw = 1;
            v.u = (corner.x + 1) * 0.5f; v.v = (corner.y + 1) * 0.5f;
            surface.vertices.push_back(v);
        }
        for (const std::uint32_t local : {0u, 1u, 2u, 0u, 2u, 3u}) {
            surface.indices.push_back(first + local);
        }
    }
    mesh.surfaces.push_back(std::move(surface));
    return mesh;
}

struct Capture {
    Pixels pixels;
    std::vector<std::uint64_t> primitives;
};

bool save_pixels(const std::string& name, const Pixels& pixels) {
    const char* directory = std::getenv("HEAVEN_SMOKE_CASCADE_CULLING_OUTPUT_DIR");
    if (!directory || !*directory) return true;
    const auto path = std::filesystem::path(directory) / (name + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    return write_screenshot_ppm(path.string(), kSize, kSize, pixels).has_value();
}

bool same_pixels(const Pixels& a, const Pixels& b) {
    if (a.size() != b.size()) return false;
    unsigned maximum = 0;
    std::size_t changed = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        unsigned pixel_difference = 0;
        for (std::size_t c = 0; c < 3; ++c) {
            const auto difference = static_cast<unsigned>(std::abs(int(a[i + c]) - int(b[i + c])));
            pixel_difference = std::max(pixel_difference, difference);
        }
        maximum = std::max(maximum, pixel_difference);
        if (pixel_difference > 1) ++changed;
    }
    std::printf("%s no-culling pixel control max=%u pixels>1=%zu\n", kTag, maximum, changed);
    // Only an RGB8 rounding allowance, not permission to remove shadow pixels.
    return maximum <= 3 && changed <= 32;
}

class CascadeFixture {
public:
    explicit CascadeFixture(filament::Engine& engine)
        : engine_(engine), textures_(engine), materials_(engine, textures_), meshes_(engine) {}
    CascadeFixture(const CascadeFixture&) = delete;
    CascadeFixture& operator=(const CascadeFixture&) = delete;
    ~CascadeFixture() {
        engine_.flushAndWait();
        bridge_.reset(); // No producer may retain a pointer into recorder_.
        clear_objects();
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
        textures_.create_placeholders();
        if (!materials_.load_base_materials()) throw std::runtime_error("material initialization failed");
        gpu_meshes_.push_back(meshes_.upload(plane_mesh()));
        gpu_meshes_.push_back(meshes_.upload(cube_mesh()));
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        if (!renderer_ || !swapchain_ || !scene_ || !view_ || !camera_) {
            throw std::runtime_error("fixture object creation failed");
        }
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
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
        if (!color_ || !depth_) throw std::runtime_error("capture texture creation failed");
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        if (!target_) throw std::runtime_error("capture target creation failed");
        view_->setScene(scene_);
        view_->setCamera(camera_);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        view_->setPostProcessingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        light_ = utils::EntityManager::get().create();
        if (filament::LightManager::Builder(filament::LightManager::Type::DIRECTIONAL)
                .color({1, 1, 1}).intensity(50000).direction({-0.3f, -0.2f, -1})
                .castShadows(true).build(engine_, light_) != filament::LightManager::Builder::Success) {
            throw std::runtime_error("fixture light creation failed");
        }
        scene_->addEntity(light_);
        bridge_ = std::make_unique<ProfileBridge>(recorder_);
    }

    void configure(filament::ShadowType type, std::uint8_t cascades, bool shadowing = true) {
        filament::LightManager::ShadowOptions options;
        options.mapSize = 1024;
        options.shadowCascades = cascades;
        options.cascadeSplitPositions[0] = 0.125f;
        options.cascadeSplitPositions[1] = 0.25f;
        options.cascadeSplitPositions[2] = 0.5f;
        options.screenSpaceContactShadows = false;
        options.stable = true;
        options.lispsm = false;
        options.normalBias = 0;
        options.constantBias = 0;
        // The ordinary DIRECTIONAL default is an angular bulb radius of 1,
        // which blurs this meter-sized blocker over several meters. Use a
        // finite, Sun-sized source so PCSS retains a measurable umbra while
        // still executing its blocker search and soft penumbra filtering.
        options.shadowBulbRadius = 0.01f;
        auto& lights = engine_.getLightManager();
        lights.setShadowOptions(lights.getInstance(light_), options);
        view_->setShadowType(type);
        view_->setShadowingEnabled(shadowing);
    }

    void separated_scene() {
        clear_objects();
        camera_->setProjection(60, 1, 1, 80, filament::Camera::Fov::VERTICAL);
        camera_->lookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
        engine_.getLightManager().setDirection(engine_.getLightManager().getInstance(light_),
                {-0.3f, -0.2f, -1});
        // Four depth populations lie well inside distinct camera split slabs.
        // Unequal populations make split-specific counts a meaningful oracle,
        // rather than relying on incidental clipping of symmetric populations.
        // No huge union bounds or instancing.
        constexpr std::array distances{6.0f, 16.0f, 32.0f, 62.0f};
        for (std::size_t layer = 0; layer < distances.size(); ++layer) {
            const float distance = distances[layer];
            const auto columns = static_cast<unsigned>(layer) + 3;
            for (unsigned column = 0; column < columns; ++column) {
                const float x_angle = -0.25f + 0.5f * static_cast<float>(column) /
                        static_cast<float>(columns - 1);
                for (const float y_angle : {-0.15f, 0.15f}) {
                    const filament::math::float3 center{x_angle * distance, y_angle * distance, -distance};
                    add_object(1, center, {1, 1, 1});
                    add_object(0, center + filament::math::float3{0, 0, -1},
                            {distance * 0.1f, distance * 0.1f, 1});
                }
            }
        }
    }

    void off_camera_scene() {
        clear_objects();
        camera_->setProjection(filament::Camera::Projection::ORTHO, -2, 2, -2, 2, 0.1, 20);
        camera_->lookAt({0, 0, 10}, {0, 0, 0}, {0, 1, 0});
        engine_.getLightManager().setDirection(engine_.getLightManager().getInstance(light_), {-1, 0, -1});
        add_object(0, {0, 0, 0}, {1.5f, 1.5f, 1});
        add_object(1, {4, 0, 4}, {1, 1, 1});
        if (camera_->getFrustum().intersects(filament::Box{{4, 0, 4}, {0.5f, 0.5f, 0.5f}})) {
            throw std::runtime_error("blocker is not outside the camera frustum");
        }
    }

    void culling(bool enabled) {
        auto& renderables = engine_.getRenderableManager();
        for (const auto entity : objects_) renderables.setCulling(renderables.getInstance(entity), enabled);
    }

    Capture capture(bool collect_shadow_counts) {
        // Explicit frame completion is fixture-only and makes resource/query
        // lifetime deterministic. It is not the application's profiling path.
        recorder_.begin_capture(60'000'000'000ull);
        for (std::uint64_t attempt = 1; attempt <= 12; ++attempt) draw(attempt);
        Capture result;
        result.pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 0);
        std::atomic_bool complete{false};
        renderer_->readPixels(target_, 0, 0, kSize, kSize,
            filament::backend::PixelBufferDescriptor(result.pixels.data(), result.pixels.size(),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &complete));
        engine_.flushAndWait();
        if (!complete.load()) throw std::runtime_error("native readback incomplete");
        const auto snapshot = recorder_.snapshot();
        recorder_.set_active(false);
        if (collect_shadow_counts) {
            std::map<std::uint64_t, std::vector<const profile::GpuSample*>> frames;
            for (const auto& sample : snapshot.gpu_samples) {
                // Workload counters remain actual draw observations when a
                // backend cannot provide timestamps. This fixture uses no
                // measured GPU intervals and never treats Pending as terminal.
                const bool terminal_work = sample.status == profile::SampleStatus::Available ||
                        sample.status == profile::SampleStatus::Unsupported;
                if (sample.name == "Shadow Pass" && terminal_work &&
                        sample.context.view_id == reinterpret_cast<std::uintptr_t>(view_)) {
                    frames[sample.context.filament_frame_id].push_back(&sample);
                }
            }
            if (frames.empty()) throw std::runtime_error("no terminal Shadow Pass workload samples");
            auto& samples = frames.rbegin()->second;
            std::ranges::sort(samples, {}, [](const auto* sample) { return sample->pass_id; });
            for (const auto* sample : samples) result.primitives.push_back(sample->primitive_count);
        }
        return result;
    }

private:
    void clear_objects() {
        for (const auto entity : objects_) {
            if (scene_) scene_->remove(entity);
            engine_.destroy(entity);
            utils::EntityManager::get().destroy(entity);
        }
        objects_.clear();
    }

    void add_object(std::size_t mesh_index, filament::math::float3 position,
                    filament::math::float3 scale) {
        const auto& mesh = gpu_meshes_.at(mesh_index);
        const auto& surface = mesh.surfaces.at(0);
        const auto buffers = meshes_.find(surface.handle);
        auto* material = materials_.get_or_create(mesh.path, surface.name, GpuMaterial{});
        if (!buffers.vb || !buffers.ib || !material) throw std::runtime_error("fixture mesh/material unavailable");
        const auto entity = utils::EntityManager::get().create();
        // Register ownership before either fallible native builder.
        objects_.push_back(entity);
        auto& transforms = engine_.getTransformManager();
        transforms.create(entity);
        transforms.setTransform(transforms.getInstance(entity),
            filament::math::mat4f::translation(position) * filament::math::mat4f::scaling(scale));
        const auto lo = surface.bbox_min;
        const auto hi = surface.bbox_max;
        const filament::Box box{{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f},
            {(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f}};
        if (filament::RenderableManager::Builder(1).boundingBox(box)
                .material(0, material).geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES,
                    buffers.vb, buffers.ib, 0, surface.index_count)
                .culling(true).castShadows(true).receiveShadows(true).build(engine_, entity) !=
                filament::RenderableManager::Builder::Success) {
            throw std::runtime_error("fixture renderable creation failed");
        }
        scene_->addEntity(entity);
    }

    void draw(std::uint64_t attempt) {
        const profile::Context context{attempt, attempt, 0, 0, 0, recorder_.session_id()};
        bridge_->update(context);
        for (int retry = 0; retry < 12; ++retry) {
            if (!renderer_->beginFrame(swapchain_)) continue;
            renderer_->render(view_);
            renderer_->endFrame();
            engine_.flushAndWait();
            return;
        }
        throw std::runtime_error("native frame repeatedly rejected");
    }

    filament::Engine& engine_;
    TextureStore textures_;
    MaterialInstanceCache materials_;
    MeshStore meshes_;
    profile::Recorder recorder_;
    std::unique_ptr<ProfileBridge> bridge_;
    std::vector<GpuMesh> gpu_meshes_;
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

void print_counts(std::string_view mode, std::uint8_t cascades, const Capture& culled,
                  const Capture& reference) {
    std::printf("%s %.*s/%u submitted primitives culled=[", kTag,
        static_cast<int>(mode.size()), mode.data(), static_cast<unsigned>(cascades));
    for (const auto count : culled.primitives) std::printf("%llu,", static_cast<unsigned long long>(count));
    std::printf("] uncull=[");
    for (const auto count : reference.primitives) std::printf("%llu,", static_cast<unsigned long long>(count));
    std::printf("]\n");
}

bool check_separated(CascadeFixture& fixture, filament::ShadowType type,
                     std::string_view mode, std::uint8_t cascades) {
    fixture.separated_scene();
    fixture.configure(type, cascades);
    fixture.culling(false);
    const auto reference = fixture.capture(true);
    fixture.culling(true);
    const auto culled = fixture.capture(true);
    print_counts(mode, cascades, culled, reference);
    bool valid = reference.primitives.size() == cascades && culled.primitives.size() == cascades &&
        std::ranges::all_of(reference.primitives, [](auto count) { return count == kSeparatedTriangles; });
    if (valid) {
        for (std::size_t i = 0; i < culled.primitives.size(); ++i) {
            valid &= culled.primitives[i] > 0 && culled.primitives[i] <= reference.primitives[i];
        }
        if (cascades == 4 && type != filament::ShadowType::PCF) {
            const auto [minimum, maximum] = std::minmax_element(culled.primitives.begin(), culled.primitives.end());
            // A far cascade may legitimately contain every near caster's
            // light-space projection. At least one split must submit less,
            // and split-specific populations must remain distinguishable.
            valid &= *minimum < *maximum && *minimum < kSeparatedTriangles;
        }
    }
    valid &= same_pixels(reference.pixels, culled.pixels);
    const auto name = std::string(mode) + "-" + std::to_string(cascades) + "-separated";
    return save_pixels(name + "-reference", reference.pixels) &&
        save_pixels(name + "-culled", culled.pixels) && valid;
}

bool check_off_camera(CascadeFixture& fixture, filament::ShadowType type,
                      std::string_view mode, std::uint8_t cascades) {
    fixture.off_camera_scene();
    fixture.configure(type, cascades, false);
    fixture.culling(true);
    const auto unshadowed = fixture.capture(false);
    fixture.configure(type, cascades);
    fixture.culling(false);
    const auto reference = fixture.capture(true);
    fixture.culling(true);
    const auto culled = fixture.capture(true);
    std::size_t darkened = 0;
    for (std::uint32_t y = kSize / 2 - 14; y < kSize / 2 + 14; ++y) {
        for (std::uint32_t x = kSize / 2 - 14; x < kSize / 2 + 14; ++x) {
            const auto index = (static_cast<std::size_t>(y) * kSize + x) * 4;
            if (unshadowed.pixels[index] > culled.pixels[index] + 10) ++darkened;
        }
    }
    std::printf("%s %.*s/%u off-camera blocker darker receiver pixels=%zu\n", kTag,
        static_cast<int>(mode.size()), mode.data(), static_cast<unsigned>(cascades), darkened);
    const auto name = std::string(mode) + "-" + std::to_string(cascades) + "-off-camera";
    const bool saved = save_pixels(name + "-unshadowed", unshadowed.pixels) &&
        save_pixels(name + "-reference", reference.pixels) && save_pixels(name + "-culled", culled.pixels);
    return same_pixels(reference.pixels, culled.pixels) && darkened > 256 && saved;
}
} // namespace

CaseResult run_cascade_shadow_culling_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Fail;
    auto* engine = backend.debug_render_context().engine;
    if (!engine) return CaseResult::Fail;
    const auto owners = [&] {
        return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
                          engine->getTextureCount(), engine->getViewCount()};
    };
    const auto baseline = owners();
    bool valid = true;
    try {
        {
            CascadeFixture fixture(*engine);
            fixture.init();
            constexpr std::array modes{
                std::pair{filament::ShadowType::PCF, std::string_view("pcf")},
                std::pair{filament::ShadowType::VSM, std::string_view("vsm")},
                std::pair{filament::ShadowType::PCSS, std::string_view("pcss")}
            };
            for (const auto& [type, name] : modes) {
                for (const std::uint8_t cascades : {std::uint8_t{1}, std::uint8_t{4}}) {
                    // Run both independently so a count regression does not
                    // hide an off-camera-caster/pixel regression in the log.
                    valid &= check_separated(fixture, type, name, cascades);
                    valid &= check_off_camera(fixture, type, name, cascades);
                }
            }
        }
        engine->flushAndWait();
        valid &= owners() == baseline;
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        return CaseResult::Fail;
    }
    std::printf("%s %s: PCF/VSM/PCSS x 1/4 cascades, actual per-pass work, conservative pixels and off-camera blocker\n",
        kTag, valid ? "PASS" : "FAIL");
    return valid ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
