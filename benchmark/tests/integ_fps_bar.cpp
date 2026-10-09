/*
 * HeavenBench — performance-window accounting, exercised without a GPU.
 *
 * The production camera-path summary and live smoke test use the same helper.
 * These fixtures verify warm-up exclusion, minimum duration, mean FPS, and
 * frame counts. A complete low-FPS sample is valid measurement data.
 */
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "benchmark/stats.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// is a free function with external linkage defined there; the Register helper's
// definition is identical so it can be reused from this translation unit.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

constexpr const char* kTag = "[integ_fps_bar]";

// Build a frame-time series of `seconds` total wall-clock time at a constant
// `fps` frame rate (each frame is 1/fps seconds long).
std::vector<float> constant_fps_series(float fps, float seconds) {
    std::vector<float> ft;
    const float dt = 1.0f / fps;
    const int n = static_cast<int>(seconds * fps);
    ft.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) ft.push_back(dt);
    return ft;
}

bool approx(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

bool integ_fps_bar() {
    bool ok = true;

    // Keep the established sampling durations independent of FPS.
    if (heaven::kPerfWarmupSeconds != 2.0f ||
        heaven::kPerfMinWindowSeconds != 60.0f) {
        std::printf("%s FAILED: measurement durations changed\n", kTag);
        ok = false;
    }

    // ── Window too short ⇒ incomplete measurement ───
    // A 30 s run is shorter than the 2 s warm-up + 60 s measurement window.
    {
        const auto ft = constant_fps_series(4000.0f, 30.0f);
        const heaven::PerfWindow w = heaven::measure_perf_window(ft);
        if (w.valid) {
            std::printf("%s FAILED: a 30 s run reported a VALID window; measurement "
                        "requires >= 60 s after the 2 s warm-up\n", kTag);
            ok = false;
        }
    }

    // ── Warm-up exclusion + mean FPS over the window ────────────
    // 70 s at a constant 4000 FPS: the first 2 s are excluded, leaving a 68 s
    // window whose mean FPS must still be 4000 (constant rate), and `valid`
    // must hold (68 s >= 60 s). The warm-up actually excluded is ~2 s.
    {
        const auto ft = constant_fps_series(4000.0f, 70.0f);
        const heaven::PerfWindow w = heaven::measure_perf_window(ft);
        if (!w.valid) {
            std::printf("%s FAILED: a 70 s run did not produce a VALID window\n", kTag);
            ok = false;
        }
        // ~2 s excluded at the front (within one frame of 2.0 s).
        if (!approx(w.warmup_seconds, heaven::kPerfWarmupSeconds, 0.01f)) {
            std::printf("%s FAILED: warm-up excluded %.4f s, expected ~2.0 s\n",
                        kTag, static_cast<double>(w.warmup_seconds));
            ok = false;
        }
        // Remaining window is ~68 s.
        if (!approx(w.window_seconds, 68.0f, 0.05f)) {
            std::printf("%s FAILED: window %.4f s, expected ~68 s after the 2 s "
                        "warm-up of a 70 s run\n", kTag,
                        static_cast<double>(w.window_seconds));
            ok = false;
        }
        // Constant 4000 FPS ⇒ mean over the window is still 4000.
        if (!approx(w.mean_fps, 4000.0f, 1.0f)) {
            std::printf("%s FAILED: mean FPS %.2f over the window, expected ~4000 "
                        "for a constant-4000-FPS series\n", kTag,
                        static_cast<double>(w.mean_fps));
            ok = false;
        }
    }

    // ── The exclusion changes the mean when warm-up frames differ ──
    // Make the first 2 s SLOW (10 FPS) and the rest FAST (4000 FPS) over a long
    // run. If the first 2 s were NOT excluded they would drag the mean down;
    // because they are excluded, the windowed mean is the fast rate.
    {
        std::vector<float> ft;
        // 2 s of slow frames (10 FPS → 0.1 s each, 20 frames).
        ft.reserve(20);
        for (int i = 0; i < 20; ++i) ft.push_back(0.1f);
        // 65 s of fast frames (4000 FPS).
        const auto fast = constant_fps_series(4000.0f, 65.0f);
        ft.insert(ft.end(), fast.begin(), fast.end());

        const heaven::PerfWindow w = heaven::measure_perf_window(ft);
        if (!w.valid) {
            std::printf("%s FAILED: slow-warmup + 65 s fast run was not VALID\n", kTag);
            ok = false;
        }
        // The slow prefix is exactly the 2 s warm-up, so the window is the fast
        // tail and its mean is ~4000, NOT the much lower blended average.
        if (!approx(w.mean_fps, 4000.0f, 5.0f)) {
            std::printf("%s FAILED: windowed mean FPS %.2f, expected ~4000 (the "
                        "slow first 2 s must be EXCLUDED)\n", kTag,
                        static_cast<double>(w.mean_fps));
            ok = false;
        }
    }

    // Binary-exact intervals pin the minimum-duration boundary and frame
    // accounting. Even 4 FPS is valid when the measurement covers 60 seconds.
    {
        auto ft = constant_fps_series(4.0f, 62.0f);
        const heaven::PerfWindow full = heaven::measure_perf_window(ft);
        if (!full.valid || full.mean_fps != 4.0f ||
            full.warmup_seconds != 2.0f || full.window_seconds != 60.0f ||
            full.frames_in_window != 240) {
            std::printf("%s FAILED: complete 4-FPS window lost time or frames\n", kTag);
            ok = false;
        }
        ft.pop_back();
        const heaven::PerfWindow short_window = heaven::measure_perf_window(ft);
        if (short_window.valid || short_window.mean_fps != 4.0f ||
            short_window.warmup_seconds != 2.0f ||
            short_window.window_seconds != 59.75f ||
            short_window.frames_in_window != 239) {
            std::printf("%s FAILED: one-frame-short window accounting is wrong\n", kTag);
            ok = false;
        }
    }

    // Exclude the entire frame crossing the warm-up boundary, then count all
    // remaining frames exactly once. A partial warm-up alone is incomplete.
    {
        std::vector<float> ft(3, 0.75f);
        ft.insert(ft.end(), 240, 0.25f);
        const heaven::PerfWindow full = heaven::measure_perf_window(ft);
        if (!full.valid || full.warmup_seconds != 2.25f ||
            full.window_seconds != 60.0f || full.frames_in_window != 240 ||
            full.mean_fps != 4.0f) {
            std::printf("%s FAILED: warm-up boundary frame was not excluded\n", kTag);
            ok = false;
        }
        const heaven::PerfWindow warmup_only = heaven::measure_perf_window({0.5f, 0.5f});
        if (warmup_only.valid || warmup_only.warmup_seconds != 1.0f ||
            warmup_only.window_seconds != 0.0f || warmup_only.frames_in_window != 0 ||
            warmup_only.mean_fps != 0.0f) {
            std::printf("%s FAILED: partial warm-up counted as measured frames\n", kTag);
            ok = false;
        }
    }

    // ── Empty / degenerate input is safe ────────────────────────
    {
        const heaven::PerfWindow w = heaven::measure_perf_window({});
        if (w.valid || w.mean_fps != 0.0f) {
            std::printf("%s FAILED: empty series should be invalid with 0 mean "
                        "FPS (got valid=%d mean=%.3f)\n", kTag, w.valid,
                        static_cast<double>(w.mean_fps));
            ok = false;
        }
    }

    if (ok) {
        std::printf("%s warm-up exclusion, minimum window, mean FPS, and frame "
                    "accounting verified headlessly.\n", kTag);
    }
    return ok;
}

const heaven::testing::Register reg_integ_fps_bar{integ_fps_bar};

} // namespace
