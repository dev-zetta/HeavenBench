/*
 * HeavenBench — rain depth-pre-pass participation smoke case
 *                     (renderer-linked, GATED)
 *
 * WHERE a new Render_Feature samples
 * scene depth, THE Render_Feature SHALL return true from `needs_depth_prepass`
 * and SHALL read scene depth only through the shared depth texture provided in
 * `FrameContext`. The Splash_System inside RainFeature projects impacts onto the
 * ground reconstructed from sampled depth, so with rain active the
 * feature returns `needs_depth_prepass() == true`, the backend runs the shared
 * half-res depth pre-pass, and the feature binds `FrameContext::depth_texture`.
 *
 * This mirrors smoke_rain_build_once.cpp (its SIBLING case) exactly for the
 * harness conventions — the render_one_active_frame helper (a stack
 * WeatherState at intensity 1.0, empty instance/mesh spans, backend.render_frame),
 * the init_pipelines SKIP, the "rain feature unavailable ⇒ SKIP" robustness
 * pattern keyed off debug_build_counters().rain_streak_builds, and build-counter
 * observation — but asserts the depth-participation contract instead of
 * build-once discipline.
 *
 * OBSERVABILITY — three test-only seams on FilamentBackend, mirroring the
 * existing debug_render_context() / debug_build_counters() instrumentation (the
 * renderer never calls them; documented as test-only; no per-effect logic added
 * to the backend):
 *   - debug_find_feature("rain") returns the registered RainFeature so the case
 *     can assert needs_depth_prepass() == true after an active frame (the
 *     feature is enabled_ && built_).
 *   - debug_depth_texture() returns the shared depth pre-pass texture, non-null
 *     only after the pass has actually run — proving the backend ran the shared
 *     depth pass *because* a depth-sampling feature (rain) requested it.
 *   - RainFeature::bound_depth_texture() (a build-count-style getter) returns the
 *     depth texture the splash instance last bound, so the case asserts the
 *     feature reads scene depth *only* through FrameContext::depth_texture: the
 *     bound handle equals debug_depth_texture() exactly.
 *
 * TIMING — the depth texture appears on the SECOND active frame, not the first:
 * the backend computes need_depth_pass from each enabled feature's
 * needs_depth_prepass() BEFORE update_features() runs. On frame 1 the rain
 * feature has not built yet (built_ == false ⇒ needs_depth_prepass() == false),
 * so no depth pass runs and FrameContext::depth_texture is null; update_features
 * then builds the geometry (built_ becomes true). On frame 2 needs_depth_prepass()
 * is true, so the backend ensures the shared depth target before update_features,
 * FrameContext::depth_texture is non-null, and RainFeature::update() binds it.
 * This case therefore renders several active frames before asserting.
 *
 * CUMULATIVE-BACKEND ROBUSTNESS: the harness shares ONE backend across cases.
 * The contract asserted here is steady-state (it holds on every frame once rain
 * is built), so no baseline bookkeeping is needed — the case simply renders
 * enough active frames for the deferred build + depth bind to settle, then
 * asserts the live state.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines)
 * fails, tolerating a headless/driver-less environment that passed the gate. It
 * also SKIPs when the registered RainFeature is UNAVAILABLE in this environment
 * (its precipitation assets did not load at init, so an active frame builds
 * nothing): rain then never returns needs_depth_prepass() == true and no depth
 * pass runs, which is the degraded headless-equivalent case rather than a
 * defect — exactly the SKIP path the sibling build-once case uses.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "rhi/render_feature.h"
#include "rhi/features/rain_feature.h"
#include "engine/types.h"
#include "engine/camera.h"

#include <filament/Texture.h>

#include <cstdio>
#include <span>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_rain_depth_prepass]";

// Render exactly one ACTIVE rain frame with an empty scene. Identical to the
// helper in smoke_rain_build_once.cpp: a stack WeatherState at intensity 1.0 so
// rain is active, which makes the backend's update_features loop call
// RainFeature::update() (build on the first call, bind depth on the next).
void render_one_active_frame(IRhiBackend& backend, const Camera& camera,
                             const LightingParams& lighting,
                             const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    WeatherState weather;
    weather.set_intensity(1.0f); // rain active ⇒ RainFeature::update() runs
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

} // namespace

CaseResult run_rain_depth_prepass_case(const CaseContext& ctx) {
    // The depth-texture / feature-lookup accessors below are FilamentBackend-
    // specific test instrumentation. The harness always constructs a
    // FilamentBackend (it is the sole IRhiBackend), so this is safe; api() lets
    // us bail gracefully if that ever changes rather than risking a bad cast —
    // mirroring smoke_rain_asset_failure.cpp.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: case needs the Filament backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the render pipeline up so the scene/render features (incl. the
    // registered RainFeature and the shared DepthPrePass) exist. A failure here
    // means the environment can't drive the backend (no usable GPU) — treat it
    // as a SKIP so the gated test still passes, mirroring the harness's
    // headless fallbacks. init_pipelines is idempotent, so an earlier case
    // having already run it is fine.
    if (auto r = backend.init_pipelines(ctx.config); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    Camera camera;
    LightingParams lighting = lighting_for_time_of_day(ctx.config.time_of_day);
    TonemapParams  tonemap;

    const int w = ctx.width;
    const int h = ctx.height;

    // Render several ACTIVE rain frames. The depth texture is produced on the
    // SECOND active frame (see the file header: build on frame 1, depth pass +
    // bind on frame 2), so a handful of frames lets the deferred build and the
    // depth bind settle into the steady state the contract describes.
    constexpr int kFrames = 4;
    for (int i = 0; i < kFrames; ++i) {
        render_one_active_frame(backend, camera, lighting, tonemap, w, h);
    }

    // ── Environment robustness: rain feature unavailable ⇒ SKIP ──
    // The registered RainFeature only builds (and only then returns
    // needs_depth_prepass() == true) when its precipitation assets loaded at
    // init. In an environment where those assets are absent — e.g. when
    // the harness CWD can't resolve the relative data_dir, the same path the
    // sibling rain_asset_failure / build_once cases tolerate against this shared
    // backend — the feature inits disabled and active frames build nothing, so
    // no depth pass ever runs. That is the headless-equivalent degraded case:
    // SKIP rather than FAIL. rain_streak_builds == 0 after several active frames
    // means the feature never built, exactly as the build-once case detects it.
    const IRhiBackend::BackendBuildCounters counters = backend.debug_build_counters();
    if (counters.rain_streak_builds == 0) {
        std::printf("%s SKIP: rain feature unavailable in this environment "
                    "(precipitation assets did not load; nothing built over %d "
                    "active frames, so no depth pass runs)\n", kTag, kFrames);
        return CaseResult::Skip;
    }

    // ── Assertions  ───────────────────────────
    bool ok = true;

    // The registered "rain" feature must be discoverable through the test-only
    // lookup seam. (If it isn't, the registration/observability contract this
    // case rests on is broken.)
    const IRenderFeature* rain = backend.debug_find_feature("rain");
    if (!rain) {
        std::printf("%s FAIL: debug_find_feature(\"rain\") returned null — the "
                    "rain feature is not registered (R6.4 cannot be observed)\n",
                    kTag);
        return CaseResult::Fail;
    }

    // The depth-sampling feature returns true from needs_depth_prepass()
    // once it is enabled and built — its splash projection samples scene depth,
    // so it declares its participation in the shared pass.
    if (!rain->needs_depth_prepass()) {
        std::printf("%s FAIL: rain->needs_depth_prepass() is false after %d "
                    "active frames (a depth-sampling feature must declare "
                    "participation once enabled+built — R6.4)\n", kTag, kFrames);
        ok = false;
    }

    // Because rain requested it, the backend ran the SHARED depth
    // pre-pass and it produced a valid (non-null) half-res depth texture. A
    // null here would mean the pass never ran despite the feature declaring it
    // needs depth.
    const filament::Texture* shared_depth = backend.debug_depth_texture();
    if (!shared_depth) {
        std::printf("%s FAIL: debug_depth_texture() is null after %d active "
                    "frames (the shared depth pre-pass must run when a feature "
                    "returns needs_depth_prepass()==true — R6.4)\n",
                    kTag, kFrames);
        ok = false;
    }

    // The feature reads scene depth ONLY through FrameContext::depth_texture:
    // the depth texture the splash instance bound must be that exact shared
    // texture. The "rain" feature is a RainFeature (the sole feature with that
    // name); static_cast to reach its build-count-style bound_depth_texture()
    // getter, mirroring how smoke_rain_asset_failure.cpp drives RainFeature
    // directly. Asserting equality (not just non-null) proves the feature did
    // not source depth from anywhere but the FrameContext handle.
    const auto* rain_impl = static_cast<const RainFeature*>(rain);
    const filament::Texture* bound = rain_impl->bound_depth_texture();
    if (!bound) {
        std::printf("%s FAIL: rain bound_depth_texture() is null after %d active "
                    "frames (the feature must bind FrameContext::depth_texture "
                    "once the shared pass has produced it — R6.4)\n",
                    kTag, kFrames);
        ok = false;
    } else if (shared_depth && bound != shared_depth) {
        std::printf("%s FAIL: rain bound a depth texture (%p) that differs from "
                    "the shared depth pre-pass texture (%p) — depth must be read "
                    "only through FrameContext::depth_texture (R6.4)\n", kTag,
                    static_cast<const void*>(bound),
                    static_cast<const void*>(shared_depth));
        ok = false;
    }

    if (!ok) return CaseResult::Fail;

    std::printf("%s rain depth participation honored: "
                "needs_depth_prepass()=true, shared depth pre-pass produced a "
                "non-null texture (%p), and rain bound exactly that texture via "
                "FrameContext::depth_texture (R6.4)\n",
                kTag, static_cast<const void*>(shared_depth));
    return CaseResult::Pass;
}

} // namespace heaven::smoke
