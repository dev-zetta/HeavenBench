/* Actual feature resources and scene membership through dry/wet transitions. */
#include "smoke_cases.h"

#include "rhi/features/rain_feature.h"
#include "rhi/filament_backend.h"
#include "rhi/frame_generation/temporal_effects.h"
#include "rhi/render_test_scene_assets.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/Texture.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace heaven::smoke {

CaseResult run_rain_activity_case(const CaseContext& ctx) {
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (auto initialized = backend.init_pipelines(ctx.config); !initialized)
        return CaseResult::Fail;

    RenderContext local = backend.debug_render_context();
    auto created = RenderTestSceneAssets::create(*local.engine);
    if (!created) return CaseResult::Fail;
    auto assets = std::move(*created);
    local.test_scene_assets = assets.get();
    local.scene = local.engine->createScene();
    if (!local.scene) return CaseResult::Fail;

    const auto destroy_texture = [&local](filament::Texture* texture) {
        if (texture) local.engine->destroy(texture);
    };
    const auto depth_texture = [&local](std::uint32_t width) {
        return filament::Texture::Builder().width(width).height(width).levels(1)
            .sampler(filament::Texture::Sampler::SAMPLER_2D)
            .format(filament::Texture::InternalFormat::R32F)
            .usage(filament::Texture::Usage::SAMPLEABLE).build(*local.engine);
    };
    std::unique_ptr<filament::Texture, decltype(destroy_texture)> first_depth(
        depth_texture(8), destroy_texture);
    std::unique_ptr<filament::Texture, decltype(destroy_texture)> next_depth(
        nullptr, destroy_texture);
    RainFeature rain;
    struct Cleanup {
        RainFeature& rain;
        RenderContext& context;
        ~Cleanup() {
            rain.destroy(context);
            context.engine->destroy(context.scene);
        }
    } cleanup{rain, local};
    if (!first_depth) return CaseResult::Fail;

    AppConfig config = ctx.config;
    config.features.rain = true;
    rain.init(local, config);
    if (!rain.enabled()) return CaseResult::Fail;
    const IRenderFeature& feature = rain;
    WeatherState weather;
    weather.cfg.dry_down_seconds = 0.5f;
    Camera camera;
    FrameContext frame;
    frame.weather = &weather;
    frame.camera = &camera;
    frame.viewport_w = ctx.width;
    frame.viewport_h = ctx.height;

    bool ok = true;
    const auto check = [&ok](bool condition, const char* reason) {
        if (!condition) std::printf("[smoke_rain_activity] FAIL: %s\n", reason);
        ok &= condition;
    };
    const auto temporal_count = [&rain] {
        std::vector<TemporalEffectDraw> draws;
        rain.append_temporal_draws(draws);
        return draws.size();
    };

    check(!feature.needs_depth_prepass(weather), "initial dry frame requested depth");
    rain.update(local, frame);
    check(local.scene->getRenderableCount() == 0 && temporal_count() == 0,
        "initial dry frame retained precipitation");
    check(rain.streak_build_count() == 0 && rain.splash_build_count() == 0,
        "initial dry frame allocated geometry");

    weather.set_intensity(0.8f);
    check(feature.needs_depth_prepass(weather), "first wet frame did not request depth before update");
    frame.depth_texture = first_depth.get();
    rain.update(local, frame);
    check(local.scene->getRenderableCount() == 2 && temporal_count() == 1,
        "wet scene/temporal membership is incomplete");
    check(rain.bound_depth_texture() == first_depth.get(), "first wet frame did not bind current depth");
    weather.advance(1.0f);
    weather.set_intensity(0.0f);
    weather.advance(0.25f);
    check(weather.effective_intensity() > 0.0f && feature.needs_depth_prepass(weather),
        "commanded zero cut off effective dry-down");
    rain.update(local, frame);
    check(local.scene->getRenderableCount() == 2 && temporal_count() == 1,
        "dry-down lost precipitation before reaching zero");

    weather.advance(0.25f);
    const float remaining_wetness = weather.surface_wetness_;
    check(weather.effective_intensity() == 0.0f && remaining_wetness > 0.0f,
        "dry fixture did not retain independent surface wetness");
    check(!feature.needs_depth_prepass(weather), "effective zero requested depth");
    frame.depth_texture = nullptr;
    rain.update(local, frame);
    check(local.scene->getRenderableCount() == 0 && temporal_count() == 0,
        "effective zero retained ordinary/temporal precipitation");
    check(!rain.needs_depth_prepass() && weather.surface_wetness_ == remaining_wetness,
        "inactive feature altered wetness or retained depth demand");
    rain.on_depth_prepass_begin(local);
    rain.on_depth_prepass_end(local);
    check(local.scene->getRenderableCount() == 0, "another depth consumer resurrected idle rain");

    rain.set_enabled(local, false);
    rain.set_enabled(local, true);
    check(local.scene->getRenderableCount() == 0, "dry toggle restored invisible geometry");
    rain.release_shared_targets(local);
    check(rain.bound_depth_texture() == nullptr, "idle target release retained borrowed depth");
    first_depth.reset();
    next_depth.reset(depth_texture(16));
    if (!next_depth) return CaseResult::Fail;
    weather.set_intensity(0.8f);
    check(feature.needs_depth_prepass(weather), "reactivation did not request fresh depth");
    frame.depth_texture = next_depth.get();
    rain.update(local, frame);
    check(rain.bound_depth_texture() == next_depth.get() && local.scene->getRenderableCount() == 2,
        "reactivation failed to bind replacement target/restore geometry");
    rain.set_enabled(local, false);
    check(local.scene->getRenderableCount() == 0 && temporal_count() == 0 &&
        !feature.needs_depth_prepass(weather), "disabled rain retained render work");
    rain.set_enabled(local, true);
    check(local.scene->getRenderableCount() == 2 && temporal_count() == 1,
        "wet toggle did not restore the same geometry");
    check(rain.streak_build_count() == 1 && rain.splash_build_count() == 1,
        "dry/wet/resize/toggle rebuilt persistent geometry");

    std::printf("[smoke_rain_activity] %s: dry/wet/dry-down, ordinary/temporal "
        "membership, first-frame depth, idle resize and persistent buffers\n", ok ? "PASS" : "FAIL");
    return ok ? CaseResult::Pass : CaseResult::Fail;
}

} // namespace heaven::smoke
