#include "benchmark/input_response.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
using heaven::InputResponseKind;
using heaven::InputResponseRecorder;
using Json = nlohmann::json;
constexpr std::int64_t ms = 1'000'000;
bool require(bool value, const char* message) {
    if (!value) std::fprintf(stderr, "input_response_test: %s\n", message);
    return value;
}
const Json& event(const Json& report, std::size_t index) { return report.at("events").at(index); }
std::size_t samples(const Json& report, const char* kind, const char* metric) {
    return report.at("summary").at(kind).at(metric).at("sample_count").get<std::size_t>();
}
bool pending_survives_rejection_and_unrendered_ui() {
    InputResponseRecorder recorder;
    recorder.set_initial_state(false, 0, 0);
    if (!recorder.record(InputResponseKind::UiToggle, 1 * ms, 1)) return false;
    recorder.set_response_state(true, 0, 0);
    // A rejected attempt does not call accepted_frame. Even an accepted scene
    // without a rendered UI cannot consume the pending UI response.
    recorder.accepted_frame(9, 1, 2 * ms, 3 * ms, false);
    auto report = recorder.report();
    if (!require(event(report, 0)["status"] == "pending", "unrendered UI consumed pending input")) return false;
    recorder.accepted_frame(11, 2, 4 * ms, 5 * ms, true);
    recorder.frontend_complete(2, 6 * ms);
    recorder.gpu_frame(37, 4 * ms + 1, 7 * ms, 9 * ms);
    report = recorder.report();
    return require(event(report, 0)["attempt_id"] == "11" &&
        event(report, 0)["submission_id"] == "2" && event(report, 0)["filament_frame_id"] == "37" &&
        event(report, 0)["frontend_status"] == "available" && event(report, 0)["gpu_status"] == "available" &&
        report["summary"]["ui_toggle"]["frontend_encode_ms"]["p50_ms"] == 5.0 &&
        report["summary"]["ui_toggle"]["gpu_fence_observed_upper_bound_ms"]["max_ms"] == 8.0,
        "rejected-attempt carry or endpoint attribution failed");
}
bool coalescing_net_zero_and_overwritten_state() {
    InputResponseRecorder recorder;
    recorder.set_initial_state(false, 0, 0);
    if (!recorder.record(InputResponseKind::UiToggle, 1 * ms, 1) ||
            !recorder.record(InputResponseKind::UiToggle, 2 * ms, 0) ||
            !recorder.record(InputResponseKind::CameraLook, 3 * ms, 5, 2) ||
            !recorder.record(InputResponseKind::CameraLook, 4 * ms, 10, 3)) return false;
    recorder.set_response_state(false, 10, 3);
    recorder.accepted_frame(1, 1, 5 * ms, 6 * ms, true);
    recorder.frontend_complete(1, 7 * ms);
    recorder.gpu_frame(2, 5 * ms + 1, 8 * ms, 9 * ms);
    if (!recorder.record(InputResponseKind::CameraLook, 10 * ms, 11, 3)) return false;
    recorder.set_response_state(false, 12, 3);
    recorder.accepted_frame(2, 2, 11 * ms, 12 * ms, true);
    const auto report = recorder.report();
    return require(event(report, 0)["status"] == "coalesced" &&
        event(report, 0)["superseded_by_event_id"] == "2" && event(report, 1)["status"] == "no_net_change" &&
        event(report, 2)["status"] == "coalesced" && event(report, 3)["status"] == "associated" &&
        event(report, 4)["reason"] == "response_overwritten_before_accepted_frame" &&
        samples(report, "ui_toggle", "frontend_encode_ms") == 0 &&
        samples(report, "camera_look", "gpu_fence_observed_upper_bound_ms") == 1,
        "coalesced or invisible transient states were measured");
}
bool exact_out_of_order_matching() {
    InputResponseRecorder recorder;
    recorder.set_initial_state(false, 0, 0);
    if (!recorder.record(InputResponseKind::CameraLook, 1 * ms, 1)) return false;
    recorder.set_response_state(false, 1, 0);
    recorder.accepted_frame(10, 1, 2 * ms, 3 * ms, true);
    recorder.frontend_complete(1, 4 * ms);
    if (!recorder.record(InputResponseKind::CameraLook, 5 * ms, 2)) return false;
    recorder.set_response_state(false, 2, 0);
    recorder.accepted_frame(12, 2, 6 * ms, 7 * ms, true);
    // Completion of source 2 can precede publication of source 1, and can
    // arrive before source 2's frontend completion notification.
    recorder.gpu_frame(102, 6 * ms + 10, 8 * ms, 10 * ms);
    recorder.frontend_complete(2, 9 * ms);
    recorder.gpu_frame(99, 2 * ms + 10, 5 * ms, 6 * ms);
    const auto report = recorder.report();
    return require(event(report, 0)["filament_frame_id"] == "99" &&
        event(report, 1)["filament_frame_id"] == "102" &&
        event(report, 0)["gpu_complete_ns"] == std::to_string(6 * ms) &&
        event(report, 1)["frontend_complete_ns"] == std::to_string(9 * ms) &&
        samples(report, "camera_look", "frontend_encode_ms") == 2 &&
        samples(report, "camera_look", "gpu_fence_observed_upper_bound_ms") == 2 &&
        report["summary"]["camera_look"]["frontend_encode_ms"]["p50_ms"] == 3.0 &&
        report["summary"]["camera_look"]["frontend_encode_ms"]["p95_ms"] == 4.0,
        "out-of-order histories were associated with the latest frame");
}
bool unavailable_nonmonotonic_and_ambiguous() {
    InputResponseRecorder recorder;
    recorder.set_initial_state(false, 0, 0);
    for (std::uint64_t i = 1; i <= 3; ++i) {
        const auto time = static_cast<std::int64_t>(i) * 10 * ms;
        if (!recorder.record(InputResponseKind::CameraLook, time, static_cast<double>(i))) return false;
        recorder.set_response_state(false, static_cast<double>(i), 0);
        recorder.accepted_frame(i, i, time + ms, time + 2 * ms, true);
        recorder.frontend_complete(i, i == 3 ? time : time + 3 * ms);
        recorder.gpu_frame(static_cast<std::uint32_t>(i), time + ms,
            time + 4 * ms, i == 1 ? 0 : time + 3 * ms);
    }
    const auto unavailable = recorder.report();
    if (!require(event(unavailable, 0)["gpu_reason"] == "gpu_completion_unavailable" &&
            event(unavailable, 1)["gpu_reason"] == "invalid_or_nonmonotonic_gpu_time" &&
            event(unavailable, 2)["frontend_status"] == "unavailable" &&
            samples(unavailable, "camera_look", "gpu_fence_observed_upper_bound_ms") == 0 &&
            samples(unavailable, "camera_look", "frontend_encode_ms") == 2,
            "unavailable or reversed endpoint generated a latency")) return false;
    InputResponseRecorder ambiguous;
    ambiguous.set_initial_state(false, 0, 0);
    for (std::uint64_t i = 1; i <= 2; ++i) {
        if (!ambiguous.record(InputResponseKind::CameraLook, ms, static_cast<double>(i))) return false;
        ambiguous.set_response_state(false, static_cast<double>(i), 0);
        ambiguous.accepted_frame(i, i, 2 * ms, 4 * ms, true);
        ambiguous.frontend_complete(i, 5 * ms);
    }
    ambiguous.gpu_frame(10, 3 * ms, 6 * ms, 7 * ms);
    const auto report = ambiguous.report();
    return require(report["counters"]["ambiguous_gpu_samples"] == 1 &&
        event(report, 0)["gpu_reason"] == "ambiguous_frame_time_bracket" &&
        event(report, 1)["gpu_status"] == "unavailable" &&
        samples(report, "camera_look", "gpu_fence_observed_upper_bound_ms") == 0,
        "ambiguous timestamp brackets fabricated an association");
}
bool bounded_capacity_and_lifecycle() {
    InputResponseRecorder recorder;
    recorder.set_initial_state(false, 0, 0);
    for (std::size_t i = 0; i < 2 * InputResponseRecorder::frame_capacity; ++i)
        recorder.accepted_frame(static_cast<std::uint64_t>(i) + 1,
            static_cast<std::uint64_t>(i) + 1, 2 * ms, 3 * ms, true);
    for (std::size_t i = 0; i <= InputResponseRecorder::frame_capacity; ++i) {
        const auto identity = static_cast<std::uint64_t>(i) + 1;
        const auto yaw = static_cast<double>(identity);
        if (!recorder.record(InputResponseKind::CameraLook, ms, yaw)) return false;
        recorder.set_response_state(false, yaw, 0);
        const auto begin = static_cast<std::int64_t>(identity + 1) * 10 * ms;
        recorder.accepted_frame(identity, identity, begin, begin + ms, true);
        recorder.frontend_complete(identity, begin + 2 * ms);
    }
    recorder.history_missing();
    auto report = recorder.report();
    if (!require(report["counters"]["frame_probe_overflows"] == 1 &&
            event(report, InputResponseRecorder::frame_capacity)["reason"] == "frame_probe_capacity_exhausted" &&
            report["counters"]["history_missing"] == 1,
            "frame-probe storage was unbounded or loss hidden")) return false;
    if (!recorder.record(InputResponseKind::UiToggle, 1000 * ms, 1)) return false;
    recorder.retire_pending("reload");
    report = recorder.report();
    if (!require(event(report, 0)["frontend_status"] == "available" &&
            event(report, 0)["gpu_status"] == "unavailable" && event(report, 0)["lifecycle_reason"] == "reload" &&
            event(report, InputResponseRecorder::frame_capacity + 1)["status"] == "unavailable",
            "reload lost completed endpoints or carried old pending input")) return false;
    recorder.set_response_state(true, 100, 0);
    recorder.accepted_frame(99, 99, 1001 * ms, 1002 * ms, true);
    if (!recorder.record(InputResponseKind::CameraLook, 1100 * ms, 101)) return false;
    recorder.set_response_state(true, 101, 0);
    recorder.accepted_frame(100, 100, 1101 * ms, 1102 * ms, true);
    recorder.frontend_complete(100, 1103 * ms);
    recorder.gpu_frame(500, 1101 * ms, 1104 * ms, 1105 * ms);
    report = recorder.report();
    if (!require(event(report, InputResponseRecorder::frame_capacity + 2)["gpu_status"] == "available",
            "reload could not reuse retired frame-probe storage")) return false;
    InputResponseRecorder full;
    full.set_initial_state(false, 0, 0);
    for (std::size_t i = 0; i < InputResponseRecorder::event_capacity; ++i)
        if (!full.record(InputResponseKind::CameraLook, ms, static_cast<double>(i))) return false;
    if (!require(!full.record(InputResponseKind::CameraLook, ms, 9999), "event overflow was accepted")) return false;
    report = full.report();
    return require(report["events"].size() == InputResponseRecorder::event_capacity &&
        report["counters"]["dropped_events"] == 1 && event(report, InputResponseRecorder::event_capacity - 1)["status"] == "coalesced",
        "event storage grew or a dropped newer response left stale attribution");
}
bool invalid_inputs_and_exact_serialization() {
    InputResponseRecorder recorder;
    recorder.set_initial_state(false, 0, 0);
    if (!require(!recorder.record(InputResponseKind::CameraLook, 0, 1) &&
            !recorder.record(InputResponseKind::CameraLook, 1, std::numeric_limits<double>::infinity()),
            "invalid consumption input accepted")) return false;
    constexpr std::int64_t origin = 9'007'199'254'740'993;
    if (!recorder.record(InputResponseKind::CameraLook, origin, 1)) return false;
    recorder.set_response_state(false, 1, 0);
    recorder.accepted_frame(9'007'199'254'740'993ull, 1, origin + 1, origin + 2, true);
    recorder.frontend_complete(1, origin + 3);
    recorder.gpu_frame(3, origin + 1, origin + 4, origin + 5);
    const auto report = Json::parse(recorder.report().dump());
    return require(event(report, 2)["consumed_ns"] == "9007199254740993" &&
        event(report, 2)["attempt_id"] == "9007199254740993" &&
        report["physical_input_to_photon_measured"] == false && report["display_latency_measured"] == false &&
        InputResponseRecorder::now_ns() > 0,
        "large host timestamps lost precision or software report claimed physical latency");
}
} // namespace
int main() {
    const bool ok = pending_survives_rejection_and_unrendered_ui() &&
        coalescing_net_zero_and_overwritten_state() && exact_out_of_order_matching() &&
        unavailable_nonmonotonic_and_ambiguous() && bounded_capacity_and_lifecycle() &&
        invalid_inputs_and_exact_serialization();
    if (ok) std::puts("input_response_test: all checks passed");
    return ok ? 0 : 1;
}
