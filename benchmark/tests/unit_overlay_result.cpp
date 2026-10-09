/*
 * HeavenBench — example test: the final Result_Report overlay
 *
 *
 *
 * Exercises OverlayUI::build() headlessly (Dear ImGui with no Filament
 * backend) against BenchmarkStatus snapshots, asserting the Result_Report's
 * three observable behaviours:
 *
 *   -: when a run has completed (has_result), the "Benchmark Result"
 *     window is present and shows the four reported figures — Average FPS,
 *     Score, Min FPS, Max FPS — sourced from status.result.
 *   -: while the result has not been dismissed, the report stays
 *     present/active across consecutive builds (persistence across frames).
 *     Conversely, with no result the window is absent.
 *   -: activating the "Dismiss" control raises
 *     OverlayIntents::dismiss_result.
 *
 * Headless harness. A fresh ImGui context is created and torn down inside the
 * test function: IMGUI_CHECKVERSION + CreateContext, a fixed 1280x720
 * DisplaySize, a baked font atlas, and DeltaTime = 1/60 s. Each "frame" is a
 * NewFrame() -> build(status) -> Render() cycle; no GPU/renderer backend is
 * needed because ImGui::Render() only fills draw data.
 *
 * Widget inspection: ImGui::FindWindowByName() (imgui_internal.h) locates the
 * report window so its Active/WasActive lifecycle can be queried, and
 * ImGui::LogToClipboard() captures the text rendered that frame so the four
 * field strings can be matched exactly (built with the same round_fps /
 * round_score formatting the overlay uses).
 *
 * Dismiss click: rather than fake mouse coordinates (fragile against layout),
 * the click is driven deterministically through ImGui's own activation path.
 * After one build frame registers the window + button, the button's ID is
 * recomputed from the window (window->GetID("Dismiss"), the same seed ImGui
 * uses) and queued via ImGui::ActivateItemByID(). On the next frame NewFrame()
 * -> NavUpdate() promotes that queued id to NavActivateId, and when build()
 * re-submits the button ButtonBehavior() reports it pressed — exactly as a real
 * press+release would, with no dependence on cursor position.
 *
 * Registers with the shared heaven_benchmark_tests harness (test_main.cpp) at
 * static-init time; the callable returns true on success and prints a
 * diagnostic + returns false on failure.
 */
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "benchmark/benchmark_system.h"
#include "benchmark/score_calculator.h"
#include "rhi/overlay/overlay_ui.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

constexpr const char* kResultWindow = "Benchmark Result";

// Build a Complete-state snapshot carrying a populated result. The four
// reported figures are distinct, cleanly rounding values so the rendered field
// strings are unambiguous.
heaven::BenchmarkStatus make_complete_status() {
    heaven::BenchmarkStatus status;
    status.state              = heaven::BenchmarkState::Complete;
    status.scene_count        = 26;
    status.has_result         = true;
    status.result.average_fps = 123.4;
    status.result.performance.scores.overall = 3108.0;
    status.result.performance.scores.cpu = 4200.0;
    status.result.performance.scores.gpu = 2900.0;
    status.result.performance.metrics.average_fps = 123.4;
    status.result.performance.metrics.path_geomean_fps = 120.0;
    status.result.performance.metrics.one_percent_low_fps = 90.1;
    status.result.performance.metrics.effective_fps = 104.0;
    status.result.performance.metrics.cpu.updates_per_second = 252.0;
    status.result.performance.metrics.gpu_equivalent_fps = 174.0;
    status.result.min_fps     = 90.1f;
    status.result.max_fps     = 210.5f;
    status.result.frames      = 7404;
    status.result.total_time  = 60.0;
    return status;
}

// One headless ImGui frame around an OverlayUI build. When `capture` is set the
// text rendered this frame is logged to the private clipboard buffer so the
// caller can inspect it after the frame.
heaven::OverlayIntents run_frame(heaven::OverlayUI& overlay,
                                 const heaven::BenchmarkStatus& status,
                                 bool capture = false) {
    ImGui::NewFrame();
    if (capture) {
        ImGui::LogToClipboard();
    }
    heaven::OverlayIntents intents =
        overlay.build(status, heaven::BenchConfig{}, heaven::ConfigUiState{});
    if (capture) {
        ImGui::LogFinish();
    }
    ImGui::Render();
    return intents;
}

bool unit_overlay_result() {
    bool ok = true;

    // ── Headless ImGui context ───────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io   = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime   = 1.0f / 60.0f;

    // Bake the font atlas so text layout has real glyph metrics (the report
    // window auto-resizes to its contents, so this keeps the Dismiss button
    // on-screen and hit-testable).
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires an unsigned char** output.
    unsigned char* pixels = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &tex_w, &tex_h);

    heaven::OverlayUI overlay;
    overlay.set_application_build_info({"1.0.0-test", "zen4"});

    // ── Assertion 2: absent when there is no result (inverse) ──
    // An Idle snapshot with has_result == false must not produce the report
    // window. Checked first so the window has never been created in this
    // context.
    {
        heaven::BenchmarkStatus idle;  // defaults: Idle, has_result == false
        run_frame(overlay, idle);

        const ImGuiWindow* win = ImGui::FindWindowByName(kResultWindow);
        const bool absent = (win == nullptr) || (!win->Active && !win->WasActive);
        if (!absent) {
            std::printf(
                "[unit_overlay_result] FAILED: result window present with no "
                "result (Active=%d WasActive=%d)\n",
                win ? win->Active : 0, win ? win->WasActive : 0);
            ok = false;
        }
    }

    // ── Assertion 1: present + four fields + persistence  ──
    // A completed run renders the report. The four figures are matched against
    // the exact strings the overlay emits (same round_fps / round_score
    // formatting), confirming they are sourced from status.result.
    const heaven::BenchmarkStatus complete = make_complete_status();
    {
        heaven::OverlayIntents intents = run_frame(overlay, complete, /*capture=*/true);

        const ImGuiWindow* win = ImGui::FindWindowByName(kResultWindow);
        if (win == nullptr || !win->Active) {
            std::printf(
                "[unit_overlay_result] FAILED: result window missing/inactive "
                "for a completed run (win=%p)\n",
                static_cast<const void*>(win));
            ok = false;
        }

        // No dismiss intent should arise just from rendering the report.
        if (intents.dismiss_result) {
            std::printf("[unit_overlay_result] FAILED: dismiss_result raised "
                        "without activating the Dismiss control\n");
            ok = false;
        }

        // The score triplet and primary workload rates, formatted exactly as
        // the overlay does.
        char expected[8][64];
        std::snprintf(expected[0], sizeof expected[0],
                      "HeavenBench 1.0.0-test [zen4]");
        std::snprintf(expected[1], sizeof expected[1], "Average FPS: %.1f",
                      heaven::round_fps(
                          complete.result.performance.metrics.average_fps));
        std::snprintf(expected[2], sizeof expected[2], "Overall: %lld",
                      heaven::round_score(
                          complete.result.performance.scores.overall));
        std::snprintf(expected[3], sizeof expected[3], "CPU: %lld",
                      heaven::round_score(
                          complete.result.performance.scores.cpu));
        std::snprintf(expected[4], sizeof expected[4], "GPU: %lld",
                      heaven::round_score(
                          complete.result.performance.scores.gpu));
        std::snprintf(expected[5], sizeof expected[5], "1%% low: %.1f FPS",
                      heaven::round_fps(
                          complete.result.performance.metrics.one_percent_low_fps));
        std::snprintf(expected[6], sizeof expected[6], "Update rate: %.1f/s",
                      complete.result.performance.metrics.cpu.updates_per_second);
        std::snprintf(expected[7], sizeof expected[7], "Equivalent FPS: %.1f",
                      heaven::round_fps(
                          complete.result.performance.metrics.gpu_equivalent_fps));

        const char* captured = ImGui::GetClipboardText();
        const char* haystack = captured ? captured : "";
        for (const auto& field : expected) {
            if (std::strstr(haystack, field) == nullptr) {
                std::printf("[unit_overlay_result] FAILED: result report is "
                            "missing field \"%s\"\n",
                            field);
                ok = false;
            }
        }
    }

    // Persistence across builds until dismissed: with the result
    // still undismissed, the window must stay present/active on every
    // subsequent build.
    for (int frame = 0; frame < 3; ++frame) {
        run_frame(overlay, complete);

        const ImGuiWindow* win = ImGui::FindWindowByName(kResultWindow);
        if (win == nullptr || !win->Active) {
            std::printf(
                "[unit_overlay_result] FAILED: result window not persistent on "
                "build %d (win=%p)\n",
                frame, static_cast<const void*>(win));
            ok = false;
            break;
        }
    }

    // ── Assertion 3: Dismiss raises dismiss_result  ─────
    // Frame A registers the window + Dismiss button. The button's ID is then
    // recomputed from the window (same seed ImGui used) and activation is
    // queued. Frame B's NewFrame promotes the queued id, and the re-submitted
    // button reports pressed.
    {
        run_frame(overlay, complete);  // frame A: register widgets

        ImGuiWindow* win = ImGui::FindWindowByName(kResultWindow);
        if (win == nullptr) {
            std::printf("[unit_overlay_result] FAILED: result window missing "
                        "before dismiss activation\n");
            ok = false;
        } else {
            const ImGuiID dismiss_id = win->GetID("Dismiss");
            ImGui::ActivateItemByID(dismiss_id);

            heaven::OverlayIntents intents =
                run_frame(overlay, complete);  // frame B: activation lands

            if (!intents.dismiss_result) {
                std::printf("[unit_overlay_result] FAILED: activating Dismiss "
                            "did not raise dismiss_result\n");
                ok = false;
            }
            // Activating Dismiss must not spuriously raise other intents.
            if (intents.quit || intents.start_benchmark) {
                std::printf("[unit_overlay_result] FAILED: Dismiss activation "
                            "raised an unrelated intent (quit=%d start=%d)\n",
                            intents.quit, intents.start_benchmark);
                ok = false;
            }
        }
    }

    ImGui::DestroyContext(ctx);
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_overlay_result{unit_overlay_result};

} // namespace
