/*
 * HeavenBench — renderer-linked pixel-difference helper smoke case.
 *
 * This case renders an empty scene and checks synthetic RGBA8 comparisons.
 * It reports Skip for full-scene parity because no accepted reference image
 * set is available. Neither these checks nor production --benchmark perform
 * a full-scene reference comparison. Real diagnostic views can be collected
 * with --capture and --camera-tour-time / --camera-pose, or with
 * scripts/visual_audit.py, then reviewed separately.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "rhi/filament_backend.h"
#include "engine/types.h"
#include "engine/camera.h"
#include "benchmark/pixel_parity.h"

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_shipping_look_parity]";

// Render exactly one empty-scene frame on the live backend with the new weather
// features contributing NOTHING — a stack WeatherState left at intensity 0, the
// Shipping_Look-equivalent state. The backend tolerates empty
// instance/mesh spans, so this drives the full per-frame path (and presents)
// without loading the heavy Heaven scene, mirroring the sibling smoke cases.
void render_one_shipping_look_frame(IRhiBackend& backend, const Camera& camera,
                                    const LightingParams& lighting,
                                    const TonemapParams& tonemap, int w, int h) {
    MaterialResolver resolver =
        [](const std::string&, const std::string&) -> GpuMaterial {
            return GpuMaterial{};
        };
    // Default WeatherState: target_intensity 0, not drying ⇒ effective
    // intensity 0. Rain renders zero streaks/splashes, wet-reflections keeps the
    // shipping SSR + {0,0,0,1} wetness global — the disabled-features parity
    // state the bar is defined over.
    WeatherState weather;
    backend.render_frame(std::span<const Instance>{},
                         std::span<const int>{},
                         std::span<const GpuMesh>{},
                         resolver, camera, lighting, weather, tonemap, w, h);
}

} // namespace

CaseResult run_shipping_look_parity_case(const CaseContext& ctx) {
    // The backend bring-up below is FilamentBackend-specific. The harness always
    // constructs a FilamentBackend (it is the sole IRhiBackend), so this is
    // safe; api() lets us bail gracefully if that ever changes rather than
    // risking a bad cast — mirroring the sibling rain cases.
    if (ctx.backend.api() != GraphicsApi::Filament) {
        std::printf("%s SKIP: case needs the Filament backend\n", kTag);
        return CaseResult::Skip;
    }
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);

    // Bring the render pipeline up so frames actually present on the live
    // backend. A failure here means the environment can't drive the backend (no
    // usable GPU) — SKIP so the gated test still passes headlessly. init_pipelines
    // is idempotent, so an earlier case having already run it is fine.
    if (auto r = backend.init_pipelines(ctx.config); !r) {
        std::printf("%s SKIP: init_pipelines failed (%s)\n",
                    kTag, r.error().message.c_str());
        return CaseResult::Skip;
    }

    // The disabled-features parity configuration the bar is defined over:
    // --no-rain --no-wet-reflections and rain intensity 0. The shared backend
    // was already brought up, so we cannot re-init it disabled here; instead we
    // render at intensity 0 (below), the state in which BOTH new features
    // contribute nothing  — equivalent to the disabled
    // Shipping_Look for parity purposes. We assert the config we WOULD pass
    // matches the bar's definition so the intent is pinned.
    AppConfig parity = ctx.config;
    parity.features.rain            = false; // --no-rain
    parity.features.wet_reflections = false; // --no-wet-reflections
    parity.weather.rain_intensity   = 0.0f;  // intensity 0
    if (parity.features.rain || parity.features.wet_reflections ||
        parity.weather.rain_intensity != 0.0f) {
        std::printf("%s FAIL: parity config not in the disabled-features state "
                    "(rain=%d wet=%d intensity=%.3f; expected 0/0/0 — R9.4)\n",
                    kTag, parity.features.rain, parity.features.wet_reflections,
                    static_cast<double>(parity.weather.rain_intensity));
        return CaseResult::Fail;
    }

    Camera camera;
    LightingParams lighting = lighting_for_time_of_day(ctx.config.time_of_day);
    TonemapParams  tonemap;

    const int w = ctx.width;
    const int h = ctx.height;

    // Render a handful of Shipping_Look-equivalent frames (intensity 0) so the
    // disabled-features render path runs end-to-end on the live GPU.
    constexpr int kFrames = 4;
    for (int i = 0; i < kFrames; ++i) {
        render_one_shipping_look_frame(backend, camera, lighting, tonemap, w, h);
    }

    // ── Prove the pixel-diff path is live end-to-end here ───────
    // The pure comparison the parity bar is defined over (heaven::compare_rgba8
    // + within_shipping_look_tolerance, tolerance kShippingLookMaxChannelDelta)
    // is compiled into THIS renderer-linked binary as well as the headless test
    // target. Exercise it on small synthetic RGBA8 buffers so a regression in
    // the comparison logic surfaces here too, not only in heaven_benchmark_tests.
    bool ok = true;

    constexpr std::size_t kW = 8, kH = 8;
    std::vector<std::uint8_t> reference(kW * kH * 4);
    for (std::size_t px = 0; px < kW * kH; ++px) {
        const std::size_t base = px * 4;
        reference[base + 0] = static_cast<std::uint8_t>((px * 7)  & 0xFF);
        reference[base + 1] = static_cast<std::uint8_t>((px * 13) & 0xFF);
        reference[base + 2] = static_cast<std::uint8_t>((px * 29) & 0xFF);
        reference[base + 3] = 255;
    }

    // Identical ⇒ max delta 0 ⇒ within tolerance.
    {
        std::vector<std::uint8_t> captured = reference;
        const PixelDiff d = compare_rgba8(captured, reference, kW, kH);
        if (!within_shipping_look_tolerance(d) || d.max_channel_delta != 0) {
            std::printf("%s FAIL: identical buffers not within tolerance "
                        "(size_match=%d max_delta=%d)\n", kTag, d.size_match,
                        d.max_channel_delta);
            ok = false;
        }
    }
    // A 2-level drift ⇒ exceeds tolerance.
    {
        std::vector<std::uint8_t> captured = reference;
        captured[2] = static_cast<std::uint8_t>(
            reference[2] <= 253 ? reference[2] + 2 : reference[2] - 2);
        const PixelDiff d = compare_rgba8(captured, reference, kW, kH);
        if (within_shipping_look_tolerance(d) || d.max_channel_delta != 2) {
            std::printf("%s FAIL: a 2-level drift was treated as within "
                        "tolerance (max_delta=%d within=%d — R9.4 allows at "
                        "most %d)\n", kTag, d.max_channel_delta,
                        within_shipping_look_tolerance(d),
                        kShippingLookMaxChannelDelta);
            ok = false;
        }
    }
    // Size mismatch ⇒ failure.
    {
        const std::vector<std::uint8_t> smaller((kW) * (kH - 1) * 4, 0);
        const PixelDiff d = compare_rgba8(smaller, reference, kW, kH);
        if (within_shipping_look_tolerance(d) || d.size_match) {
            std::printf("%s FAIL: a size mismatch was treated as comparable "
                        "(size_match=%d within=%d)\n", kTag, d.size_match,
                        within_shipping_look_tolerance(d));
            ok = false;
        }
    }

    if (!ok) return CaseResult::Fail;

    // ── Honest SKIP: the genuine capture vs reference cannot run here ──
    // The disabled-features render path ran on the live GPU and the pixel-diff
    // helper is verified live, but the pre-recorded Shipping_Look REFERENCE SET,
    // the full Heaven SCENE, and the authored CAMERA PATH are all unavailable in
    // this 64x64 bare-backend harness, so the real sampled-camera-path
    // comparison cannot be performed. Faking a pass is forbidden; the genuine
    // assertion requires separate captures and accepted reference images. SKIP (exit 0).
    std::printf("%s SKIP: rendered %d Shipping_Look-equivalent frames "
                "(intensity 0, new features contribute nothing) and verified the "
                "compare_rgba8 / 1-of-255 tolerance path live in the renderer "
                "binary, but the pre-recorded Shipping_Look reference set, the "
                "full Heaven scene, and the benchmark camera path are unavailable "
                "in this harness — the genuine sampled-camera-path parity "
                "comparison requires separately accepted reference images\n",
                kTag, kFrames);
    return CaseResult::Skip;
}

} // namespace heaven::smoke
