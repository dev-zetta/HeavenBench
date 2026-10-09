/*
 * HeavenBench — live performance-window measurement smoke case.
 *
 * Render a short sequence on the real backend and exercise the same window
 * helper used by Application::report_perf_window(). A short sample remains
 * incomplete; a synthetic full-length sample verifies timing arithmetic.
 * The smoke harness has a small window and no full Heaven camera-path scene,
 * so its measured FPS is not a performance baseline. Real comparisons require
 * matching hardware, drivers, output resolution, and effective settings.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"
#include "engine/types.h"
#include "engine/camera.h"
#include "benchmark/stats.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <span>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_fps_bar]";

// Render exactly one empty-scene frame on the live backend, mirroring the
// empty-frame helpers in the sibling smoke cases. The backend tolerates empty
// instance/mesh spans, so this drives the full per-frame path (and presents)
// without loading the heavy Heaven scene.
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

} // namespace

CaseResult run_fps_bar_case(const CaseContext& ctx) {
    IRhiBackend& backend = ctx.backend;

    // Bring the render pipeline up so frames actually present on the live
    // backend. A failure here means the environment can't drive the backend
    // (no usable GPU) — SKIP so the gated test still passes headlessly.
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

    // Record REAL per-frame wall-clock deltas off the live backend — the same
    // series report_perf_window() builds over the production run. A handful of
    // frames is plenty: the point is to exercise the measurement path on real
    // GPU timings, not to reach the 60 s measurement window (which needs the full
    // camera-path run the production --benchmark path drives).
    using Clock = std::chrono::steady_clock;
    constexpr int kFrames = 30;
    std::vector<float> frame_times;
    frame_times.reserve(kFrames);

    auto prev = Clock::now();
    for (int i = 0; i < kFrames; ++i) {
        render_one_frame(backend, camera, lighting, tonemap, w, h);
        auto now = Clock::now();
        frame_times.push_back(std::chrono::duration<float>(now - prev).count());
        prev = now;
    }

    bool ok = true;

    // Keep the sampling durations stable. They determine measurement coverage,
    // not whether a particular GPU or configuration is fast enough.
    if (kPerfWarmupSeconds != 2.0f || kPerfMinWindowSeconds != 60.0f) {
        std::printf("%s FAIL: measurement durations changed\n", kTag);
        ok = false;
    }

    // A short smoke run cannot provide the full comparison window.
    const PerfWindow live = measure_perf_window(frame_times);
    if (live.valid) {
        std::printf("%s FAIL: a %d-frame smoke run produced a VALID measurement window "
                    "(%.3f s); measurement requires >= %.0f s after the %.0f s "
                    "warm-up — a short run must be incomplete\n",
                    kTag, kFrames, static_cast<double>(live.window_seconds),
                    static_cast<double>(kPerfMinWindowSeconds),
                    static_cast<double>(kPerfWarmupSeconds));
        ok = false;
    }

    // ── The measurement path itself is correct on a synthetic full-length
    //    series at a known rate (proves the same helper would classify a real
    //    >= 60 s run; the production --benchmark path supplies the real one). ─
    {
        std::vector<float> synth;
        const float dt = 1.0f / 4000.0f;          // 4000 FPS
        const int n = static_cast<int>(62.0f * 4000.0f); // 62 s ⇒ valid window
        synth.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) synth.push_back(dt);
        const PerfWindow full = measure_perf_window(synth);
        if (!full.valid || std::fabs(full.mean_fps - 4000.0f) > 1.0f ||
            full.frames_in_window != 240000 ||
            std::fabs(full.window_seconds - 60.0f) > 0.001f) {
            std::printf("%s FAIL: synthetic 62 s @ 4000 FPS series should be a "
                        "complete 4000-FPS window with 240000 frames (valid=%d "
                        "mean=%.1f)\n", kTag, full.valid,
                        static_cast<double>(full.mean_fps));
            ok = false;
        }
    }

    if (!ok) return CaseResult::Fail;

    std::printf("%s performance-window accounting verified: %d real frames "
                "measured (window %.4f s, incomplete as expected for a short "
                "run), plus a synthetic full-length sample.\n",
                kTag, kFrames, static_cast<double>(live.window_seconds));
    return CaseResult::Pass;
}

} // namespace heaven::smoke
