#include "rhi/overlay/overlay_ui.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>
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

bool unit_driver_notice() {
    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 1200.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires unsigned char**.
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    bool ok = true;
    const auto expect = [&ok](bool condition, const char* message) {
        if (!condition) {
            std::printf("[unit_driver_notice] FAILED: %s\n", message);
            ok = false;
        }
    };
    heaven::OverlayUI overlay;
    heaven::BenchmarkStatus status;
    status.state = heaven::BenchmarkState::Running;
    heaven::DebugSidebarInfo sidebar;
    sidebar.gpu_name = "Selected GPU";
    sidebar.gpu_driver_version = "23.3.6";
    const auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        const auto intents = overlay.build(status, {}, {});
        overlay.draw_debug_sidebar(sidebar);
        const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        expect(!intents.cancel && !intents.quit,
               "driver advice must not cancel a running benchmark");
        ImGui::Render();
        return text;
    };
    const auto visible = [] {
        const ImGuiWindow* window = ImGui::FindWindowByName("Graphics driver");
        return window && window->Active;
    };

    heaven::DriverCheck check;
    check.severity = heaven::DriverCheckSeverity::Warning;
    check.summary = "Driver predates the recommended baseline";
    check.recommendation = "Upgrade the compatible distribution driver package";
    check.help_url = "https://docs.mesa3d.org/";
    overlay.set_driver_check(check, "Selected GPU | Vulkan | Mesa 23.3.6");
    frame();
    const std::string text = frame();
    expect(visible(), "startup warning must be visible during automated runs");
    expect(text.contains(check.summary) && text.contains(check.recommendation) &&
               text.contains(check.help_url) && text.contains("Selected GPU"),
           "notice must show detected adapter, reason and upgrade instructions");
    expect(ImGui::GetCurrentContext()->OpenPopupStack.empty(),
           "driver warning must be non-modal");

    ImGuiWindow* notice = ImGui::FindWindowByName("Graphics driver");
    if (notice) ImGui::ActivateItemByID(notice->GetID("Dismiss"));
    frame();
    frame();
    expect(!visible(), "dismissed notice must stay closed on following frames");

    ImGuiWindow* diagnostics = ImGui::FindWindowByName("Diagnostics Sidebar");
    if (diagnostics) {
        ImGui::ActivateItemByID(diagnostics->GetID("Driver details"));
    }
    frame();
    frame();
    expect(visible(), "Diagnostics must reopen dismissed driver guidance");

    check.severity = heaven::DriverCheckSeverity::Information;
    check.summary = "No matching driver warnings";
    overlay.set_driver_check(check, "Selected GPU | Vulkan | Mesa 26.0.8");
    const std::string informational = frame();
    expect(!visible(), "informational status must not open a startup warning");
    expect(informational.contains(check.summary),
           "informational status must remain available in Diagnostics");

    ImGui::DestroyContext(context);
    return ok;
}

const heaven::testing::Register reg_unit_driver_notice{unit_driver_notice};

} // namespace
