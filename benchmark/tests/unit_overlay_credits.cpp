/* Real headless Credits widgets: small/scaled layouts, scrolling and controls. */
#include "rhi/overlay/overlay_ui.h"
#include "rhi/overlay/credits_catalog.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
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
    if (!condition) std::printf("[unit_overlay_credits] FAILED: %s\n", description);
    return condition;
}

heaven::OverlayIntents frame(heaven::OverlayUI& overlay, std::string* log = nullptr) {
    ImGui::NewFrame();
    if (log) ImGui::LogToBuffer();
    heaven::BenchmarkStatus status;
    status.state = heaven::BenchmarkState::Running;
    status.fade_alpha = 1.0f;
    auto result = overlay.build(status, heaven::BenchConfig{}, heaven::ConfigUiState{});
    if (log) {
        *log = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
    }
    ImGui::Render();
    return result;
}

ImGuiID menu_id(const char* label) {
    const ImGuiWindow* menu = ImGui::FindWindowByName("##MainMenuBar");
    if (!menu) return 0;
    ImGuiID id = ImHashStr("##MenuBar", 0, menu->ID);
    id = ImHashStr(label, 0, id);
    return ImHashStr("", 0, id);
}

bool layout(ImVec2 display, float scale) {
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = display;
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FontScaleMain = scale;
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires an unsigned char**.
    unsigned char* pixels = nullptr;
    int atlas_width = 0;
    int atlas_height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);

    heaven::OverlayUI overlay;
    frame(overlay);
    frame(overlay);
    bool ok = require(!overlay.credits_open(), "Credits opened without an explicit request");
    const ImGuiID credits_id = menu_id("Credits");
    ok = require(credits_id != 0, "Credits toolbar item missing") && ok;
    ImGui::ActivateItemByID(credits_id);
    const auto opened = frame(overlay);
    frame(overlay);
    ok = require(overlay.credits_open() && !opened.cancel && !opened.quit &&
                 !opened.start_benchmark && opened.edits.empty(),
                 "Credits changed benchmark/configuration state") && ok;
    ImGuiWindow* window = ImGui::FindWindowByName("Credits##credits");
    ok = require(window && window->Active, "Credits did not render") && ok;
    ok = require(context->OpenPopupStack.empty(), "Credits blocked the toolbar with a modal popup") && ok;
    if (window) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ok = require(window->Pos.x >= viewport->WorkPos.x &&
                     window->Pos.y >= viewport->WorkPos.y &&
                     window->Rect().Max.x <= viewport->WorkPos.x + viewport->WorkSize.x + 0.5f &&
                     window->Rect().Max.y <= viewport->WorkPos.y + viewport->WorkSize.y + 0.5f,
                     "Credits escaped the work area below the toolbar") && ok;
        ok = require(window->DC.ChildWindows.Size == 1, "Credits scroll region missing") && ok;
        if (window->DC.ChildWindows.Size == 1) {
            ImGuiWindow* text = window->DC.ChildWindows[0];
            ok = require(text->ScrollMax.y > 0.0f, "Long credits cannot scroll") && ok;
            ok = require(text->ContentSize.x <= text->InnerRect.GetWidth() + 1.0f,
                         "Credits text overflowed horizontally") && ok;
            // Exercise the actual child scroll range, rather than a duplicated
            // layout formula. The footer remains outside this moving region.
            ImGui::SetScrollY(text, text->ScrollMax.y);
            frame(overlay);
            ok = require(text->Scroll.y > 0.0f && text->Scroll.y <= text->ScrollMax.y,
                         "Credits scroll position did not advance") && ok;
        }
        std::string logged;
        frame(overlay, &logged);
        const char* captured = logged.c_str();
        for (const auto& section : heaven::credits::kSections) {
            ok = require(std::strstr(captured, section.title) != nullptr,
                         "A shared credits section was not rendered") && ok;
            for (const auto& entry : section.entries) {
                ok = require(std::strstr(captured, entry.name) != nullptr,
                             "A shared author/acknowledgment was not rendered") && ok;
            }
        }
        ok = require(std::strstr(captured, heaven::credits::kOriginalNotice) != nullptr,
                     "Original copyright notice missing") && ok;

        // Sweep actual pointer hover to catch toolbar items that exist but are
        // clipped beyond a narrow display; navigation activation alone cannot.
        constexpr std::array labels{"Cancel", "Settings", "Quit", "Credits"};
        std::array<bool, labels.size()> reachable{};
        const ImGuiWindow* menu = ImGui::FindWindowByName("##MainMenuBar");
        const float menu_y = menu ? menu->Pos.y + menu->Size.y * 0.5f : 1.0f;
        for (int x = 0; static_cast<float>(x) < display.x; x += 2) {
            io.AddMousePosEvent(static_cast<float>(x), menu_y);
            frame(overlay);
            for (std::size_t i = 0; i < labels.size(); ++i)
                reachable[i] = reachable[i] || context->HoveredId == menu_id(labels[i]);
        }
        for (const bool visible : reachable)
            ok = require(visible, "A primary toolbar control was clipped/unreachable") && ok;

        // A nonmodal window must leave all existing playback controls callable.
        ImGui::ActivateItemByID(menu_id("Settings"));
        const auto settings = frame(overlay);
        ok = require(settings.open_panel == heaven::OverlayIntents::Panel::Quality &&
                     overlay.credits_open(), "Settings became unavailable with Credits open") && ok;
        ImGui::ActivateItemByID(menu_id("Cancel"));
        ok = require(frame(overlay).cancel, "Cancel became unavailable with Credits open") && ok;
        ImGui::ActivateItemByID(menu_id("Quit"));
        ok = require(frame(overlay).quit, "Quit became unavailable with Credits open") && ok;

        ImGui::ActivateItemByID(window->GetID("Close##credits_close"));
        frame(overlay);
        ok = require(!overlay.credits_open(), "Close button did not dismiss Credits") && ok;
        ImGui::ActivateItemByID(menu_id("Credits"));
        frame(overlay);
        ok = require(overlay.dismiss_credits() && !overlay.credits_open() &&
                     !overlay.dismiss_credits(), "Escape dismissal was not consumed exactly once") && ok;
    }
    ImGui::DestroyContext(context);
    return ok;
}

bool credits() {
    static_assert(heaven::credits::kContent.size() + heaven::credits::kTechnology.size() +
        heaven::credits::kQualityAssurance.size() + heaven::credits::kProduction.size() +
        heaven::credits::kThanks.size() == 21);
    bool ok = true;
    for (const auto& [display, scale] : std::array{
            std::pair{ImVec2(1280.0f, 720.0f), 1.0f},
            std::pair{ImVec2(640.0f, 360.0f), 1.5f},
            std::pair{ImVec2(360.0f, 640.0f), 1.0f},
            std::pair{ImVec2(768.0f, 1024.0f), 2.5f},
            std::pair{ImVec2(1272.0f, 900.0f), 2.5f}}) {
        ok = layout(display, scale) && ok;
    }
    if (ok) std::printf("[unit_overlay_credits] passed\n");
    return ok;
}

const heaven::testing::Register register_credits(credits);

} // namespace
