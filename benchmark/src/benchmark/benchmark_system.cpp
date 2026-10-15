/*
 * HeavenBench — Run lifecycle state machine
 */
#include "benchmark/benchmark_system.h"

#include "benchmark/score_calculator.h"
#include "engine/camera.h"

#include <cmath>
#include <cstdio>
#include <numeric>

namespace heaven {

BenchmarkSystem::BenchmarkSystem(const SceneMarkers* markers,
                                 const CameraPath*   path,
                                 FramePacingProbe    pacing_probe,
                                 SystemInfoGatherer   gather_system,
                                 SettingsInfoGatherer gather_settings)
    : markers_(markers),
      path_(path),
      player_(path),
      pacing_probe_(std::move(pacing_probe)),
      gather_system_(std::move(gather_system)),
      gather_settings_(std::move(gather_settings)) {}

// ── Start ───────────────────────────────────────────────────────

bool BenchmarkSystem::request_start() {
    // Refuse while a run is already active so the active run's clock, stats, and
    // scene index are left untouched.
    if (state_ == BenchmarkState::Running) {
        std::fprintf(stderr,
                     "[benchmark] start ignored: a run is already active\n");
        return false;
    }

    // Refuse when the assets are unusable: missing/empty scene markers
    // or a camera path with fewer than two keyframes
    // The renderer stays in free-fly mode.
    if (markers_ == nullptr || markers_->count() == 0) {
        std::fprintf(stderr,
                     "[benchmark] start refused: scene markers are missing or "
                     "empty\n");
        return false;
    }
    if (path_ == nullptr || path_->keyframes.size() < 2) {
        std::fprintf(stderr,
                     "[benchmark] start refused: camera path has fewer than two "
                     "keyframes\n");
        return false;
    }

    // Refuse when frame pacing cannot be disabled so the measured frame rate is
    // never constrained by the display refresh.
    if (pacing_probe_ && !pacing_probe_()) {
        std::fprintf(stderr,
                     "[benchmark] start refused: frame pacing could not be "
                     "disabled\n");
        return false;
    }

    // All preconditions pass: reset the clock, stats, and scene index, clear any
    // prior result, and enter Running. No state was
    // mutated before this point, so a refused start has no side effects.
    player_.reset();
    stats_.reset();
    frame_intervals_.clear();
    gpu_times_ms_.clear();
    segment_frame_intervals_.assign(
        markers_ != nullptr ? markers_->count() : 0, {});
    scene_index_ = 0;
    result_      = RunResult{};
    has_result_  = false;
    diagnostic_microprofile_ = microprofile_recording_;
    diagnostic_frame_generation_ = frame_generation_requested_;
    diagnostic_presentation_trace_ = presentation_trace_requested_;
    diagnostic_  = diagnostic_microprofile_ || diagnostic_frame_generation_ || diagnostic_presentation_trace_;
    state_       = BenchmarkState::Running;
    return true;
}

// ── Cancel ──────────────────────────────────────────────────────

void BenchmarkSystem::cancel() {
    // Only an active run can be cancelled; otherwise this is a safe no-op.
    if (state_ != BenchmarkState::Running) {
        return;
    }

    // Stop the clock and stats and discard the collected statistics so no
    // Result_Report, score, or run statistics from the cancelled run are ever
    // exposed. The clock stops advancing because tick()
    // only advances while Running.
    stats_.reset();
    has_result_ = false;
    result_     = RunResult{};
    state_      = BenchmarkState::Cancelled;
}

// ── Dismiss result ──────────────────────────────────────────────

void BenchmarkSystem::dismiss_result() {
    // Only a completed run carries a Result_Report to dismiss.
    if (state_ != BenchmarkState::Complete) {
        return;
    }

    result_     = RunResult{};
    has_result_ = false;
    state_      = BenchmarkState::Idle;
}

// ── Per-frame tick ──────────────────────────────────────────────

void BenchmarkSystem::tick(float raw_dt_seconds, Camera& camera,
                           bool record_as_presented) {
    // Cancelled is transient: the moment after it is entered, settle to Idle so
    // the renderer is fully interactive again. The camera is
    // left untouched.
    if (state_ == BenchmarkState::Cancelled) {
        state_ = BenchmarkState::Idle;
        return;
    }

    // Every non-Running state leaves the camera alone and keeps the stats and
    // playback clock frozen.
    if (state_ != BenchmarkState::Running) {
        return;
    }

    // Advance the playback clock 1:1 with real elapsed time, with no scaling.
    // The production caller records only after the backend accepts the frame;
    // headless callers retain the original one-call behavior through the
    // default record_as_presented=true argument.
    player_.advance(raw_dt_seconds);

    // Select the active scene from the marker whose time is the latest not
    // exceeding the current playback time.
    if (markers_ != nullptr) {
        scene_index_ = markers_->scene_index_at(player_.time());
    }

    // Drive the camera along the authored path at the current playback time
    // The player evaluates without looping and holds the
    // final keyframe past the end.
    player_.apply(camera);

    // Complete the run once playback reaches or exceeds the path duration
    // enter_complete() freezes the stats and computes the
    // result.
    if (record_as_presented) {
        record_presented_frame(raw_dt_seconds);
    }
}

void BenchmarkSystem::record_presented_frame(float elapsed_seconds) {
    if (state_ != BenchmarkState::Running) return;
    stats_.add_frame(elapsed_seconds);
    if (elapsed_seconds > 0.0f && std::isfinite(elapsed_seconds)) {
        frame_intervals_.push_back(elapsed_seconds);
        if (scene_index_ < segment_frame_intervals_.size()) {
            segment_frame_intervals_[scene_index_].push_back(elapsed_seconds);
        }
    }
    if (player_.at_end()) {
        enter_complete();
    }
}

// ── Completion ──────────────────────────────────────────────────

void BenchmarkSystem::enter_complete() {
    result_.performance = compute_hardware_scores(
        frame_intervals_, segment_frame_intervals_,
        cpu_metrics_, gpu_times_ms_);
    result_.diagnostic = diagnostic_;
    result_.frame_generation_requested = diagnostic_frame_generation_;
    result_.frame_generation = frame_generation_status_;
    if (diagnostic_microprofile_) result_.diagnostic_reason = "microprofiler recording";
    if (diagnostic_frame_generation_) {
        if (!result_.diagnostic_reason.empty()) result_.diagnostic_reason += "; ";
        result_.diagnostic_reason += "frame generation requested";
    }
    if (diagnostic_presentation_trace_) {
        if (!result_.diagnostic_reason.empty()) result_.diagnostic_reason += "; ";
        result_.diagnostic_reason += "presentation timing observation";
    }
    if (diagnostic_) result_.performance.scores = {};
    result_.average_fps = result_.performance.metrics.average_fps;
    result_.min_fps     = stats_.min_fps();
    result_.max_fps     = stats_.max_fps();
    result_.frames      = stats_.frames();
    result_.total_time  = std::accumulate(
        frame_intervals_.begin(), frame_intervals_.end(), 0.0);

    // Gather the System_Info / Settings_Info synchronously within this
    // completing frame, before the Result_Report is first built (Requirement
    // The gatherers bind the impure host/backend
    // sources to the pure RunInfo formatters and return fully-formed
    // descriptions with the Unknown_Value_Placeholder already substituted for
    // any datum they cannot determine, so gathering never blocks or delays the
    // report. When a gatherer is not supplied (the pure
    // unit/property test path) the corresponding field stays default-constructed
    // and the state machine remains OS/backend-free.
    if (gather_system_) {
        result_.system = gather_system_();
    }
    if (gather_settings_) {
        result_.settings = gather_settings_();
    }
    result_.cpu_stress = cpu_stress_metrics_;
    result_.renderer_features = renderer_feature_stats_;

    has_result_         = true;
    state_              = BenchmarkState::Complete;

    // Write the completed run's figures to the application log together with the
    // System_Info and Settings_Info lines, in the same logging style
    // The raw (unrounded) values are logged; the overlay
    // applies display rounding separately. The info lines carry the
    // placeholder-substituted descriptions verbatim (empty when no gatherer was
    // supplied).
    if (diagnostic_) {
        std::fprintf(stderr,
                     "[benchmark] Diagnostic run complete: unscored (%s) | "
                     "average %.2f FPS | frames %llu | time %.3f s\n",
                     result_.diagnostic_reason.c_str(),
                     result_.average_fps,
                     static_cast<unsigned long long>(result_.frames), result_.total_time);
    } else std::fprintf(stderr,
                 "[benchmark] Run complete: score version %d | overall %.0f | "
                 "CPU %.0f | GPU %.0f | average %.2f FPS | 1%% low %.2f FPS | "
                 "frames %llu | time %.3f s\n",
                 result_.performance.score_version,
                 result_.performance.scores.overall,
                 result_.performance.scores.cpu,
                 result_.performance.scores.gpu,
                 result_.average_fps,
                 result_.performance.metrics.one_percent_low_fps,
                 static_cast<unsigned long long>(result_.frames),
                 result_.total_time);
    std::fprintf(stderr,
                 "[benchmark] System_Info: Platform: %s | CPU: %s | GPU: %s\n",
                 result_.system.platform.c_str(),
                 result_.system.cpu.c_str(),
                 result_.system.gpu.c_str());
    std::fprintf(stderr,
                 "[benchmark] Settings_Info: Render API: %s | Display: %s | "
                 "Quality: %s\n",
                 result_.settings.render_api.c_str(),
                 result_.settings.display_mode.c_str(),
                 result_.settings.quality_preset.c_str());
}

// ── Overlay snapshot ────────────────────────────────────────────

BenchmarkStatus BenchmarkSystem::status() const {
    BenchmarkStatus s;
    s.scene_count = markers_ != nullptr ? markers_->count() : 0;

    switch (state_) {
        case BenchmarkState::Running: {
            s.state       = BenchmarkState::Running;
            s.diagnostic  = diagnostic_;
            s.scene_index = scene_index_;
            s.frames      = stats_.frames();
            s.elapsed     = stats_.total_time();
            s.min_fps     = stats_.min_fps();
            s.max_fps     = stats_.max_fps();
            s.fade_alpha  = player_.fade_alpha();

            // Cumulative Average_FPS so far (frames / measured time); the guard
            // returns 0 before any time has accumulated.
            s.average_fps = s.elapsed > 0.0
                ? static_cast<double>(s.frames) / s.elapsed
                : 0.0;
            break;
        }
        case BenchmarkState::Complete: {
            s.state       = BenchmarkState::Complete;
            s.diagnostic  = result_.diagnostic;
            s.scene_index = scene_index_;
            s.average_fps = result_.average_fps;
            s.elapsed     = result_.total_time;
            s.frames      = result_.frames;
            s.min_fps     = result_.min_fps;
            s.max_fps     = result_.max_fps;
            s.has_result  = has_result_;
            s.result      = result_;
            break;
        }
        case BenchmarkState::Idle:
        case BenchmarkState::Cancelled:
        default:
            // Idle, and the transient Cancelled state, present as a clean Idle
            // snapshot: the overlay shows the Menu_Bar with no run statistics or
            // Result_Report.
            s.state = BenchmarkState::Idle;
            break;
    }

    return s;
}

} // namespace heaven
