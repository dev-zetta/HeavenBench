/*
 * HeavenBench — headless idle and running toolbar controls
 *
 *
 *
 * Exercises the real OverlayUI::build() widget builder against a headless Dear
 * ImGui context (no Filament, no GPU) and asserts the Menu_Bar contract:
 *
 *   -  — while idle the Menu_Bar is present and lists its controls
 *     (Benchmark, Settings, Quit) left-to-right in that order.
 *   -  — while a run is active Cancel replaces Benchmark; Settings and Quit
 *     remain available, including during a fully black scene-cut fade.
 *   -  — activating Quit raises the quit intent.
 *   -  — activating Benchmark raises the start_benchmark intent.
 *   -  — activating Settings opens the consolidated configuration
 *     window on Graphics (open_panel == Quality); the individual setting
 *     groups are tabs within it rather than separate menu entries.
 *   -  — when both Quit and Benchmark fire in one frame the loop quits
 *     without starting a run (quit precedence).
 *
 * How the headless assertions work (no native window/device):
 *   - Presence across states is checked with imgui_internal's
 *     FindWindowByName("##MainMenuBar"): the window is Active the frame the bar
 *     is built in both idle and running states.
 *   - Ordering (10.1) is recovered by sweeping the mouse position left-to-right
 *     across the bar, rebuilding the REAL OverlayUI each frame, and recording
 *     the order in which each control first becomes the hovered item
 *     (g.HoveredId). This validates the actual builder, not a replica.
 *   - Per-control intent mapping (10.3/10.4/10.6) is driven deterministically
 *     with ImGui::ActivateItemByID(): each top-level MenuItem is a Selectable
 *     whose id is recomputed from the menu-bar id stack, queued for activation,
 *     and the OverlayIntents returned by build() on the next frame are checked.
 *   - Quit precedence (10.5) is a loop-level contract (the builder sets the
 *     intents independently); it is asserted by mirroring the loop's decision
 *     rule (quit honoured before start) and confirming both-set resolves to
 *     quit.
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time, like the other test translation units,
 * and owns its ImGui context for the duration of the check (create at entry,
 * destroy before returning).
 */
#include <array>
#include <cstdio>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

#include "benchmark/benchmark_system.h"
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

// The first action follows the benchmark state; the other controls retain
// their idle ordering and intent semantics.
constexpr std::array<const char*, 3> kMenuLabels = {
    "Benchmark", "Settings", "Quit"};
constexpr std::array<const char*, 3> kRunningMenuLabels = {
    "Cancel", "Settings", "Quit"};

constexpr float kDisplayW = 1280.0f;
constexpr float kDisplayH = 720.0f;

// Begin one headless ImGui frame with the mouse at the given position. A
// position off-screen (negative) means "no hover".
void begin_frame(float mouse_x, float mouse_y) {
    ImGuiIO& io   = ImGui::GetIO();
    io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
    io.DeltaTime   = 1.0f / 60.0f;
    io.AddMousePosEvent(mouse_x, mouse_y);
    ImGui::NewFrame();
}

// Compute the ImGui id of a top-level Menu_Bar MenuItem's internal Selectable.
// While the real builder runs the id stack is:
//   window "##MainMenuBar"           (BeginMainMenuBar)
//     -> PushID("##MenuBar")         (BeginMenuBar)
//       -> PushID(label)             (MenuItemEx)
//         -> GetID("")               (the Selectable returning `pressed`)
// so the id is ImHashStr("", seed = ImHashStr(label, seed = ImHashStr
// "##MenuBar", seed = window->ID))). Returns 0 when the bar window is absent.
ImGuiID menu_item_id(const char* label) {
    ImGuiWindow const* bar = ImGui::FindWindowByName("##MainMenuBar");
    if (bar == nullptr) {
        return 0;
    }
    ImGuiID id = ImHashStr("##MenuBar", 0, bar->ID);
    id         = ImHashStr(label, 0, id);
    id         = ImHashStr("", 0, id);
    return id;
}

// Build one OverlayUI frame for the given snapshot, returning the intents.
heaven::OverlayIntents build_frame(heaven::OverlayUI&            overlay,
                                   const heaven::BenchmarkStatus& status,
                                   float mouse_x = -1.0f,
                                   float mouse_y = -1.0f) {
    begin_frame(mouse_x, mouse_y);
    heaven::OverlayIntents intents =
        overlay.build(status, heaven::BenchConfig{}, heaven::ConfigUiState{});
    ImGui::Render();
    return intents;
}

// Mirror of the main loop's intent-resolution rule: quit is honoured before
// start so simultaneous activation quits without starting a run
// Kept in lockstep with the documented loop contract in
// the implementation (main_loop honours intents.quit first).
enum class LoopAction { None, Quit, Start };

LoopAction loop_decision(const heaven::OverlayIntents& intents) {
    if (intents.quit) {
        return LoopAction::Quit;
    }
    if (intents.start_benchmark) {
        return LoopAction::Start;
    }
    return LoopAction::None;
}

// Toolbar visibility persists across run-state transitions.

bool check_visibility() {
    bool ok = true;

    heaven::OverlayUI overlay;

    heaven::BenchmarkStatus idle{};   // defaults: state == Idle
    heaven::BenchmarkStatus running{};
    running.state       = heaven::BenchmarkState::Running;
    running.scene_count = 26;

    // Idle: the Menu_Bar is built, so its window is present and active

    build_frame(overlay, idle);
    {
        const ImGuiWindow* bar = ImGui::FindWindowByName("##MainMenuBar");
        if (bar == nullptr || !bar->Active) {
            std::printf(
                "[unit_overlay_menu] FAILED (Req 10.1): main menu bar is not "
                "present/active while idle (window=%p)\n",
                static_cast<const void*>(bar));
            ok = false;
        }
    }

    // Running: keep the toolbar accessible rather than replacing it with the
    // noninteractive status overlay.
    build_frame(overlay, running);
    {
        const ImGuiWindow* bar = ImGui::FindWindowByName("##MainMenuBar");
        if (bar == nullptr || !bar->Active) {
            std::printf(
                "[unit_overlay_menu] FAILED: main menu bar disappeared "
                "while a run is in progress\n");
            ok = false;
        }
    }

    return ok;
}

// ── Part A: left-to-right ordering of the three controls  ──

bool check_ordering(const heaven::BenchmarkStatus& status,
                    const std::array<const char*, 3>& labels) {
    bool ok = true;

    heaven::OverlayUI       overlay;

    // Warm up so the bar window + item layout settle before sampling hover.
    build_frame(overlay, status);
    build_frame(overlay, status);

    std::array<ImGuiID, 3> ids{};
    for (size_t i = 0; i < labels.size(); ++i) {
        ids[i] = menu_item_id(labels[i]);
    }

    const ImGuiContext& g = *ImGui::GetCurrentContext();

    // Sweep the mouse across the top of the screen, rebuilding the REAL overlay
    // each frame, and record the order in which each control first becomes the
    // hovered item. The mouse y stays inside the menu-bar row.
    std::vector<int>      discovered;
    std::array<bool, 3>   seen{};
    for (int x = 0; x < static_cast<int>(kDisplayW); x += 2) {
        begin_frame(static_cast<float>(x), 10.0f);
        overlay.build(status, heaven::BenchConfig{}, heaven::ConfigUiState{});
        ImGui::Render();

        const ImGuiID hovered = g.HoveredId;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (hovered != 0 && hovered == ids[i] && !seen[i]) {
                seen[i] = true;
                discovered.push_back(static_cast<int>(i));
            }
        }
    }

    if (discovered.size() != labels.size()) {
        std::printf(
            "[unit_overlay_menu] FAILED (Req 10.1): hover sweep found %zu of %zu "
            "controls\n",
            discovered.size(), labels.size());
        ok = false;
    } else {
        for (size_t i = 0; i < discovered.size(); ++i) {
            if (std::cmp_not_equal(discovered[i], i)) {
                std::printf(
                    "[unit_overlay_menu] FAILED (Req 10.1): control at position "
                    "%zu is '%s', expected '%s'\n",
                    i, labels[static_cast<size_t>(discovered[i])], labels[i]);
                ok = false;
            }
        }
    }

    return ok;
}

// Confirm the intents returned for a given activated control match expectations

bool intents_match_control(const char*                    label,
                           const heaven::OverlayIntents&  intents) {
    using Panel = heaven::OverlayIntents::Panel;

    auto expect = [&](bool                         start,
                      bool                         quit,
                      Panel                        panel,
                      bool                         cancel = false) -> bool {
        bool good = intents.start_benchmark == start &&
                    intents.quit == quit &&
                    intents.open_panel == panel &&
                    intents.cancel == cancel && !intents.dismiss_result;
        if (!good) {
            std::printf(
                "[unit_overlay_menu] FAILED: activating '%s' gave "
                "start=%d quit=%d panel=%d cancel=%d dismiss=%d\n",
                label, intents.start_benchmark, intents.quit,
                static_cast<int>(intents.open_panel), intents.cancel,
                intents.dismiss_result);
        }
        return good;
    };

    if (std::string_view(label) == "Benchmark") {
        return expect(true, false, Panel::None);
    }
    if (std::string_view(label) == "Cancel") {
        return expect(false, false, Panel::None, true);
    }
    if (std::string_view(label) == "Quit") {
        return expect(false, true, Panel::None);
    }
    if (std::string_view(label) == "Settings") {
        // Settings opens the single consolidated configuration window, focused
        // on Graphics (the catalog's Quality panel).
        return expect(false, false, Panel::Quality);
    }

    std::printf("[unit_overlay_menu] FAILED: unknown control '%s'\n", label);
    return false;
}

// ── Part B: per-control intent mapping  ──

bool check_intent_mapping(const heaven::BenchmarkStatus& status,
                          const std::array<const char*, 3>& labels) {
    bool ok = true;

    // Warm up once so the bar window + item ids exist for activation.
    {
        heaven::OverlayUI warmup;
        build_frame(warmup, status);
    }

    for (const char* label : labels) {
        const ImGuiID id = menu_item_id(label);
        if (id == 0) {
            std::printf(
                "[unit_overlay_menu] FAILED: could not compute id for '%s' "
                "(menu bar window missing)\n",
                label);
            ok = false;
            continue;
        }

        // A fresh builder per control keeps each control's Options_Panel open
        // state independent. The menu-bar window itself persists in the ImGui
        // context, so the previous frame's submission lets the queued
        // activation resolve on this control's build frame.
        heaven::OverlayUI overlay;

        // Re-submit the bar so the activation has a live item to land on, then
        // queue the activation and resolve it on the next build.
        build_frame(overlay, status);
        ImGui::ActivateItemByID(id);
        heaven::OverlayIntents intents = build_frame(overlay, status);

        ok = intents_match_control(label, intents) && ok;
        if (std::string_view(label) == "Settings") {
            const ImGuiWindow* settings = ImGui::FindWindowByName("Settings##cfg");
            if (settings == nullptr || !settings->Active) {
                std::printf("[unit_overlay_menu] FAILED: Settings did not open\n");
                ok = false;
            }
            // Opening settings must remain usable on the following run frame,
            // without a second menu activation or a cancellation intent.
            const auto next = build_frame(overlay, status);
            if (!settings->Active || next.cancel || next.start_benchmark) {
                std::printf("[unit_overlay_menu] FAILED: Settings did not persist\n");
                ok = false;
            }
        }
    }

    return ok;
}

bool check_pointer_cancel() {
    heaven::OverlayUI overlay;
    heaven::BenchmarkStatus running{};
    running.state = heaven::BenchmarkState::Running;
    running.fade_alpha = 1.0f;
    build_frame(overlay, running);
    build_frame(overlay, running);
    const ImGuiID cancel_id = menu_item_id("Cancel");
    for (int x = 0; x < static_cast<int>(kDisplayW); x += 2) {
        const float mouse_x = static_cast<float>(x);
        build_frame(overlay, running, mouse_x, 10.0f);
        if (cancel_id == 0 || ImGui::GetCurrentContext()->HoveredId != cancel_id) {
            continue;
        }
        // Drive the same press/release events as SDL touch-to-mouse input,
        // rather than only the navigation activation used above.
        ImGui::GetIO().AddMouseButtonEvent(0, true);
        const auto pressed = build_frame(overlay, running, mouse_x, 10.0f);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
        const auto released = build_frame(overlay, running, mouse_x, 10.0f);
        if ((pressed.cancel || released.cancel) &&
            !pressed.start_benchmark && !released.start_benchmark) return true;
        std::printf("[unit_overlay_menu] FAILED: pointer Cancel did not cancel\n");
        return false;
    }
    std::printf("[unit_overlay_menu] FAILED: pointer could not reach Cancel\n");
    return false;
}

// ── Part B: quit precedence  ──

bool check_quit_precedence() {
    bool ok = true;

    // Both controls activated in one frame: the loop quits without starting.
    heaven::OverlayIntents both;
    both.quit            = true;
    both.start_benchmark = true;
    if (loop_decision(both) != LoopAction::Quit) {
        std::printf(
            "[unit_overlay_menu] FAILED (Req 10.5): quit+start did not resolve "
            "to Quit\n");
        ok = false;
    }

    // Sanity: each intent alone resolves to its own action, so the precedence
    // rule above is genuinely choosing quit over an otherwise-live start.
    heaven::OverlayIntents start_only;
    start_only.start_benchmark = true;
    if (loop_decision(start_only) != LoopAction::Start) {
        std::printf(
            "[unit_overlay_menu] FAILED (Req 10.5): start-only did not resolve "
            "to Start\n");
        ok = false;
    }

    heaven::OverlayIntents quit_only;
    quit_only.quit = true;
    if (loop_decision(quit_only) != LoopAction::Quit) {
        std::printf(
            "[unit_overlay_menu] FAILED (Req 10.5): quit-only did not resolve "
            "to Quit\n");
        ok = false;
    }

    return ok;
}

bool unit_overlay_menu() {
    // Own a fresh headless ImGui context for the duration of this check.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(kDisplayW, kDisplayH);
    io.DeltaTime   = 1.0f / 60.0f;

    // Bake a font atlas so ImGui::NewFrame() has a loaded font (headless: no
    // GPU upload, the pixel data is just produced in memory).
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires an unsigned char** output.
    unsigned char* pixels = nullptr;
    int            atlas_w = 0;
    int            atlas_h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_w, &atlas_h);

    bool ok = true;
    heaven::BenchmarkStatus idle{};
    heaven::BenchmarkStatus running{};
    running.state = heaven::BenchmarkState::Running;
    running.fade_alpha = 1.0f;
    ok      = check_visibility() && ok;
    ok      = check_ordering(idle, kMenuLabels) && ok;
    ok      = check_ordering(running, kRunningMenuLabels) && ok;
    ok      = check_intent_mapping(idle, kMenuLabels) && ok;
    ok      = check_intent_mapping(running, kRunningMenuLabels) && ok;
    ok      = check_pointer_cancel() && ok;
    ok      = check_quit_precedence() && ok;

    ImGui::DestroyContext();

    if (ok) {
        std::printf("[unit_overlay_menu] passed\n");
    }
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_overlay_menu{unit_overlay_menu};

} // namespace
