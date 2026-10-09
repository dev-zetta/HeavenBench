/*
 * HeavenBench — example/snapshot test: Result_Report System / Settings blocks
 *
 *
 *
 * Exercises OverlayUI::build() headlessly (Dear ImGui with no Filament
 * backend) against BenchmarkStatus snapshots, asserting that a completed run's
 * Result_Report renders the two labelled detail blocks read straight from the
 * snapshot:
 *
 *   -: a labelled "System" block with three lines — the
 *     Platform_Description, the CPU_Description, and the GPU_Description from
 *     status.result.system.
 *   -: a labelled "Settings" block with three lines — the
 *     Render_API_Description, the Display_Mode_Description, and the
 *     Quality_Preset_Label from status.result.settings.
 *
 * The overlay performs no gathering, formatting, or placeholder logic; the
 * snapshot strings are rendered verbatim. A second snapshot whose GPU and
 * render-API lines already carry the Unknown_Value_Placeholder
 * (heaven::kUnknownGpu / heaven::kUnknownRenderApi) confirms the placeholder
 * text reaches the screen unchanged.
 *
 * The four headline figures (Average FPS / Score / Min FPS / Max FPS) are also
 * re-checked here for rounding  using the same round_fps /
 * round_score formatting the overlay uses, mirroring unit_overlay_result.cpp.
 *
 * Headless harness identical to unit_overlay_result.cpp: a fresh ImGui context
 * (IMGUI_CHECKVERSION + CreateContext, 1280x720 DisplaySize, baked font atlas,
 * DeltaTime = 1/60 s), each "frame" a NewFrame() -> build(status) -> Render()
 * cycle, and the text rendered that frame captured via ImGui::LogToClipboard()
 * for exact substring matching. Registers with the shared
 * heaven_benchmark_tests harness at static-init time.
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "benchmark/benchmark_system.h"
#include "benchmark/run_info.h"
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

// Assert that every expected substring appears in the captured frame text;
// report each miss and flip *ok to false.
void expect_all_present(const char* captured,
                        const char* const* expected,
                        size_t count,
                        bool* ok) {
    const char* haystack = captured ? captured : "";
    for (size_t i = 0; i < count; ++i) {
        if (std::strstr(haystack, expected[i]) == nullptr) {
            std::printf("[unit_overlay_result_info] FAILED: result report is "
                        "missing expected line \"%s\"\n",
                        expected[i]);
            *ok = false;
        }
    }
}

// A Complete-state snapshot carrying a fully-populated result with distinct,
// unambiguous System / Settings strings and cleanly-rounding figures.
heaven::BenchmarkStatus make_populated_status() {
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

    status.result.system.platform = "Linux 7.0.0-22-generic x86_64";
    status.result.system.cpu      = "AMD Ryzen 9 7945HX3D (2295MHz) x32";
    status.result.system.gpu      = "AMD Radeon RX 7900 XTX (24576MB)";

    status.result.settings.render_api     = "Vulkan";
    status.result.settings.display_mode   = "1600x900 8xAA windowed";
    status.result.settings.quality_preset = "Extreme";
    return status;
}

bool unit_overlay_result_info() {
    bool ok = true;

    // ── Headless ImGui context ───────────────────────────────────
    IMGUI_CHECKVERSION();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime   = 1.0f / 60.0f;

    // Bake the font atlas so text layout has real glyph metrics (the report
    // window auto-resizes to its contents).
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires an unsigned char** output.
    unsigned char* pixels = nullptr;
    int tex_w = 0;
    int tex_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &tex_w, &tex_h);

    heaven::OverlayUI overlay;

    // The former corner overlays are consolidated into one categorized,
    // fixed-left diagnostics sidebar.
    {
        constexpr const char* kGpuName = "AMD Radeon RX 7900 XTX";
        heaven::DebugSidebarInfo info;
        info.fps = 123.4f;
        info.frame_time_ms = 8.1f;
        info.one_percent_low_fps = 98.7f;
        info.gpu_frame_ms = 7.25;
        info.gpu_name = kGpuName;
        info.gpu_driver = "Mesa RADV";
        info.graphics_api = "Vulkan";
        info.graphics_api_version = "1.4.0";
        info.gpu_utilization_percent = 87.0;
        info.device_vram_used_mb = 8192.0;
        info.device_vram_total_mb = 24576.0;
        info.vertices = 1'234'567;
        info.triangles = 2'345'678;
        info.base_vertices = 123'456;
        info.base_triangles = 234'567;
        info.width = 1280;
        info.height = 720;
        info.render_preset = "extreme";
        info.platform = "Linux test";
        info.cpu = "Test CPU";

        ImGui::NewFrame();
        ImGui::LogToClipboard();
        overlay.draw_debug_sidebar(info);
        ImGui::LogFinish();
        ImGui::Render();

        const char* captured = ImGui::GetClipboardText();
        constexpr const char* expected[] = {
            "Performance", "FPS", "123.4", "GPU", kGpuName,
            "Utilization", "87.0 %", "Scene", "Mesh vertices", "Mesh triangles",
            "Base mesh", "1,234,567",
            "Renderer", "1280x720", "System", "Linux test",
        };
        for (const char* value : expected) {
            if (captured == nullptr || std::strstr(captured, value) == nullptr) {
                std::printf("[unit_overlay_result_info] FAILED: diagnostics "
                            "sidebar is missing \"%s\"\n", value);
                ok = false;
            }
        }

        ImGuiWindow const* sidebar = ImGui::FindWindowByName("Diagnostics Sidebar");
        if (sidebar == nullptr || !sidebar->Active ||
            std::abs(sidebar->Pos.x) > 0.5f ||
            std::abs(sidebar->Size.x - 330.0f) > 0.5f ||
            std::abs(sidebar->Size.y - 720.0f) > 0.5f) {
            std::printf("[unit_overlay_result_info] FAILED: diagnostics "
                        "sidebar is not active and pinned to the left work area\n");
            ok = false;
        }
    }

    // ── Assertion 1: populated System / Settings blocks  ──
    // A completed run renders both labelled blocks, each with its three lines
    // sourced verbatim from status.result. The block labels themselves
    // ("System" / "Settings", drawn via SeparatorText) are checked too so the
    // blocks are demonstrably labelled, not bare text.
    const heaven::BenchmarkStatus populated = make_populated_status();
    {
        run_frame(overlay, populated, /*capture=*/true);

        ImGuiWindow const* win = ImGui::FindWindowByName(kResultWindow);
        if (win == nullptr || !win->Active) {
            std::printf("[unit_overlay_result_info] FAILED: result window "
                        "missing/inactive for a completed run (win=%p)\n",
                        static_cast<const void*>(win));
            ok = false;
        }

        const char* captured = ImGui::GetClipboardText();

        // The score triplet and primary rates, rounded exactly as the overlay
        // formats them.
        char figures[8][64];
        std::snprintf(figures[0], sizeof figures[0], "Average FPS: %.1f",
                      heaven::round_fps(
                          populated.result.performance.metrics.average_fps));
        std::snprintf(figures[1], sizeof figures[1], "Overall: %lld",
                      heaven::round_score(
                          populated.result.performance.scores.overall));
        std::snprintf(figures[2], sizeof figures[2], "CPU: %lld",
                      heaven::round_score(
                          populated.result.performance.scores.cpu));
        std::snprintf(figures[3], sizeof figures[3], "GPU: %lld",
                      heaven::round_score(
                          populated.result.performance.scores.gpu));
        std::snprintf(figures[4], sizeof figures[4], "1%% low: %.1f FPS",
                      heaven::round_fps(
                          populated.result.performance.metrics.one_percent_low_fps));
        std::snprintf(figures[5], sizeof figures[5], "Update rate: %.1f/s",
                      populated.result.performance.metrics.cpu.updates_per_second);
        std::snprintf(figures[6], sizeof figures[6], "Equivalent FPS: %.1f",
                      heaven::round_fps(
                          populated.result.performance.metrics.gpu_equivalent_fps));
        std::snprintf(figures[7], sizeof figures[7], "Real-time speed: %.1fx",
                      populated.result.performance.metrics.cpu.updates_per_second /
                          heaven::kCpuSimulationRate);
        const char* const figure_ptrs[8] = {
            figures[0], figures[1], figures[2], figures[3],
            figures[4], figures[5], figures[6], figures[7],
        };
        expect_all_present(captured, figure_ptrs, 8, &ok);

        // Labelled System block: "System" header + three lines.
        const char* const system_lines[4] = {
            "System",
            populated.result.system.platform.c_str(),
            populated.result.system.cpu.c_str(),
            populated.result.system.gpu.c_str(),
        };
        expect_all_present(captured, system_lines, 4, &ok);

        // Labelled Settings block: "Settings" header + three lines.
        const char* const settings_lines[4] = {
            "Settings",
            populated.result.settings.render_api.c_str(),
            populated.result.settings.display_mode.c_str(),
            populated.result.settings.quality_preset.c_str(),
        };
        expect_all_present(captured, settings_lines, 4, &ok);
    }

    // ── Assertion 2: placeholder strings render verbatim  ──
    // A snapshot whose GPU and render-API lines already carry the
    // Unknown_Value_Placeholder must render that exact placeholder text — the
    // overlay does no substitution of its own.
    {
        heaven::BenchmarkStatus placeholder = make_populated_status();
        placeholder.result.system.gpu        = heaven::kUnknownGpu;
        placeholder.result.settings.render_api = heaven::kUnknownRenderApi;

        run_frame(overlay, placeholder, /*capture=*/true);

        const char* captured = ImGui::GetClipboardText();
        const char* const placeholder_lines[2] = {
            heaven::kUnknownGpu,        // "Unknown GPU"
            heaven::kUnknownRenderApi,  // "Unknown API"
        };
        expect_all_present(captured, placeholder_lines, 2, &ok);

        // The still-valid datums must remain present (placeholder substitution
        // is per-datum, not all-or-nothing).
        const char* const still_valid[2] = {
            placeholder.result.system.platform.c_str(),
            placeholder.result.settings.display_mode.c_str(),
        };
        expect_all_present(captured, still_valid, 2, &ok);
    }

    ImGui::DestroyContext(ctx);
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_overlay_result_info{unit_overlay_result_info};

} // namespace
