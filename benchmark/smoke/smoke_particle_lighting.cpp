// Production smoke must respond to incident light without becoming an emitter.
// Use the feature's real materials, slot geometry and update path at fixed time;
// only illumination changes between captures. No scene assets or LUTs are needed.
#include "smoke_cases.h"
#include "rhi/filament_backend.h"
#include "rhi/features/particle_feature.h"
#include "rhi/render_test_scene_assets.h"
#include "rhi/screenshot_ppm.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
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
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_particle_lighting]";
constexpr std::uint32_t kSize = 192;
using Pixels = std::vector<float>;
using Float3 = filament::math::float3;

class ParticleLightingFixture {
public:
    ParticleLightingFixture(filament::Engine& engine, RenderContext context)
        : engine_(engine), context_(context) {}
    ParticleLightingFixture(const ParticleLightingFixture&) = delete;
    ParticleLightingFixture& operator=(const ParticleLightingFixture&) = delete;
    ~ParticleLightingFixture() {
        engine_.flushAndWait();
        particles_.destroy(context_);
        assets_.reset();
        if (view_) {
            view_->setRenderTarget(nullptr);
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
        }
        if (scene_) {
            scene_->setIndirectLight(nullptr);
            if (point_) scene_->remove(point_);
        }
        engine_.destroy(indirect_);
        if (point_) {
            engine_.destroy(point_);
            utils::EntityManager::get().destroy(point_);
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

    void init(const AppConfig& source) {
        renderer_ = engine_.createRenderer();
        swapchain_ = engine_.createSwapChain(kSize, kSize);
        scene_ = engine_.createScene();
        view_ = engine_.createView();
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        if (!renderer_ || !swapchain_ || !scene_ || !view_ || !camera_)
            throw std::runtime_error("fixture object creation failed");
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera_->setExposure(16, 1.0f / 125, 100);
        camera_->setProjection(filament::Camera::Projection::ORTHO, -3, 3, -3, 3, 0.1, 30);
        camera_->lookAt({0, -10, 2}, {0, 0, 2}, {0, 0, 1});
        // Filament permits combining these documented texture usage flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        // The optical oracle subtracts four rendered colors. FP16 rounding at
        // every overlapping puff otherwise accumulates a false dependence on
        // incident light; retain FLOAT32 through blending and readback here.
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).usage(usage).build(engine_);
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
        view_->setShadowingEnabled(false);
        view_->setAntiAliasing(filament::AntiAliasing::NONE);
        view_->setMultiSampleAntiAliasingOptions({.enabled = false});
        view_->setTemporalAntiAliasingOptions({.enabled = false});
        view_->setAmbientOcclusionOptions({.enabled = false});
        view_->setDithering(filament::Dithering::NONE);

        auto assets = RenderTestSceneAssets::create(engine_);
        if (!assets) throw std::runtime_error(assets.error().message);
        assets_ = std::move(*assets);
        context_.engine = &engine_;
        context_.scene = scene_;
        context_.view = view_;
        context_.renderer = renderer_;
        context_.camera = camera_;
        context_.test_scene_assets = assets_.get();
        context_.output_target = target_;
        AppConfig config = source;
        config.features.particles = true;
        config.features.heat_haze = false;
        particles_.init(context_, config);
        std::array<ParticleEmitter, 2> emitters;
        for (std::size_t i = 0; i < emitters.size(); ++i) {
            auto& emitter = emitters[i];
            emitter.name = i == 0 ? "lighting_fire_smoke" : "lighting_fire_smoke_white";
            emitter.material_name = i == 0 ? "fire_smoke" : "fire_smoke_white";
            emitter.transform[3] = {i == 0 ? -1.2f : 1.2f, 0, 0, 1};
            emitter.life_mean = 4;
            emitter.spawn_rate = 15;
            emitter.velocity_mean = 0.45f;
            emitter.radius_mean = 0.5f;
            emitter.growth_mean = 0.15f;
        }
        particles_.set_emitters(emitters);
        FrameContext frame;
        frame.time = 2.5f;
        frame.viewport_w = static_cast<int>(kSize);
        frame.viewport_h = static_cast<int>(kSize);
        particles_.update(context_, frame);
        if (scene_->getRenderableCount() != emitters.size() || particles_.needs_depth_prepass())
            throw std::runtime_error("production smoke emitters did not build in isolation");

        // Only the constant SH coefficient is nonzero: a genuine uniform IBL,
        // with no texture sampling or sky/background contribution to the ROI.
        const std::array<Float3, 9> sh{{{1, 1, 1}, {}, {}, {}, {}, {}, {}, {}, {}}};
        indirect_ = filament::IndirectLight::Builder().irradiance(3, sh.data())
            .intensity(0).build(engine_);
        if (!indirect_) throw std::runtime_error("constant irradiance creation failed");
        scene_->setIndirectLight(indirect_);
        point_ = utils::EntityManager::get().create();
        if (filament::LightManager::Builder(filament::LightManager::Type::POINT)
                .position({0, -3, 2}).color({1, 0.02f, 0.01f}).intensity(500000)
                .falloff(20).castShadows(false).build(engine_, point_) !=
                filament::LightManager::Builder::Success)
            throw std::runtime_error("local light creation failed");
    }

    Pixels capture(float ambient, Float3 color, bool point = false, Float3 background = {}) {
        renderer_->setClearOptions({.clearColor = {background.x, background.y, background.z, 1},
            .clear = true, .discard = true});
        indirect_->setIntensity(ambient);
        // Retain the same uniform field and vary its color through SH itself.
        // Replacing an owned light is deliberate fixture setup, not frame work.
        scene_->setIndirectLight(nullptr);
        engine_.destroy(indirect_);
        const std::array<Float3, 9> sh{{color, {}, {}, {}, {}, {}, {}, {}, {}}};
        indirect_ = filament::IndirectLight::Builder().irradiance(3, sh.data())
            .intensity(ambient).build(engine_);
        if (!indirect_) throw std::runtime_error("irradiance replacement failed");
        scene_->setIndirectLight(indirect_);
        if (point) scene_->addEntity(point_);
        else scene_->remove(point_);
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
    void draw() {
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
    RenderContext context_;
    ParticleFeature particles_;
    std::unique_ptr<RenderTestSceneAssets> assets_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    filament::IndirectLight* indirect_ = nullptr;
    utils::Entity camera_entity_, point_;
};

std::array<double, 3> mean_roi(const Pixels& pixels, std::size_t emitter) {
    std::array<double, 3> mean{};
    const std::uint32_t left = emitter == 0 ? 35 : 111;
    constexpr std::uint32_t width = 46;
    constexpr std::uint32_t bottom = 38, top = 148;
    for (std::uint32_t y = bottom; y < top; ++y) {
        for (std::uint32_t x = left; x < left + width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * kSize + x) * 4;
            for (std::size_t c = 0; c < mean.size(); ++c) {
                if (!std::isfinite(pixels[index + c]) || pixels[index + c] < 0)
                    throw std::runtime_error("nonfinite or negative smoke radiance");
                mean[c] += pixels[index + c];
            }
        }
    }
    for (auto& value : mean) value /= static_cast<double>(width * (top - bottom));
    return mean;
}

struct Coverage {
    double mean_opacity = 0;
    double peak_opacity = 0;
    double lighting_error = 0;
    std::uint32_t covered_pixels = 0;
};

void validate_backgrounds(const Pixels& dark_black, const Pixels& dark_white,
        const Pixels& lit_black, const Pixels& lit_white) {
    // Every fixed-time billboard fits strictly inside this orthographic view.
    // Uncovered corners qualify the background step used by the optical oracle.
    for (const std::uint32_t y : {0u, kSize - 1}) {
        for (const std::uint32_t x : {0u, kSize - 1}) {
            const auto index = (static_cast<std::size_t>(y) * kSize + x) * 4;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const double dark_t = static_cast<double>(dark_white[index + channel]) -
                    static_cast<double>(dark_black[index + channel]);
                const double lit_t = static_cast<double>(lit_white[index + channel]) -
                    static_cast<double>(lit_black[index + channel]);
                if (!std::isfinite(dark_t) || !std::isfinite(lit_t) ||
                        std::abs(dark_t - 1.0) > 0.001 || std::abs(lit_t - 1.0) > 0.001)
                    throw std::runtime_error("smoke background calibration failed");
            }
        }
    }
}

Coverage coverage_roi(const Pixels& dark_black, const Pixels& dark_white,
        const Pixels& lit_black, const Pixels& lit_white, std::size_t emitter) {
    Coverage result;
    const std::uint32_t left = emitter == 0 ? 35 : 111;
    constexpr std::uint32_t width = 46, bottom = 38, top = 148;
    for (std::uint32_t y = bottom; y < top; ++y) {
        for (std::uint32_t x = left; x < left + width; ++x) {
            const auto index = (static_cast<std::size_t>(y) * kSize + x) * 4;
            double opacity = 0;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                for (const float signal : {dark_black[index + channel], dark_white[index + channel],
                        lit_black[index + channel], lit_white[index + channel]}) {
                    if (!std::isfinite(signal) || signal < 0 || signal > 2)
                        throw std::runtime_error("smoke optical oracle exceeded its FLOAT32 range");
                }
                // For fixed premultiplied over-compositing C = L + T * B.
                // Changing only a unit background isolates transmission T,
                // independently of smoke albedo, exposure or incident light.
                const double dark_t = static_cast<double>(dark_white[index + channel]) -
                    static_cast<double>(dark_black[index + channel]);
                const double lit_t = static_cast<double>(lit_white[index + channel]) -
                    static_cast<double>(lit_black[index + channel]);
                if (!std::isfinite(dark_t) || !std::isfinite(lit_t) ||
                        dark_t < -0.003 || dark_t > 1.003 || lit_t < -0.003 || lit_t > 1.003)
                    throw std::runtime_error("smoke background transmission outside [0,1]");
                opacity += std::clamp(1.0 - dark_t, 0.0, 1.0) / 3.0;
                result.lighting_error = std::max(result.lighting_error, std::abs(lit_t - dark_t));
            }
            result.mean_opacity += opacity;
            result.peak_opacity = std::max(result.peak_opacity, opacity);
            if (opacity > 0.1) ++result.covered_pixels;
        }
    }
    result.mean_opacity /= static_cast<double>(width * (top - bottom));
    return result;
}

bool save_pixels(const char* name, const Pixels& pixels) {
    const char* output = std::getenv("HEAVEN_SMOKE_PARTICLE_LIGHTING_OUTPUT_DIR");
    if (!output || !*output) return true;
    const auto path = std::filesystem::path(output) / (std::string(name) + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    std::vector<std::uint8_t> bytes(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(std::lround(std::clamp(pixels[i], 0.0f, 1.0f) * 255.0f));
    return write_screenshot_ppm(path, kSize, kSize, bytes).has_value();
}
} // namespace

CaseResult run_particle_lighting_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Fail;
    auto context = backend.debug_render_context();
    auto* engine = context.engine;
    if (!engine) return CaseResult::Fail;
    const auto owners = [&] {
        return std::tuple{engine->getVertexBufferCount(), engine->getIndexBufferCount(),
            engine->getViewCount(), engine->getTextureCount(), engine->getMaterialCount(),
            engine->getSceneCount(), engine->getRenderTargetCount(), engine->getSwapChainCount()};
    };
    const auto baseline = owners();
    bool valid = true;
    try {
        ParticleLightingFixture fixture(*engine, context);
        fixture.init(ctx.config);
        const auto dark = fixture.capture(0, {1, 1, 1});
        const auto low = fixture.capture(2000, {1, 1, 1});
        const auto high = fixture.capture(50000, {1, 1, 1});
        const auto blue = fixture.capture(50000, {0.02f, 0.05f, 1});
        const auto point = fixture.capture(0, {1, 1, 1}, true);
        // The point light selects a different lighting shader variant. Returning
        // to the same white IBL must reproduce its original response, including
        // when this fixture follows material/program teardown in the full suite.
        const auto returned_high = fixture.capture(50000, {1, 1, 1});
        const auto returned_dark = fixture.capture(0, {1, 1, 1});
        const auto dark_white = fixture.capture(0, {1, 1, 1}, false, {1, 1, 1});
        const auto high_white = fixture.capture(50000, {1, 1, 1}, false, {1, 1, 1});
        validate_backgrounds(dark, dark_white, high, high_white);
        for (std::size_t emitter = 0; emitter < 2; ++emitter) {
            const auto a = mean_roi(dark, emitter), b = mean_roi(low, emitter),
                c = mean_roi(high, emitter), d = mean_roi(blue, emitter),
                e = mean_roi(point, emitter), f = mean_roi(returned_dark, emitter),
                g = mean_roi(returned_high, emitter);
            const double zero = a[0] + a[1] + a[2];
            const double dim = b[0] + b[1] + b[2];
            const double bright = c[0] + c[1] + c[2];
            const double repeated = g[0] + g[1] + g[2];
            // Permit 0.1% of the measured signal across variant switches. Check
            // each color channel so an equal total cannot hide a color change.
            bool repeat_matches = true;
            for (std::size_t channel = 0; channel < c.size(); ++channel)
                repeat_matches &= std::abs(g[channel] - c[channel]) <=
                    std::max(1e-7, std::abs(c[channel]) * 0.001);
            const bool passed = zero < 1e-6 && f[0] + f[1] + f[2] < 1e-6 &&
                dim > zero + 1e-5 && bright > dim * 4 &&
                d[2] > 2 * d[0] + 1e-5 && e[0] > 2 * e[2] + 1e-5 && e[0] > 1e-5 &&
                repeat_matches;
            valid &= passed;
            std::printf("%s %s dark/low/high=%.7g/%.7g/%.7g blueRGB=(%.7g,%.7g,%.7g) "
                "pointRGB=(%.7g,%.7g,%.7g) returnedHigh=%.7g repeat=%s returnedDark=%.7g: %s\n", kTag,
                emitter == 0 ? "fire_smoke" : "fire_smoke_white", zero, dim, bright,
                d[0], d[1], d[2], e[0], e[1], e[2], repeated, repeat_matches ? "PASS" : "FAIL",
                f[0] + f[1] + f[2], passed ? "PASS" : "FAIL");
            const auto coverage = coverage_roi(dark, dark_white, high, high_white, emitter);
            // Both fixed emitters have 60 puffs. Reserve FLOAT32 rounding for
            // all 120 possible over-blends, multiply/add and four colors in
            // the background-pair difference, with signal bounded below 2.
            // These coverage bounds reject the measured thin baseline while
            // retaining translucent cores; they are not original-engine parity.
            constexpr double transmission_tolerance =
                16.0 * 120.0 * std::numeric_limits<float>::epsilon();
            const double minimum_mean_opacity = emitter == 0 ? 0.14 : 0.16;
            const bool coverage_passed = coverage.mean_opacity > minimum_mean_opacity &&
                coverage.mean_opacity < 0.35 && coverage.peak_opacity > 0.6 &&
                coverage.peak_opacity < 0.9 && coverage.covered_pixels > 1500 &&
                coverage.lighting_error < transmission_tolerance;
            valid &= coverage_passed;
            std::printf("%s %s background opacity mean/peak=%.7g/%.7g covered>0.1=%u "
                "lighting transmission error=%.7g: %s\n", kTag,
                emitter == 0 ? "fire_smoke" : "fire_smoke_white", coverage.mean_opacity,
                coverage.peak_opacity, coverage.covered_pixels, coverage.lighting_error,
                coverage_passed ? "PASS" : "FAIL");
        }
        valid &= save_pixels("zero-light", dark) && save_pixels("low-ibl", low) &&
            save_pixels("high-ibl", high) && save_pixels("blue-ibl", blue) &&
            save_pixels("red-point", point) && save_pixels("returned-high-ibl", returned_high) &&
            save_pixels("returned-zero-light", returned_dark) &&
            save_pixels("zero-light-white-background", dark_white) &&
            save_pixels("high-ibl-white-background", high_white);
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        valid = false;
    }
    engine->flushAndWait();
    if (owners() != baseline) {
        std::printf("%s FAIL: fixture resources did not return to baseline\n", kTag);
        valid = false;
    }
    return valid ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
