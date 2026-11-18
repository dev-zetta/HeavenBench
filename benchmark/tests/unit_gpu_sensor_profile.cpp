#include "benchmark/gpu_sensor_profile.h"
#include "benchmark/gpu_sensor_telemetry.h"
#include "benchmark/microprofile_report.h"
#include "benchmark/microprofiler.h"
#include "rhi/overlay/microprofiler_ui.h"

#include "json.hpp"
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}
namespace {
using namespace heaven::profile;
bool require(bool condition, const char* message) {
    if (!condition) std::printf("[unit_gpu_sensor_profile] FAILED: %s\n", message);
    return condition;
}
const Counter* counter(const Snapshot& snapshot, std::string_view name) {
    const auto found = std::find_if(snapshot.counters.rbegin(), snapshot.counters.rend(),
        [&](const auto& item) { return item.name == name; });
    return found == snapshot.counters.rend() ? nullptr : &*found;
}
bool value_is(const Snapshot& snapshot, std::string_view name, double expected) {
    const auto* item = counter(snapshot, name);
    return item && item->value == expected;
}
Recorder make_recorder() { return Recorder({10'000'000'000ull, std::size_t{2} * 1024u * 1024u, 2}); }

bool disabled_and_missing() {
    auto recorder = make_recorder();
    GpuSensorReading reading;
    reading.reason = "Sensor unsupported";
    record_gpu_sensor_sample(recorder, reading);
    const auto disabled = recorder.snapshot();
    if (!require(disabled.counters.empty() && disabled.metadata.empty() && disabled.memory_bytes == 0,
        "disabled bridge recorded or allocated recorder storage")) return false;
    recorder.begin_capture();
    record_gpu_sensor_sample(recorder, reading);
    recorder.set_active(false);
    const auto missing = recorder.snapshot();
    return require(missing.counters.size() == 18 &&
        std::ranges::all_of(missing.counters, [](const auto& item) {
            return item.name.ends_with(" available") && item.value == 0.0;
        }) && !counter(missing, "GPU throttled") && !counter(missing, "GPU fan RPM") &&
        !counter(missing, "GPU power W") && missing.metadata.at("gpu_sensor_throttle_reason") == "Unavailable",
        "missing sensors became measured zero or unsupported throttle became no throttle");
}

bool measurements_context_and_export() {
    auto recorder = make_recorder();
    recorder.begin_capture();
    recorder.set_thread_name("Main");
    const Context frame{11, 22, 33, 44, 55, recorder.session_id()};
    recorder.set_frame_context(frame);
    GpuSensorReading reading;
    reading.available = true;
    reading.temperature_c = 75.5;
    reading.vram_temperature_c = 82.0;
    reading.temperature_high_c = 80.0;
    reading.temperature_critical_c = 90.0;
    reading.vram_temperature_high_c = 90.0;
    reading.vram_temperature_critical_c = 100.0;
    reading.power_w = 132.25;
    reading.power_limit_w = 190.0;
    reading.core_clock_mhz = 2450.0;
    reading.memory_clock_mhz = 2375.0;
    reading.fan_rpm = 0.0;
    reading.utilization_percent = 99.0;
    reading.memory_utilization_percent = 66.0;
    reading.device_vram_used_mb = 6144.0;
    reading.device_vram_total_mb = 12288.0;
    reading.sample_monotonic_ns = Recorder::steady_now_ns();
    reading.reason = "Host sensor </script> & test";
    reading.throttle_reason = "None";
    record_gpu_sensor_sample(recorder, reading);
    if (!require(recorder.frame_context() == frame, "host import changed caller frame context")) return false;
    recorder.set_active(false);
    const auto snapshot = recorder.snapshot();
    const std::array expected{
        std::pair{"GPU temperature C", 75.5}, std::pair{"GPU VRAM temperature C", 82.0},
        std::pair{"GPU temperature high C", 80.0}, std::pair{"GPU temperature critical C", 90.0},
        std::pair{"GPU VRAM temperature high C", 90.0}, std::pair{"GPU VRAM temperature critical C", 100.0},
        std::pair{"GPU power W", 132.25}, std::pair{"GPU power limit W", 190.0},
        std::pair{"GPU graphics clock MHz", 2450.0}, std::pair{"GPU memory clock MHz", 2375.0},
        std::pair{"GPU fan RPM", 0.0}, std::pair{"GPU utilization percent", 99.0},
        std::pair{"GPU memory utilization percent", 66.0}, std::pair{"GPU device VRAM used MiB", 6144.0},
        std::pair{"GPU device VRAM total MiB", 12288.0}};
    if (!require(std::ranges::all_of(expected, [&](const auto& item) {
        return value_is(snapshot, item.first, item.second);
    }) && value_is(snapshot, "GPU throttle status available", 1.0) &&
        value_is(snapshot, "GPU throttled", 0.0) && value_is(snapshot, "GPU fan available", 1.0) &&
        value_is(snapshot, "GPU sensor sample age available", 1.0) &&
        counter(snapshot, "GPU sensor sample age ms") && counter(snapshot, "GPU sensor sample age ms")->value >= 0.0,
        "units, threshold values, actual power, cap or measured zero fan were lost")) return false;
    const Context host{0, 0, 0, 0, 0, recorder.session_id()};
    if (!require(std::ranges::all_of(snapshot.counters, [&](const auto& item) {
        return item.context == host && item.category == "Host GPU telemetry" && std::isfinite(item.value);
    }) && snapshot.gpu_samples.empty(), "sensors inherited GPU/frame identities or fabricated GPU samples")) return false;

    const auto json = nlohmann::json::parse(snapshot_json(snapshot));
    const auto html = snapshot_html(snapshot);
    constexpr std::string_view marker = R"(<script id="data" type="application/json">)";
    const auto begin = html.find(marker);
    const auto end = html.find("</script>", begin + marker.size());
    if (!require(begin != std::string::npos && end != std::string::npos,
        "HTML embedded data block is absent")) return false;
    const auto embedded = nlohmann::json::parse(html.substr(begin + marker.size(), end - begin - marker.size()));
    const auto parsed = parse_snapshot(snapshot_json(snapshot));
    return require(embedded == json && parsed && parsed->counters.size() == snapshot.counters.size() &&
        parsed->metadata == snapshot.metadata && html.find("\\u003c/script\\u003e") != std::string::npos &&
        snapshot.metadata.at("gpu_sensor_sample_monotonic_ns") == std::to_string(reading.sample_monotonic_ns),
        "JSON/HTML counters or exact host sample timestamp differ");
}

bool invalid_samples_and_state_changes() {
    auto recorder = make_recorder();
    recorder.begin_capture();
    GpuSensorReading invalid;
    invalid.temperature_c = std::numeric_limits<double>::quiet_NaN();
    invalid.power_w = std::numeric_limits<double>::infinity();
    invalid.power_limit_w = -std::numeric_limits<double>::infinity();
    invalid.fan_rpm = -2.0;
    invalid.sample_monotonic_ns = std::numeric_limits<std::uint64_t>::max();
    invalid.reason = std::string(500, 'x');
    invalid.throttle_reason = "power limit 1, thermal";
    record_gpu_sensor_sample(recorder, invalid);
    auto snapshot = recorder.snapshot();
    if (!require(!counter(snapshot, "GPU temperature C") && !counter(snapshot, "GPU power W") &&
        !counter(snapshot, "GPU power limit W") && !counter(snapshot, "GPU fan RPM") &&
        value_is(snapshot, "GPU temperature available", 0.0) && value_is(snapshot, "GPU power available", 0.0) &&
        value_is(snapshot, "GPU fan available", 0.0) && value_is(snapshot, "GPU sensor sample age available", 0.0) &&
        !counter(snapshot, "GPU sensor sample age ms") && value_is(snapshot, "GPU throttled", 1.0) &&
        snapshot.dropped_events == 0 && snapshot.metadata.at("gpu_sensor_reason").size() <= 256 &&
        snapshot.metadata.at("gpu_sensor_reason").ends_with(" [truncated]"),
        "nonfinite/negative values or future timestamps were imported as measurements")) return false;
    invalid.reason = "Recovered";
    invalid.throttle_reason = "None";
    record_gpu_sensor_sample(recorder, invalid);
    invalid.throttle_reason.clear();
    record_gpu_sensor_sample(recorder, invalid);
    recorder.set_active(false);
    snapshot = recorder.snapshot();
    return require(snapshot.metadata.size() == 4 && snapshot.metadata.at("gpu_sensor_reason") == "Recovered" &&
        snapshot.metadata.at("gpu_sensor_throttle_reason") == "Unavailable" &&
        value_is(snapshot, "GPU throttle status available", 0.0) &&
        std::ranges::count_if(snapshot.counters, [](const auto& item) { return item.name == "GPU throttled"; }) == 2,
        "state changes grew metadata or unavailable throttling emitted a false clear status");
}

bool live_sensor_visibility() {
    struct ImGuiCleanup { ~ImGuiCleanup() { ImGui::DestroyContext(); } } cleanup;
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280.0f, 1800.0f);
    io.DeltaTime = 1.0f / 60.0f;
    // NOLINTNEXTLINE(misc-const-correctness) -- ImGui requires unsigned char**.
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->session_id = 1;
    snapshot->end_ns = 1'000'000'000ull;
    snapshot->counters = {
        {1, 100, "GPU sensors available", "Host GPU telemetry", 1.0, {}},
        {1, 110, "GPU temperature C", "Host GPU telemetry", 70.0, {}},
        {1, 200, "GPU sensors available", "Host GPU telemetry", 1.0, {}},
        {1, 210, "GPU temperature available", "Host GPU telemetry", 0.0, {}},
        {1, 220, "GPU fan available", "Host GPU telemetry", 1.0, {}},
        {1, 230, "GPU fan RPM", "Host GPU telemetry", 0.0, {}}};
    for (std::uint64_t i = 0; i < 100; ++i)
        snapshot->counters.push_back({1, 1000 + i, "Frame workload", "Counter", 1.0, {}});
    heaven::MicroprofilerUI ui;
    ui.set_snapshot(snapshot);
    ui.set_open(true);
    for (int i = 0; i < 2; ++i) {
        ImGui::NewFrame();
        static_cast<void>(ui.draw());
        ImGui::Render();
    }
    auto* window = ImGui::FindWindowByName("Microprofiler");
    if (!require(window != nullptr, "profiler widget did not open")) return false;
    window->StateStorage.SetInt(ImHashStr("Host GPU telemetry", 0, window->ID), 1);
    ImGui::NewFrame();
    ImGui::LogToBuffer(10);
    static_cast<void>(ui.draw());
    const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::Render();
    return require(text.find("GPU fan RPM: 0") != std::string::npos &&
        text.find("GPU temperature available: Unavailable") != std::string::npos &&
        text.find("GPU temperature C: 70") == std::string::npos &&
        text.find("not GPU-aligned or per-frame") != std::string::npos,
        "low-rate sensors disappeared behind frame counters or stale values remained visible");
}

const heaven::testing::Register register_disabled{disabled_and_missing};
const heaven::testing::Register register_measurements{measurements_context_and_export};
const heaven::testing::Register register_invalid{invalid_samples_and_state_changes};
const heaven::testing::Register register_widget{live_sensor_visibility};
} // namespace
