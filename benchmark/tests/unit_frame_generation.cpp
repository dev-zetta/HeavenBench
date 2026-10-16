/* Frame-generation event accounting, configuration and diagnostic tour contract. */
#include "engine/frame_generation.h"
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/camera.h"
#include "benchmark/benchmark_system.h"
#include "benchmark/result_report.h"
#include "rhi/overlay/config_controller.h"
#include "json.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
}

namespace {
using namespace heaven;

bool check(bool condition, const char* message) {
    if (!condition) std::printf("[unit_frame_generation] %s\n", message);
    return condition;
}

bool frame_generation_accounting() {
    FrameGenerationState state;
    const auto initial = state.status();
    if (!check(initial.requested == FrameGenerationMode::Off &&
               initial.effective == FrameGenerationMode::Off &&
               !initial.capability.available && !initial.history_ready &&
               !initial.counters.generated_frames_available &&
               !initial.counters.presentation_submissions_available && !initial.sdk_presented_frames &&
               !initial.counters.presentation_feedback_available, "defaults claim active generation or display feedback")) return false;
    state.request(FrameGenerationMode::Fsr3);
    state.set_capability({FrameGenerationMode::Fsr3, false, "Motion vectors unavailable"});
    if (!check(state.status().effective == FrameGenerationMode::Off &&
               state.status().reason == "Motion vectors unavailable", "unsupported request claims active provider")) return false;
    if (!check(state.record_real_frame(false) == 0 && state.record_real_frame(true) == 1 &&
               state.record_generated_frame(1) == 0, "rejected frame advanced history or unavailable provider generated a frame")) return false;
    state.set_capability({FrameGenerationMode::Fsr3, true, "Ready"});
    if (!check(state.record_real_frame(true) == 2 && !state.status().history_ready &&
               state.record_generated_frame(2) == 0 && state.record_real_frame(false) == 0 &&
               state.record_real_frame(true) == 3 && state.record_generated_frame(3) == 1 &&
               state.record_generated_frame(3) == 0 && state.record_generated_frame(2) == 0,
               "history warmup, rejected attempt, duplicate or stale generation was accepted")) return false;
    if (!check(state.record_presented_frame(false, true) == 1 &&
               state.record_presented_frame(true, false) == 0 &&
               state.record_presented_frame(true, true) == 2 &&
               state.record_presented_frame(true, true) == 0,
               "successful and failed presentations were conflated")) return false;
    auto counters = state.status().counters;
    if (!check(counters.real_frames == 3 && counters.rejected_real_frames == 2 &&
               counters.generated_frames == 1 && counters.presented_frames == 2 &&
               counters.real_presented_frames == 1 && counters.generated_presented_frames == 1 &&
               counters.failed_presentations == 2 && !counters.presentation_feedback_available &&
               counters.confirmed_displayed_frames == 0, "render/generation/submission/display counts were conflated")) return false;
    if (!check(!state.record_display_feedback(3) && state.record_display_feedback(1) &&
               !state.record_display_feedback(0) && state.record_display_feedback(1) &&
               state.status().counters.confirmed_displayed_frames == 1,
               "impossible, regressing or duplicate display feedback changed totals")) return false;
    const auto epoch = state.status().history_epoch;
    state.invalidate_history();
    if (!check(state.status().history_epoch == epoch + 1 && !state.status().history_ready &&
               state.record_generated_frame(3) == 0, "resize/cut reset retained usable interpolation history")) return false;
    state.request(FrameGenerationMode::Dlss);
    return check(state.status().effective == FrameGenerationMode::Off &&
                 !state.status().history_ready, "one provider capability enabled another provider");
}

bool frame_generation_configuration() {
    const auto defaults = parse_cli(std::array<std::string_view, 1>{"heaven"});
    if (!check(defaults.config.frame_generation == FrameGenerationMode::Off, "CLI default is not off")) return false;
    for (const auto mode : {FrameGenerationMode::Off, FrameGenerationMode::Fsr3, FrameGenerationMode::Dlss}) {
        const auto id = frame_generation_mode_id(mode);
        const auto parsed = parse_cli(std::array<std::string_view, 3>{"heaven", "--frame-generation", id});
        if (!check(parsed.diagnostics.empty() && parsed.config.frame_generation == mode &&
                   parsed.seen_keys.contains("frame_generation"), "valid request was lost in CLI/config precedence")) return false;
        const auto& entry = catalog_entry(SettingId::FrameGeneration);
        BenchConfig config;
        if (!check(entry.apply == Apply::Live && entry.apply_to(config, SettingValue{mode}) == SettingValue{mode} &&
                   config.frame_generation == mode, "catalog cannot request a supported mode identity")) return false;
        const auto serialized = serialize_config(config);
        const auto json = nlohmann::json::parse(serialized);
        if (!check(std::string_view{json.at("frame_generation").get_ref<const std::string&>()} == id,
                   "persisted mode is not canonical")) return false;
    }
    for (const auto invalid : {"", "auto", "fsr", "FSR3", "dlss4", "off-extra"}) {
        const auto parsed = parse_cli(std::array<std::string_view, 5>{
            "heaven", "--frame-generation", "fsr3", "--frame-generation", invalid});
        if (!check(!parsed.diagnostics.empty() && parsed.config.frame_generation == FrameGenerationMode::Fsr3,
                   "invalid mode erased the prior valid request")) return false;
    }
    return check(generate_help().find("--frame-generation") != std::string::npos,
                 "CLI help omits the request option");
}

bool frame_generation_scoring() {
    CameraPath path;
    CameraKeyframe first;
    first.time = 0.0f;
    first.position = glm::vec3(0.0f);
    first.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    first.fov = 60.0f;
    auto last = first;
    last.time = 1.0f;
    path.keyframes = {first, last};
    path.duration = 1.0f;
    SceneMarkers markers;
    markers.times = {0.0f};
    BenchmarkSystem system(&markers, &path, [] { return true; });
    Camera camera;
    if (!system.request_start()) return false;
    system.tick(0.25f, camera);
    system.set_frame_generation_requested(true);
    system.set_frame_generation_requested(false);
    if (!check(system.status().diagnostic, "off toggle restored scoring during tour")) return false;
    system.tick(0.75f, camera);
    const auto result = system.status().result;
    const auto report = build_benchmark_report(result, BenchConfig{}, {"test", "test"},
                                               std::chrono::system_clock::time_point{});
    const auto json = nlohmann::json::parse(report.json);
    if (!check(result.diagnostic && result.frames == 2 && result.average_fps == 2.0 &&
               result.performance.scores.overall == 0.0 &&
               json.at("run").at("diagnostic_reason") == "frame generation requested" &&
               json.at("scores").at("overall").is_null() &&
               json.at("frame_generation").at("generated_frames").is_null() &&
               json.at("frame_generation").at("presentation_submissions").is_null() &&
               json.at("frame_generation").at("failed_presentations").is_null() &&
               json.at("frame_generation").at("sdk_presentation_submissions").is_null() &&
               json.at("frame_generation").at("confirmed_displayed_frames").is_null() &&
               report.html.find("Failed native submissions:</td><td><div class=\"highlight\">Unavailable</div>") != std::string::npos &&
               report.html.find("frame generation requested") != std::string::npos,
               "FG diagnostic lost real-render stats or awarded a score")) return false;
    auto measured_result = result;
    auto& fg = measured_result.frame_generation;
    fg.requested = fg.effective = FrameGenerationMode::Fsr3;
    fg.counters.real_frames = 7;
    fg.counters.generated_frames = 3;
    fg.counters.presented_frames = 9;
    fg.counters.real_presented_frames = 6;
    fg.counters.generated_presented_frames = 3;
    fg.counters.generated_frames_available = true;
    fg.counters.presentation_submissions_available = true;
    auto measured_report = build_benchmark_report(measured_result, BenchConfig{}, {"test", "test"},
                                                  std::chrono::system_clock::time_point{});
    auto measured = nlohmann::json::parse(measured_report.json).at("frame_generation");
    if (!check(measured.at("real_frames") == 7 && measured.at("generated_frames") == 3 &&
               measured.at("presentation_submissions") == 9 &&
               measured.at("real_presentation_submissions") == 6 &&
               measured.at("generated_presentation_submissions") == 3 &&
               measured.at("failed_presentations") == 0 &&
               measured.at("confirmed_displayed_frames").is_null() &&
               measured_report.html.find("Failed native submissions:</td><td><div class=\"highlight\">0</div>") != std::string::npos &&
               measured_report.html.find("Real native submissions") != std::string::npos,
               "exact native event counts were lost or presented as displayed frames")) return false;
    fg.counters.failed_presentations = 2;
    measured_report = build_benchmark_report(measured_result, BenchConfig{}, {"test", "test"},
                                             std::chrono::system_clock::time_point{});
    measured = nlohmann::json::parse(measured_report.json).at("frame_generation");
    if (!check(measured.at("failed_presentations") == 2 &&
               measured_report.html.find("Failed native submissions:</td><td><div class=\"highlight\">2</div>") != std::string::npos,
               "measured native presentation failures were hidden from JSON or HTML")) return false;
    // A provider-reported combined count cannot establish a generated count or
    // a real/generated split. A measured SDK zero remains distinguishable from
    // an unavailable total, and old exact state counters must remain hidden.
    fg.requested = fg.effective = FrameGenerationMode::Dlss;
    fg.counters.generated_frames_available = false;
    fg.counters.presentation_submissions_available = false;
    fg.sdk_presented_frames = 0;
    measured_report = build_benchmark_report(measured_result, BenchConfig{}, {"test", "test"},
                                             std::chrono::system_clock::time_point{});
    measured = nlohmann::json::parse(measured_report.json).at("frame_generation");
    if (!check(measured.at("generated_frames").is_null() &&
               measured.at("presentation_submissions").is_null() &&
               measured.at("real_presentation_submissions").is_null() &&
               measured.at("generated_presentation_submissions").is_null() &&
               measured.at("failed_presentations").is_null() &&
               measured.at("sdk_presentation_submissions") == 0 &&
               measured.at("confirmed_displayed_frames").is_null() &&
               measured_report.html.find("Failed native submissions:</td><td><div class=\"highlight\">Unavailable</div>") != std::string::npos &&
               measured_report.html.find("SDK native submissions (combined)") != std::string::npos &&
               measured_report.html.find("Unavailable") != std::string::npos,
               "unavailable SDK split counts became measured zeros or inferred values")) return false;
    system.dismiss_result();
    if (!system.request_start()) return false;
    system.tick(1.0f, camera);
    if (!check(!system.status().result.diagnostic && system.status().result.performance.scores.overall > 0.0,
               "FG request tainted a later ordinary tour")) return false;
    const auto ordinary_report = build_benchmark_report(system.status().result, BenchConfig{}, {"test", "test"},
                                                        std::chrono::system_clock::time_point{});
    if (!check(!nlohmann::json::parse(ordinary_report.json).contains("frame_generation"),
               "ordinary report gained diagnostic counter metadata")) return false;
    system.dismiss_result();
    system.set_frame_generation_requested(true);
    if (!system.request_start()) return false;
    system.set_frame_generation_requested(false);
    system.set_microprofile_recording(true);
    system.set_microprofile_recording(false);
    system.tick(1.0f, camera);
    if (!check(system.status().result.diagnostic_reason == "microprofiler recording; frame generation requested",
               "request at start or combined diagnostic reasons were lost")) return false;
    system.dismiss_result();
    for (bool before_start : {false,true}) {
        system.set_presentation_trace_requested(before_start);
        if (!system.request_start()) return false;
        system.tick(.25f,camera);
        system.set_presentation_trace_requested(true);
        system.set_presentation_trace_requested(false);
        system.tick(.75f,camera);
        const auto observed = system.status().result;
        const auto observed_report = build_benchmark_report(observed,BenchConfig{},
            {"test","test"},std::chrono::system_clock::time_point{});
        const auto observed_json = nlohmann::json::parse(observed_report.json);
        if (!check(observed.diagnostic && observed.frames == 2 &&
                observed.diagnostic_reason == "presentation timing observation" &&
                observed.performance.scores.overall == 0.0 &&
                observed_json.at("scores").at("overall").is_null() &&
                observed_json.at("scores").at("gpu").is_null(),
                "Off tour presentation observer lost statistics or received a score")) return false;
        system.dismiss_result();
    }
    for (bool before_start : {false, true}) {
        system.set_input_response_requested(before_start);
        if (!system.request_start()) return false;
        system.tick(.25f, camera);
        system.set_input_response_requested(true);
        system.set_input_response_requested(false);
        system.tick(.75f, camera);
        const auto result = system.status().result;
        const auto report = nlohmann::json::parse(build_benchmark_report(result,
            BenchConfig{}, {"test", "test"}, std::chrono::system_clock::time_point{}).json);
        if (!check(result.diagnostic && result.frames == 2 &&
                result.diagnostic_reason == "software input response observation" &&
                report.at("scores").at("overall").is_null(),
                "software response recording received a score or lost its sticky reason")) return false;
        system.dismiss_result();
    }
    if (!system.request_start()) return false;
    system.set_frame_generation_requested(true);
    system.cancel();
    system.set_frame_generation_requested(false);
    system.tick(0.1f, camera);
    return check(!system.status().has_result && system.request_start() && !system.status().diagnostic,
                 "cancelled diagnostic leaked into next tour");
}

struct Effects : RendererEffects {
    BenchmarkSystem& system;
    int cancellations = 0;
    explicit Effects(BenchmarkSystem& owner) : system(owner) {}
    void set_feature_enabled(const char* /*name*/, bool /*on*/) override {}
    void apply_live(SettingId id, const BenchConfig& config) override {
        if (id == SettingId::FrameGeneration)
            system.set_frame_generation_requested(config.frame_generation != FrameGenerationMode::Off);
    }
    void cancel_active_run() override { ++cancellations; system.cancel(); }
    Result<void> rebuild() override { return {}; }
};

bool frame_generation_live_edit() {
    SceneMarkers markers;
    markers.times = {0.0f};
    CameraPath path;
    path.keyframes.resize(2);
    path.keyframes[1].time = 1.0f;
    path.duration = 1.0f;
    BenchmarkSystem system(&markers, &path, [] { return true; });
    if (!system.request_start()) return false;
    Effects effects(system);
    BenchConfig working, active;
    ConfigUiState ui;
    OverlayIntents intents;
    emit_edit(intents, SettingId::FrameGeneration, FrameGenerationMode::Fsr3);
    apply_config_intents(working, active, ui, intents, effects);
    if (!check(system.is_running() && system.status().diagnostic && effects.cancellations == 0 &&
               active.frame_generation == FrameGenerationMode::Fsr3 && !ui.dirty,
               "live FG request cancelled playback or failed to latch diagnostic")) return false;
    intents.edits.clear();
    emit_edit(intents, SettingId::TimeOfDay, 10.0f);
    apply_config_intents(working, active, ui, intents, effects);
    return check(effects.cancellations == 1 && !system.is_running(),
                 "ordinary live workload edits no longer cancel the score");
}

const heaven::testing::Register accounting{frame_generation_accounting};
const heaven::testing::Register configuration{frame_generation_configuration};
const heaven::testing::Register scoring{frame_generation_scoring};
const heaven::testing::Register live_edit{frame_generation_live_edit};
}
