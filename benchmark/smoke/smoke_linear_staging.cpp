// Native FLOAT -> RGBA16F cubemap uploads, sampled through an unlit RGBA8 target.
// This isolates upload conversion from IBL filtering and FLOAT readback.
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
#include <filament/SwapChain.h>
#include <filament/Texture.h>
#include <filament/TextureSampler.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <backend/DriverEnums.h>
#include <backend/PixelBufferDescriptor.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef HEAVEN_SMOKE_LINEAR_STAGE_PACKAGE
#define HEAVEN_SMOKE_LINEAR_STAGE_PACKAGE ""
#endif

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_linear_staging]";
constexpr std::uint32_t kWidth = 192, kHeight = 128;
constexpr std::array<std::uint32_t, 4> kSides = {1, 5, 64, 256};
using Pixels = std::vector<std::uint8_t>;

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

std::array<float, 4> face_color(unsigned face, unsigned epoch, std::size_t cube) {
    // Exactly representable in binary32 and binary16; no tone map is involved.
    // Each channel identifies an independent input, so a stale binding cannot
    // pass by sampling another cube uploaded during the same epoch.
    return {static_cast<float>(epoch + 1) / 16,
            static_cast<float>(cube + 1) / 8,
            static_cast<float>(face + 1) / 8, 1};
}

class LinearStageFixture {
public:
    explicit LinearStageFixture(filament::Engine& engine) : engine_(engine), meshes_(engine) {}
    LinearStageFixture(const LinearStageFixture&) = delete;
    LinearStageFixture& operator=(const LinearStageFixture&) = delete;
    ~LinearStageFixture() {
        engine_.flushAndWait();
        if (view_) {
            view_->setRenderTarget(nullptr);
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
        }
        if (object_) {
            if (scene_) scene_->remove(object_);
            engine_.destroy(object_);
            utils::EntityManager::get().destroy(object_);
        }
        engine_.destroy(instance_);
        engine_.destroy(material_);
        for (auto* cube : cubes_) engine_.destroy(cube);
        meshes_.release(mesh_);
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
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kWidth, kHeight);
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
        // Filament documents these flags as a combinable bitmask.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto color_usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kWidth).height(kHeight).levels(1)
            .format(filament::Texture::InternalFormat::RGBA8).usage(color_usage).build(engine_);
        depth_ = filament::Texture::Builder().width(kWidth).height(kHeight).levels(1)
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
        view_->setViewport({0, 0, kWidth, kHeight});
        view_->setPostProcessingEnabled(false);
        view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);
        std::ifstream stream(HEAVEN_SMOKE_LINEAR_STAGE_PACKAGE, std::ios::binary);
        require(stream.is_open(), "test-only material package unavailable");
        const std::vector<char> package{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}};
        require(!package.empty(), "test-only material package is empty");
        material_ = filament::Material::Builder().package(package.data(), package.size()).build(engine_);
        require(material_, "test-only material build failed");
        instance_ = material_->createInstance();
        require(instance_, "test-only material instance unavailable");
        Mesh mesh;
        mesh.name = mesh.path = "linear-stage-native-oracle";
        Surface surface;
        surface.name = "large-triangle";
        for (const auto point : std::array<glm::vec2, 3>{{{-4, -4}, {4, -4}, {0, 4}}}) {
            Vertex vertex{};
            vertex.px = point.x;
            vertex.py = point.y;
            vertex.nz = 1;
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
            .material(0, instance_).boundingBox({{0, 0, 0}, {4, 4, .01f}})
            .culling(false).castShadows(false).receiveShadows(false).build(engine_, object_) ==
                filament::RenderableManager::Builder::Success, "triangle renderable build failed");
        scene_->addEntity(object_);
        // COLOR_ATTACHMENT selects a stable readable default layout; the
        // upload still follows FLOAT-to-HALF image blits rather than memcpy.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto cube_usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::SAMPLEABLE) |
            std::to_underlying(filament::Texture::Usage::UPLOADABLE) |
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT));
        for (std::size_t i = 0; i < kSides.size(); ++i) {
            cubes_[i] = filament::Texture::Builder().width(kSides[i]).height(kSides[i]).levels(1)
                .sampler(filament::Texture::Sampler::SAMPLER_CUBEMAP)
                .format(filament::Texture::InternalFormat::RGBA16F).usage(cube_usage).build(engine_);
            require(cubes_[i], "FLOAT-to-HALF cubemap unavailable");
        }
    }

    void upload(std::size_t cube, unsigned epoch) {
        const auto side = kSides.at(cube);
        const std::size_t count = static_cast<std::size_t>(side) * side * 6 * 4;
        auto pixels = std::make_unique<float[]>(count);
        for (unsigned face = 0; face < 6; ++face) {
            const auto color = face_color(face, epoch, cube);
            for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(side) * side; ++pixel) {
                const auto at = (static_cast<std::size_t>(face) * side * side + pixel) * 4;
                std::copy(color.begin(), color.end(), pixels.get() + at);
            }
        }
        std::fprintf(stderr, "%s queued upload side=%u epoch=%u\n", kTag, side, epoch);
        std::fflush(stderr);
        cubes_.at(cube)->setImage(engine_, 0, 0, 0, 0, side, side, 6,
            filament::backend::PixelBufferDescriptor(pixels.release(), count * sizeof(float),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::FLOAT,
                [](void* buffer, std::size_t, void*) { delete[] static_cast<float*>(buffer); }));
    }

    void flush_without_wait() { engine_.flush(); }

    Pixels capture(std::size_t cube) {
        instance_->setParameter("source", cubes_.at(cube),
            filament::TextureSampler(filament::TextureSampler::MinFilter::NEAREST,
                filament::TextureSampler::MagFilter::NEAREST));
        bool accepted = false;
        for (int retry = 0; retry < 12 && !accepted; ++retry) {
            if (!renderer_->beginFrame(swapchain_)) continue;
            renderer_->render(view_);
            renderer_->endFrame();
            accepted = true;
        }
        require(accepted, "native linear-stage frame repeatedly rejected");
        Pixels pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
        std::atomic_bool complete{false};
        std::fprintf(stderr, "%s RGBA8 readback begin side=%u\n", kTag, kSides.at(cube));
        std::fflush(stderr);
        renderer_->readPixels(target_, 0, 0, kWidth, kHeight,
            filament::backend::PixelBufferDescriptor(pixels.data(), pixels.size(),
                filament::backend::PixelDataFormat::RGBA, filament::backend::PixelDataType::UBYTE,
                [](void*, std::size_t, void* user) { static_cast<std::atomic_bool*>(user)->store(true); }, &complete));
        engine_.flushAndWait();
        require(complete.load(), "native linear-stage color readback incomplete");
        return pixels;
    }

private:
    filament::Engine& engine_;
    MeshStore meshes_;
    GpuMesh mesh_;
    filament::Material* material_ = nullptr;
    filament::MaterialInstance* instance_ = nullptr;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    std::array<filament::Texture*, kSides.size()> cubes_{};
    utils::Entity camera_entity_, object_;
};

void save_pixels(const Pixels& pixels, const std::string& name) {
    const char* directory = std::getenv("HEAVEN_SMOKE_LINEAR_STAGE_OUTPUT_DIR");
    if (!directory || !*directory) return;
    const auto path = std::filesystem::path(directory) / (name + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    require(write_screenshot_ppm(path, kWidth, kHeight, pixels).has_value(), "linear-stage capture save failed");
}

void check_pixels(const Pixels& pixels, unsigned epoch, std::size_t cube) {
    unsigned maximum = 0, compared = 0;
    for (unsigned face = 0; face < 6; ++face) {
        const auto expected = face_color(face, epoch, cube);
        for (std::uint32_t y = 8; y < kHeight - 8; ++y) {
            for (std::uint32_t x = face * 32 + 4; x < (face + 1) * 32 - 4; ++x) {
                const auto at = (static_cast<std::size_t>(y) * kWidth + x) * 4;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const int expected_byte = static_cast<int>(std::lround(expected[channel] * 255));
                    maximum = std::max(maximum, static_cast<unsigned>(std::abs(int(pixels[at + channel]) - expected_byte)));
                    ++compared;
                }
            }
        }
    }
    std::fprintf(stderr, "%s epoch=%u compared=%u channels max RGBA8 error=%u (limit=2)\n",
        kTag, epoch, compared, maximum);
    std::fflush(stderr);
    require(maximum <= 2, "FLOAT-to-HALF cubemap pixels differ from independent palette oracle");
}
} // namespace

CaseResult run_linear_staging_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    if (!*HEAVEN_SMOKE_LINEAR_STAGE_PACKAGE) {
        std::printf("%s SKIP: test-only material package was not configured\n", kTag);
        return CaseResult::Skip;
    }
    std::fprintf(stderr, "%s backend pipeline init begin\n", kTag);
    std::fflush(stderr);
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Skip;
    auto& context = backend.debug_render_context();
    if (!context.engine) return CaseResult::Skip;
    try {
        std::fprintf(stderr, "%s private fixture init begin\n", kTag);
        std::fflush(stderr);
        LinearStageFixture fixture(*context.engine);
        fixture.init();
        // Queue all four sizes before the first completion. Every upload must
        // retain its own stage until its actual command-buffer fence retires.
        for (std::size_t cube = 0; cube < kSides.size(); ++cube) fixture.upload(cube, 0);
        fixture.flush_without_wait();
        for (std::size_t cube = 0; cube < kSides.size(); ++cube) {
            const auto pixels = fixture.capture(cube);
            save_pixels(pixels, "fresh-" + std::to_string(kSides[cube]));
            check_pixels(pixels, 0, cube);
        }
        // Equal-sized uploads after completion exercise pool-reuse candidates;
        // rapid nonblocking flushes between writes also exercise held stages.
        // Pixel results prove preservation, not exact native allocation reuse.
        for (unsigned batch = 0; batch < 3; ++batch) {
            for (unsigned queued = 0; queued < 3; ++queued) {
                for (std::size_t cube = 0; cube < kSides.size(); ++cube) fixture.upload(cube, batch * 3 + queued + 1);
                fixture.flush_without_wait();
            }
            const unsigned epoch = batch * 3 + 3;
            for (std::size_t cube = 0; cube < kSides.size(); ++cube) {
                const auto pixels = fixture.capture(cube);
                save_pixels(pixels, "queued-" + std::to_string(kSides[cube]) + "-epoch" + std::to_string(epoch));
                check_pixels(pixels, epoch, cube);
            }
        }
        std::printf("%s PASS: sixteen six-face pixel oracles, forty FLOAT-to-HALF uploads, sizes 1/5/64/256; RGBA8 max error <=2\n", kTag);
        return CaseResult::Pass;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s FAIL: %s\n", kTag, error.what());
        return CaseResult::Fail;
    }
}
} // namespace heaven::smoke
