#include "benchmark/input_response.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace heaven {
namespace {
using Json = nlohmann::json;
constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
enum class EventStatus { Pending, Coalesced, NoNetChange, Associated, Unavailable };
enum class StageStatus { Pending, Available, Unavailable, NotApplicable };
const char* name(InputResponseKind kind) {
    return kind == InputResponseKind::UiToggle ? "ui_toggle" : "camera_look";
}
const char* name(EventStatus status) {
    constexpr std::array names{"pending", "coalesced", "no_net_change", "associated", "unavailable"};
    return names[static_cast<std::size_t>(status)];
}
const char* name(StageStatus status) {
    constexpr std::array names{"pending", "available", "unavailable", "not_applicable"};
    return names[static_cast<std::size_t>(status)];
}
Json distribution(std::vector<double> values) {
    Json out{{"sample_count", values.size()}, {"p50_ms", nullptr},
        {"p95_ms", nullptr}, {"max_ms", nullptr}};
    if (values.empty()) return out;
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double p) {
        return values[static_cast<std::size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1];
    };
    out["p50_ms"] = percentile(0.50);
    out["p95_ms"] = percentile(0.95);
    out["max_ms"] = values.back();
    return out;
}
} // namespace

struct InputResponseRecorder::Impl {
    struct State { bool ui = false; double yaw = 0, pitch = 0; };
    struct Event {
        InputResponseKind kind = InputResponseKind::UiToggle;
        EventStatus status = EventStatus::Pending;
        StageStatus frontend = StageStatus::NotApplicable;
        StageStatus backend = StageStatus::NotApplicable;
        StageStatus gpu = StageStatus::NotApplicable;
        std::uint64_t id = 0, superseded_by = 0, attempt = 0, submission = 0;
        std::uint32_t filament_id = 0;
        std::int64_t consumed = 0, begin_lo = 0, begin_hi = 0;
        std::int64_t filament_begin_ns = 0, frontend_ns = 0, backend_ns = 0, gpu_ns = 0;
        double x = 0, y = 0, response_x = 0, response_y = 0;
        const char* reason = "awaiting_accepted_real_frame";
        const char* frontend_reason = "no_response_frame";
        const char* backend_reason = "no_response_frame";
        const char* gpu_reason = "no_response_frame";
        std::array<char, 96> lifecycle_reason{};
    };
    struct Frame {
        bool active = false;
        std::uint64_t submission = 0;
        std::int64_t lo = 0, hi = 0;
        std::array<std::size_t, 2> events{none, none};
    };
    std::array<Event, event_capacity> events{};
    std::array<Frame, frame_capacity> frames{};
    std::array<std::size_t, 2> pending{none, none};
    std::size_t count = 0;
    State current{}, baseline{};
    bool state_valid = false;
    std::array<bool, 2> baseline_valid{false, false};
    std::uint64_t dropped_events = 0, invalid_inputs = 0, probe_overflows = 0;
    std::uint64_t missing_history = 0, unmatched_gpu_samples = 0;
    std::uint64_t ambiguous_gpu_samples = 0, invalid_gpu_samples = 0;
    std::uint64_t duplicate_frontend_samples = 0;

    static bool valid(const State& state) {
        return std::isfinite(state.yaw) && std::isfinite(state.pitch);
    }
    static bool same(InputResponseKind kind, const State& a, const State& b) {
        return kind == InputResponseKind::UiToggle ? a.ui == b.ui :
            a.yaw == b.yaw && a.pitch == b.pitch;
    }
    void unavailable(Event& event, const char* reason) {
        event.status = EventStatus::Unavailable;
        event.reason = reason;
        event.frontend = event.backend = event.gpu = StageStatus::Unavailable;
        event.frontend_reason = event.backend_reason = event.gpu_reason = reason;
    }
    void release_if_complete(Frame& frame) {
        for (const auto index : frame.events) {
            if (index != none && (events[index].frontend == StageStatus::Pending ||
                    events[index].gpu == StageStatus::Pending)) return;
        }
        frame.active = false;
    }
    void coalesce(std::size_t kind, std::uint64_t successor) {
        if (pending[kind] == none) return;
        auto& previous = events[pending[kind]];
        previous.status = EventStatus::Coalesced;
        previous.reason = "superseded_before_accepted_response";
        previous.superseded_by = successor;
        pending[kind] = none;
    }
};

InputResponseRecorder::InputResponseRecorder() : impl_(std::make_unique<Impl>()) {}
InputResponseRecorder::~InputResponseRecorder() = default;
std::int64_t InputResponseRecorder::now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void InputResponseRecorder::set_initial_state(bool ui, double yaw, double pitch) {
    set_response_state(ui, yaw, pitch);
    impl_->baseline = impl_->current;
    impl_->baseline_valid.fill(impl_->state_valid);
}
void InputResponseRecorder::set_response_state(bool ui, double yaw, double pitch) {
    impl_->current = {ui, yaw, pitch};
    impl_->state_valid = Impl::valid(impl_->current);
}
bool InputResponseRecorder::record(InputResponseKind kind, std::int64_t consumed_ns,
        double x, double y) {
    auto& s = *impl_;
    const auto k = static_cast<std::size_t>(kind);
    if (k >= s.pending.size()) { ++s.invalid_inputs; return false; }
    if (s.count == event_capacity) {
        s.coalesce(k, 0);
        ++s.dropped_events;
        return false;
    }
    const auto index = s.count++;
    auto& event = s.events[index];
    event.kind = kind;
    event.id = static_cast<std::uint64_t>(index) + 1;
    event.consumed = consumed_ns;
    event.x = x;
    event.y = y;
    s.coalesce(k, event.id);
    if (consumed_ns <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
            (kind == InputResponseKind::UiToggle && x != 0 && x != 1)) {
        ++s.invalid_inputs;
        s.unavailable(event, "invalid_consumed_time_or_response_state");
        return false;
    }
    s.pending[k] = index;
    return true;
}
void InputResponseRecorder::accepted_frame(std::uint64_t attempt, std::uint64_t submission,
        std::int64_t begin_lo, std::int64_t begin_hi, bool ui_rendered) {
    auto& s = *impl_;
    std::array<std::size_t, 2> associated{none, none};
    for (std::size_t k = 0; k < s.pending.size(); ++k) {
        if (s.pending[k] == none || (k == 0 && !ui_rendered)) continue;
        const auto index = std::exchange(s.pending[k], none);
        auto& event = s.events[index];
        event.attempt = attempt;
        event.submission = submission;
        event.begin_lo = begin_lo;
        event.begin_hi = begin_hi;
        event.response_x = k == 0 ? (s.current.ui ? 1.0 : 0.0) : s.current.yaw;
        event.response_y = k == 0 ? 0.0 : s.current.pitch;
        if (!s.state_valid || !s.baseline_valid[k]) {
            s.unavailable(event, "response_baseline_unavailable");
        } else if (event.x != event.response_x || (k == 1 && event.y != event.response_y)) {
            event.status = EventStatus::Coalesced;
            event.reason = "response_overwritten_before_accepted_frame";
        } else if (Impl::same(event.kind, s.current, s.baseline)) {
            event.status = EventStatus::NoNetChange;
            event.reason = "state_equals_last_accepted_response";
        } else if (attempt == 0 || submission == 0 || begin_lo <= 0 || begin_hi < begin_lo ||
                event.consumed > begin_lo) {
            s.unavailable(event, "invalid_accepted_frame_time_or_identity");
        } else {
            event.status = EventStatus::Associated;
            event.reason = "first_accepted_real_response_frame";
            event.frontend = event.backend = event.gpu = StageStatus::Pending;
            event.frontend_reason = "awaiting_frontend_completion";
            event.backend_reason = event.gpu_reason = "awaiting_frame_history";
            associated[k] = index;
        }
    }
    if (s.state_valid) {
        if (ui_rendered) {
            s.baseline.ui = s.current.ui;
            s.baseline_valid[0] = true;
        }
        s.baseline.yaw = s.current.yaw;
        s.baseline.pitch = s.current.pitch;
        s.baseline_valid[1] = true;
    }
    if (associated[0] == none && associated[1] == none) return;
    auto slot = std::find_if(s.frames.begin(), s.frames.end(),
        [](const auto& frame) { return !frame.active; });
    const bool duplicate = std::any_of(s.frames.begin(), s.frames.end(),
        [submission](const auto& frame) { return frame.active && frame.submission == submission; });
    if (slot == s.frames.end() || duplicate) {
        ++s.probe_overflows;
        for (const auto index : associated) if (index != none)
            s.unavailable(s.events[index], duplicate ? "duplicate_submission_identity" : "frame_probe_capacity_exhausted");
        return;
    }
    *slot = {true, submission, begin_lo, begin_hi, associated};
}
void InputResponseRecorder::frontend_complete(std::uint64_t submission, std::int64_t ns) {
    auto& s = *impl_;
    for (auto& frame : s.frames) {
        if (!frame.active || frame.submission != submission) continue;
        for (const auto index : frame.events) {
            if (index == none) continue;
            auto& event = s.events[index];
            if (event.frontend != StageStatus::Pending) { ++s.duplicate_frontend_samples; continue; }
            event.frontend_ns = ns;
            const bool valid = ns > 0 && ns >= event.consumed && ns >= frame.hi;
            event.frontend = valid ? StageStatus::Available : StageStatus::Unavailable;
            event.frontend_reason = valid ? "frontend_encode_return_observed" : "invalid_or_nonmonotonic_frontend_time";
        }
        s.release_if_complete(frame);
    }
}
void InputResponseRecorder::gpu_frame(std::uint32_t filament_id, std::int64_t begin_ns,
        std::int64_t backend_end_ns, std::int64_t gpu_complete_ns) {
    auto& s = *impl_;
    std::array<std::size_t, frame_capacity> matches{};
    std::size_t count = 0;
    for (std::size_t i = 0; i < s.frames.size(); ++i) {
        const auto& frame = s.frames[i];
        if (!frame.active || begin_ns < frame.lo || begin_ns > frame.hi) continue;
        const bool pending = std::any_of(frame.events.begin(), frame.events.end(), [&](auto index) {
            return index != none && s.events[index].gpu == StageStatus::Pending;
        });
        if (pending) matches[count++] = i;
    }
    if (count == 0) {
        ++s.unmatched_gpu_samples;
        if (begin_ns <= 0 || filament_id == 0) ++s.invalid_gpu_samples;
        return;
    }
    if (count > 1) ++s.ambiguous_gpu_samples;
    const bool valid_backend = filament_id != 0 && begin_ns > 0 && backend_end_ns >= begin_ns;
    const bool valid_gpu = filament_id != 0 && begin_ns > 0 && gpu_complete_ns > 0 &&
        gpu_complete_ns >= begin_ns && (!valid_backend || gpu_complete_ns >= backend_end_ns);
    if (count == 1 && !valid_gpu) ++s.invalid_gpu_samples;
    for (std::size_t i = 0; i < count; ++i) {
        auto& frame = s.frames[matches[i]];
        for (const auto index : frame.events) {
            if (index == none) continue;
            auto& event = s.events[index];
            event.filament_id = filament_id;
            event.filament_begin_ns = begin_ns;
            event.backend_ns = backend_end_ns;
            event.gpu_ns = gpu_complete_ns;
            event.backend = count == 1 && valid_backend ? StageStatus::Available : StageStatus::Unavailable;
            event.gpu = count == 1 && valid_gpu ? StageStatus::Available : StageStatus::Unavailable;
            event.backend_reason = count > 1 ? "ambiguous_frame_time_bracket" : valid_backend ?
                "backend_end_observed" : "invalid_or_nonmonotonic_backend_time";
            event.gpu_reason = count > 1 ? "ambiguous_frame_time_bracket" : valid_gpu ?
                "host_gpu_fence_observation_upper_bound" : gpu_complete_ns <= 0 ?
                "gpu_completion_unavailable" : "invalid_or_nonmonotonic_gpu_time";
        }
        s.release_if_complete(frame);
    }
}
void InputResponseRecorder::history_missing() { ++impl_->missing_history; }
void InputResponseRecorder::retire_pending(const char* reason) {
    auto& s = *impl_;
    const auto retire = [reason](Impl::Event& event) {
        const char* text = reason ? reason : "lifecycle_retired";
        const auto length = std::min(std::strlen(text), event.lifecycle_reason.size() - 1);
        std::memcpy(event.lifecycle_reason.data(), text, length);
        event.lifecycle_reason[length] = '\0';
    };
    for (auto& index : s.pending) {
        if (index == none) continue;
        auto& event = s.events[index];
        s.unavailable(event, "retired_before_accepted_response");
        retire(event);
        index = none;
    }
    for (auto& frame : s.frames) {
        if (!frame.active) continue;
        for (const auto index : frame.events) {
            if (index == none) continue;
            auto& event = s.events[index];
            retire(event);
            if (event.frontend == StageStatus::Pending) {
                event.frontend = StageStatus::Unavailable;
                event.frontend_reason = "lifecycle_retired";
            }
            if (event.backend == StageStatus::Pending) {
                event.backend = StageStatus::Unavailable;
                event.backend_reason = "lifecycle_retired";
            }
            if (event.gpu == StageStatus::Pending) {
                event.gpu = StageStatus::Unavailable;
                event.gpu_reason = "lifecycle_retired";
            }
        }
        frame.active = false;
    }
    s.baseline_valid.fill(false);
}
Json InputResponseRecorder::report() const {
    const auto& s = *impl_;
    Json out{{"schema", 1}, {"clock", "std::chrono::steady_clock"},
        {"timestamp_encoding", "decimal_string_nanoseconds"},
        {"measurement_scope", "software_consumption_to_first_accepted_real_response"},
        {"frontend_metric", "CPU event consumption to frontend render encoding return"},
        {"gpu_metric", "CPU event consumption to host GPU fence observation upper bound; includes queueing, provider pacing and observer scheduling"},
        {"physical_input_to_photon_measured", false}, {"display_latency_measured", false},
        {"capacity", {{"events", event_capacity}, {"frame_probes", frame_capacity}}},
        {"counters", {{"recorded_events", s.count}, {"dropped_events", s.dropped_events},
            {"invalid_inputs", s.invalid_inputs}, {"frame_probe_overflows", s.probe_overflows},
            {"history_missing", s.missing_history}, {"unmatched_gpu_samples", s.unmatched_gpu_samples},
            {"ambiguous_gpu_samples", s.ambiguous_gpu_samples}, {"invalid_gpu_samples", s.invalid_gpu_samples},
            {"duplicate_frontend_samples", s.duplicate_frontend_samples}}},
        {"events", Json::array()}, {"summary", Json::object()}};
    std::array<std::vector<double>, 2> frontend, backend, gpu;
    std::array<Json, 2> counts{Json::object(), Json::object()};
    for (std::size_t i = 0; i < s.count; ++i) {
        const auto& event = s.events[i];
        const auto k = static_cast<std::size_t>(event.kind);
        const auto add = [&](StageStatus status, std::int64_t time, auto& values) {
            if (status == StageStatus::Available) values[k].push_back(
                static_cast<double>(time - event.consumed) / 1'000'000.0);
        };
        add(event.frontend, event.frontend_ns, frontend);
        add(event.backend, event.backend_ns, backend);
        add(event.gpu, event.gpu_ns, gpu);
        auto& count = counts[k][name(event.status)];
        count = count.is_null() ? 1 : count.get<std::uint64_t>() + 1;
        out["events"].push_back({{"id", std::to_string(event.id)}, {"kind", name(event.kind)},
            {"status", name(event.status)}, {"reason", event.reason},
            {"lifecycle_reason", event.lifecycle_reason.data()},
            {"superseded_by_event_id", std::to_string(event.superseded_by)},
            {"consumed_ns", std::to_string(event.consumed)},
            {"input_state", {event.x, event.y}}, {"response_state", {event.response_x, event.response_y}},
            {"attempt_id", std::to_string(event.attempt)}, {"submission_id", std::to_string(event.submission)},
            {"filament_frame_id", std::to_string(event.filament_id)},
            {"begin_lo_ns", std::to_string(event.begin_lo)}, {"begin_hi_ns", std::to_string(event.begin_hi)},
            {"filament_begin_ns", std::to_string(event.filament_begin_ns)},
            {"frontend_complete_ns", std::to_string(event.frontend_ns)},
            {"backend_end_ns", std::to_string(event.backend_ns)}, {"gpu_complete_ns", std::to_string(event.gpu_ns)},
            {"frontend_status", name(event.frontend)}, {"frontend_reason", event.frontend_reason},
            {"backend_status", name(event.backend)}, {"backend_reason", event.backend_reason},
            {"gpu_status", name(event.gpu)}, {"gpu_reason", event.gpu_reason}});
    }
    for (std::size_t k = 0; k < 2; ++k) out["summary"][name(static_cast<InputResponseKind>(k))] = {
        {"event_status_counts", counts[k]}, {"frontend_encode_ms", distribution(std::move(frontend[k]))},
        {"backend_end_ms", distribution(std::move(backend[k]))},
        {"gpu_fence_observed_upper_bound_ms", distribution(std::move(gpu[k]))}};
    return out;
}
} // namespace heaven
