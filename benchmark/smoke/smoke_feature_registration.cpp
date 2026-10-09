/*
 * HeavenBench — feature-registration smoke case
 *                     (renderer-linked, GATED)
 *
 * THE Renderer SHALL register
 * each new Render_Feature exactly once in `init_features`. This case asserts the
 * cardinality of that registration directly: after the render pipeline is brought
 * up (init_features runs inside init_pipelines), the backend's owning
 * render_features_ vector contains EXACTLY ONE feature named "rain"  and
 * EXACTLY ONE named "wet-reflections"  — no missing registration, no
 * accidental double-registration.
 *
 * This mirrors smoke_rain_depth_prepass.cpp (its SIBLING case) for the harness
 * conventions — the GraphicsApi::Filament guard + static_cast<FilamentBackend&>,
 * and the init_pipelines SKIP that tolerates a headless/driver-less environment
 * that passed the shared gate. It does NOT render a frame.
 *
 * OBSERVABILITY — the "exactly one" assertion needs a COUNT, which
 * debug_find_feature(name) cannot give (it returns the first match, good for
 * existence + fetching but blind to duplicates). So this case uses a minimal
 * test-only seam on FilamentBackend, mirroring the existing debug_* seams
 * (debug_render_context / debug_build_counters / debug_find_feature /
 * debug_depth_texture):
 *   - debug_count_features(name) returns how many entries in render_features_
 *     have name() equal to the argument. The renderer never calls it; it is
 *     documented test-only and adds no per-effect logic to the backend.
 * debug_find_feature(name) is also asserted non-null for each feature so a
 * "registered zero times" failure prints a clear, specific diagnostic.
 *
 * NO ASSET DEPENDENCY — unlike the sibling rain build-once / depth-pre-pass
 * cases, this case needs NO "rain unavailable ⇒ SKIP" guard. Registration is
 * asset-independent: init_features unconditionally constructs each feature,
 * calls init() (which may set the feature available_=false/enabled_=false when
 * its precipitation assets are absent), and pushes it into render_features_
 * regardless. The feature is therefore PRESENT in the vector even when it
 * initialised disabled, so the count holds whether or not rain assets loaded —
 * the contract this case asserts is purely about registration cardinality.
 *
 * CUMULATIVE-BACKEND ROBUSTNESS: the harness shares ONE backend across cases.
 * render_features_ is populated exactly once by init_features (called from the
 * idempotent init_pipelines), so the count is stable no matter how many cases
 * ran before this one — no baseline bookkeeping is needed.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if its own deeper bring-up (init_pipelines)
 * fails, tolerating a headless/driver-less environment that passed the gate.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "rhi/render_feature.h"
#include "engine/types.h"

#include <cstdio>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_feature_registration]";

} // namespace

CaseResult run_feature_registration_case(const CaseContext& ctx) {
    // The debug_count_features / debug_find_feature accessors below are
    // FilamentBackend-specific test instrumentation. The harness always
    // constructs a FilamentBackend (it is the sole IRhiBackend), so this is
    // safe; api() lets us bail gracefully if that ever changes rather than
    // risking a bad cast — mirroring smoke_rain_depth_prepass.cpp.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: case needs the Filament backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the render pipeline up so init_features() has run and the registered
    // features (incl. rain and wet-reflections) exist in render_features_. A
    // failure here means the environment can't drive the backend (no usable
    // GPU) — treat it as a SKIP so the gated test still passes, mirroring the
    // harness's headless fallbacks. init_pipelines is idempotent, so an earlier
    // case having already run it is fine. (No active frame is needed: the
    // features are PUSHED into render_features_ during init_features regardless
    // of asset availability — see this file's header.)
    if (auto r = backend.init_pipelines(ctx.config); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    // ── Assertions  ─────────────────────
    bool ok = true;

    // Exactly one feature named "rain" is registered. A clear-diagnostic
    // existence check first (so a "registered zero times" failure names the
    // feature), then the cardinality check that catches both a missing and a
    // duplicate registration.
    if (!backend.debug_find_feature("rain")) {
        std::printf("%s FAIL: no feature named \"rain\" is registered — the "
                    "rain feature must be registered exactly once in "
                    "init_features (R6.2)\n", kTag);
        ok = false;
    }
    const int rain_count = backend.debug_count_features("rain");
    if (rain_count != 1) {
        std::printf("%s FAIL: render_features_ contains %d features named "
                    "\"rain\" (expected exactly 1 — R6.2)\n", kTag, rain_count);
        ok = false;
    }

    // Exactly one feature named "wet-reflections" is registered — the
    // additional modern technique selected by these changes, also a Render_Feature
    // registered once in init_features.
    if (!backend.debug_find_feature("wet-reflections")) {
        std::printf("%s FAIL: no feature named \"wet-reflections\" is "
                    "registered — the wet-reflections feature must be "
                    "registered exactly once in init_features (R9.1)\n", kTag);
        ok = false;
    }
    const int wet_count = backend.debug_count_features("wet-reflections");
    if (wet_count != 1) {
        std::printf("%s FAIL: render_features_ contains %d features named "
                    "\"wet-reflections\" (expected exactly 1 — R9.1)\n",
                    kTag, wet_count);
        ok = false;
    }

    if (!ok) return CaseResult::Fail;

    std::printf("%s feature registration honored: exactly one \"rain\" feature "
                "(R6.2) and exactly one \"wet-reflections\" feature (R9.1) are "
                "registered in render_features_\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
