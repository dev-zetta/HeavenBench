#include "smoke_cases.h"
#include "benchmark/microprofiler.h"
#include "benchmark/microprofile_report.h"
#include "engine/camera.h"
#include "rhi/filament_backend.h"
#include <filament/Engine.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <unordered_set>

namespace heaven::smoke {
CaseResult run_microprofiler_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    auto& backend = static_cast<FilamentBackend&>(ctx.backend);
    if (auto result = backend.init_pipelines(ctx.config); !result) return CaseResult::Fail;
    auto& render = backend.debug_render_context();
    profile::Recorder recorder;
    profile::set_recorder(&recorder);
    backend.set_trace_sink(&recorder);
    struct Drain {
        FilamentBackend& backend;
        filament::Engine& engine;
        ~Drain() {
            // Test teardown only; the ordinary profiler path never waits.
            engine.flushAndWait();
            backend.set_trace_sink(nullptr);
            profile::set_recorder(nullptr);
        }
    } drain{backend, *render.engine};
    recorder.begin_capture();
    recorder.set_thread_name("Microprofiler fixture main");
    Camera camera;
    WeatherState weather;
    TonemapParams tonemap;
    const auto lighting = lighting_for_time_of_day(12.0f);
    const MaterialResolver resolve = [](const std::string&, const std::string&) { return GpuMaterial{}; };
    uint64_t accepted = 0;
    auto frame = [&](uint64_t attempt) {
        profile::Context context{attempt, accepted + 1, 0, 0, 0, recorder.session_id()};
        recorder.set_frame_context(context);
        recorder.frame_begin(context);
        backend.set_trace_frame_context(context);
        const auto result = backend.render_frame({}, {}, {}, resolve, camera, lighting,
            weather, tonemap, ctx.width, ctx.height);
        if (result.presented) ++accepted;
        context.submission_id = result.presented ? accepted : 0;
        recorder.frame_end(context, result.presented ? profile::FrameOutcome::Accepted : profile::FrameOutcome::Rejected);
    };
    for (uint64_t attempt = 1; attempt <= 48; ++attempt) frame(attempt);
    // Exercise the existing swap-chain drain/recreation with recording active.
    // Query storage must remain valid across this resource lifetime boundary.
    backend.resize(ctx.width, ctx.height);
    for (uint64_t attempt = 49; attempt <= 56; ++attempt) frame(attempt);
    const auto capabilities = backend.trace_capabilities();
    recorder.set_active(false);
    for (uint64_t attempt = 57; attempt <= 64; ++attempt) frame(attempt);
    const auto snapshot = recorder.snapshot();
    bool valid = accepted > 0 && !snapshot.spans.empty() && !snapshot.gpu_samples.empty();
    size_t available = 0, unavailable = 0;
    bool submitted_instances = false;
    std::unordered_set<uint64_t> views;
    for (const auto& sample : snapshot.gpu_samples) {
        valid &= sample.context.capture_id == recorder.session_id() &&
            sample.context.attempt_id > 0 && sample.context.attempt_id <= 56 &&
            sample.context.renderer_id != 0 && sample.context.filament_frame_id != 0 &&
            sample.context.view_id != 0 && sample.pass_id != 0;
        views.insert(sample.context.view_id);
        if (sample.status == profile::SampleStatus::Available) {
            ++available;
            valid &= sample.source == profile::TimerSource::HardwareTimestamp && sample.end_ns >= sample.begin_ns;
            submitted_instances |= sample.command_count > 0 && sample.primitive_count > 0 &&
                sample.draw_instance_invocations > 0;
        } else ++unavailable;
    }
    valid &= views.size() > 1;
    valid &= capabilities.hardware_timestamps ? available > 0 && submitted_instances : unavailable > 0;
    valid &= profile::parse_snapshot(profile::snapshot_json(snapshot)).has_value();
    if (const char* output = std::getenv("HEAVEN_SMOKE_MICROPROFILE_OUTPUT")) {
        profile::ReportWriter writer;
        valid &= writer.submit(snapshot, std::filesystem::path(output));
        writer.wait();
        valid &= writer.status().state == profile::ReportState::Complete;
    }
    // A fresh capture rejects outstanding callbacks from the old generation.
    recorder.begin_capture();
    for (uint64_t attempt = 10001; attempt <= 10016; ++attempt) frame(attempt);
    valid &= backend.reconfigure_for_reload(ctx.config).has_value();
    render.engine->flushAndWait();
    for (uint64_t attempt = 10017; attempt <= 10024; ++attempt) frame(attempt);
    recorder.set_active(false);
    const auto next = recorder.snapshot();
    valid &= !next.gpu_samples.empty() && std::ranges::all_of(next.gpu_samples, [&](const auto& sample) {
        return sample.context.capture_id == recorder.session_id() && sample.context.attempt_id >= 10001;
    });
    std::printf("[smoke_microprofiler] accepted=%llu CPU=%zu GPU=%zu available=%zu unavailable=%zu views=%zu instances=%s hardware=%s reason=%s: %s\n",
        static_cast<unsigned long long>(accepted), snapshot.spans.size(), snapshot.gpu_samples.size(),
        available, unavailable, views.size(), submitted_instances ? "yes" : "no",
        capabilities.hardware_timestamps ? "yes" : "no", capabilities.reason.c_str(),
        valid ? "PASS" : "FAIL");
    return valid ? CaseResult::Pass : CaseResult::Fail;
}
} // namespace heaven::smoke
