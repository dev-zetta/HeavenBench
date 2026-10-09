/*
 * HeavenBench — dynamic-resolution configuration and controller smoke.
 *
 * A private NOOP engine verifies real View option storage and the configured
 * FSR range. Filament's actual PID implementation receives synthetic GPU
 * durations to verify that our finite target permits downscaling under load.
 * NOOP does not support timer queries, so this case cannot prove live GPU
 * scale movement or image quality. Actual captures expose
 * render_resolution_scale; scored benchmarks deliberately disable adaptive
 * resolution to preserve the measured workload.
 */

#include "smoke_cases.h"

#include "rhi/post_process.h"   // configure_post_processing
#include "rhi/frame_timing.h"
#include "rhi/rhi.h"            // TonemapParams
#include "engine/types.h"       // QualityKnobs / clamp_dynres_min_scale / ToneMapperSelection

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/View.h>
#include <filament/ColorGrading.h>
#include <filament/Options.h>

// Test the same PID implementation used by FView::updateScale, with explicit
// GPU-duration samples. NOOP has no real timer-query or presentation evidence.
#include "../../lib/filament/filament/src/PIDController.h"

#include <cmath>
#include <cstdio>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_dynamic_resolution]";

constexpr float kEps = 1e-4f;

[[nodiscard]] bool approx(float a, float b) { return std::fabs(a - b) <= kEps; }

} // namespace

CaseResult run_dynamic_resolution_case(const CaseContext& /*ctx*/) {
    // Hermetic NOOP engine: no GPU/display needed, isolated from the shared
    // backend. If even NOOP can't be created, SKIP (exit 0 via the harness).
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine\n", kTag);
        return CaseResult::Skip;
    }

    filament::Scene* scene = engine->createScene();
    filament::View*  view  = engine->createView();
    view->setScene(scene);

    const TonemapParams tonemap{};   // Shipping_Look defaults
    QualityKnobs        quality{};   // defaults: MSAA off

    // The single backend-owned ColorGrading, threaded through every call so the
    // previous instance is destroyed by configure_post_processing each time.
    filament::ColorGrading* color_grading = nullptr;

    bool ok = true;

    // Reproduce the zero-refresh failure with the actual Filament controller.
    // The application's shared timing input must make slow GPU samples ask
    // for a smaller render area, and fast samples ask for a larger one.
    {
        const auto fixed = frame_timing_options(false);
        const auto adaptive = frame_timing_options(true);
        const float target_ms = 1000.0f * static_cast<float>(adaptive.frame_rate.interval) /
            adaptive.display.refreshRate;
        filament::PIDController slow;
        filament::PIDController fast;
        const float proportional_gain = 1.0f - std::exp(-adaptive.frame_rate.scaleRate);
        slow.setParallelGains(proportional_gain, 0.002f, 0.0f);
        fast.setParallelGains(proportional_gain, 0.002f, 0.0f);
        const float slow_command = slow.update(40.0f / target_ms, 1.0f, 1.0f);
        const float fast_command = fast.update(4.0f / target_ms, 1.0f, 1.0f);
        if (fixed.display.refreshRate != 0.0f || !std::isfinite(target_ms) ||
            !approx(target_ms, 1000.0f / 60.0f) || slow_command >= 0.0f ||
            fast_command <= 0.0f) {
            std::printf("%s FAIL: adaptive GPU target/control invalid: target=%g ms, "
                        "slow=%g, fast=%g\n", kTag, target_ms, slow_command, fast_command);
            ok = false;
        }
    }

    // ──  Enabled: FSR scaling range configured on the View
    // Drive the config path with dynamic resolution enabled and a 0.5 minimum
    // scale, then read the View's DynamicResolutionOptions back. minScale,
    // maxScale, and quality are stored verbatim by Filament (the copy precedes
    // the backend-capability `enabled` clamp), so they survive on the NOOP
    // backend: minScale=={0.5,0.5}, maxScale=={1,1}, quality==HIGH (FSR1). The
    // (min_scale, 1.0] range is what lets Filament's frame-time controller move
    // the internal scale toward the minimum when frame time exceeds the target
    // and toward 1.0 when below it; quality HIGH + maxScale 1.0
    // is the FSR upscale to the full display resolution. The live
    // `enabled=true` bit is backend-capability-gated (see the header caveat) and
    // verified by real diagnostic captures, so it is NOT asserted here.
    filament::QualityLevel enabled_quality = filament::QualityLevel::LOW;
    {
        constexpr float kMinScale = 0.5f;
        color_grading = configure_post_processing(
            *engine, *view, tonemap, color_grading,
            /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false,
            /*ssr_enabled=*/true, /*hdr_output=*/false,
            /*tone_mapper=*/ToneMapperSelection::ACES,
            /*color_grading_enabled=*/false, /*grading=*/{},
            /*dynamic_resolution_enabled=*/true, /*dynres_min_scale=*/kMinScale);

        const filament::DynamicResolutionOptions dsr =
            view->getDynamicResolutionOptions();
        enabled_quality = dsr.quality;

        if (!approx(dsr.minScale.x, kMinScale) ||
            !approx(dsr.minScale.y, kMinScale)) {
            std::printf("%s FAIL: minScale resolved to {%g,%g}, expected "
                        "{%g,%g} (the configured Quality_Knob minimum scale) — "
                        "Req 12.1\n", kTag, dsr.minScale.x, dsr.minScale.y,
                        kMinScale, kMinScale);
            ok = false;
        }
        if (!approx(dsr.maxScale.x, 1.0f) || !approx(dsr.maxScale.y, 1.0f)) {
            std::printf("%s FAIL: maxScale resolved to {%g,%g}, expected {1,1} "
                        "(full-resolution upper bound) — Req 12.3\n", kTag,
                        dsr.maxScale.x, dsr.maxScale.y);
            ok = false;
        }
        if (dsr.quality != filament::QualityLevel::HIGH) {
            std::printf("%s FAIL: upscaling quality resolved to %d, expected "
                        "HIGH (AMD FidelityFX FSR1, full-display-resolution "
                        "upscale) — Req 12.3\n", kTag,
                        static_cast<int>(dsr.quality));
            ok = false;
        }

        // The controller's scaling RANGE: minScale strictly below maxScale and
        // maxScale pinned at 1.0. This is the exact configuration that lets the
        // frame-time controller scale DOWN toward minScale (frame time over
        // target) and UP toward 1.0 (under target) —.
        if (!(dsr.minScale.x < dsr.maxScale.x) || !approx(dsr.maxScale.x, 1.0f)) {
            std::printf("%s FAIL: the scaling range is not (minScale < maxScale "
                        "== 1.0): minScale.x=%g maxScale.x=%g — the frame-time "
                        "controller cannot scale between the minimum and 1.0 "
                        "(Req 12.1/12.2)\n", kTag, dsr.minScale.x, dsr.maxScale.x);
            ok = false;
        }
    }

    // ──  Disabled: full-resolution render, dynamic resolution off ──
    // With the toggle off the View's DynamicResolutionOptions resolve to
    // enabled=false (full-resolution render, genuine on every backend) and the
    // default LOW upscaler — the FSR HIGH path is NOT configured. This matches
    // the Shipping_Look. The ≤1/255 pixel parity bar is owned by
    // smoke_shipping_look_parity.cpp / --benchmark.
    {
        color_grading = configure_post_processing(
            *engine, *view, tonemap, color_grading,
            /*fog_density_override=*/-1.0f, quality, /*msaa_enabled=*/false,
            /*ssr_enabled=*/true, /*hdr_output=*/false,
            /*tone_mapper=*/ToneMapperSelection::ACES,
            /*color_grading_enabled=*/false, /*grading=*/{},
            /*dynamic_resolution_enabled=*/false, /*dynres_min_scale=*/1.0f);

        const filament::DynamicResolutionOptions dsr =
            view->getDynamicResolutionOptions();
        if (dsr.enabled) {
            std::printf("%s FAIL: dynamic resolution disabled left the View's "
                        "DynamicResolutionOptions.enabled true — full-resolution "
                        "Shipping_Look parity broken (Req 12.5)\n", kTag);
            ok = false;
        }
        if (dsr.quality == filament::QualityLevel::HIGH) {
            std::printf("%s FAIL: dynamic resolution disabled left the FSR HIGH "
                        "upscaler configured — the disabled branch must not "
                        "configure FSR (Req 12.5)\n", kTag);
            ok = false;
        }

        // Non-vacuous contrast: the enabled config selected the FSR HIGH
        // upscaler while the disabled config did not — proving the toggle
        // genuinely drives the dynamic-resolution config path (the same
        // toggle-drives-the-path check smoke_msaa_fallback.cpp makes), not a
        // path that never writes the options at all.
        if (!(enabled_quality == filament::QualityLevel::HIGH &&
              dsr.quality != filament::QualityLevel::HIGH)) {
            std::printf("%s FAIL: enabled vs disabled did not differ on the "
                        "upscaler (enabled quality=%d, disabled quality=%d) — "
                        "the toggle does not drive the config path (Req "
                        "12.1/12.5)\n", kTag, static_cast<int>(enabled_quality),
                        static_cast<int>(dsr.quality));
            ok = false;
        }
    }

    // ──  clamp_dynres_min_scale keeps the minimum in (0, 1] ───
    // The frame-time controller's scaling range depends on a minimum scale that
    // is strictly > 0 and <= 1. The pure clamp helper enforces that bound and
    // flags clamped iff the input was out of range (NaN collapsing to the safe
    // default, reported clamped). The clamp math itself is unit-tested by
    // here we assert the bound the controller relies on.
    {
        const QualityKnobs k{};

        // In-range value passes through unchanged and is NOT flagged clamped.
        const FloatKnobClamp in_range = clamp_dynres_min_scale(0.5f, k);
        if (in_range.clamped || !approx(in_range.value, 0.5f)) {
            std::printf("%s FAIL: clamp_dynres_min_scale(0.5) returned value=%g "
                        "clamped=%d, expected 0.5 / not clamped — Req 12.4\n",
                        kTag, in_range.value,
                        static_cast<int>(in_range.clamped));
            ok = false;
        }

        // Under-range value clamps UP to the lower bound and is flagged.
        const FloatKnobClamp under = clamp_dynres_min_scale(0.01f, k);
        if (!under.clamped ||
            !approx(under.value, QualityKnobs::k_dynres_min_scale_lo)) {
            std::printf("%s FAIL: clamp_dynres_min_scale(0.01) returned value=%g "
                        "clamped=%d, expected %g / clamped — Req 12.6\n", kTag,
                        under.value, static_cast<int>(under.clamped),
                        QualityKnobs::k_dynres_min_scale_lo);
            ok = false;
        }

        // Over-range value clamps DOWN to the upper bound (1.0) and is flagged.
        const FloatKnobClamp over = clamp_dynres_min_scale(2.0f, k);
        if (!over.clamped ||
            !approx(over.value, QualityKnobs::k_dynres_min_scale_hi)) {
            std::printf("%s FAIL: clamp_dynres_min_scale(2.0) returned value=%g "
                        "clamped=%d, expected %g / clamped — Req 12.6\n", kTag,
                        over.value, static_cast<int>(over.clamped),
                        QualityKnobs::k_dynres_min_scale_hi);
            ok = false;
        }

        // NaN collapses to the safe default and is reported clamped.
        const FloatKnobClamp nan_in =
            clamp_dynres_min_scale(std::nanf(""), k);
        if (!nan_in.clamped || !approx(nan_in.value, QualityKnobs{}.dynres_min_scale)) {
            std::printf("%s FAIL: clamp_dynres_min_scale(NaN) returned value=%g "
                        "clamped=%d, expected the %g default / clamped — Req "
                        "12.6/15.4\n", kTag, nan_in.value,
                        static_cast<int>(nan_in.clamped),
                        QualityKnobs{}.dynres_min_scale);
            ok = false;
        }

        // Every clamp result lands strictly in (0, 1] — the bound the
        // controller's (min_scale, 1.0] range depends on.
        const FloatKnobClamp results[] = { in_range, under, over, nan_in };
        for (const FloatKnobClamp& r : results) {
            if (!(r.value > 0.0f) || !(r.value <= 1.0f)) {
                std::printf("%s FAIL: clamp_dynres_min_scale produced %g, "
                            "outside the required (0, 1] range — Req 12.4\n",
                            kTag, r.value);
                ok = false;
            }
        }
    }

    // Tear down our private engine resources (user objects before the engine).
    if (color_grading) engine->destroy(color_grading);
    engine->destroy(view);
    engine->destroy(scene);
    filament::Engine::destroy(&engine);

    if (!ok) return CaseResult::Fail;

    std::printf("%s dynamic resolution enabled configures the View for FSR "
                "scaling (minScale={0.5,0.5}, maxScale={1,1}, quality=HIGH/FSR1) "
                "so Filament's frame-time controller can scale between the "
                "minimum and 1.0 and FSR-upscale to the full display resolution "
                "(Req 12.1/12.2/12.3; the live enabled bit is backend-gated and "
                "verified by real captures); disabled leaves it off (enabled=false, "
                "default LOW upscaler — full-resolution Shipping_Look render, Req "
                "12.5) and the toggle genuinely drives the path (HIGH vs LOW); "
                "and clamp_dynres_min_scale keeps the minimum scale within (0,1] "
                "(Req 12.4/12.5). Genuine per-frame scale ramp + FSR pixels are "
                "--benchmark-owned.\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
