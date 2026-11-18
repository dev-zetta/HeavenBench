#include "benchmark/gpu_thermal_warning.h"
#include "rhi/overlay/overlay_ui.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
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
using heaven::GpuThermalSample;
using heaven::GpuThermalWarning;
using heaven::ThermalSeverity;

bool expect(bool condition, const char* description) {
    if (!condition) std::printf("[unit_gpu_thermal_warning] FAILED: %s\n", description);
    return condition;
}

GpuThermalSample sample(std::uint64_t id, double gpu_c, double vram_c = -1.0) {
    GpuThermalSample result;
    result.gpu_c = gpu_c;
    result.vram_c = vram_c;
    result.age_seconds = 0.0;
    result.sample_monotonic_ns = id;
    return result;
}

bool unit_gpu_thermal_warning_policy() {
    bool ok = true;
    GpuThermalWarning warning;
    ok &= expect(warning.update({}).severity == ThermalSeverity::Normal,
                 "missing telemetry cannot create a warning");
    ok &= expect(warning.update(sample(1, 79.9)).severity == ThermalSeverity::Normal,
                 "GPU below 80 Celsius is normal");
    ok &= expect(warning.update(sample(2, 80.0)).severity == ThermalSeverity::Normal,
                 "one high sample does not create an advisory");
    for (int frame = 0; frame < 20; ++frame) {
        ok &= expect(warning.update(sample(2, 80.0)).severity == ThermalSeverity::Normal,
                     "drawing one cached sample cannot advance persistence");
    }
    ok &= expect(warning.update(sample(1, 100.0)).severity == ThermalSeverity::Normal,
                 "an out-of-order sample cannot escalate the warning");
    ok &= expect(warning.update(sample(3, 80.1)).severity == ThermalSeverity::High,
                 "two distinct consecutive high samples create High");
    ok &= expect(warning.update(sample(4, 77.0)).severity == ThermalSeverity::High,
                 "High retains its three-Celsius hysteresis boundary");
    ok &= expect(warning.update(sample(5, 76.9)).severity == ThermalSeverity::Normal,
                 "High clears below its hysteresis boundary");
    ok &= expect(warning.update(sample(6, 80.0)).severity == ThermalSeverity::Normal,
                 "cooled warning must establish persistence again");
    ok &= expect(warning.update(sample(7, 79.9)).severity == ThermalSeverity::Normal,
                 "a cool sample breaks pending High persistence");
    ok &= expect(warning.update(sample(8, 80.0)).severity == ThermalSeverity::Normal,
                 "flapping temperatures cannot accumulate separated high samples");
    ok &= expect(warning.update(sample(9, 90.0)).severity == ThermalSeverity::Extreme,
                 "Extreme at 90 Celsius is immediate");
    ok &= expect(warning.update(sample(10, 87.0)).severity == ThermalSeverity::Extreme,
                 "Extreme retains its three-Celsius hysteresis boundary");
    ok &= expect(warning.update(sample(11, 86.9)).severity == ThermalSeverity::High,
                 "Extreme cools to High before clearing");

    auto stale = sample(11, 100.0);
    stale.age_seconds = 5.01;
    ok &= expect(warning.update(stale).severity == ThermalSeverity::Normal,
                 "stale cached sample clears an existing warning");
    auto fresh = sample(12, 90.0);
    fresh.age_seconds = 5.0;
    ok &= expect(warning.update(fresh).severity == ThermalSeverity::Extreme,
                 "five-second freshness boundary remains usable");
    auto missing = sample(13, -1.0);
    ok &= expect(warning.update(missing).severity == ThermalSeverity::Normal,
                 "loss of all temperature sensors clears a warning");
    ok &= expect(warning.update(sample(14, std::numeric_limits<double>::quiet_NaN())).severity == ThermalSeverity::Normal,
                 "NaN is unavailable rather than a usable temperature");
    ok &= expect(warning.update(sample(15, 90000.0)).severity == ThermalSeverity::Normal,
                 "millidegrees cannot be mistaken for Celsius");
    auto invalid_age = sample(16, 100.0);
    invalid_age.age_seconds = -1.0;
    ok &= expect(warning.update(invalid_age).severity == ThermalSeverity::Normal,
                 "unknown sample age cannot create an alert");
    invalid_age.age_seconds = std::numeric_limits<double>::infinity();
    ok &= expect(warning.update(invalid_age).severity == ThermalSeverity::Normal,
                 "nonfinite age cannot create an alert");
    ok &= expect(warning.update(sample(0, 100.0)).severity == ThermalSeverity::Normal,
                 "missing sample identity cannot create an alert");

    auto both = warning.update(sample(17, 95.0, 95.0));
    ok &= expect(both.gpu == ThermalSeverity::Extreme && both.vram == ThermalSeverity::Normal,
                 "GPU and VRAM track persistence independently");
    both = warning.update(sample(18, -1.0, 95.0));
    ok &= expect(both.gpu == ThermalSeverity::Normal && both.vram == ThermalSeverity::High &&
                 both.severity == ThermalSeverity::High,
                 "one missing sensor clears only its own alert");
    both = warning.update(sample(19, -1.0, 100.0));
    ok &= expect(both.vram == ThermalSeverity::Extreme,
                 "VRAM has an independent 100-Celsius Extreme boundary");
    warning.reset();
    ok &= expect(warning.update(sample(20, 80.0, 90.0)).severity == ThermalSeverity::Normal,
                 "explicit reset clears both persistence histories");
    return ok;
}

bool unit_gpu_thermal_warning_limits() {
    bool ok = true;
    GpuThermalWarning warning;
    auto reading = sample(1, 70.0, 70.0);
    auto status = warning.update(reading);
    ok &= expect(status.gpu_thresholds.high_c == 80.0 && status.gpu_thresholds.extreme_c == 90.0 &&
                 status.vram_thresholds.high_c == 90.0 && status.vram_thresholds.extreme_c == 100.0,
                 "documented GPU and VRAM advisory defaults use Celsius");
    reading.sample_monotonic_ns++;
    reading.gpu_high_c = 60.0;
    reading.gpu_critical_c = 100.0;
    status = warning.update(reading);
    ok &= expect(status.gpu_thresholds.high_c == 80.0 && status.gpu_thresholds.extreme_c == 90.0,
                 "xe pkg maximum of 60 C is ignored and critical 100 C cannot raise defaults");
    reading.sample_monotonic_ns++;
    reading.gpu_high_c = 110.0;
    reading.gpu_critical_c = 85.0;
    reading.vram_high_c = 85.0;
    reading.vram_critical_c = 95.0;
    status = warning.update(reading);
    ok &= expect(status.gpu_thresholds.high_c == 75.0 && status.gpu_thresholds.extreme_c == 85.0,
                 "sane lower critical limit supplies ten-Celsius warning headroom");
    ok &= expect(status.vram_thresholds.high_c == 85.0 && status.vram_thresholds.extreme_c == 95.0,
                 "sane lower VRAM limits lower advisory thresholds");
    reading.sample_monotonic_ns++;
    reading.gpu_high_c = 70.0;
    reading.gpu_critical_c = std::numeric_limits<double>::quiet_NaN();
    reading.vram_high_c = 110.0;
    reading.vram_critical_c = 125.0;
    status = warning.update(reading);
    ok &= expect(status.gpu_thresholds.high_c == 70.0 && status.gpu_thresholds.extreme_c == 90.0,
                 "plausible independent warning limit can lower High");
    ok &= expect(status.vram_thresholds.high_c == 90.0 && status.vram_thresholds.extreme_c == 100.0,
                 "higher VRAM sensor limits cannot raise advisory ceilings");
    reading.sample_monotonic_ns++;
    reading.gpu_high_c = 60000.0;
    reading.gpu_critical_c = 100000.0;
    status = warning.update(reading);
    ok &= expect(status.gpu_thresholds.high_c == 80.0 && status.gpu_thresholds.extreme_c == 90.0,
                 "unconverted driver millidegrees are rejected");
    return ok;
}

bool unit_gpu_thermal_warning_overlay() {
    bool ok = true;
    IMGUI_CHECKVERSION();
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires an unsigned char** output.
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    heaven::OverlayUI overlay;
    heaven::BenchmarkStatus running;
    running.state = heaven::BenchmarkState::Running;
    heaven::DebugSidebarInfo info;
    info.gpu_temperature_c = 95.0;
    info.gpu_vram_temperature_c = 102.0;
    info.gpu_telemetry_sample_age_seconds = 0.0;
    info.gpu_telemetry_sample_monotonic_ns = 1;

    auto frame = [&](bool capture) {
        ImGui::NewFrame();
        if (capture) ImGui::LogToClipboard();
        const auto intents = overlay.build(running, heaven::BenchConfig{}, heaven::ConfigUiState{});
        // Deliberately do not draw the diagnostics sidebar: the advisory must
        // remain visible with that independent panel closed.
        overlay.draw_thermal_warning(info);
        if (capture) ImGui::LogFinish();
        ImGui::Render();
        return intents;
    };
    frame(false);
    const auto intents = frame(true);
    const ImGuiWindow* window = ImGui::FindWindowByName("GPU temperature advisory");
    ok &= expect(window != nullptr && window->Active,
                 "thermal warning is visible without the diagnostics sidebar");
    if (window != nullptr) {
        ok &= expect((window->Flags & ImGuiWindowFlags_NoInputs) == ImGuiWindowFlags_NoInputs,
                     "corner advisory cannot intercept desktop or touch controls");
        ok &= expect(window->Pos.x >= 0.0f && window->Pos.y > ImGui::GetFrameHeight() &&
                     window->Pos.x + window->Size.x <= 1280.0f &&
                     window->Pos.y + window->Size.y <= 720.0f,
                     "desktop advisory stays inside the viewport below the toolbar");
    }
    const char* text = ImGui::GetClipboardText();
    ok &= expect(text != nullptr && std::strstr(text, "Extreme GPU temperature") != nullptr &&
                 std::strstr(text, "GPU 95.0 C") != nullptr &&
                 std::strstr(text, "VRAM 102.0 C") != nullptr &&
                 std::strstr(text, "Stop benchmark and check cooling") != nullptr,
                 "Extreme advisory identifies temperatures and the cooling action");
    ok &= expect(text != nullptr && std::strstr(text, "Cancel") != nullptr &&
                 std::strstr(text, "Settings") != nullptr && std::strstr(text, "Quit") != nullptr,
                 "running controls remain present during a thermal advisory");
    ok &= expect(!intents.cancel && !intents.quit,
                 "an advisory does not automatically cancel or quit");

    io.DisplaySize = ImVec2(360.0f, 640.0f);
    frame(false);
    frame(false);
    window = ImGui::FindWindowByName("GPU temperature advisory");
    ok &= expect(window != nullptr && window->Active && window->Size.x <= 340.0f &&
                 window->Pos.x >= 0.0f && window->Pos.y > ImGui::GetFrameHeight() &&
                 window->Pos.x + window->Size.x <= 360.0f &&
                 window->Pos.y + window->Size.y <= 640.0f,
                 "small touch-sized viewport retains an unobstructed top toolbar");
    info.gpu_telemetry_sample_age_seconds = 5.1;
    frame(false);
    window = ImGui::FindWindowByName("GPU temperature advisory");
    ok &= expect(window != nullptr && !window->Active,
                 "stale telemetry removes the corner warning");
    info.gpu_temperature_c = -1.0;
    info.gpu_vram_temperature_c = 92.0;
    info.gpu_telemetry_sample_age_seconds = 0.0;
    info.gpu_telemetry_sample_monotonic_ns = 2;
    frame(false);
    window = ImGui::FindWindowByName("GPU temperature advisory");
    ok &= expect(window != nullptr && !window->Active,
                 "one fresh high VRAM sample does not immediately restore the warning");
    info.gpu_telemetry_sample_monotonic_ns = 3;
    frame(true);
    text = ImGui::GetClipboardText();
    ok &= expect(text != nullptr && std::strstr(text, "High VRAM temperature") != nullptr &&
                 std::strstr(text, "GPU N/A") != nullptr &&
                 std::strstr(text, "VRAM 92.0 C") != nullptr &&
                 std::strstr(text, "Check cooling") != nullptr &&
                 std::strstr(text, "Stop benchmark") == nullptr,
                 "High advisory identifies VRAM and preserves an unavailable GPU temperature");
    ImGui::DestroyContext(context);
    return ok;
}

const heaven::testing::Register register_thermal_policy{unit_gpu_thermal_warning_policy};
const heaven::testing::Register register_thermal_limits{unit_gpu_thermal_warning_limits};
const heaven::testing::Register register_thermal_overlay{unit_gpu_thermal_warning_overlay};
} // namespace
