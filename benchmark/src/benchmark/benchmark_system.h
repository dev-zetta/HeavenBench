/*
 * HeavenBench — Run lifecycle state machine
 *
 * BenchmarkSystem orchestrates a single benchmark run end to end: idle →
 * running → complete / cancelled. It owns the playback clock (via
 * CameraPathPlayer), the per-run statistics (StatsCollector), the active scene
 * index, and the completed-run result. It is driven once per frame from the
 * main loop, which supplies the raw frame delta and the Camera& to steer.
 *
 * Pure CPU logic — no Filament, SDL, or ImGui dependency — so it compiles into
 * the host-runnable test target alongside SceneMarkers, StatsCollector,
 * ScoreCalculator, and the non-looping CameraPath additions. The overlay reads
 * an immutable BenchmarkStatus snapshot and never sees engine types.
 */
#ifndef HEAVENBENCH_SRC_BENCHMARK_BENCHMARK_SYSTEM_H_
#define HEAVENBENCH_SRC_BENCHMARK_BENCHMARK_SYSTEM_H_

#include "benchmark/score_calculator.h"
#include "benchmark/run_info.h"
#include "benchmark/scene_markers.h"
#include "benchmark/stats_collector.h"
#include "engine/camera_path.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace heaven {

class Camera;

// Run lifecycle states. Cancelled is a transient bookkeeping state: the moment
// it is entered the run's stats are discarded and no result is exposed, and the
// system reports Idle to the overlay on the next frame.
enum class BenchmarkState { Idle, Running, Complete, Cancelled };

// Abstracts "is frame pacing off?" so the system can refuse to start
// without depending on the backend type. main.cpp passes a
// probe backed by IRhiBackend::frame_pacing_disabled().
using FramePacingProbe = std::function<bool()>;

// Gathers the host System_Info / run Settings_Info when a run completes. Both
// are injected callbacks so the pure state machine never touches the OS or the
// backend. main.cpp binds them to LinuxHostInfoSource +
// IRhiBackend::gpu_info()/render_api_name() + AppConfig. Each returns
// fully-formed descriptions with the per-datum Unknown_Value_Placeholder
// already substituted, so enter_complete() does no formatting itself. The state
// machine invokes them exactly once, synchronously, inside enter_complete() —
// within the frame the run completes and before the Result_Report is first
// built; a source that cannot determine a datum returns
// the placeholder rather than blocking. Left empty for the
// pure unit/property test path, which keeps those tests OS/backend-free.
using SystemInfoGatherer   = std::function<SystemInfo()>;
using SettingsInfoGatherer = std::function<SettingsInfo()>;

// Snapshot of a completed run, surfaced in the Result_Report.
struct RunResult {
    bool diagnostic = false; // Recording overhead makes this run unscored.
    HardwareScoreResult performance{};
    double   average_fps = 0.0;
    float    min_fps     = 0.0f;
    float    max_fps     = 0.0f;
    uint64_t frames      = 0;
    double   total_time  = 0.0;   // seconds (unclamped, measured)
    SystemInfo   system{};        // Platform / CPU / GPU
    SettingsInfo settings{};      // Render API / Display / Quality
    CpuStressMetrics cpu_stress{};
    RendererFeatureStats renderer_features{};
};

// Immutable per-frame snapshot the overlay reads. No engine types leak in, so
// the overlay widget builder stays decoupled from the system internals.
struct BenchmarkStatus {
    BenchmarkState state       = BenchmarkState::Idle;
    size_t         scene_index = 0;       // zero-based (display as +1)
    size_t         scene_count = 0;
    double         average_fps = 0.0;     // cumulative so far (0 when no time yet)
    double         elapsed     = 0.0;     // total measured time, seconds
    uint64_t       frames      = 0;
    float          min_fps     = 0.0f;
    float          max_fps     = 0.0f;
    float          fade_alpha  = 0.0f;   // scene-cut black fade [0,1] (Running)
    bool           diagnostic  = false; // Sticky for the current/completed tour.
    bool           has_result  = false;   // result report should be shown
    RunResult      result{};
};

class BenchmarkSystem {
public:
    // markers and path are non-owning; both must outlive the system. Either may
    // be null/unusable — request_start() refuses in that case rather than the
    // constructor failing, so the renderer stays interactive

    BenchmarkSystem(const SceneMarkers* markers,
                    const CameraPath*   path,
                    FramePacingProbe    pacing_probe,
                    SystemInfoGatherer   gather_system   = {},
                    SettingsInfoGatherer gather_settings = {});

    // Begin a run. Returns false (and stays Idle, logging the cause) when a run
    // is already active, when the scene markers or camera
    // path are unusable, or when frame pacing cannot be
    // disabled. On success: resets the playback clock, the
    // StatsCollector, and the scene index, clears any prior result, and enters
    // Running.
    bool request_start();

    // Cancel the active run: mark Cancelled, stop the clock and stats, and
    // discard the collected statistics so no result is exposed (Requirement
    // Safe no-op when no run is active.
    void cancel();

    // Dismiss a completed run's Result_Report and return to Idle. No-op unless
    // the system is in the Complete state.
    void dismiss_result();

    // Scene reload replaces the borrowed marker object. Retire any run/result
    // before rebinding; the replacement must outlive this system.
    void set_scene_markers(const SceneMarkers* markers) {
        cancel();
        dismiss_result();
        markers_ = markers;
    }

    // Advance one frame. While Running: advances the clock 1:1 with real time
    // feeds the StatsCollector, updates the
    // scene index from the markers, drives the camera along
    // the path, and transitions to Complete at end-of-path
    // The transient Cancelled state settles to Idle
    // here. In every non-Running state the camera is left
    // untouched and the stats and clock stay frozen.
    void tick(float raw_dt_seconds, Camera& camera,
              bool record_as_presented = true);

    // Commit an actually-presented frame after the renderer accepts it. The
    // main application advances playback with tick(..., false) before drawing,
    // then calls this with the elapsed interval accumulated across any rejected
    // beginFrame attempts. Headless tests keep tick's default true behavior.
    void record_presented_frame(float elapsed_seconds);

    // True after playback has reached the final camera key and the next
    // accepted presented frame will complete the run. The application uses
    // this seam to drain asynchronous GPU timestamps before enter_complete()
    // snapshots the score.
    [[nodiscard]] bool will_complete_on_present() const {
        return state_ == BenchmarkState::Running && player_.at_end();
    }

    // Recording before start seeds the next run; recording at any point while
    // Running permanently retires that run's scores. Stopping recording cannot
    // make an already observed tour comparable to an uninstrumented tour.
    void set_microprofile_recording(bool recording) {
        microprofile_recording_ = recording;
        if (recording && state_ == BenchmarkState::Running) diagnostic_ = true;
    }

    // Replace the provisional per-frame GPU samples with the backend's final,
    // deduplicated and benchmark-bounded set immediately before completion.
    void set_gpu_benchmark_times(std::vector<double> samples) {
        gpu_times_ms_ = std::move(samples);
    }

    // Install the deterministic CPU phase result used by the next graphical
    // run. The application performs a fresh CPU phase before each request.
    void set_cpu_benchmark_metrics(const CpuBenchmarkMetrics& metrics) {
        cpu_metrics_ = metrics;
    }
    void set_cpu_stress_metrics(const CpuStressMetrics& metrics) {
        cpu_stress_metrics_ = metrics;
    }
    void set_renderer_feature_stats(const RendererFeatureStats& stats) {
        renderer_feature_stats_ = stats;
        if (has_result_) result_.renderer_features = stats;
    }

    [[nodiscard]] BenchmarkState  state() const { return state_; }
    [[nodiscard]] bool            is_running() const { return state_ == BenchmarkState::Running; }
    [[nodiscard]] float           time_of_day(float fallback) const {
        return player_.time_of_day(fallback);
    }
    [[nodiscard]] BenchmarkStatus status() const;   // builds the overlay snapshot

private:
    // Compute the RunResult from the collected stats, log it,
    // and enter Complete. The stats are left intact (frozen) so the snapshot and
    // result report read the completed figures.
    void enter_complete();

    const SceneMarkers* markers_;
    const CameraPath*   path_;
    CameraPathPlayer    player_;
    StatsCollector      stats_;
    FramePacingProbe    pacing_probe_;
    SystemInfoGatherer   gather_system_;
    SettingsInfoGatherer gather_settings_;
    BenchmarkState      state_       = BenchmarkState::Idle;
    size_t              scene_index_ = 0;
    CpuBenchmarkMetrics cpu_metrics_{};
    CpuStressMetrics cpu_stress_metrics_{};
    RendererFeatureStats renderer_feature_stats_{};
    std::vector<float> frame_intervals_;
    std::vector<std::vector<float>> segment_frame_intervals_;
    std::vector<double> gpu_times_ms_;
    RunResult           result_{};
    bool                has_result_  = false;
    bool                microprofile_recording_ = false;
    bool                diagnostic_ = false;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_BENCHMARK_BENCHMARK_SYSTEM_H_
