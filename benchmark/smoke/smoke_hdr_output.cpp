// Exercise the real HDR output subsystem and embedded material on the active
// GPU. This validates signal values in an offscreen fp16 target, not a display's
// HDR metadata, compositor behavior, calibration, or physical luminance.
#include "smoke_cases.h"

#include "rhi/filament_backend.h"
#include "rhi/hdr_output.h"

#include <filament/Camera.h>
#include <filament/Engine.h>
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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <utility>

namespace heaven::smoke {
namespace {

constexpr const char* kTag = "[smoke_hdr_output]";
constexpr std::uint32_t kExtent = 4;
constexpr std::size_t kPixels = static_cast<std::size_t>(kExtent) * kExtent;
using Color = std::array<double, 3>;
using Matrix = std::array<Color, 3>;

struct Sample {
    const char* name;
    filament::math::float4 scene;
    // Premultiplied sRGB code values, matching the production UI target.
    filament::math::float4 layer;
    bool has_scene = true;
    bool has_layer = true;
    float reference_nits = 100.0f;
    float peak_nits = 1000.0f;
    bool clear_removed_layer = false;
};

Color multiply(const Matrix& matrix, const Color& color) {
    Color result{};
    for (std::size_t row = 0; row < result.size(); ++row) {
        for (std::size_t col = 0; col < color.size(); ++col) {
            result[row] += matrix[row][col] * color[col];
        }
    }
    return result;
}

double encode_pq(double nits) {
    // ST.2084's exact rational constants, evaluated in double precision.
    double p = std::pow(std::max(nits, 0.0) / 10000.0, 2610.0 / 16384.0);
    return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
                    (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
}

Color expected_signal(const Sample& sample) {
    Color linear{};
    if (sample.has_scene) {
        linear = {sample.scene.r, sample.scene.g, sample.scene.b};
    }
    if (sample.has_layer) {
        double alpha = std::clamp(static_cast<double>(sample.layer.a), 0.0, 1.0);
        Color srgb{sample.layer.r, sample.layer.g, sample.layer.b};
        for (double& channel : srgb) {
            channel = alpha > 0.0 ? std::clamp(channel / alpha, 0.0, 1.0) : 0.0;
            channel = channel <= 0.04045 ? channel / 12.92
                : std::pow((channel + 0.055) / 1.055, 2.4);
        }
        // Independent two-stage D65 transform via CIE XYZ, rather than the
        // shader's pre-multiplied direct conversion matrix. Primaries agree
        // with Filament's ColorSpaceUtils.h; rows are written explicitly here.
        constexpr Matrix srgb_to_xyz{{
            {0.4124560, 0.3575760, 0.1804380},
            {0.2126730, 0.7151520, 0.0721750},
            {0.0193339, 0.1191920, 0.9503040}}};
        constexpr Matrix xyz_to_rec2020{{
            {1.7166634, -0.3556733, -0.2533681},
            {-0.6666738, 1.6164557, 0.0157683},
            {0.0176425, -0.0427770, 0.9422433}}};
        const Color ui = multiply(xyz_to_rec2020, multiply(srgb_to_xyz, srgb));
        for (std::size_t channel = 0; channel < linear.size(); ++channel) {
            linear[channel] = linear[channel] * (1.0 - alpha) + ui[channel] * alpha;
        }
    }
    for (double& channel : linear) {
        channel = encode_pq(std::min(channel * sample.reference_nits,
                                     static_cast<double>(sample.peak_nits)));
    }
    return linear;
}

class OutputFixture {
public:
    explicit OutputFixture(filament::Engine& engine) : engine_(engine) {
        renderer_ = engine_.createRenderer();
        renderer_->setDisplayInfo({.refreshRate = 0.0f});
        renderer_->setFrameRateOptions({.interval = 0});
        loading_renderer_ = engine_.createRenderer();
        loading_renderer_->setDisplayInfo({.refreshRate = 0.0f});
        loading_renderer_->setFrameRateOptions({.interval = 0});
        swapchain_ = engine_.createSwapChain(kExtent, kExtent);
        camera_entity_ = utils::EntityManager::get().create();
        camera_ = engine_.createCamera(camera_entity_);
        camera_->setProjection(filament::Camera::Projection::ORTHO, -1, 1, -1, 1,
                               0.1, 10.0);
        output_ = std::make_unique<HdrOutput>(BackendCore{
            .engine = &engine_, .renderer = renderer_, .camera = camera_});
    }

    ~OutputFixture() {
        if (scene_view_) scene_view_->setRenderTarget(nullptr);
        if (layer_view_) layer_view_->setRenderTarget(nullptr);
        if (scene_) scene_->setSkybox(nullptr);
        if (layer_scene_) layer_scene_->setSkybox(nullptr);
        output_->set_output_target(nullptr);
        output_.reset();
        engine_.destroy(scene_view_);
        engine_.destroy(layer_view_);
        engine_.destroy(scene_);
        engine_.destroy(layer_scene_);
        engine_.destroy(scene_sky_);
        engine_.destroy(layer_sky_);
        engine_.destroy(encoded_target_);
        engine_.destroy(encoded_texture_);
        engine_.destroyCameraComponent(camera_entity_);
        utils::EntityManager::get().destroy(camera_entity_);
        engine_.destroy(renderer_);
        engine_.destroy(loading_renderer_);
        engine_.destroy(swapchain_);
    }

    OutputFixture(const OutputFixture&) = delete;
    OutputFixture& operator=(const OutputFixture&) = delete;

    bool init() {
        if (!output_->ensure(kExtent, kExtent)) return false;
        // Filament explicitly permits combinations of these usage flags.
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
        constexpr auto usage = static_cast<filament::Texture::Usage>(
            std::to_underlying(filament::Texture::Usage::COLOR_ATTACHMENT) |
            std::to_underlying(filament::Texture::Usage::BLIT_SRC));
        encoded_texture_ = filament::Texture::Builder()
            .width(kExtent).height(kExtent).levels(1)
            .format(filament::Texture::InternalFormat::RGBA16F)
            .usage(usage).build(engine_);
        encoded_target_ = filament::RenderTarget::Builder()
            .texture(filament::RenderTarget::AttachmentPoint::COLOR0, encoded_texture_)
            .build(engine_);
        output_->set_output_target(encoded_target_);

        scene_ = engine_.createScene();
        layer_scene_ = engine_.createScene();
        scene_sky_ = filament::Skybox::Builder().color({0, 0, 0, 1}).build(engine_);
        layer_sky_ = filament::Skybox::Builder().color({0, 0, 0, 0}).build(engine_);
        scene_->setSkybox(scene_sky_);
        layer_scene_->setSkybox(layer_sky_);
        scene_view_ = make_view(scene_, output_->scene_rt());
        layer_view_ = make_view(layer_scene_, output_->sdr_layer_rt());
        return true;
    }

    bool check(const Sample& sample) {
        auto* active_renderer = use_loading_renderer_ ? loading_renderer_ : renderer_;
        const auto scene_history = renderer_->getFrameInfoHistory();
        scene_sky_->setColor(sample.scene);
        layer_sky_->setColor(sample.layer);
        output_->set_luminance(sample.reference_nits, sample.peak_nits);
        if (!output_->ensure(kExtent, kExtent)) return false;
        layer_view_->setBlendMode(initial_layer_mode_);
        output_->bind_sdr_layer(*layer_view_);
        if (layer_view_->getBlendMode() != filament::View::BlendMode::OPAQUE ||
            layer_view_->getRenderTarget() != output_->sdr_layer_rt() ||
            layer_scene_->getSkybox() != output_->sdr_layer_background()) {
            std::printf("%s FAIL: production layer binder did not reset target/background/blend\n",
                        kTag);
            return false;
        }
        // A uniform source Skybox supplies test pixels in place of UI geometry;
        // removing it leaves the production binder's transparent clear Skybox.
        if (!sample.clear_removed_layer) layer_scene_->setSkybox(layer_sky_);
        output_->prepare(kExtent, kExtent, sample.has_scene, sample.has_layer);
        if (!active_renderer->beginFrame(swapchain_)) {
            std::printf("%s FAIL: frame rejected for %s\n", kTag, sample.name);
            return false;
        }
        active_renderer->render(scene_view_);
        active_renderer->render(layer_view_);
        if (use_loading_renderer_) output_->encode(*active_renderer);
        else output_->encode();

        std::array<float, kPixels * 4> pixels{};
        pixels.fill(std::numeric_limits<float>::quiet_NaN());
        std::atomic_bool completed{false};
        active_renderer->readPixels(encoded_target_, 0, 0, kExtent, kExtent,
            filament::backend::PixelBufferDescriptor(pixels.data(), sizeof(pixels),
                filament::backend::PixelDataFormat::RGBA,
                filament::backend::PixelDataType::FLOAT,
                [](void*, std::size_t, void* user) {
                    static_cast<std::atomic_bool*>(user)->store(true);
                }, &completed));
        active_renderer->endFrame();
        // GPU finish plus callback purge, so pixels and callback state remain
        // alive until readback is complete. The smoke process has an outer timeout.
        engine_.flushAndWait();
        if (!completed.load()) {
            std::printf("%s FAIL: readback incomplete for %s\n", kTag, sample.name);
            return false;
        }
        if (use_loading_renderer_) {
            const auto history = renderer_->getFrameInfoHistory();
            if (history.size() != scene_history.size() ||
                    (!history.empty() && history[0].frameId != scene_history[0].frameId)) {
                std::printf("%s FAIL: loading encode advanced scene renderer history\n", kTag);
                return false;
            }
        }

        const Color expected = expected_signal(sample);
        // Input/output are fp16; permit rounding and shader pow approximation,
        // while remaining well below errors from gamma or premultiplication bugs.
        constexpr double tolerance = 0.0015;
        for (std::size_t pixel = 0; pixel < kPixels; ++pixel) {
            for (std::size_t channel = 0; channel < 4; ++channel) {
                double want = channel < 3 ? expected[channel] : 1.0;
                float actual = pixels[pixel * 4 + channel];
                if (!std::isfinite(actual) || std::abs(actual - want) > tolerance) {
                    std::printf("%s FAIL: %s pixel=%zu channel=%zu actual=%.7f expected=%.7f\n",
                                kTag, sample.name, pixel, channel,
                                static_cast<double>(actual), want);
                    return false;
                }
            }
        }
        return true;
    }

    void set_layer_blend_mode(filament::View::BlendMode mode) {
        initial_layer_mode_ = mode;
        use_loading_renderer_ = mode == filament::View::BlendMode::TRANSLUCENT;
    }

private:
    filament::View* make_view(filament::Scene* scene, filament::RenderTarget* target) {
        auto* view = engine_.createView();
        view->setScene(scene);
        view->setCamera(camera_);
        view->setRenderTarget(target);
        view->setViewport({0, 0, kExtent, kExtent});
        view->setPostProcessingEnabled(false);
        view->setShadowingEnabled(false);
        view->setBlendMode(filament::View::BlendMode::OPAQUE);
        return view;
    }

    filament::Engine& engine_;
    filament::Renderer* renderer_ = nullptr;
    filament::Renderer* loading_renderer_ = nullptr;
    bool use_loading_renderer_ = false;
    filament::SwapChain* swapchain_ = nullptr;
    utils::Entity camera_entity_;
    filament::Camera* camera_ = nullptr;
    std::unique_ptr<HdrOutput> output_;
    filament::Texture* encoded_texture_ = nullptr;
    filament::RenderTarget* encoded_target_ = nullptr;
    filament::Scene* scene_ = nullptr;
    filament::Scene* layer_scene_ = nullptr;
    filament::Skybox* scene_sky_ = nullptr;
    filament::Skybox* layer_sky_ = nullptr;
    filament::View* scene_view_ = nullptr;
    filament::View* layer_view_ = nullptr;
    filament::View::BlendMode initial_layer_mode_ = filament::View::BlendMode::OPAQUE;
};

} // namespace

CaseResult run_hdr_output_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (const auto initialized = backend.init_pipelines(ctx.config); !initialized) {
        std::printf("%s FAIL: %s\n", kTag, initialized.error().message.c_str());
        return CaseResult::Fail;
    }
    auto* engine = backend.debug_render_context().engine;
    if (!engine) {
        std::printf("%s FAIL: active backend has no engine\n", kTag);
        return CaseResult::Fail;
    }
    // Fixed published signal points independently sanity-check the CPU oracle.
    if (std::abs(encode_pq(100.0) - 0.508078421517399) > 1e-10 ||
        std::abs(encode_pq(400.0) - 0.652578597563067) > 1e-10) {
        std::printf("%s FAIL: ST.2084 reference points disagree\n", kTag);
        return CaseResult::Fail;
    }
    OutputFixture fixture(*engine);
    if (!fixture.init()) {
        std::printf("%s FAIL: HDR output pipeline unavailable\n", kTag);
        return CaseResult::Fail;
    }
    const std::array samples{
        Sample{"black", {0, 0, 0, 1}, {0, 0, 0, 0}, true, false},
        Sample{"scene 100 nits", {1, 1, 1, 1}, {0, 0, 0, 0}, true, false},
        Sample{"peak clamp", {16, 10, 4, 1}, {0, 0, 0, 0}, true, false},
        Sample{"white splash 400 nits", {7, 3, 5, 1}, {1, 1, 1, 1}, false, true, 400},
        Sample{"removed UI clears previous white", {0.25f, 0.5f, 1, 1},
               {0, 0, 0, 0}, true, true, 100, 1000, true},
        Sample{"half white over black", {0, 0, 0, 1}, {0.5f, 0.5f, 0.5f, 0.5f},
               true, true, 400},
        Sample{"sRGB midgray", {1, 0, 0, 1}, {0.5f, 0.5f, 0.5f, 1}},
        Sample{"sRGB low branch", {0, 0, 0, 1}, {0.03125f, 0.03125f, 0.03125f, 1}},
        Sample{"red primary", {0, 0, 0, 1}, {1, 0, 0, 1}},
        Sample{"green primary", {0, 0, 0, 1}, {0, 1, 0, 1}},
        Sample{"blue primary", {0, 0, 0, 1}, {0, 0, 1, 1}},
        Sample{"half red over scene", {0.25f, 0.5f, 1, 1}, {0.5f, 0, 0, 0.5f}},
        Sample{"small alpha midgray", {0.25f, 0.5f, 1, 1},
               {0.03125f, 0.03125f, 0.03125f, 0.0625f}},
        Sample{"zero alpha ignores RGB", {0.25f, 0.5f, 1, 1}, {1, 1, 1, 0}},
        Sample{"disabled UI ignores stale texture", {0.25f, 0.5f, 1, 1},
               {1, 1, 1, 1}, true, false},
        Sample{"both layers disabled", {7, 3, 5, 1}, {1, 1, 1, 1}, false, false}};
    // Splash starts opaque; filagui starts translucent. The production binder
    // must make either isolated HDR layer opaque so removed UI actually clears,
    // while its texture still preserves the UI pixels' premultiplied alpha.
    constexpr std::array modes{filament::View::BlendMode::OPAQUE,
                               filament::View::BlendMode::TRANSLUCENT};
    for (const auto mode : modes) {
        fixture.set_layer_blend_mode(mode);
        for (const auto& sample : samples) {
            if (!fixture.check(sample)) {
                std::printf("%s layer mode: %s\n", kTag,
                    mode == filament::View::BlendMode::OPAQUE ? "opaque" : "translucent");
                return CaseResult::Fail;
            }
        }
    }
    std::printf("%s PASS: %zu real-material PQ/color/alpha/clear readbacks "
                "(opaque + translucent callers, scene + loading renderers)\n",
                kTag, samples.size() * modes.size());
    return CaseResult::Pass;
}

} // namespace heaven::smoke
