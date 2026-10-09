/*
 * HeavenBench — example test: the running status overlay content
 *
 *
 *
 * OverlayUI::build() is a pure Dear ImGui widget builder driven by an immutable
 * BenchmarkStatus snapshot. This example pins the on-screen content of the live
 * status overlay:
 *
 *   - A Running snapshot emits the "Benchmarking..." label, the scene
 *     position as "Scene {index+1} / {count}", the five statistic
 *     fields Average FPS / Time / Frames / Min FPS / Max FPS, and the
 *     toolbar/ESC cancel hint. The toolbar remains active and the scene fade
 *     stays behind it; status and diagnostics occupy separate screen regions.
 *   - A non-running (Idle) snapshot omits all of the above.
 *
 * Inspecting emitted widgets headlessly
 * -------------------------------------
 * There is no real renderer here, so we drive a self-contained, headless Dear
 * ImGui context (CreateContext, a baked font atlas, a display size, a positive
 * delta time) and run full NewFrame -> build -> Render cycles. To read back what
 * text the overlay actually submitted we use ImGui's text-capture log
 * (LogToBuffer): while logging is enabled, ImGui routes every rendered string
 * into an internal buffer AND disables clipping, so the captured text is exactly
 * what the widgets emitted regardless of window position or viewport size. We
 * also query FindWindowByName("##benchmark_running") and its Active flag as a
 * structural signal that the running window was (or was not) submitted this
 * frame. Both helpers live in <imgui_internal.h>.
 *
 * The context is created and destroyed inside each capture so cases stay fully
 * independent and nothing leaks into the other registered tests.
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, matching
 * the self-registration pattern of the other test translation units. The
 * registered callable returns true on success and false (after printing a
 * diagnostic) on failure.
 */
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "benchmark/benchmark_system.h"
#include "rhi/overlay/overlay_ui.h"

#include <imgui.h>
#include <imgui_internal.h>

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

// What a single headless overlay frame emitted: the captured text and whether
// the running status window was submitted (exists) and active this frame.
struct OverlayCapture {
    std::string text;
    bool        running_window_exists = false;
    bool        running_window_active = false;
    bool        menu_active = false;
    bool        status_clear_sidebar = false;
    bool        status_below_toolbar = false;
    bool        fade_behind_ui = false;
};

// Build the overlay for one snapshot inside a fresh, self-contained headless
// ImGui context and capture what it emitted. The context is created and torn
// down here so the capture cannot leak into the rest of the suite.
OverlayCapture capture_overlay(const heaven::BenchmarkStatus& status,
                               ImVec2 display_size = ImVec2(1280.0f, 720.0f)) {
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);

    // A valid context needs a display size, a positive delta time, and a baked
    // font atlas before NewFrame() will run without asserting. Building the
    // atlas via the legacy GetTexDataAsRGBA32() path marks it built without any
    // GPU upload, which is all a headless run requires. Disable the.ini file so
    // the test never touches the user settings directory.
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = display_size;
    io.DeltaTime   = 1.0f / 60.0f;
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires an unsigned char** output.
    unsigned char* pixels = nullptr;
    int            tex_w  = 0;
    int            tex_h  = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &tex_w, &tex_h);

    heaven::OverlayUI overlay;
    OverlayCapture    cap;

    // Drive two complete frames and capture on the last. ImGui windows become
    // active during the frame in which they are submitted; two frames let the
    // auto-resized window settle before we read its state.
    constexpr int kFrames = 2;
    for (int frame = 0; frame < kFrames; ++frame) {
        ImGui::NewFrame();

        const bool capture_this_frame = (frame == kFrames - 1);

        // Start capturing emitted text before any widgets are built. NewFrame
        // has already pushed the implicit fallback window, so a current window
        // exists for LogToBuffer to attach to.
        if (capture_this_frame) {
            ImGui::LogToBuffer();
        }

        overlay.build(status, heaven::BenchConfig{}, heaven::ConfigUiState{});

        if (capture_this_frame) {
            // Copy the captured text out of the live log buffer before
            // LogFinish() clears it.
            cap.text.assign(ImGui::GetCurrentContext()->LogBuffer.c_str());
            ImGui::LogFinish();
        }

        // Diagnostics contributes to the layout, not the running-only text
        // assertions: it has its own unrelated "Scene" section while idle.
        overlay.draw_debug_sidebar(heaven::DebugSidebarInfo{});

        if (capture_this_frame) {

            // The running window is active during the frame it is submitted; it
            // is never created at all in the Idle case (fresh context).
            ImGuiWindow const* running = ImGui::FindWindowByName("##benchmark_running");
            cap.running_window_exists = (running != nullptr);
            cap.running_window_active = (running != nullptr && running->Active);
            const ImGuiWindow* menu = ImGui::FindWindowByName("##MainMenuBar");
            const ImGuiWindow* sidebar = ImGui::FindWindowByName("Diagnostics Sidebar");
            cap.menu_active = menu != nullptr && menu->Active;
            if (running != nullptr && menu != nullptr && sidebar != nullptr) {
                cap.status_clear_sidebar = !running->Rect().Overlaps(sidebar->Rect());
                cap.status_below_toolbar = running->Pos.y >= menu->Rect().Max.y;
            }
        }

        ImGui::Render();
        if (capture_this_frame && status.fade_alpha == 1.0f) {
            const ImDrawList* background = ImGui::GetBackgroundDrawList();
            const ImDrawList* foreground = ImGui::GetForegroundDrawList();
            const ImDrawData* data = ImGui::GetDrawData();
            cap.fade_behind_ui = background->VtxBuffer.Size == 4 &&
                background->VtxBuffer[0].col == IM_COL32(0, 0, 0, 255) &&
                foreground->VtxBuffer.empty() && data->CmdListsCount > 1 &&
                data->CmdLists[0] == background;
        }
    }

    ImGui::DestroyContext(ctx);
    return cap;
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

bool unit_overlay_running() {
    bool ok = true;

    // ── A Running snapshot shows the full live overlay  ──

    // Scene index 4 of 26 displays as "Scene 5 / 26". The statistic
    // values are arbitrary nonzero figures; the test pins the field labels and
    // the scene line, not the formatted numbers.
    heaven::BenchmarkStatus running;
    running.state       = heaven::BenchmarkState::Running;
    running.scene_index = 4;
    running.scene_count = 26;
    running.average_fps = 123.4;
    running.elapsed     = 12.5;
    running.frames      = 1500;
    running.min_fps     = 90.0f;
    running.max_fps     = 200.0f;

    const OverlayCapture run_cap = capture_overlay(running);

    struct Expect {
        const char* needle;
        const char* what;
    };
    const Expect running_expectations[] = {
        {"Benchmarking...", "the \"Benchmarking...\" label (Req 7.1)"},
        {"Scene 5 / 26", "the scene position \"Scene 5 / 26\" (Req 7.2)"},
        {"Average FPS:", "the Average FPS field (Req 7.4)"},
        {"Time:", "the Time field (Req 7.4)"},
        {"Frames:", "the Frames field (Req 7.4)"},
        {"Min FPS:", "the Min FPS field (Req 7.4)"},
        {"Max FPS:", "the Max FPS field (Req 7.4)"},
        {"Cancel in toolbar or press ESC", "the toolbar/ESC cancel hint"},
    };
    for (const Expect& e : running_expectations) {
        if (!contains(run_cap.text, e.needle)) {
            std::printf(
                "[unit_overlay_running] FAILED: Running overlay is missing %s.\n"
                "  captured text:\n%s\n",
                e.what, run_cap.text.c_str());
            ok = false;
        }
    }

    // The running window must have been submitted and active this frame.
    if (!run_cap.running_window_active) {
        std::printf(
            "[unit_overlay_running] FAILED: the running status window "
            "(##benchmark_running) was not active for a Running snapshot "
            "(exists=%d, active=%d).\n",
            run_cap.running_window_exists ? 1 : 0,
            run_cap.running_window_active ? 1 : 0);
        ok = false;
    }

    // Test the actual ImGui draw-list ordering at a fully opaque scene fade,
    // together with the production callback's status + diagnostics layout.
    running.fade_alpha = 1.0f;
    for (const auto size : {ImVec2(1280.0f, 720.0f), ImVec2(640.0f, 480.0f)}) {
        const auto faded = capture_overlay(running, size);
        if (!faded.menu_active || !faded.fade_behind_ui ||
            !faded.status_clear_sidebar || !faded.status_below_toolbar) {
            std::printf("[unit_overlay_running] FAILED: fade/layout at %.0fx%.0f "
                        "menu=%d behind=%d sidebar=%d below=%d\n",
                        size.x, size.y, faded.menu_active, faded.fade_behind_ui,
                        faded.status_clear_sidebar, faded.status_below_toolbar);
            ok = false;
        }
    }

    // ── A non-running (Idle) snapshot omits the live overlay  ──
    heaven::BenchmarkStatus idle;
    idle.state       = heaven::BenchmarkState::Idle;
    idle.scene_count = 26;

    const OverlayCapture idle_cap = capture_overlay(idle);

    // None of the running-only content may appear while idle. "Benchmarking..."
    // is distinct from the "Benchmark" menu item, so the menu bar shown while
    // idle does not trip these checks.
    const char* const forbidden_while_idle[] = {
        "Benchmarking...",
        "Cancel in toolbar or press ESC",
        "Scene ",
        "Average FPS:",
        "Min FPS:",
        "Max FPS:",
    };
    for (const char* needle : forbidden_while_idle) {
        if (contains(idle_cap.text, needle)) {
            std::printf(
                "[unit_overlay_running] FAILED: Idle overlay unexpectedly "
                "contains running-only text \"%s\" (Req 7.6).\n"
                "  captured text:\n%s\n",
                needle, idle_cap.text.c_str());
            ok = false;
        }
    }

    // The running window must not be active while idle.
    if (idle_cap.running_window_active) {
        std::printf(
            "[unit_overlay_running] FAILED: the running status window was "
            "active for an Idle snapshot (Req 7.6).\n");
        ok = false;
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_overlay_running{unit_overlay_running};

} // namespace
