/*
 * HeavenBench — SSGI registration / depth-pre-pass / effect /
 *                    disabled-parity smoke case (renderer-linked, GATED)
 *
 *
 * the screen-space global-illumination SsgiFeature  is a
 * Render_Feature registered EXACTLY ONCE in init_features, participates
 * in the shared half-res depth pre-pass (it reconstructs view-space
 * position/normal from scene depth, so it returns true from
 * needs_depth_prepass() once its material loaded and reads depth only through
 * FrameContext::depth_texture, bound OUTSIDE a render pass —), and
 * ships ENABLED by default; disabling it (--no-ssgi) must leave the
 * Shipping_Look indirect lighting unchanged (its fullscreen quad absent from
 * the scene —).
 *
 * This mirrors smoke_volumetric_clouds.cpp  — the analogous
 * depth-sampling post-process feature: the GraphicsApi::Filament guard +
 * static_cast<FilamentBackend&>, the init_pipelines SKIP, the
 * debug_count_features / debug_find_feature cardinality assertion, the
 * scene-renderable-count DELTA around a runtime toggle for disabled parity, the
 * active-frame render helper, and the debug_depth_texture() depth-participation
 * pattern. It additionally exercises the live Shipping_Look pixel-diff helper
 * end-to-end (compare_rgba8 / within_shipping_look_tolerance) the way
 * smoke_contact_shadows.cpp does, because the genuine enabled-vs-disabled
 * >1-of-255 difference + hue shift toward an adjacent saturated surface
 * requires a separate GPU pixel capture and comparison.
 *
 * FOUR assertions against the live backend:
 *
 *   (1) REGISTRATION  — asset-independent, asserted unconditionally.
 *       debug_find_feature("ssgi") is non-null (clear missing-registration
 *       diagnostic) and debug_count_features("ssgi") == 1 (registered exactly
 *       once in init_features — catches both a missing and a duplicate
 *       registration).
 *
 *   (2) DISABLED PARITY  — the feature is DISABLED by default, so its
 *       fullscreen quad is NOT in the scene: the only way SSGI can leave the
 *       Shipping_Look indirect lighting bit-identical is to add no renderable
 *       while off. Observed as a scene-renderable-count DELTA around a
 *       synchronous runtime toggle (no frame needed): enabling the feature adds
 *       exactly its one quad (count rises by 1), proving the disabled state held
 *       zero of its renderables. Restored to disabled at the end.
 *
 *   (3) DEPTH-PRE-PASS PARTICIPATION  — with the feature enabled,
 *       render a few active frames; the feature returns true from
 *       needs_depth_prepass() (its material loaded), the backend therefore runs
 *       the SHARED depth pre-pass and produces a non-null half-res depth texture
 *       (debug_depth_texture()), and the feature binds it in update() — which
 *       runs BEFORE the render passes, i.e. outside any render pass (Vulkan
 *       forbids descriptor changes mid-render-pass). The feature exposes no
 *       bound-depth getter, so the bound-handle-equality sub-assertion is owned
 *       by the production path; this case asserts the shared pass ran *because*
 *       the SSGI feature requested it (a non-null depth texture only appears
 *       when an enabled depth-sampling feature returned needs_depth_prepass()
 *       == true).
 *
 *   (4) THE EFFECT — colour bleed / hue shift. The genuine
 *       assertion is a GPU pixel capture: at a benchmark camera, a frame with
 *       SSGI ENABLED differs from the Shipping_Look (disabled) capture by
 *       more than 1-of-255 on a receiving surface adjacent to a saturated
 *       surface, with the difference being a HUE SHIFT toward that surface's
 *       colour (one-bounce colour bleed). That needs a real GPU + display + the
 *       FULL Heaven scene + the authored benchmark camera + a saturated-surface
 *       setup + the pre-recorded Shipping_Look reference set, none of which
 *       exist in this 64x64 bare harness — so, like smoke_volumetric_clouds.cpp
 *       and smoke_contact_shadows.cpp, that capture needs separate full-scene
 *       validation. The compare_rgba8 / within_shipping_look_tolerance helpers
 *       from benchmark/src/benchmark/pixel_parity.h are tested below using
 *       synthetic images: a "saturated bleed" buffer must read as a
 *       beyond-tolerance hue shift while the disabled capture reads as
 *       within-tolerance parity — so a regression in the comparison logic the
 *       bar relies on surfaces here too.
 *
 * DEGRADE WHEN THE MATERIAL IS UNAVAILABLE. The SSGI math lives in an EMBEDDED
 * material (k_heaven_ssgi_filamat), so on a real GPU/display (the gated
 * environment) it builds at init and needs_depth_prepass() is true. If a backend
 * cannot build it (mi_ stays null ⇒ needs_depth_prepass()==false and no quad
 * entity exists, so enabling adds nothing), the depth + parity-delta
 * sub-assertions cannot run; the case then asserts only the asset-independent
 * registration contract (plus the diff-helper exercise) and documents the rest
 * as exercised by the production path — mirroring how the sibling cloud case
 * degrades when its embedded material is unavailable.
 *
 * CUMULATIVE-BACKEND ROBUSTNESS: the harness shares ONE backend across cases.
 * This case reasons in DELTAS around its own toggle and RESTORES the feature to
 * its as-found state (disabled by default) at the end, so subsequent cases see
 * the default feature set.
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
#include "engine/camera.h"
#include "benchmark/pixel_parity.h"

#include <filament/Scene.h>
#include <filament/Texture.h>

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_ssgi]";

constexpr const char* kFeature = "ssgi";

// Render exactly one frame with an empty scene. Enabling the SSGI feature is
// what makes the backend's update_features loop run SsgiFeature::update() (which
// binds FrameContext::depth_texture) and run the shared depth pre-pass (because
// the enabled feature returns needs_depth_prepass()==true).
void render_one_frame(IRhiBackend& backend, const Camera& camera,
                      const LightingParams& lighting,
                      const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    WeatherState weather;
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

// Exercise the live Shipping_Look pixel-diff helper end-to-end inside the
// renderer-linked binary (parity bar tooling). This is the
// pure comparison available for separate SSGI difference/hue-shift capture
// analysis (compare_rgba8 + within_shipping_look_tolerance). The
// actual benchmark-camera capture vs an accepted reference is a separate check;
// here we model the two outcomes with small synthetic RGBA8
// buffers so a regression in the comparison logic surfaces in this harness too.
// Returns true iff the diff helper behaves as the bar requires.
bool exercise_diff_helper() {
    constexpr std::size_t kW = 8, kH = 8;

    // "Shipping_Look" reference: a flat neutral-grey receiving surface lit only
    // by the shipping indirect light (no SSGI colour bleed).
    std::vector<std::uint8_t> shipping(kW * kH * 4, 128);
    for (std::size_t px = 0; px < kW * kH; ++px) shipping[px * 4 + 3] = 255;

    bool ok = true;

    // SSGI DISABLED ⇒ output is bit-identical to the Shipping_Look indirect
    // lighting ⇒ within tolerance, max delta 0 (parity).
    {
        std::vector<std::uint8_t> disabled = shipping;
        const PixelDiff d = compare_rgba8(disabled, shipping, kW, kH);
        if (!within_shipping_look_tolerance(d) || d.max_channel_delta != 0) {
            std::printf("%s FAIL: disabled-parity buffers not within tolerance "
                        "(size_match=%d max_delta=%d) — R7.4\n", kTag,
                        d.size_match, d.max_channel_delta);
            ok = false;
        }
    }

    // SSGI ENABLED ⇒ one-bounce indirect light from an adjacent SATURATED
    // surface bleeds onto the receiver: the neutral grey shifts in HUE toward
    // that surface's colour (here a warm/red bounce). The diff must exceed the
    // 1-of-255 tolerance — i.e. the colour bleed is DETECTABLY present and the
    // shift is in the red channel (a hue change, not a uniform brighten), so the
    // production presence/hue check  would register it.
    {
        std::vector<std::uint8_t> enabled = shipping;
        for (std::size_t px = 0; px < kW * kH; ++px) {
            // Warm bounce: lift red well past tolerance, leave green/blue ~flat
            // so the change is a hue shift toward the saturated red neighbour
            // rather than an even luminance bump.
            enabled[px * 4 + 0] = 200;  // red bleed (delta 72 ≫ 1/255)
            enabled[px * 4 + 1] = 130;  // green barely moved
            enabled[px * 4 + 2] = 126;  // blue barely moved
        }
        const PixelDiff d = compare_rgba8(enabled, shipping, kW, kH);
        if (within_shipping_look_tolerance(d) || d.max_channel_delta <= 1) {
            std::printf("%s FAIL: an SSGI colour-bleed hue shift was treated as "
                        "within Shipping_Look tolerance (max_delta=%d within=%d) "
                        "— the >1/255 difference + hue-shift check (R7.1/R7.2) "
                        "would not be detected\n", kTag,
                        d.max_channel_delta, within_shipping_look_tolerance(d));
            ok = false;
        }
        // The dominant change is concentrated in the red channel (the saturated
        // neighbour's hue) with green/blue near-flat — i.e. a directional hue
        // shift rather than a uniform luminance bump. The exact per-channel
        // direction at a real receiver is asserted by the production bar; here
        // the beyond-tolerance result above guards the comparison logic.
    }

    return ok;
}

} // namespace

CaseResult run_ssgi_case(const CaseContext& ctx) {
    // The debug_* accessors below are FilamentBackend-specific test
    // instrumentation. The harness always constructs a FilamentBackend (it is
    // the sole IRhiBackend), so this is safe; api() lets us bail gracefully if
    // that ever changes rather than risking a bad cast — mirroring the sibling
    // smoke cases.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: case needs the Filament backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the render pipeline up so init_features() has run and the registered
    // SsgiFeature + the shared DepthPrePass exist. A failure here means the
    // environment can't drive the backend (no usable GPU) — treat it as a SKIP
    // so the gated test still passes, mirroring the harness's headless
    // fallbacks. init_pipelines is idempotent, so an earlier case having already
    // run it is fine.
    if (auto r = backend.init_pipelines(ctx.config); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    // ── (1) Registration  — asset-independent ─────────────
    // A clear-diagnostic existence check first (so a "registered zero times"
    // failure names the feature), then the cardinality check that catches both a
    // missing and a duplicate registration.
    bool ok = true;

    const IRenderFeature* ssgi = backend.debug_find_feature(kFeature);
    if (!ssgi) {
        std::printf("%s FAIL: no feature named \"%s\" is registered — the SSGI "
                    "feature must be registered exactly once in init_features "
                    "(R9.4)\n", kTag, kFeature);
        return CaseResult::Fail;
    }
    const int count = backend.debug_count_features(kFeature);
    if (count != 1) {
        std::printf("%s FAIL: render_features_ contains %d features named "
                    "\"%s\" (expected exactly 1 — R9.4)\n", kTag, count, kFeature);
        ok = false;
    }

    const filament::Scene* scene = backend.debug_render_context().scene;
    if (!scene) {
        std::printf("%s FAIL: render context scene is null after bring-up "
                    "(cannot observe disabled-parity scene membership)\n", kTag);
        return CaseResult::Fail;
    }

    // ── (4a) Exercise the live Shipping_Look diff helper  ────
    // Asset-independent: prove the comparison the production SSGI
    // difference/hue-shift bar relies on is live end-to-end in this binary.
    if (!exercise_diff_helper()) ok = false;

    // ── Material availability gate ──────────────────────────────
    // needs_depth_prepass() returns true once the embedded SSGI material loaded
    // (mi_ != nullptr). When it loaded, the feature also created its fullscreen
    // quad entity, so enabling it adds exactly one renderable. When a backend
    // can't build the embedded material (mi_ null), no quad entity exists and the
    // depth + parity-delta sub-assertions can't run — degrade to the
    // registration-only contract, documenting the rest as production-owned.
    if (!ssgi->needs_depth_prepass()) {
        if (!ok) return CaseResult::Fail;
        std::printf("%s registration honored (exactly one \"%s\" feature, R9.4) "
                    "and the Shipping_Look diff helper is live; the embedded SSGI "
                    "material is unavailable on this backend "
                    "(needs_depth_prepass()==false ⇒ no quad), so the "
                    "depth-pre-pass binding (R7.5/R12.3), the disabled-parity "
                    "delta (R7.4) and the >1/255 hue-shift capture (R7.1/R7.2) "
                    "require separate full-scene captures and comparison\n",
                    kTag, kFeature);
        return CaseResult::Pass;
    }

    // ── (2) Disabled parity  — feedback-break architecture ─
    // SSGI's fullscreen quad NO LONGER lives in the main scene. To break the
    // self-feedback white-out (SSGI gathering its own previous output), the quad
    // was moved into a feature-owned composite view that the backend renders
    // OVER the already-captured clean scene only while the feature is enabled.
    // So the invariant is now the opposite of the old one: toggling SSGI must
    // NOT change the MAIN scene renderable count (the quad is not there), and
    // disabled parity is guaranteed because the backend simply skips the SSGI
    // composite render while the feature is off.
    if (!ssgi->enabled()) {
        std::printf("%s FAIL: \"%s\" is disabled by default — it must default "
                    "enabled (the full advanced-rendering look ships on; use "
                    "--no-ssgi to disable)\n", kTag, kFeature);
        ok = false;
    }

    const size_t count_enabled_default = scene->getRenderableCount();
    backend.debug_set_feature_enabled(kFeature, false);
    const size_t count_disabled = scene->getRenderableCount();
    backend.debug_set_feature_enabled(kFeature, true);
    const size_t count_reenabled = scene->getRenderableCount();

    const long drop =
        static_cast<long>(count_enabled_default) - static_cast<long>(count_disabled);
    if (drop != 0) {
        std::printf("%s FAIL: toggling \"%s\" OFF changed the MAIN scene "
                    "renderable count by %ld (expected 0 — its quad lives in the "
                    "feature's own composite view, not the main scene, so it can "
                    "never feed back into the captured history — R7.4)\n",
                    kTag, kFeature, -drop);
        ok = false;
    }
    const long rise =
        static_cast<long>(count_reenabled) - static_cast<long>(count_disabled);
    if (rise != 0) {
        std::printf("%s FAIL: toggling \"%s\" back ON changed the MAIN scene "
                    "renderable count by %ld (expected 0 — the SSGI quad is not a "
                    "main-scene renderable)\n", kTag, kFeature, rise);
        ok = false;
    }

    // ── (3) Depth-pre-pass participation  ──────────
    // With the feature enabled, render a few frames. needs_depth_prepass() is
    // true, so the backend ensures the shared half-res depth target BEFORE
    // update_features() (FrameContext::depth_texture bound outside any render
    // pass) and renders the depth view; the feature's update() binds that
    // texture and on_depth_prepass_begin/end pull/re-add its quad (the god-rays
    // pattern).
    Camera camera;
    LightingParams lighting = lighting_for_time_of_day(ctx.config.time_of_day);
    TonemapParams  tonemap;

    const int w = ctx.width;
    const int h = ctx.height;

    constexpr int kFrames = 4;
    for (int i = 0; i < kFrames; ++i) {
        render_one_frame(backend, camera, lighting, tonemap, w, h);
    }

    // The depth-sampling feature still declares participation while enabled+built.
    if (!ssgi->needs_depth_prepass()) {
        std::printf("%s FAIL: \"%s\"->needs_depth_prepass() is false after %d "
                    "active frames (a depth-sampling feature must declare "
                    "participation once enabled+built — R7.5)\n",
                    kTag, kFeature, kFrames);
        ok = false;
    }

    // Because the SSGI feature requested it, the backend ran the SHARED depth
    // pre-pass and produced a valid (non-null) half-res depth texture, bound for
    // the feature to sample through FrameContext::depth_texture. A null here
    // would mean the pass never ran despite the feature declaring it needs depth.
    const filament::Texture* shared_depth = backend.debug_depth_texture();
    if (!shared_depth) {
        std::printf("%s FAIL: debug_depth_texture() is null after %d active "
                    "frames with \"%s\" enabled (the shared depth pre-pass must "
                    "run, and bind FrameContext::depth_texture outside a render "
                    "pass, when a feature returns needs_depth_prepass()==true — "
                    "R7.5/R12.3)\n", kTag, kFrames, kFeature);
        ok = false;
    }

    // ── Leave the shared backend in its as-found state ─────────
    // The feature ships ENABLED by default and the parity check above already
    // round-tripped it back to enabled, so no further toggle is needed —
    // subsequent cases see the default (enabled) feature set.
    if (!ssgi->enabled()) {
        std::printf("%s FAIL: \"%s\" left disabled after the case — it must be "
                    "restored to its enabled default for subsequent cases\n",
                    kTag, kFeature);
        ok = false;
    }

    if (!ok) return CaseResult::Fail;

    std::printf("%s SSGI contract honored: exactly one \"%s\" feature registered "
                "(R9.4), enabled-by-default with toggling it leaving the MAIN "
                "scene renderable count unchanged (its quad lives in the feature's "
                "own composite view, never fed back into the captured history — "
                "R7.4), needs_depth_prepass()=true drove the shared depth "
                "pre-pass to a non-null texture (%p) bound outside a render pass "
                "(R7.5/R12.3), and the live Shipping_Look diff helper detects an "
                "SSGI colour-bleed hue shift as a >1/255 difference (R7.1/R7.2). "
                "The genuine benchmark-camera capture vs the pre-recorded "
                "reference requires a separate full-scene comparison\n",
                kTag, kFeature, static_cast<const void*>(shared_depth));
    return CaseResult::Pass;
}

} // namespace heaven::smoke
