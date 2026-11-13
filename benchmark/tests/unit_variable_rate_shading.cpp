/* GPU-free variable-rate shading identities, persistence and live edit routing. */
#include "engine/variable_rate_shading.h"
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/camera.h"
#include "benchmark/benchmark_system.h"
#include "rhi/overlay/config_controller.h"
#include "config_controller_fakes.h"
#include "json.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
using namespace heaven;

constexpr std::array kModes = {VariableRateShadingMode::Off, VariableRateShadingMode::Rate2x1,
                               VariableRateShadingMode::Rate2x2};

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[unit_variable_rate_shading] %s\n", message);
    return condition;
}

class TemporaryConfig {
public:
    TemporaryConfig() {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() /
            ("heaven-vrs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(sequence.fetch_add(1)) + ".json");
    }
    ~TemporaryConfig() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
    TemporaryConfig(const TemporaryConfig&) = delete;
    TemporaryConfig& operator=(const TemporaryConfig&) = delete;
    [[nodiscard]] std::string path() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

bool variable_rate_shading_modes() {
    constexpr std::array<std::string_view, 3> expected = {"off", "2x1", "2x2"};
    for (std::size_t i = 0; i < kModes.size(); ++i) {
        if (!check(variable_rate_shading_mode_id(kModes[i]) == expected[i] &&
                   parse_variable_rate_shading_mode(expected[i]) == kModes[i],
                   "mode identity is not canonical")) return false;
    }
    for (const auto invalid : {"", "auto", "1x1", "2X1", "4x4", "2x2-extra"}) {
        if (!check(!parse_variable_rate_shading_mode(invalid), "invalid mode identity was accepted")) return false;
    }
    const VariableRateShadingStatus status;
    return check(status.requested == VariableRateShadingMode::Off &&
                 status.effective == VariableRateShadingMode::Off &&
                 !status.supported_2x1 && !status.supported_2x2 && !status.reason.empty(),
                 "default status claims supported or active coarse shading");
}

bool variable_rate_shading_cli() {
    if (!check(parse_cli(std::array<std::string_view, 1>{"heaven"}).config.variable_rate_shading ==
                   VariableRateShadingMode::Off, "CLI default is not full-rate shading")) return false;
    for (const auto mode : kModes) {
        const auto parsed = parse_cli(std::array<std::string_view, 3>{
            "heaven", "--vrs", variable_rate_shading_mode_id(mode)});
        if (!check(parsed.diagnostics.empty() && parsed.config.variable_rate_shading == mode &&
                   parsed.seen_keys.contains("variable_rate_shading"),
                   "valid CLI mode was lost in configuration precedence")) return false;
    }
    const auto last = parse_cli(std::array<std::string_view, 5>{"heaven", "--vrs", "2x2", "--vrs", "off"});
    if (!check(last.diagnostics.empty() && last.config.variable_rate_shading == VariableRateShadingMode::Off,
               "later valid mode did not win")) return false;
    for (const auto invalid : {"", "auto", "2X2", "4x4"}) {
        const auto parsed = parse_cli(std::array<std::string_view, 5>{
            "heaven", "--vrs", "2x1", "--vrs", invalid});
        if (!check(!parsed.diagnostics.empty() && parsed.config.variable_rate_shading == VariableRateShadingMode::Rate2x1,
                   "invalid CLI value erased the previous request")) return false;
    }
    BenchConfig saved;
    saved.variable_rate_shading = VariableRateShadingMode::Rate2x2;
    apply_cli_overlay(saved, parse_cli(std::array<std::string_view, 3>{"heaven", "--vrs", "auto"}));
    if (!check(saved.variable_rate_shading == VariableRateShadingMode::Rate2x2,
               "invalid CLI overlay replaced the saved configuration")) return false;
    return check(generate_help().find("--vrs") != std::string::npos, "help omits the VRS request");
}

bool variable_rate_shading_persistence() {
    const TemporaryConfig file;
    for (const auto mode : kModes) {
        BenchConfig config;
        config.variable_rate_shading = mode;
        const auto json = nlohmann::json::parse(serialize_config(config));
        if (!check(json.at("variable_rate_shading").get_ref<const std::string&>() == variable_rate_shading_mode_id(mode),
                   "persisted mode is not canonical")) return false;
        const auto write_diagnostics = write_config_file(file.path(), config);
        const auto loaded = load_config_file(file.path(), BenchConfig{});
        if (!check(write_diagnostics.empty() && loaded.parsed_ok && loaded.diagnostics.empty() &&
                   loaded.config.variable_rate_shading == mode,
                   "valid mode did not survive writing and loading")) return false;
    }
    for (const auto malformed : {R"({"variable_rate_shading":"4x4"})", R"({"variable_rate_shading":2})"}) {
        {
            std::ofstream stream(file.path(), std::ios::trunc);
            stream << malformed;
            if (!check(stream.good(), "malformed-input fixture could not be written")) return false;
        }
        BenchConfig base;
        base.variable_rate_shading = VariableRateShadingMode::Rate2x1;
        const auto loaded = load_config_file(file.path(), base);
        if (!check(loaded.parsed_ok && !loaded.diagnostics.empty() &&
                   loaded.config.variable_rate_shading == base.variable_rate_shading,
                   "unknown or mistyped saved mode replaced the existing configuration")) return false;
    }
    return true;
}

bool variable_rate_shading_catalog() {
    const auto& entry = catalog_entry(SettingId::VariableRateShading);
    if (!check(entry.panel == Panel::Quality && entry.apply == Apply::Live && entry.widget == Widget::Combo &&
               catalog_section(entry.id) == "Image quality - high GPU impact", "VRS is not a live image-quality setting")) return false;
    BenchConfig config;
    for (const auto mode : kModes) {
        if (!check(entry.apply_to(config, SettingValue{mode}) == SettingValue{mode} &&
                   entry.read(config) == SettingValue{mode} && config.variable_rate_shading == mode,
                   "catalog did not apply a valid mode")) return false;
    }
    const auto invalid_mode = std::bit_cast<VariableRateShadingMode>(std::uint8_t{255});
    for (const auto& invalid : {SettingValue{invalid_mode}, SettingValue{2}}) {
        if (!check(entry.apply_to(config, invalid) == SettingValue{VariableRateShadingMode::Rate2x2} &&
                   config.variable_rate_shading == VariableRateShadingMode::Rate2x2,
                   "invalid catalog value replaced the valid mode")) return false;
    }
    const auto impact = catalog_performance_impact(entry.id);
    if (!check(impact.has_value(), "VRS lacks performance guidance")) return false;
    if (impact) {
        if (!check(impact->estimate == "GPU: May reduce fragment work" &&
                   impact->explanation.find("opaque lit main-color") != std::string_view::npos &&
                   impact->explanation.find("quality") != std::string_view::npos &&
                   impact->explanation.find("sample count") != std::string_view::npos,
                   "guidance omits workload scope, quality or capability limits")) return false;
    }
    BenchConfig working;
    BenchConfig active;
    ConfigUiState ui;
    OverlayIntents intents;
    intents.edits.push_back({SettingId::VariableRateShading, VariableRateShadingMode::Rate2x1});
    heaven::testing::FakeRendererEffects effects;
    const auto operations = apply_config_intents(working, active, ui, intents, effects);
    return check(operations == ConfigOpLog{ConfigOp::Edit} &&
                 working.variable_rate_shading == VariableRateShadingMode::Rate2x1 &&
                 active.variable_rate_shading == VariableRateShadingMode::Rate2x1 &&
                 effects.cancel_count == 1 && effects.rebuild_count == 0 && effects.feature_calls.empty() &&
                 effects.live_apply_count(SettingId::VariableRateShading) == 1 &&
                 effects.live_applies.front().active.variable_rate_shading == VariableRateShadingMode::Rate2x1 &&
                 !is_dirty(working, active), "live VRS edit bypassed run cancellation or runtime dispatch");
}

bool variable_rate_shading_completed_snapshot() {
    CameraPath path;
    CameraKeyframe first{};
    first.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    first.fov = 60.0f;
    auto last = first;
    last.time = 1.0f;
    path.keyframes = {first, last};
    path.duration = last.time;
    SceneMarkers markers;
    markers.times = {0.0f};
    BenchmarkSystem benchmark(&markers, &path, [] { return true; });
    Camera camera;
    VariableRateShadingStatus status;
    status.requested = status.effective = VariableRateShadingMode::Rate2x1;
    status.supported_2x1 = true;
    status.reason = "2x1 active";
    benchmark.set_variable_rate_shading_status(status);
    if (!benchmark.request_start()) return false;
    benchmark.tick(0.5f, camera);
    status.requested = VariableRateShadingMode::Rate2x2;
    status.effective = VariableRateShadingMode::Off;
    status.reason = "2x2 unavailable with current sample count";
    benchmark.set_variable_rate_shading_status(status);
    benchmark.tick(0.5f, camera);
    const auto completed = benchmark.status();
    if (!check(completed.has_result && completed.state == BenchmarkState::Complete &&
               !completed.diagnostic && !completed.result.diagnostic &&
               completed.result.variable_rate_shading.requested == status.requested &&
               completed.result.variable_rate_shading.effective == status.effective &&
               completed.result.variable_rate_shading.supported_2x1 &&
               !completed.result.variable_rate_shading.supported_2x2 &&
               completed.result.variable_rate_shading.reason == status.reason,
               "completed run did not snapshot the latest VRS capability without changing score policy")) return false;
    benchmark.set_variable_rate_shading_status(VariableRateShadingStatus{});
    const auto retained = benchmark.status().result.variable_rate_shading;
    return check(retained.requested == status.requested && retained.effective == status.effective &&
                 retained.supported_2x1 && !retained.supported_2x2 && retained.reason == status.reason,
                 "later renderer configuration rewrote the completed VRS snapshot");
}

const heaven::testing::Register modes{&variable_rate_shading_modes};
const heaven::testing::Register cli{&variable_rate_shading_cli};
const heaven::testing::Register persistence{&variable_rate_shading_persistence};
const heaven::testing::Register catalog{&variable_rate_shading_catalog};
const heaven::testing::Register snapshot{&variable_rate_shading_completed_snapshot};
} // namespace
