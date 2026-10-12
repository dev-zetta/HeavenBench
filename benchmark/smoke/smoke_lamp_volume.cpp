// Native production fog/material readback. A labelled lamp adds a finite local
// halo, independently of optical-axis flares, without adding extinction.
#include "smoke_cases.h"
#include "rhi/features/volfog_feature.h"
#include "rhi/filament_backend.h"
#include "rhi/filament_util.h"
#include "rhi/screenshot_ppm.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/LightManager.h>
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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace heaven::smoke {
namespace {
constexpr const char* kTag = "[smoke_lamp_volume]";
constexpr std::uint32_t kSize = 128;
using Pixels = std::vector<float>;

struct CaptureState {
    glm::vec3 offset{0.0f};
    glm::vec3 source{0, 8, 0};
    float depth = 2000.0f;
    float power = 1.0f;
    bool tagged = true;
    bool listed = true;
    bool enabled = true;
    float background = 0.0f;
};

class LampVolumeFixture {
public:
    LampVolumeFixture(filament::Engine& engine, RenderContext context)
        : engine_(engine), context_(context) {}
    LampVolumeFixture(const LampVolumeFixture&) = delete;
    LampVolumeFixture& operator=(const LampVolumeFixture&) = delete;
    ~LampVolumeFixture() {
        engine_.flushAndWait();
        fog_.release_shared_targets(context_);
        fog_.destroy(context_);
        if (view_) {
            view_->setRenderTarget(nullptr);
            view_->setScene(nullptr);
            view_->setCamera(nullptr);
        }
        if (light_) {
            engine_.destroy(light_);
            utils::EntityManager::get().destroy(light_);
        }
        engine_.destroy(view_);
        engine_.destroy(scene_);
        engine_.destroy(target_);
        engine_.destroy(depth_);
        engine_.destroy(color_);
        engine_.destroy(scene_depth_);
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
            throw std::runtime_error("native fixture objects unavailable");
        renderer_->setDisplayInfo({.refreshRate = 0});
        renderer_->setFrameRateOptions({.interval = 0});
        renderer_->setClearOptions({.clearColor = {0, 0, 0, 1}, .clear = true, .discard = true});
        camera_->setExposure(16, 1.0f / 125, 100);
        camera_->setProjection(60, 1, 0.1, 2000);
        // Filament permits combining its documented texture usage flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        color_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::RGBA32F).usage(usage).build(engine_);
        depth_ = filament::Texture::Builder().width(kSize).height(kSize).levels(1)
            .format(filament::Texture::InternalFormat::DEPTH32F)
            .usage(filament::Texture::Usage::DEPTH_ATTACHMENT).build(engine_);
        if (!color_ || !depth_) throw std::runtime_error("native FLOAT targets unavailable");
        target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, color_)
            .texture(filament::RenderTarget::AttachmentPoint::DEPTH, depth_).build(engine_);
        if (!target_) throw std::runtime_error("native capture target unavailable");
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

        context_.engine = &engine_;
        context_.scene = scene_;
        context_.view = view_;
        context_.renderer = renderer_;
        context_.camera = camera_;
        context_.output_target = target_;
        AppConfig config = source;
        config.features.volumetric_fog = true;
        fog_.init(context_, config);
        if (!fog_.needs_depth_prepass() || scene_->getRenderableCount() != 1)
            throw std::runtime_error("production fog did not initialize exactly one quad");
        light_ = utils::EntityManager::get().create();
        restore_source_component();
        replace_depth(1);
    }

    void remove_source_component() {
        auto& manager = engine_.getLightManager();
        if (!manager.getInstance(light_))
            throw std::runtime_error("source component missing before retirement control");
        manager.destroy(light_);
        if (manager.getInstance(light_))
            throw std::runtime_error("source component survived retirement");
    }

    void restore_source_component() {
        if (engine_.getLightManager().getInstance(light_))
            throw std::runtime_error("source component already present before restoration");
        if (filament::LightManager::Builder(filament::LightManager::Type::POINT)
                .position({0, 8, 0}).color({1, 0.71665f, 0.15f})
                .intensity(100).falloff(9).castShadows(false).build(engine_, light_) !=
                filament::LightManager::Builder::Success)
            throw std::runtime_error("borrowed lamp source unavailable");
    }

    void replace_depth(std::uint32_t extent) {
        fog_.release_shared_targets(context_);
        engine_.flushAndWait();
        engine_.destroy(scene_depth_);
        depth_extent_ = extent;
        scene_depth_ = filament::Texture::Builder().width(extent).height(extent).levels(1)
            .format(filament::Texture::InternalFormat::R32F).build(engine_);
        if (!scene_depth_) throw std::runtime_error("synthetic linear depth unavailable");
    }

    Pixels capture(const CaptureState& state) {
        fog_.set_enabled(context_, state.enabled);
        Camera camera;
        camera.position = state.offset;
        camera.yaw = 90;
        camera.pitch = 0;
        camera.fov = 60;
        const auto& p = state.offset;
        camera_->lookAt({p.x, p.y, p.z}, {p.x, p.y + 1, p.z}, {0, 0, 1});
        auto& manager = engine_.getLightManager();
        const auto component = manager.getInstance(light_);
        const glm::vec3 position = state.offset + state.source;
        if (component) {
            manager.setPosition(component, {position.x, position.y, position.z});
            manager.setIntensityCandela(component, 100.0f * state.power);
        }
        SceneLightInfo info;
        info.entity = light_;
        // Deliberately stale descriptor position: the source's native live
        // LightManager position is the authority, including floating updates.
        info.world_pos = {300, 300, 300};
        info.local_volume_source = state.tagged;
        info.nominal_intensity = 100.0f;
        info.intensity = 100.0f * state.power;
        auto samples = std::make_unique<float[]>(static_cast<std::size_t>(depth_extent_) * depth_extent_);
        std::fill_n(samples.get(), static_cast<std::size_t>(depth_extent_) * depth_extent_, state.depth);
        filament::Texture::PixelBufferDescriptor descriptor(samples.release(),
            static_cast<std::size_t>(depth_extent_) * depth_extent_ * sizeof(float),
            filament::Texture::Format::R, filament::Texture::Type::FLOAT,
            [](void* memory, std::size_t, void*) { delete[] static_cast<float*>(memory); });
        scene_depth_->setImage(engine_, 0, std::move(descriptor));
        LightingParams lighting;
        lighting.time_of_day = 0;
        lighting.sun_intensity = 0;
        FrameContext frame;
        frame.camera = &camera;
        frame.lighting = &lighting;
        frame.viewport_w = frame.viewport_h = static_cast<int>(kSize);
        frame.capture_frame = 0;
        frame.depth_texture = scene_depth_;
        frame.depth_w = frame.depth_h = static_cast<int>(depth_extent_);
        // Empty scene_light_entities isolates the new local-volume signal
        // from the existing fog ray-march's ordinary punctual inscatter.
        if (state.listed) frame.scene_lights = std::span<const SceneLightInfo>(&info, 1);
        if (state.enabled) fog_.update(context_, frame);
        renderer_->setClearOptions({.clearColor = {
            state.background, state.background, state.background, 1}, .clear = true, .discard = true});
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
        for (const auto value : pixels)
            if (!std::isfinite(value)) throw std::runtime_error("nonfinite native volume signal");
        return pixels;
    }

    Pixels contribution(CaptureState state) {
        auto off = state;
        off.power = 0;
        const auto baseline = capture(off);
        auto signal = capture(state);
        for (std::size_t i = 0; i < signal.size(); ++i) signal[i] -= baseline[i];
        return signal;
    }

    bool depth_hook_round_trip() {
        const auto before = scene_->getRenderableCount();
        fog_.on_depth_prepass_begin(context_);
        const auto removed = scene_->getRenderableCount();
        fog_.on_depth_prepass_end(context_);
        return before == 1 && removed == 0 && scene_->getRenderableCount() == before;
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
    VolfogFeature fog_;
    filament::Renderer* renderer_ = nullptr;
    filament::SwapChain* swapchain_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::View* view_ = nullptr;
    filament::Camera* camera_ = nullptr;
    filament::Texture* color_ = nullptr;
    filament::Texture* depth_ = nullptr;
    filament::RenderTarget* target_ = nullptr;
    filament::Texture* scene_depth_ = nullptr;
    std::uint32_t depth_extent_ = 1;
    utils::Entity camera_entity_, light_;
};

double channel_sum(const Pixels& pixels, std::size_t channel = 0) {
    double result = 0;
    for (std::size_t i = channel; i < pixels.size(); i += 4) result += pixels[i];
    return result;
}

double peak(const Pixels& pixels) {
    double result = 0;
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        result = std::max(result, static_cast<double>(pixels[i]));
    return result;
}

bool matches(const Pixels& a, const Pixels& b, double scale = 1, double relative = 0.002) {
    if (a.size() != b.size()) return false;
    const double tolerance = std::max(2e-6, std::max(peak(a), peak(b)) * relative);
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i] * scale) > tolerance) return false;
    return true;
}

bool save_pixels(const char* name, const Pixels& pixels) {
    const char* output = std::getenv("HEAVEN_SMOKE_LAMP_VOLUME_OUTPUT_DIR");
    if (!output || !*output) return true;
    const auto path = std::filesystem::path(output) / (std::string(name) + ".ppm");
    std::filesystem::create_directories(path.parent_path());
    std::vector<std::uint8_t> bytes(pixels.size());
    for (std::size_t i = 0; i < pixels.size(); ++i)
        bytes[i] = static_cast<std::uint8_t>(std::lround(std::clamp(pixels[i], 0.0f, 1.0f) * 255));
    return write_screenshot_ppm(path, kSize, kSize, bytes).has_value();
}
} // namespace

CaseResult run_lamp_volume_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (!backend.init_pipelines(ctx.config)) return CaseResult::Fail;
    auto context = backend.debug_render_context();
    auto* engine = context.engine;
    if (!engine || !context.resources) return CaseResult::Fail;
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
        LampVolumeFixture fixture(*engine, context);
        fixture.init(ctx.config);
        CaptureState state;
        const auto full = fixture.contribution(state);
        const double full_sum = channel_sum(full), full_peak = peak(full);
        bool bounded = full_sum > 1e-3 && full_peak > 1e-5 && full_peak < 2 &&
            channel_sum(full, 0) > channel_sum(full, 1) &&
            channel_sum(full, 1) > channel_sum(full, 2);
        // Corners cannot intersect an eight-metre distant three-metre sphere.
        // This geometric support oracle does not repeat the radial shader math.
        for (const std::uint32_t y : {0u, kSize - 1}) {
            for (const std::uint32_t x : {0u, kSize - 1}) {
                const auto index = (static_cast<std::size_t>(y) * kSize + x) * 4;
                bounded &= std::abs(full[index]) < 2e-6;
            }
        }
        for (std::size_t i = 0; i < full.size(); i += 4) {
            bounded &= full[i] >= -2e-6 && std::abs(full[i + 3]) < 2e-6;
        }
        const auto row_red = [&](std::uint32_t x) {
            return full[(static_cast<std::size_t>(kSize / 2) * kSize + x) * 4];
        };
        bounded &= row_red(64) > row_red(84) && row_red(84) > row_red(101) &&
            row_red(101) > 1e-6f && std::abs(row_red(115)) < 2e-6;
        for (std::uint32_t x = kSize / 2 + 1; x < kSize; ++x)
            bounded &= std::abs(row_red(x) - row_red(x - 1)) < full_peak * 0.25;
        valid &= bounded && fixture.depth_hook_round_trip();

        state.power = 0.5f;
        const auto half = fixture.contribution(state);
        state.power = 0;
        const auto off = fixture.contribution(state);
        state.power = night_luminaire_gain(12);
        const auto day = fixture.contribution(state);
        state.power = night_luminaire_gain(21);
        const auto restored = fixture.contribution(state);
        valid &= matches(half, full, 0.5) && peak(off) < 2e-6 && peak(day) < 2e-6 &&
            matches(restored, full);

        state.depth = 1;
        const auto blocked = fixture.contribution(state);
        state.depth = 7;
        const auto partial = fixture.contribution(state);
        valid &= peak(blocked) < 2e-6 && channel_sum(partial) > 1e-4 &&
            channel_sum(partial) < full_sum * 0.95;
        state.depth = 0.25f;
        state.source = {0, 0, 0};
        const auto inside = fixture.contribution(state);
        valid &= channel_sum(inside) > 1e-3 && peak(inside) < 2;
        state = {};
        state.depth = 0;
        const auto no_geometry = fixture.contribution(state);
        valid &= matches(no_geometry, full);
        state.source = {0, -8, 0};
        const auto behind = fixture.contribution(state);
        valid &= peak(behind) < 2e-6;
        state = {};
        state.source = {0, 400, 0};
        const auto distant = fixture.contribution(state);
        // The finite authored volume remains independent of the ambient fog
        // march's 250-metre cap; its angular footprint is only a few pixels.
        valid &= channel_sum(distant) > 1e-6;

        state = {};
        const float angle = glm::radians(25.0f);
        state.source = {8 * std::sin(angle), 8 * std::cos(angle), 0};
        const auto off_axis = fixture.contribution(state);
        valid &= channel_sum(off_axis) > full_sum * 0.05;
        state = {};
        state.offset = {150, -75, 20};
        const auto translated = fixture.contribution(state);
        valid &= matches(translated, full, 1, 0.005);

        state = {};
        state.tagged = false;
        const auto untagged = fixture.contribution(state);
        state.tagged = true;
        state.listed = false;
        const auto removed = fixture.contribution(state);
        state.listed = true;
        state.enabled = false;
        const auto disabled = fixture.contribution(state);
        valid &= peak(untagged) < 2e-6 && peak(removed) < 2e-6 && peak(disabled) < 2e-6;
        state.enabled = true;
        fixture.replace_depth(2);
        const auto rebound = fixture.contribution(state);
        valid &= matches(rebound, full);

        // Retire only the native component while the same source entity stays
        // listed. Compare against an earlier actual lamp-off image as well as
        // subtraction: otherwise a stale halo in both subtraction images could
        // cancel and hide failure to clear retired source uniforms.
        state = {};
        auto off_state = state;
        off_state.power = 0;
        const auto retirement_baseline = fixture.capture(off_state);
        static_cast<void>(fixture.capture(state));
        fixture.remove_source_component();
        const auto retired_source = fixture.capture(state);
        const auto retired_contribution = fixture.contribution(state);
        const bool retired_matches = matches(retired_source, retirement_baseline) &&
            peak(retired_contribution) < 2e-6;
        fixture.restore_source_component();
        const auto recovered_source = fixture.contribution(state);
        const bool recovered_matches = matches(recovered_source, full);
        valid &= retired_matches && recovered_matches;

        // Background transmission is independent of the additive halo. Four
        // actual native images isolate extinction without reproducing its math.
        state = {};
        const auto on_black = fixture.capture(state);
        state.power = 0;
        const auto off_black = fixture.capture(state);
        state.background = 1;
        const auto off_white = fixture.capture(state);
        state.power = 1;
        const auto on_white = fixture.capture(state);
        double transmission_error = 0;
        for (std::size_t i = 0; i < on_black.size(); ++i) {
            if (i % 4 == 3) continue;
            const double on_t = static_cast<double>(on_white[i]) - on_black[i];
            const double off_t = static_cast<double>(off_white[i]) - off_black[i];
            transmission_error = std::max(transmission_error, std::abs(on_t - off_t));
        }
        valid &= transmission_error < 2e-5 && save_pixels("full-contribution", full) &&
            save_pixels("partial-contribution", partial) && save_pixels("inside-contribution", inside) &&
            save_pixels("off-axis-contribution", off_axis);
        std::printf("%s full(sum/peak)=%.8g/%.8g half=%.8g blocked=%.8g partial=%.8g "
            "inside=%.8g offAxis=%.8g transmissionError=%.8g bounded=%s "
            "sourceRetired=%s sourceRecovered=%s: %s\n", kTag,
            full_sum, full_peak, channel_sum(half), peak(blocked), channel_sum(partial),
            channel_sum(inside), channel_sum(off_axis), transmission_error,
            bounded ? "yes" : "NO", retired_matches ? "yes" : "NO",
            recovered_matches ? "yes" : "NO", valid ? "PASS" : "FAIL");
    } catch (const std::exception& error) {
        std::printf("%s FAIL: %s\n", kTag, error.what());
        valid = false;
    }
    engine->flushAndWait();
    if (owners() != baseline) {
        std::printf("%s FAIL: fixture-owned native resources did not return to baseline\n", kTag);
        valid = false;
    }
    return valid ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
