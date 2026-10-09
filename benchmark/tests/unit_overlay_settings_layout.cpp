/*
 * Exercise real Settings layout with the tablet's font/style scale, without
 * a native window or GPU. Desktop retains its remembered window dimensions.
 */
#include "rhi/overlay/overlay_ui.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

bool require(bool condition, const char* description) {
    if (!condition) {
        std::printf("[unit_overlay_settings_layout] FAILED: %s\n", description);
    }
    return condition;
}

void frame(heaven::OverlayUI& overlay, bool running = true, bool capture = false) {
    ImGui::NewFrame();
    if (capture) ImGui::LogToClipboard();
    heaven::BenchmarkStatus status;
    status.state = running ? heaven::BenchmarkState::Running : heaven::BenchmarkState::Idle;
    overlay.build(status, heaven::BenchConfig{}, heaven::ConfigUiState{});
    if (capture) ImGui::LogFinish();
    ImGui::Render();
}

bool layout(float scale, ImVec2 display, bool open_before_running = false) {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = display;
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires unsigned char**.
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    heaven::OverlayUI overlay;
    frame(overlay, !open_before_running);
    frame(overlay, !open_before_running);
    const ImGuiWindow* menu = ImGui::FindWindowByName("##MainMenuBar");
    if (!require(menu != nullptr && menu->Active,
                 "Playback toolbar was not available to open Settings")) {
        ImGui::DestroyContext(context);
        return false;
    }
    ImGuiID id = ImHashStr("##MenuBar", 0, menu->ID);
    id = ImHashStr("Settings", 0, id);
    id = ImHashStr("", 0, id);
    ImGui::ActivateItemByID(id);
    for (int i = 0; i < 4; ++i) frame(overlay, !open_before_running);
    // Also exercise creating Running status after an already-open Settings
    // window: submission order alone must not hide its interactive controls.
    frame(overlay);
    frame(overlay);

    const ImGuiWindow* settings = ImGui::FindWindowByName("Settings##cfg");
    bool ok = require(settings != nullptr && settings->Active,
                      "Settings did not open during playback");
    if (settings != nullptr) {
        frame(overlay, true, true);
        const char* captured = ImGui::GetClipboardText();
        ok = require(captured != nullptr &&
                         std::strstr(captured, "Performance estimates describe enabled workloads") != nullptr &&
                         std::strstr(captured, "Estimated:") != nullptr,
                     "Settings did not render the estimate legend and per-control impact labels") && ok;
        const ImGuiWindow* status = ImGui::FindWindowByName("##benchmark_running");
        const ImDrawData* draws = ImGui::GetDrawData();
        int settings_order = -1;
        int status_order = -1;
        for (int i = 0; i < draws->CmdListsCount; ++i) {
            if (draws->CmdLists[i] == settings->DrawList) settings_order = i;
            if (status != nullptr && draws->CmdLists[i] == status->DrawList) status_order = i;
        }
        ok = require(status_order >= 0 && settings_order > status_order,
                     "Running status covered the interactive Settings window") && ok;
#ifdef __ANDROID__
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ok = require(settings->Pos.x >= viewport->WorkPos.x &&
                     settings->Pos.y >= viewport->WorkPos.y &&
                     settings->Rect().Max.x <= viewport->WorkPos.x + viewport->WorkSize.x &&
                     settings->Rect().Max.y <= viewport->WorkPos.y + viewport->WorkSize.y,
                     "Android Settings escaped the work area below the toolbar") && ok;
        if (scale > 1.0f) {
            ok = require(settings->Size.x > 520.0f,
                         "Android Settings retained the unscaled desktop width") && ok;
        }

        const ImGuiID bar_id = ImHashStr("##settings_tabs", 0, settings->ID);
        const ImGuiTabBar* tabs = context->TabBars.GetByKey(bar_id);
        ok = require(tabs != nullptr, "Settings tab bar was not submitted") && ok;
        if (tabs != nullptr) {
            ok = require(tabs->WidthAllTabsIdeal <= tabs->BarRect.GetWidth(),
                         "Settings tab labels were compressed or scrolled") && ok;
            constexpr std::array labels{
                "Graphics", "Display", "Environment", "Camera", "Geometry", "Audio"};
            for (const char* label : labels) {
                const ImGuiID tab_id = ImHashStr(label, 0, tabs->ID);
                ImGui::ActivateItemByID(tab_id);
                for (int i = 0; i < 3; ++i) frame(overlay);
                ok = require(tabs->VisibleTabId == tab_id,
                             "A Settings tab could not be selected") && ok;
                // ItemSize records complete label/marker/estimate extents even for
                // vertically scrolled controls. Horizontal overflow would
                // reproduce the clipped labels from the tablet screenshots.
                if (!require(settings->ContentSize.x <= settings->InnerRect.GetWidth(),
                             "A Settings label, reload marker or impact estimate overflowed horizontally")) {
                    std::printf("  tab=%s content=%.1f inner=%.1f scale=%.1f\n",
                                label, settings->ContentSize.x,
                                settings->InnerRect.GetWidth(), scale);
                    ok = false;
                }
            }
        }
#else
        ok = require(settings->Size.x == 520.0f && settings->Size.y == 640.0f,
                     "Desktop Settings default size changed") && ok;
        ImGui::SetWindowSize("Settings##cfg", ImVec2(610.0f, 500.0f));
        frame(overlay);
        frame(overlay);
        ok = require(settings->Size.x == 610.0f && settings->Size.y == 500.0f,
                     "Desktop Settings stopped remembering a user resize") && ok;
#endif
    }
    ImGui::DestroyContext(context);
    return ok;
}

bool settings_layout() {
    bool ok = layout(1.0f, ImVec2(1272.0f, 900.0f));
    ok = layout(2.5f, ImVec2(1272.0f, 900.0f)) && ok;
    ok = layout(2.5f, ImVec2(1272.0f, 900.0f), true) && ok;
    return ok;
}

const heaven::testing::Register register_settings_layout(settings_layout);

} // namespace
