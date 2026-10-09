/*
 * HeavenBench — MSAA unsupported-count fallback / disabled-parity smoke
 *                     case (renderer-linked, GATED)
 *
 * MSAA is a
 * View_Option_Group applied through the existing post-processing configuration
 * path (configure_post_processing in rhi/post_process.cpp), NOT an
 * IRenderFeature in render_features_. Its init-time GPU-capability resolution
 * is the pure helper configure_msaa(requested, supported) (which wraps
 * heaven::msaa_fallback). This case asserts that:
 *
 *    the unsupported-count fallback applies the HIGHEST supported sample
 *          count <= the request (down to 1 when nothing at or below the request
 *          is supported), emits a single requested-vs-applied diagnostic, and
 *          continues — exercised here by driving configure_msaa with a requested
 *          count NOT in a forced-unavailable supported set and reading back the
 *          applied count. When the request IS supported the request is returned
 *          unchanged (no fallback, no diagnostic).
 *
 *    with the MSAA Feature_Toggle disabled the configuration path
 *          configures 1 sample (MSAA off) so anti-aliasing is identical to the
 *          Shipping_Look (TAA only) — exercised by calling
 *          configure_post_processing with msaa_enabled=false against a NOOP
 *          engine/view and reading the View's MultiSampleAntiAliasingOptions
 *          back (sampleCount resolves to 1, enabled false), while the TAA block
 *          is left unchanged (still the shipping TAA configuration).
 *
 * WHY THIS LIVES IN THE SMOKE HARNESS (not heaven_benchmark_tests):
 * configure_post_processing operates on a real filament::View (it sets the
 * View's MSAA + TAA + bloom/SSAO/SSR/fog options and builds a ColorGrading on a
 * real filament::Engine). The headless RapidCheck target
 * (heaven_benchmark_tests) deliberately links NEITHER Filament NOR SDL, so it
 * cannot construct an Engine/View. This target links the full renderer, so it
 * can — matching the "GPU smoke tests (gated)" testing strategy. The
 * pure fallback math (msaa_fallback) is separately unit-tested by
 * here we assert the configuration-path consequence.
 *
 * HOW WE "FORCE THE SAMPLE COUNT UNAVAILABLE" deterministically without a GPU
 * dependency: configure_msaa takes the supported sample-count set as a span, so
 * the seam is the argument itself — exactly the modelling
 * smoke_wet_reflections_capability.cpp uses for its forced-unavailable
 * capability probe. We feed a supported set that EXCLUDES the requested count to
 * drive the real fallback branch on any hardware.
 *
 * HERMETIC ENGINE: the case stands up its OWN no-op (NOOP) Filament engine +
 * scene + view, runs configure_post_processing against them, and reads the
 * View's MSAA/TAA state back. NOOP needs no GPU/display, and using a private
 * engine means the assertion does not depend on (or perturb) the shared backend
 * the harness brought up. This mirrors smoke_wet_reflections_capability.cpp and
 * smoke_soft_shadows_toggle.cpp (the sibling self-contained config-path cases).
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 * It SKIPs (exit 0 via the harness) if it cannot create its NOOP engine.
 */

#include "smoke_cases.h"

#include "rhi/post_process.h"   // configure_post_processing / configure_msaa
#include "rhi/rhi.h"            // TonemapParams
#include "engine/types.h"       // QualityKnobs / msaa_fallback

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/ColorGrading.h>
#include <filament/Options.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <span>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_msaa_fallback]";

constexpr float kEps = 1e-4f;

[[nodiscard]] bool approx(float a, float b) { return std::fabs(a - b) <= kEps; }

} // namespace

CaseResult run_msaa_fallback_case(const CaseContext& /*ctx*/) {
    bool ok = true;

    // ──  Unsupported-count fallback via the configure_msaa seam ───────
    // The supported set is the seam (modelled on the wet-reflections forced
    // probe): feed a request that is NOT in the set and assert the highest
    // supported count <= request is applied. configure_msaa emits the single
    // requested-vs-applied diagnostic on this path; the harness does not capture
    // stdout, so we assert the observable applied count (the diagnostic itself
    // is covered by code inspection).
    {
        // GPU supports {1,2,4} but the request is 8 (e.g. an over-ambitious
        // config): fall back to 4, the highest supported <= 8.
        constexpr std::array<int, 3> supported_124{1, 2, 4};
        const int applied8 =
            configure_msaa(8, std::span<const int>(supported_124));
        if (applied8 != 4) {
            std::printf("%s FAIL: configure_msaa(8, {1,2,4}) applied %d, "
                        "expected 4 (highest supported <= request) — R4.5\n",
                        kTag, applied8);
            ok = false;
        }

        // GPU supports only {1,2}: a request of 4 falls back to 2.
        constexpr std::array<int, 2> supported_12{1, 2};
        const int applied4 =
            configure_msaa(4, std::span<const int>(supported_12));
        if (applied4 != 2) {
            std::printf("%s FAIL: configure_msaa(4, {1,2}) applied %d, expected "
                        "2 (highest supported <= request) — R4.5\n",
                        kTag, applied4);
            ok = false;
        }

        // Nothing at or below the request is supported: fall back to 1 sample
        // (MSAA effectively off) rather than aborting.
        constexpr std::array<int, 2> supported_24{2, 4};
        const int applied1 =
            configure_msaa(1, std::span<const int>(supported_24));
        if (applied1 != 1) {
            std::printf("%s FAIL: configure_msaa(1, {2,4}) applied %d, expected "
                        "1 (nothing supported <= request ⇒ MSAA off) — R4.5\n",
                        kTag, applied1);
            ok = false;
        }

        // When the request IS supported it is honoured unchanged — no fallback,
        // no diagnostic.
        const int applied_exact =
            configure_msaa(4, std::span<const int>(supported_124));
        if (applied_exact != 4) {
            std::printf("%s FAIL: configure_msaa(4, {1,2,4}) applied %d, expected "
                        "4 (request supported ⇒ no fallback) — R4.5\n",
                        kTag, applied_exact);
            ok = false;
        }
    }

    // ──  Disabled parity: 1-sample (TAA-only) through the config path ──
    // Hermetic NOOP engine: no GPU/display needed, and isolated from the shared
    // backend the harness stood up. If even NOOP can't be created, SKIP so the
    // gated test still passes (exit 0 via the harness). NOOP reports
    // FEATURE_LEVEL_1, so the View stores the MSAA options we read back.
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine\n", kTag);
        return CaseResult::Skip;
    }

    filament::Scene* scene = engine->createScene();
    filament::View*  view  = engine->createView();
    view->setScene(scene);

    const TonemapParams tonemap{};       // shipping defaults
    QualityKnobs        quality{};       // default msaa_samples == 1

    filament::ColorGrading* color_grading = nullptr;

    // MSAA DISABLED ⇒ the config path configures 1 sample (MSAA off), leaving
    // anti-aliasing identical to the Shipping_Look (TAA only).
    color_grading = configure_post_processing(
        *engine, *view, tonemap, color_grading,
        /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false);

    {
        const filament::MultiSampleAntiAliasingOptions msaa =
            view->getMultiSampleAntiAliasingOptions();
        if (msaa.sampleCount != 1 || msaa.enabled) {
            std::printf("%s FAIL: with MSAA disabled the View resolved to "
                        "sampleCount=%u enabled=%d, expected 1 sample / disabled "
                        "(TAA-only Shipping_Look parity) — R4.4\n",
                        kTag, static_cast<unsigned>(msaa.sampleCount),
                        static_cast<int>(msaa.enabled));
            ok = false;
        }
    }

    // The TAA block is left UNCHANGED by the MSAA path: it is still the
    // shipping TAA configuration (enabled, feedback 0.12, sharpness 0.20, hdr).
    {
        const filament::TemporalAntiAliasingOptions taa =
            view->getTemporalAntiAliasingOptions();
        if (!taa.enabled || !taa.hdr ||
            !approx(taa.feedback, 0.12f) || !approx(taa.sharpness, 0.20f)) {
            std::printf("%s FAIL: TAA block changed by the MSAA-disabled config "
                        "path (enabled=%d hdr=%d feedback=%g sharpness=%g; "
                        "expected 1/1/0.12/0.20) — R4.1/R4.4\n",
                        kTag, static_cast<int>(taa.enabled),
                        static_cast<int>(taa.hdr),
                        taa.feedback, taa.sharpness);
            ok = false;
        }
    }

    // Sanity: MSAA ENABLED at the Quality_Knob count drives the View to that
    // sample count through the SAME path — proving the disabled 1-sample result
    // above is the toggle's doing, not a path that never writes MSAA at all.
    quality.msaa_samples = 4;
    color_grading = configure_post_processing(
        *engine, *view, tonemap, color_grading,
        /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/true);
    {
        const filament::MultiSampleAntiAliasingOptions msaa =
            view->getMultiSampleAntiAliasingOptions();
        if (msaa.sampleCount != 4 || !msaa.enabled) {
            std::printf("%s FAIL: with MSAA enabled at 4 samples the View "
                        "resolved to sampleCount=%u enabled=%d, expected 4 / "
                        "enabled — the Quality_Knob did not drive the config "
                        "path\n",
                        kTag, static_cast<unsigned>(msaa.sampleCount),
                        static_cast<int>(msaa.enabled));
            ok = false;
        }
    }

    // Tear down our private engine resources (user objects before the engine).
    if (color_grading) engine->destroy(color_grading);
    engine->destroy(view);
    engine->destroy(scene);
    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s MSAA fallback picks the highest supported count <= request "
                "(8⇒4, 4⇒2, none⇒1) and the disabled toggle resolves to 1 "
                "sample (TAA-only Shipping_Look parity) with the TAA block "
                "unchanged (R4.4/R4.5)\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
