// Real raster oracle for coarse opaque lit shading and full-rate controls.
// This does not infer activation from an option setter or a timing difference.
#include "smoke_cases.h"
#include "rhi/filament_backend.h"
#include "rhi/mesh_store.h"
#include "rhi/screenshot_ppm.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Renderer.h>
#include <filament/RenderTarget.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/DriverEnums.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef HEAVEN_SMOKE_VRS_LIT_PACKAGE
#define HEAVEN_SMOKE_VRS_LIT_PACKAGE ""
#endif
#ifndef HEAVEN_SMOKE_VRS_MASKED_PACKAGE
#define HEAVEN_SMOKE_VRS_MASKED_PACKAGE ""
#endif
#ifndef HEAVEN_SMOKE_VRS_UNLIT_PACKAGE
#define HEAVEN_SMOKE_VRS_UNLIT_PACKAGE ""
#endif

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_variable_rate_shading]";
constexpr std::uint32_t kSize = 128;
constexpr std::uint32_t kRoiBegin = 32, kRoiEnd = 96;
using Pixels = std::vector<std::uint8_t>;
using NativeRate = filament::backend::FragmentShadingRate;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool enabled_environment(const char* name) {
    const char* value = std::getenv(name);
    return value && std::string_view(value) == "1";
}

NativeRate native_rate(VariableRateShadingMode mode) {
    switch (mode) {
        case VariableRateShadingMode::Off: return NativeRate::FULL;
        case VariableRateShadingMode::Rate2x1: return NativeRate::RATE_2X1;
        case VariableRateShadingMode::Rate2x2: return NativeRate::RATE_2X2;
    }
    return NativeRate::FULL;
}

class BackendStateRestore {
public:
    BackendStateRestore(IRhiBackend& backend, filament::View& view)
        : backend_(backend), view_(view), msaa_(view.getMultiSampleAntiAliasingOptions()),
          mode_(backend.variable_rate_shading_status().requested) {}
    ~BackendStateRestore() {
        view_.setMultiSampleAntiAliasingOptions(msaa_);
        backend_.set_variable_rate_shading(mode_);
    }
    BackendStateRestore(const BackendStateRestore&) = delete;
    BackendStateRestore& operator=(const BackendStateRestore&) = delete;

private:
    IRhiBackend& backend_;
    filament::View& view_;
    filament::MultiSampleAntiAliasingOptions msaa_;
    VariableRateShadingMode mode_;
};

class VrsFixture {
public:
    explicit VrsFixture(filament::Engine& engine) : engine_(engine), meshes_(engine) {}
    VrsFixture(const VrsFixture&) = delete;
    VrsFixture& operator=(const VrsFixture&) = delete;
    ~VrsFixture() {
        engine_.flushAndWait();
        if (view_) {
            view_->setRenderTarget(nullptr);
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
        }
        if (scene_) scene_->setSkybox(nullptr);
        if (object_) {
            if (scene_) scene_->remove(object_);
            engine_.destroy(object_);
            utils::EntityManager::get().destroy(object_);
        }
        for (auto* instance : instances_) if (instance) engine_.destroy(instance);
        for (auto* material : materials_) if (material) engine_.destroy(material);
        meshes_.release(mesh_);
        engine_.destroy(view_);
        engine_.destroy(scene_);
        engine_.destroy(skybox_);
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
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        auto* camera = engine_.createCamera(camera_entity_);
        require(renderer_ && swapchain_ && scene_ && view_ && camera, "native owners unavailable");
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera->setProjection(filament::Camera::Projection::ORTHO, -1, 1, -1, 1, .1, 10);
        camera->lookAt({0, 0, 2}, {0, 0, 0}, {0, 1, 0});
        camera->setExposure(1.0f);
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
        require(color_ && depth_, "native capture textures unavailable");
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        require(target_, "native capture target unavailable");
        view_->setScene(scene_);
        view_->setCamera(camera);
        view_->setRenderTarget(target_);
        view_->setViewport({0, 0, kSize, kSize});
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        // A scene without an explicit IBL or skybox inherits Filament's
        // default ambient intensity. Zero the fallback so lighting cannot
        // saturate this emissive fragment-coordinate color oracle.
        skybox_ = filament::Skybox::Builder().color({0, 0, 0, 1}).intensity(0).build(engine_);
        require(skybox_, "zero-intensity skybox unavailable");
        scene_->setSkybox(skybox_);
        constexpr std::array paths = {HEAVEN_SMOKE_VRS_LIT_PACKAGE,
            HEAVEN_SMOKE_VRS_MASKED_PACKAGE, HEAVEN_SMOKE_VRS_UNLIT_PACKAGE};
        for (std::size_t i = 0; i < paths.size(); ++i) {
            std::ifstream stream(paths[i], std::ios::binary);
            require(stream.is_open(), "test-only material package unavailable");
            const std::vector<char> package{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}};
            require(!package.empty(), "test-only material package is empty");
            materials_[i] = filament::Material::Builder().package(package.data(), package.size()).build(engine_);
            require(materials_[i], "test-only material package build failed");
            instances_[i] = materials_[i]->createInstance();
            require(instances_[i], "test-only material instance unavailable");
        }
        Mesh mesh;
        mesh.name = mesh.path = "vrs-native-oracle";
        Surface surface;
        surface.name = "large-triangle";
        for (const auto point : std::array<glm::vec2, 3>{{{-4, -4}, {4, -4}, {0, 4}}}) {
            Vertex vertex{};
            vertex.px = point.x;
            vertex.py = point.y;
            vertex.nz = 1;
            vertex.u = vertex.u1 = (point.x + 4) / 8;
            vertex.v = vertex.v1 = (point.y + 4) / 8;
            vertex.displacement_normal_scale = 1;
            vertex.tx = vertex.tw = 1;
            surface.vertices.push_back(vertex);
        }
        surface.indices = {0, 1, 2};
        mesh.surfaces.push_back(std::move(surface));
        mesh_ = meshes_.upload(mesh);
        require(mesh_.surfaces.size() == 1, "triangle upload failed");
        const auto buffers = meshes_.find(mesh_.surfaces.front().handle);
        require(buffers.vb && buffers.ib, "triangle buffers unavailable");
        object_ = utils::EntityManager::get().create();
        require(filament::RenderableManager::Builder(1)
            .geometry(0, filament::RenderableManager::PrimitiveType::TRIANGLES, buffers.vb, buffers.ib, 0, 3)
            .material(0, instances_.front()).boundingBox({{0, 0, 0}, {4, 4, .01f}})
            .culling(false).castShadows(false).receiveShadows(false).build(engine_, object_) ==
                filament::RenderableManager::Builder::Success, "triangle renderable build failed");
        scene_->addEntity(object_);
    }

    Pixels capture(VariableRateShadingMode mode, unsigned samples, std::size_t material, int pattern) {
        require(material < instances_.size(), "unknown test material");
        const auto mode_id = variable_rate_shading_mode_id(mode);
        std::fprintf(stderr, "%s capture begin mode=%.*s material=%zu pattern=%d samples=%u format=RGBA8\n",
            kTag, static_cast<int>(mode_id.size()), mode_id.data(), material, pattern, samples);
        std::fflush(stderr);
        view_->setFragmentShadingRate(native_rate(mode));
        view_->setMultiSampleAntiAliasingOptions({.enabled = samples > 1,
            .sampleCount = static_cast<std::uint8_t>(samples)});
        instances_[material]->setParameter("pattern", pattern);
        auto& manager = engine_.getRenderableManager();
        manager.setMaterialInstanceAt(manager.getInstance(object_), 0, instances_[material]);
        for (int frame = 0; frame < 3; ++frame) {
            bool accepted = false;
            for (int retry = 0; retry < 12 && !accepted; ++retry) {
                if (!renderer_->beginFrame(swapchain_)) continue;
                renderer_->render(view_);
                renderer_->endFrame();
                std::fprintf(stderr, "%s frame=%d submitted; draining\n", kTag, frame);
                std::fflush(stderr);
                engine_.flushAndWait();
                std::fprintf(stderr, "%s frame=%d completed\n", kTag, frame);
                std::fflush(stderr);
                accepted = true;
            }
            require(accepted, "native VRS frame repeatedly rejected");
        }
        Pixels pixels(static_cast<std::size_t>(kSize) * kSize * 4);
        std::atomic_bool complete{false};
        std::fprintf(stderr, "%s readback begin\n", kTag);
        std::fflush(stderr);
        renderer_->readPixels(target_, 0, 0, kSize, kSize,
            filament::backend::PixelBufferDescriptor(pixels.data(), pixels.size(),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) { static_cast<std::atomic_bool*>(user)->store(true); }, &complete));
        engine_.flushAndWait();
        require(complete.load(), "native VRS color readback incomplete");
        std::fprintf(stderr, "%s readback completed\n", kTag);
        std::fflush(stderr);
        return pixels;
    }

private:
    filament::Engine& engine_;
    MeshStore meshes_;
    GpuMesh mesh_;
    std::array<filament::Material*, 3> materials_{};
    std::array<filament::MaterialInstance*, 3> instances_{};
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::Skybox* skybox_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    utils::Entity camera_entity_, object_;
};

std::size_t offset(std::uint32_t x, std::uint32_t y) {
    return (static_cast<std::size_t>(y) * kSize + x) * 4;
}

void check_coordinate_groups(const Pixels& pixels, unsigned width, unsigned height) {
    for (unsigned axis = 0; axis < 2; ++axis) {
        const unsigned group = axis ? height : width;
        unsigned best_errors = kSize * kSize;
        for (unsigned phase = 0; phase < group; ++phase) {
            unsigned errors = 0;
            for (std::uint32_t y = kRoiBegin; y < kRoiEnd - 1; ++y) {
                for (std::uint32_t x = kRoiBegin; x < kRoiEnd - 1; ++x) {
                    const auto a = offset(x, y) + axis;
                    const auto b = offset(x + (axis == 0 ? 1u : 0u), y + (axis == 1 ? 1u : 0u)) + axis;
                    const bool equal = std::abs(int(pixels[a]) - int(pixels[b])) <= 1;
                    const unsigned coordinate = axis ? y : x;
                    const bool expected_equal = group > 1 && (coordinate + phase) % group != group - 1;
                    if (equal != expected_equal) ++errors;
                }
            }
            best_errors = std::min(best_errors, errors);
        }
        std::printf("%s coordinate axis=%c expected group=%u mismatched interior edges=%u\n",
            kTag, axis ? 'Y' : 'X', group, best_errors);
        require(best_errors == 0, "actual fragment-coordinate grouping differs from requested raster rate");
    }
}

void check_checker(const Pixels& pixels, unsigned width, unsigned height) {
    for (std::uint32_t y = kRoiBegin; y < kRoiEnd - 1; ++y) {
        for (std::uint32_t x = kRoiBegin; x < kRoiEnd - 1; ++x) {
            const auto a = offset(x, y), right = offset(x + 1, y), up = offset(x, y + 1);
            const bool visible = std::max(pixels[a], pixels[a + 1]) > 100 &&
                std::abs(int(pixels[a]) - int(pixels[a + 1])) > 80;
            if (!visible) std::printf("%s invalid checker pixel x=%u y=%u RGBA=%u,%u,%u,%u\n",
                kTag, x, y, pixels[a], pixels[a + 1], pixels[a + 2], pixels[a + 3]);
            require(visible, "checker does not contain a visible red or green fragment");
            const bool red = pixels[a] > pixels[a + 1];
            const bool red_right = pixels[right] > pixels[right + 1];
            const bool red_up = pixels[up] > pixels[up + 1];
            require((red == red_right) == (width == 2) && (red == red_up) == (height == 2),
                "one-pixel checker did not respond to coarse shading along expected axes");
        }
    }
}

void require_parity(const Pixels& actual, const Pixels& expected) {
    require(actual.size() == expected.size(), "capture sizes differ");
    unsigned maximum = 0;
    for (std::uint32_t y = kRoiBegin; y < kRoiEnd; ++y) {
        for (std::uint32_t x = kRoiBegin; x < kRoiEnd; ++x) {
            const auto at = offset(x, y);
            for (std::size_t channel = 0; channel < 3; ++channel) maximum = std::max(maximum,
                static_cast<unsigned>(std::abs(int(actual[at + channel]) - int(expected[at + channel]))));
        }
    }
    require(maximum <= 1, "full-rate return/control changed interior pixels");
}

void save_pixels(const Pixels& pixels, const std::string& name) {
    const char* directory = std::getenv("HEAVEN_SMOKE_VRS_OUTPUT_DIR");
    if (!directory || !*directory) return;
    const auto path = std::filesystem::path(directory) / (name + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    require(write_screenshot_ppm(path, kSize, kSize, pixels).has_value(), "VRS capture save failed");
}
} // namespace

CaseResult run_variable_rate_shading_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    if (!*HEAVEN_SMOKE_VRS_LIT_PACKAGE || !*HEAVEN_SMOKE_VRS_MASKED_PACKAGE || !*HEAVEN_SMOKE_VRS_UNLIT_PACKAGE) {
        std::printf("%s SKIP: test-only material packages were not configured\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Skip;
    auto& context = backend.debug_render_context();
    if (!context.engine || !context.view) return CaseResult::Skip;
    BackendStateRestore restore(backend, *context.view);
    try {
        unsigned coarse_cases = 0;
        const bool full_rate_only = enabled_environment("HEAVEN_SMOKE_VRS_FULL_RATE_ONLY");
        std::fprintf(stderr, "%s fixture init begin; full-rate-only=%d\n", kTag, full_rate_only);
        std::fflush(stderr);
        VrsFixture fixture(*context.engine);
        fixture.init();
        std::fprintf(stderr, "%s fixture init completed\n", kTag);
        std::fflush(stderr);
        for (const unsigned samples : {1u, 4u}) {
            context.view->setMultiSampleAntiAliasingOptions({.enabled = samples > 1,
                .sampleCount = static_cast<std::uint8_t>(samples)});
            std::array<Pixels, 3> baseline_checkers;
            std::array<Pixels, 3> baseline_coordinates;
            for (std::size_t material = 0; material < 3; ++material) {
                baseline_checkers[material] = fixture.capture(VariableRateShadingMode::Off, samples, material, 0);
                baseline_coordinates[material] = fixture.capture(VariableRateShadingMode::Off, samples, material, 1);
                const std::string baseline_name = "off-" + std::to_string(samples) + "x-material" + std::to_string(material);
                save_pixels(baseline_checkers[material], baseline_name + "-checker");
                save_pixels(baseline_coordinates[material], baseline_name + "-coordinates");
            }
            for (std::size_t material = 0; material < 3; ++material) {
                const auto& off_checker = baseline_checkers[material];
                const auto& off_coordinates = baseline_coordinates[material];
                check_checker(off_checker, 1, 1);
                check_coordinate_groups(off_coordinates, 1, 1);
                if (full_rate_only) continue;
                for (const auto mode : {VariableRateShadingMode::Rate2x1, VariableRateShadingMode::Rate2x2}) {
                    backend.set_variable_rate_shading(mode);
                    const auto status = backend.variable_rate_shading_status();
                    const bool supported = mode == VariableRateShadingMode::Rate2x1 ? status.supported_2x1 : status.supported_2x2;
                    require(status.requested == mode && !status.reason.empty(), "RHI request or capability reason is missing");
                    require(status.effective == (supported ? mode : VariableRateShadingMode::Off),
                        "RHI effective rate does not match support at current sample count");
                    require(context.view->getFragmentShadingRate() == native_rate(status.effective),
                        "RHI setter did not update the production view rate");
                    const auto checker = fixture.capture(mode, samples, material, 0);
                    const auto coordinates = fixture.capture(mode, samples, material, 1);
                    const std::string name = std::string(variable_rate_shading_mode_id(mode)) + "-" +
                        std::to_string(samples) + "x-material" + std::to_string(material);
                    save_pixels(checker, name + "-checker");
                    save_pixels(coordinates, name + "-coordinates");
                    const bool coarse = material == 0 && supported;
                    const unsigned width = coarse ? 2u : 1u;
                    const unsigned height = coarse && mode == VariableRateShadingMode::Rate2x2 ? 2u : 1u;
                    check_checker(checker, width, height);
                    check_coordinate_groups(coordinates, width, height);
                    if (!coarse) {
                        require_parity(checker, off_checker);
                        require_parity(coordinates, off_coordinates);
                    } else ++coarse_cases;
                    backend.set_variable_rate_shading(VariableRateShadingMode::Off);
                    require_parity(fixture.capture(VariableRateShadingMode::Off, samples, material, 0), off_checker);
                    require_parity(fixture.capture(VariableRateShadingMode::Off, samples, material, 1), off_coordinates);
                    const auto mode_id = variable_rate_shading_mode_id(mode);
                    const auto effective_id = variable_rate_shading_mode_id(status.effective);
                    std::printf("%s mode=%.*s MSAA=%u material=%zu effective=%.*s %s pixel oracle and Off-return PASS (%s)\n",
                        kTag, static_cast<int>(mode_id.size()), mode_id.data(), samples, material,
                        static_cast<int>(effective_id.size()), effective_id.data(), coarse ? "coarse" : "full-rate",
                        status.reason.c_str());
                }
            }
        }
        if (full_rate_only) {
            std::printf("%s full-rate-only pixel controls PASS; no coarse-rate GPU draw attempted\n", kTag);
            return CaseResult::Pass;
        }
        if (coarse_cases == 0) {
            std::printf("%s SKIP: full-rate/fallback controls passed; hardware exposes no enabled coarse rate\n", kTag);
            return CaseResult::Skip;
        }
        return CaseResult::Pass;
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        return CaseResult::Fail;
    }
}
} // namespace heaven::smoke
