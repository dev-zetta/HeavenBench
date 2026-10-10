/*
 * HeavenBench — Entry point
 *
 * A modern C++23 renderer for the Unigine Heaven scene, layered
 * on Google Filament (Vulkan with OpenGL fallback). The hand-rolled
 * GL 4.5 backend that used to sit alongside it was retired in v0.4.
 *
 * Usage:
 *   --data PATH           Loose assets, ZIP, or ZIP folder (default: assets)
 *   --width W --height H  Window size
 *   --fullscreen          Fullscreen mode
 *   --no-tessellation     Disable CPU tessellation entirely
 *   --no-heat-haze        Disable the chimney refraction grab pass
 */

#include "engine/types.h"
#include "engine/cli_parser.h"
#include "engine/config_file.h"
#include "engine/render_presets.h"
#include "engine/render_test_scene_catalog.h"
#include "engine/render_test_scene.h"
#include "asset_packer/asset_packer.h"
#include "engine/asset_file_system.h"
#include "engine/ground_relief_generator.h"
#include "engine/null_backend.h"
#include "engine/loading_benchmark.h"
#include "engine/staged_scene_load.h"
#include "engine/camera.h"
#include "engine/scene_rhi.h"
#include "engine/material_rhi.h"
#include "engine/path_resolver.h"
#include "engine/runtime_paths.h"
#include "engine/music_player.h"
#include "engine/surface_displacement_policy.h"
#include "engine/camera_path.h"
#include "engine/camera_collision.h"
#include "engine/tessellation_cache_archive.h"
#include "engine/startup_timer.h"
#include "engine/submission_retry.h"
#include "rhi/rhi.h"
#include "rhi/filament_util.h"
#include "rhi/overlay/overlay_ui.h"
#include "rhi/overlay/config_controller.h"
#include "benchmark/stats.h"
#include "benchmark/build_info.h"
#include "benchmark/driver_check.h"
#include "benchmark/cpu_benchmark.h"
#include "benchmark/scene_markers.h"
#include "benchmark/run_info.h"
#include "benchmark/benchmark_system.h"
#include "benchmark/result_report.h"
#include "benchmark/microprofiler.h"
#include "benchmark/microprofile_report.h"
#include "json.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#ifdef __ANDROID__
#include "engine/preview_frame_pacing.h"
#include <SDL2/SDL_syswm.h>
#endif
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <future>
#include <fstream>
#include <memory>
#include <limits>
#include <mutex>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#ifndef HEAVENBENCH_VERSION
#define HEAVENBENCH_VERSION "unknown"
#endif

#ifndef HEAVENBENCH_ARCHITECTURE
#define HEAVENBENCH_ARCHITECTURE "unknown"
#endif

using namespace heaven;

// ── Argument parsing + configuration resolution ─────────────────

// The flag inventory, value parsing, weather/quality handling, and --help
// listing live in the GPU-free, host-testable engine/cli_parser module;
// JSON Config_File load/write/overlay lives in engine/config_file. Both
// deliberately avoid I/O and return collected diagnostics. `resolve_config`
// is the orchestrator that owns ALL printing and exit decisions: it routes
// every diagnostic (CLI and config alike) through the same fprintf(stderr,…)
// path, prints --help unchanged, and applies the precedence CLI > file >
// default before handing the Effective_Configuration to Application.

// Startup flow:
//   1. parse_cli() → emit its diagnostics.
//   2. --help → print the generated listing + Controls footer, exit(0).
//   3. Start from defaults and load --config FILE when provided. The repository
//      launcher supplies the canonical root config.json explicitly. Load
//      failures leave the current config unchanged and are NOT fatal — we print
//      diagnostics and continue into the render loop.
//   4. apply_cli_overlay() re-applies the flags actually present on the
//      command line so CLI wins over the file.
//   5. --write-config → serialize the Effective_Configuration, emit the
//      writer's diagnostics, and exit without entering the render loop.
//      A write failure returns a nonzero process status.
//   6. Otherwise return the Effective_Configuration to Application.
// Used-asset packer activation intent surfaced out of the parsed command line
// so main() can branch to the packer BEFORE constructing the renderer (no
// window, no backend, no writes on the renderer path). The four fields mirror
// the corresponding ParseResult members and are kept as a small POD so
// resolve_config can keep owning all diagnostic
// printing while main owns the renderer-vs-packer branch.
struct PackInvocation {
    bool                       requested = false;  // --pack-assets token seen
    std::optional<std::string> target;             // --pack-assets value
    bool                       move      = false;  // --pack-move
    bool                       overwrite = false;  // --pack-overwrite
    std::optional<std::string> texture_set_manifest;
};

struct ResolvedConfig {
    BenchConfig    config;
    PackInvocation pack;
    bool build_tessellation_cache = false;
    std::optional<std::string> tessellation_cache_target;
    bool benchmark_loading = false;
    std::optional<std::string> loading_benchmark_output;
    std::string    config_path = "config.json";
    VisualCaptureOptions capture;
    MicroprofileOptions microprofile;
};

namespace {

constexpr const char* kConfigFileName = "config.json";
const ApplicationBuildInfo kApplicationBuildInfo{
    HEAVENBENCH_VERSION,
    HEAVENBENCH_ARCHITECTURE,
};

std::optional<std::filesystem::path> executable_dir_from_invocation(
        const std::optional<std::filesystem::path>& invocation) {
    namespace fs = std::filesystem;

    if (char* base = SDL_GetBasePath()) {
        fs::path result(base);
        SDL_free(base);
        if (!result.empty()) return result;
    }

    fs::path exe_dir;
    if (invocation && !invocation->empty()) {
        exe_dir = invocation->parent_path();
    }

    if (exe_dir.empty()) {
        std::error_code ec;
        const fs::path resolved = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) exe_dir = resolved.parent_path();
    }

    if (!exe_dir.empty()) {
        return exe_dir;
    }

    return std::nullopt;
}

std::filesystem::path executable_dir_or_cwd(
        const std::optional<std::filesystem::path>& invocation) {
    namespace fs = std::filesystem;

    if (auto exe_dir = executable_dir_from_invocation(invocation)) {
        return *exe_dir;
    }

    std::error_code ec;
    fs::path cwd = fs::current_path(ec);
    if (!ec) {
        return cwd;
    }

    return {};
}

std::optional<std::filesystem::path> runtime_user_directory() {
    if (auto configured = runtime_path_from_environment("HEAVEN_USER_DIR")) {
        return configured;
    }
#ifdef __ANDROID__
    if (const char* external = SDL_AndroidGetExternalStoragePath();
        external != nullptr && *external != '\0') {
        return std::filesystem::path(external);
    }
    if (char* preferred = SDL_GetPrefPath("zetta", "HeavenBench")) {
        std::filesystem::path result(preferred);
        SDL_free(preferred);
        if (!result.empty()) return result;
    }
    throw std::runtime_error("Cannot determine writable Android application storage");
#else
    return std::nullopt;
#endif
}

std::string relative_config_label(const std::string& path) {
    namespace fs = std::filesystem;

    const fs::path p(path);
    if (p.is_relative()) {
        return p.generic_string();
    }

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        const fs::path rel = fs::relative(p, cwd, ec);
        if (!ec && !rel.empty()) {
            return rel.generic_string();
        }
    }

    return p.filename().generic_string();
}

bool load_config_into(heaven::BenchConfig& cfg,
                      const std::string& path,
                      const std::string& display_path,
                      const std::function<void(const std::vector<std::string>&)>& emit) {
    heaven::ConfigLoadResult lr = heaven::load_config_file(path, cfg);
    emit(lr.diagnostics);
    cfg = lr.config;
    if (lr.parsed_ok) {
        std::printf("Loaded config file: %s\n", display_path.c_str());
    }
    return lr.parsed_ok;
}

ResolvedConfig resolve_config(std::span<const std::string_view> args,
                              const RuntimePaths& runtime_paths) {
    heaven::ParseResult r = heaven::parse_cli(args);

    // The single diagnostic sink: every CLI/config message goes to stderr
    // through the existing path.
    auto emit = [](const std::vector<std::string>& msgs) {
        for (const std::string& msg : msgs)
            std::fprintf(stderr, "%s\n", msg.c_str());
    };
    emit(r.diagnostics);

    // Surface the packer activation intent for main()'s startup branch. This is
    // captured from the parsed result (not a BenchConfig field) exactly like
    // --config/--write-config. --pack-assets takes an optional value; when none
    // is present pack_target stays unset and main defaults it to "assets", so
    // there is no missing-value diagnostic to print here.
    PackInvocation pack;
    pack.requested = r.pack_requested;
    pack.target    = r.pack_target;
    pack.move      = r.pack_move;
    pack.overwrite = r.pack_overwrite;
    pack.texture_set_manifest = r.pack_texture_set;

    if (r.help_requested) {
        std::printf("%s", heaven::generate_help().c_str());
        std::printf(
            "\nControls: WASD=move, QE=up/down, Right-Mouse(hold)=look, "
            "Shift=fast, ESC=cancel run / quit\n"
            "          B=start benchmark run, C=toggle camera collision\n");
        std::exit(0);
    }
    if (r.list_test_scenes_requested) {
        for (const auto& scene : render_test_scene_catalog()) {
            std::printf("%-20s %.*s\n  %.*s\n", std::string(scene.id).c_str(),
                        static_cast<int>(scene.title.size()), scene.title.data(),
                        static_cast<int>(scene.description.size()), scene.description.data());
        }
        std::exit(0);
    }
    if (r.test_scene_invalid) throw std::runtime_error("Unknown render test scene; see --list-test-scenes");

    // Explicit files keep CLI precedence. Packaged launches also load an
    // existing config from their writable user root; development launchers
    // continue selecting the repository configuration explicitly.
    heaven::BenchConfig cfg;
    if (const auto default_data = runtime_path_from_environment("HEAVEN_DEFAULT_DATA")) {
        cfg.data_dir = default_data->string();
    }
    std::string config_path = runtime_paths.default_config_file().string();
    if (r.config_path) {
        // Preserve the CLI's cwd-relative file when the UI later saves it.
        config_path = std::filesystem::absolute(*r.config_path).string();
        load_config_into(cfg, *r.config_path,
                         relative_config_label(*r.config_path), emit);
    } else if (runtime_paths.user_directory()) {
        std::error_code error;
        if (std::filesystem::exists(config_path, error)) {
            load_config_into(cfg, config_path, config_path, emit);
        }
    }

    // Re-apply only the flags actually present on the command line so the CLI
    // wins where it overlaps the file: CLI > file > default.
    heaven::apply_cli_overlay(cfg, r);
    if (!cfg.render_test_scene.empty() &&
        (cfg.benchmark_mode || pack.requested || r.build_tessellation_cache_requested ||
         r.benchmark_loading_requested)) {
        throw std::runtime_error("Render test scenes cannot be combined with Heaven packing or scored/loading benchmarks");
    }
    if (r.microprofile.invalid) throw std::runtime_error("Invalid microprofiler options");
    if (r.capture.invalid || ((r.capture.output || r.capture.profile_output) && cfg.benchmark_mode)) {
        throw std::runtime_error("Invalid diagnostic options; capture/profile cannot be combined with a scored benchmark");
    }
    if (runtime_paths.user_directory() && cfg.tessellation.cache_file.empty()) {
        cfg.tessellation.cache_file = runtime_paths.tessellation_cache_file("", "", false);
    }

    // --write-config: serialize the Effective_Configuration and exit without
    // entering the render loop; report failures to calling scripts.
    if (r.write_config_path) {
        const auto diagnostics = heaven::write_config_file(*r.write_config_path, cfg);
        emit(diagnostics);
        std::exit(diagnostics.empty() ? 0 : 1);
    }

    return ResolvedConfig{
        std::move(cfg),
        std::move(pack),
        r.build_tessellation_cache_requested,
        std::move(r.tessellation_cache_target),
        r.benchmark_loading_requested,
        std::move(r.loading_benchmark_output),
        std::move(config_path), std::move(r.capture), std::move(r.microprofile)};
}

// ── Application ─────────────────────────────────────────────────

// Gather the selectable window resolutions for the overlay's Settings tab: the
// monitor's supported display modes plus a handful of common windowed sizes
// that fit within the desktop, de-duplicated and sorted largest-first. SDL is
// the source (the pure OverlayUI stays SDL-free and receives this list via
// set_available_resolutions). Returns an empty list if SDL cannot report modes,
// in which case the overlay falls back to plain Width/Height number fields.
std::vector<std::pair<int, int>> gather_window_resolutions(SDL_Window* win) {
#ifdef __ANDROID__
    // The Activity owns the Android Surface; desktop window modes cannot
    // resize it. Offer only its current pixel extent.
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && SDL_strcmp(driver, "android") == 0) {
        int width = 0;
        int height = 0;
        if (win) get_drawable_size(win, width, height);
        if (width > 0 && height > 0) return {{width, height}};
        return {};
    }
#endif

    std::vector<std::pair<int, int>> out;
    auto add_unique = [&out](int w, int h) {
        if (w <= 0 || h <= 0) return;
        for (const auto& p : out)
            if (p.first == w && p.second == h) return;
        out.emplace_back(w, h);
    };

    int disp = win ? SDL_GetWindowDisplayIndex(win) : 0;
    disp = std::max(disp, 0);

    // Desktop extent, used to filter the common windowed sizes so we never offer
    // a window larger than the screen.
    int desk_w = 0, desk_h = 0;
    SDL_DisplayMode desktop;
    if (SDL_GetDesktopDisplayMode(disp, &desktop) == 0) {
        desk_w = desktop.w;
        desk_h = desktop.h;
    }

    // The monitor's advertised modes (fullscreen-capable resolutions).
    const int mode_count = SDL_GetNumDisplayModes(disp);
    for (int i = 0; i < mode_count; ++i) {
        SDL_DisplayMode mode;
        if (SDL_GetDisplayMode(disp, i, &mode) == 0) {
            add_unique(mode.w, mode.h);
        }
    }

    // Common windowed resolutions, included when they fit within the desktop.
    constexpr std::array kCommon = {
        std::pair{1280, 720},  std::pair{1366, 768},
        std::pair{1600, 900},  std::pair{1920, 1080},
        std::pair{2560, 1440}, std::pair{3440, 1440},
        std::pair{3840, 2160},
    };
    for (const auto& [w, h] : kCommon) {
        if ((desk_w == 0 || w <= desk_w) && (desk_h == 0 || h <= desk_h)) {
            add_unique(w, h);
        }
    }

    // Largest-first so the dropdown reads top-down from highest resolution.
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    return out;
}

class Application {
public:
    Application(BenchConfig config, RuntimePaths runtime_paths,
                std::string config_path, VisualCaptureOptions capture,
                MicroprofileOptions microprofile)
        : config_(std::move(config)),
          runtime_paths_(std::move(runtime_paths)),
          capture_(std::move(capture)), microprofile_(std::move(microprofile)) {
        profile::set_recorder(&micro_recorder_);
        micro_recorder_.set_thread_name("Main");
        config_ui_.current_config_name =
            config_path.empty() ? std::string(kConfigFileName) : std::move(config_path);
    }

    // Deterministic teardown: stop the Music_Player here, in the destructor
    // BODY, so it runs before ANY member is destroyed — in particular before
    // window_ (declared first, so destroyed last) runs SDL_Quit() in its
    // destructor (rhi.cpp). stop() pauses the callback, closes the audio
    // device, and joins the music thread, so no SDL audio callback can fire
    // against a torn-down SDL or a freed PCM buffer. It is idempotent
    // and safe when start() was a no-op or was never reached (init failure).
    ~Application() {
        if (music_player_) music_player_->stop();
        // Producer callbacks borrow the recorder. Drain the backend before
        // removing that recorder, including error and interrupted-load paths.
        backend_.reset();
        micro_writer_.wait();
        profile::set_recorder(nullptr);
    }


    int run() {
        if (microprofile_.output_base) {
            micro_recorder_.begin_capture(std::numeric_limits<uint64_t>::max());
            update_microprofile_metadata("startup");
        }
        if (auto r = init(); !r) {
            if (microprofile_.output_base) save_microprofile(*microprofile_.output_base + ".startup");
            if (exit_requested_) return 0;
            std::fprintf(stderr, "Initialization failed: %s\n", r.error().message.c_str());
            return 1;
        }

        if (microprofile_.output_base) {
            bench_->set_microprofile_recording(true);
            if (microprofile_.visible) overlay_ui_.set_microprofiler_open(true);
        }

        stats_.reset_frame_clock();
        main_loop();

        if (microprofile_.output_base && !microprofile_finished_) {
            // Shutdown may overtake the startup export on short/cancelled runs.
            // Join its worker here, outside the interactive render loop.
            micro_writer_.wait();
            const auto export_status = micro_writer_.status();
            if (export_status.state == profile::ReportState::Error) {
                microprofile_export_error_ = true;
                microprofile_export_error_message_ = export_status.error;
            }
            save_microprofile(*microprofile_.output_base + (microprofile_runtime_started_ ? "" : ".startup"));
            microprofile_finished_ = true;
        }
        micro_writer_.wait();
        if (micro_writer_.status().state == profile::ReportState::Error || microprofile_export_error_) {
            const auto message = microprofile_export_error_message_.empty()
                ? micro_writer_.status().error : microprofile_export_error_message_;
            std::fprintf(stderr, "Microprofile export failed: %s\n", message.c_str());
            return 1;
        }

        if (retired_reload_error_) {
            std::fprintf(stderr, "%s\n", retired_reload_error_->message.c_str());
            return 1;
        }

        if (capture_.output) {
            if (!capture_finished_) {
                std::fprintf(stderr, "Capture interrupted before completion\n");
                return 1;
            }
            return 0;
        }

        if (capture_.profile_output && !profile_finished_) {
            std::fprintf(stderr, "Profile interrupted before completion\n");
            return 1;
        }

        stats_.print_results();
        print_texture_stats();

        return 0;
    }

private:
    void update_microprofile_metadata(std::string_view phase) {
        if (microprofile_metadata_capture_id_ != micro_recorder_.session_id()) {
            microprofile_metadata_capture_id_ = micro_recorder_.session_id();
            microprofile_resolution_width_ = window_.width;
            microprofile_resolution_height_ = window_.height;
            microprofile_initial_overlay_visible_ = overlay_ui_.microprofiler_open();
            microprofile_overlay_mixed_ = false;
            micro_recorder_.set_metadata("effective_settings", serialize_config(config_));
            micro_recorder_.set_metadata("settings_mixed", "false");
            micro_recorder_.set_metadata("overlay_visible", microprofile_initial_overlay_visible_ ? "true" : "false");
            micro_recorder_.set_metadata("camera_start", nlohmann::json{
                camera_.position.x, camera_.position.y, camera_.position.z,
                camera_.yaw, camera_.pitch, camera_.fov}.dump());
            micro_recorder_.set_metadata("resolution", std::to_string(window_.width) + "x" + std::to_string(window_.height));
        }
        micro_recorder_.set_metadata("phase", std::string(phase));
        micro_recorder_.set_metadata("application_version", HEAVENBENCH_VERSION);
        micro_recorder_.set_metadata("architecture", HEAVENBENCH_ARCHITECTURE);
        micro_recorder_.set_metadata("benchmark_score", "false");
        micro_recorder_.set_metadata("application_overlay", capture_.output || capture_.profile_output
            ? "capture: profiler only or hidden" : "application controls");
        microprofile_overlay_changed();
        if (microprofile_overlay_mixed_) micro_recorder_.set_metadata("overlay_visible_final",
            overlay_ui_.microprofiler_open() ? "true" : "false");
        micro_recorder_.set_metadata("cpu", active_system_info_.cpu);
        micro_recorder_.set_metadata("platform", active_system_info_.platform);
        micro_recorder_.set_metadata("camera_end", nlohmann::json{
            camera_.position.x, camera_.position.y, camera_.position.z,
            camera_.yaw, camera_.pitch, camera_.fov}.dump());
        if (backend_) {
            micro_recorder_.set_metadata("api", backend_->render_api_name());
            micro_recorder_.set_metadata("gpu", active_gpu_name_);
            micro_recorder_.set_metadata("driver", active_gpu_info_.driver_name);
            micro_recorder_.set_metadata("driver_version", active_gpu_info_.driver_version);
            const auto capability = backend_->trace_capabilities();
            micro_recorder_.set_metadata("gpu_timestamps", capability.hardware_timestamps ? "hardware" : "unavailable");
            micro_recorder_.set_metadata("gpu_timing_reason", capability.reason);
            micro_recorder_.set_metadata("gpu_timer_backend", capability.backend);
            micro_recorder_.set_metadata("gpu_timestamp_valid_bits", std::to_string(capability.timestamp_valid_bits));
            micro_recorder_.set_metadata("gpu_timestamp_period_ns", std::to_string(capability.timestamp_period_ns));
            micro_recorder_.set_metadata("gpu_pass_limit", "256");
            micro_recorder_.set_metadata("gpu_unresolved_frame_limit", "16");
            micro_recorder_.set_metadata("gpu_clock", "frame-relative; uncalibrated with CPU");
        }
    }

    void microprofile_configuration_changed() {
        if (!micro_recorder_.active()) return;
        micro_recorder_.set_metadata("settings_mixed", "true");
        micro_recorder_.set_metadata("effective_settings_final", serialize_config(config_));
        micro_recorder_.set_metadata("resolution_final", std::to_string(window_.width) + "x" + std::to_string(window_.height));
        micro_recorder_.counter("Effective settings changed", 1, "Configuration");
    }

    void microprofile_overlay_changed() {
        if (!micro_recorder_.active() || microprofile_overlay_mixed_ ||
            microprofile_metadata_capture_id_ != micro_recorder_.session_id() ||
            microprofile_initial_overlay_visible_ == overlay_ui_.microprofiler_open()) return;
        microprofile_overlay_mixed_ = true;
        micro_recorder_.set_metadata("settings_mixed", "true");
        micro_recorder_.set_metadata("overlay_visible_final", overlay_ui_.microprofiler_open() ? "true" : "false");
        micro_recorder_.counter("Profiler visibility changed", 1, "Configuration");
    }

    std::filesystem::path microprofile_path(std::string_view requested = {}) const {
        const auto directory = runtime_paths_.user_directory().value_or(std::filesystem::path(".")) / "profiles";
        if (!requested.empty()) {
            const std::filesystem::path path(requested);
            return path.is_absolute() ? path : directory / path;
        }
        return directory / ("capture-" + std::to_string(profile::Recorder::steady_now_ns()));
    }

    void save_microprofile(const std::filesystem::path& prefix) {
        micro_recorder_.set_active(false);
        if (bench_) bench_->set_microprofile_recording(false);
        if (backend_) {
            auto context = micro_recorder_.frame_context();
            context.capture_id = micro_recorder_.session_id();
            backend_->set_trace_frame_context(context);
        }
        update_microprofile_metadata(microprofile_runtime_started_ ? "runtime" : "startup");
        if (!micro_writer_.submit(micro_recorder_.snapshot(), prefix)) {
            microprofile_export_error_ = true;
            microprofile_export_error_message_ = "export worker is busy";
            microprofile_feedback_ = "Microprofile export is busy; capture retained in memory";
        } else {
            microprofile_feedback_ = "Saving " + prefix.string();
            std::printf("Microprofile export: %s.{json,html}\n", prefix.string().c_str());
        }
    }

    void service_microprofiler() {
        const auto& intents = last_intents_.microprofile;
        microprofile_overlay_changed();
        if (intents.start) {
            microprofile_start_requested_ = true;
            microprofile_history_ns_ = static_cast<uint64_t>(intents.duration_seconds * 1e9);
        }
        if (intents.stop) micro_recorder_.set_active(false);
        if (bench_) bench_->set_microprofile_recording(micro_recorder_.active());
        overlay_ui_.set_microprofiler_recording(micro_recorder_.active());
        if (intents.save) {
            const auto snapshot = intents.snapshot ? *intents.snapshot : micro_recorder_.snapshot();
            const auto prefix = microprofile_path(intents.output_base);
            if (micro_writer_.submit(snapshot, prefix)) microprofile_feedback_ = "Saving " + prefix.string();
            else microprofile_feedback_ = "Export busy; previous capture is still being saved";
        }
        const auto now = profile::Recorder::steady_now_ns();
        if (overlay_ui_.microprofiler_open() && now - microprofile_snapshot_ns_ >= 250'000'000ull) {
            profile::Scope scope("Profiler snapshot", "Profiler");
            microprofile_snapshot_ns_ = now;
            update_microprofile_metadata("runtime");
            const auto status = micro_writer_.status();
            if (status.state == profile::ReportState::Complete) microprofile_feedback_ = "Saved " + status.prefix;
            else if (status.state == profile::ReportState::Error) microprofile_feedback_ = status.error;
            else if (status.state == profile::ReportState::Writing)
                microprofile_feedback_ = "Saving " + std::to_string(static_cast<int>(status.progress * 100.0f)) + "%";
            overlay_ui_.set_microprofiler_snapshot(std::make_shared<const profile::Snapshot>(micro_recorder_.snapshot()),
                microprofile_feedback_);
        }
    }

    struct TraceIteration {
        profile::Recorder& recorder;
        profile::Context context;
        profile::FrameKind kind = profile::FrameKind::Render;
        profile::FrameOutcome outcome = profile::FrameOutcome::Rejected;
        TraceIteration(profile::Recorder& rec, profile::Context c,
                       profile::FrameKind k = profile::FrameKind::Render) : recorder(rec), context(c), kind(k) {
            recorder.set_frame_context(context);
            recorder.frame_begin(context, kind);
        }
        void finish(profile::FrameOutcome result, uint64_t submission = 0) {
            context.submission_id = submission;
            outcome = result;
        }
        ~TraceIteration() { recorder.frame_end(context, outcome, kind); }
    };

    // Loading is modal for configuration commit, but the video owner keeps
    // pumping events and drawing. CPU tasks never receive the real backend.
    void loading_frame(std::string_view text, float progress, bool cancellable = true) {
        profile::Context context{++microprofile_attempt_id_, 0, 0, 0, 0, micro_recorder_.session_id()};
        profile::ContextScope inherited_context(micro_recorder_, context);
        TraceIteration iteration(micro_recorder_, context, profile::FrameKind::Loading);
        profile::Scope scope("Loading splash and events", "Loading");
        if (backend_) backend_->set_trace_frame_context(context);
        SDL_Event event;
        bool refresh_surface = false;
        while (SDL_PollEvent(&event)) {
            // Keep releases and keyboard state in the main overlay current
            // while its widgets are hidden by the modal loading screen.
            static_cast<void>(backend_->overlay_handle_event(&event));
            if (event.type == SDL_QUIT ||
                (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                loading_cancelled_ = true;
                exit_requested_ = true;
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                loading_cancelled_ = true;
                if (!bench_) exit_requested_ = true;
            }
#ifdef __ANDROID__
            if (event.type == SDL_APP_WILLENTERBACKGROUND) {
                loading_paused_ = true;
                loading_resume_pending_ = false;
            }
            if (event.type == SDL_APP_DIDENTERFOREGROUND) {
                loading_paused_ = true;
                loading_resume_pending_ = true;
            }
#endif
            if (event.type == SDL_WINDOWEVENT &&
                (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                 event.window.event == SDL_WINDOWEVENT_RESIZED ||
                 event.window.event == SDL_WINDOWEVENT_RESTORED)) refresh_surface = true;
        }
#ifdef __ANDROID__
        if (loading_resume_pending_) {
            int width = 0, height = 0;
            get_drawable_size(window_.window, width, height);
            SDL_SysWMinfo native_info{};
            SDL_VERSION(&native_info.version);
            if (width > 0 && height > 0 &&
                SDL_GetWindowWMInfo(window_.window, &native_info) == SDL_TRUE &&
                native_info.subsystem == SDL_SYSWM_ANDROID && native_info.info.android.window) {
                backend_->resize(width, height);
                submission_retry_.reset();
                window_.width = width;
                window_.height = height;
                config_.width = working_config_.width = width;
                config_.height = working_config_.height = height;
                config_ui_.dirty = is_dirty(working_config_, config_);
                overlay_ui_.set_available_resolutions(gather_window_resolutions(window_.window));
                loading_paused_ = false;
                loading_resume_pending_ = false;
                refresh_surface = false;
            }
        }
#endif
        if (!loading_paused_ && refresh_surface) {
            int width = 0, height = 0;
            get_drawable_size(window_.window, width, height);
            if (width > 0 && height > 0) {
                window_.width = width;
                window_.height = height;
                backend_->resize(width, height);
                submission_retry_.reset();
#ifdef __ANDROID__
                config_.width = working_config_.width = width;
                config_.height = working_config_.height = height;
                config_ui_.dirty = is_dirty(working_config_, config_);
                overlay_ui_.set_available_resolutions(gather_window_resolutions(window_.window));
#endif
            }
        }
        if (!loading_paused_ && window_.width > 0 && window_.height > 0) {
            backend_->show_loading_screen(config_.data_dir,
                loading_cancelled_ ? "Cancelling loading..." : text,
                window_.width, window_.height, progress);
            ++loading_frames_;
            // This legacy void splash API does not report beginFrame acceptance.
            // Keep that outcome unknown rather than inventing a rejected frame.
            iteration.finish(profile::FrameOutcome::Pending);
        }
        if (loading_cancelled_) iteration.finish(profile::FrameOutcome::Cancelled);
        if (cancellable && loading_cancelled_) throw std::runtime_error("Loading cancelled");
    }

    template<class Work>
    auto loading_task(std::string_view initial_status, Work work) {
        profile::Scope loading_scope("Dispatch and join loading task", "Loading");
        const auto task_context = micro_recorder_.frame_context();
        const auto task_id = ++microprofile_flow_id_;
        micro_recorder_.flow(task_id, profile::FlowPhase::Begin, initial_status, task_context);
        using Output = std::invoke_result_t<Work, const LoadingProgressCallback&>;
        struct Mailbox {
            std::mutex mutex;
            std::string text;
            float fraction = -1.0f;
            std::atomic<bool> cancel = false;
        } mailbox;
        mailbox.text = initial_status;
        const LoadingProgressCallback report = [&](std::string_view text, float fraction) {
            if (mailbox.cancel.load()) throw std::runtime_error("Loading cancelled");
            std::lock_guard lock(mailbox.mutex);
            mailbox.text = text;
            mailbox.fraction = fraction;
        };
        const auto frames_before = loading_frames_;
        auto future = std::async(std::launch::async, [&]() -> Output {
            profile::ContextScope context(micro_recorder_, task_context);
            micro_recorder_.set_thread_name("Scene loader");
            micro_recorder_.flow(task_id, profile::FlowPhase::Step, initial_status, task_context);
            profile::Scope worker_scope("Scene loading worker", "Loading");
            try { return work(report); }
            catch (const std::exception& error) { return std::unexpected(Error{error.what()}); }
            catch (...) { return std::unexpected(Error{"Loading worker failed"}); }
        });
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            std::string text;
            float fraction;
            {
                std::lock_guard lock(mailbox.mutex);
                text = mailbox.text;
                fraction = mailbox.fraction;
            }
            loading_frame(text, fraction, false);
            if (loading_cancelled_) mailbox.cancel.store(true);
            SDL_Delay(16);
        }
        auto result = future.get(); // Join before borrowed assets/locals can expire.
        micro_recorder_.flow(task_id, profile::FlowPhase::End, initial_status, task_context);
        std::printf("[loading] %.*s: %llu UI frames while CPU worker ran\n",
            static_cast<int>(initial_status.size()), initial_status.data(),
            static_cast<unsigned long long>(loading_frames_ - frames_before));
        if (loading_cancelled_) return Output{std::unexpected(Error{"Loading cancelled"})};
        return result;
    }

    struct CpuSceneLoad {
        RhiScene scene;
        RhiMaterialDatabase materials;
        CameraCollision collision;
    };

    Result<CpuSceneLoad> load_scene_in_background(const AppConfig& settings,
                                                 const std::string& cache_file) {
        if (!assets_ || !paths_) return std::unexpected(Error{"Missing loading asset context"});
        const float radius = camera_collision_.radius();
        const bool collision_enabled = camera_collision_.enabled();
        return loading_task("Preparing scene", [this, settings, cache_file, radius, collision_enabled]
                (const LoadingProgressCallback& progress) -> Result<CpuSceneLoad> {
            CpuSceneLoad prepared;
            prepared.collision.set_radius(radius);
            prepared.collision.set_enabled(collision_enabled);
            progress("Loading materials", -1.0f);
            prepared.materials.build(*paths_, nullptr, std::cref(*assets_));
            auto scene = stage_scene_load(settings, prepared.materials, cache_file,
                &prepared.collision, std::cref(*assets_), progress);
            if (!scene) return std::unexpected(scene.error());
            prepared.scene = std::move(*scene);
            // Staging uses texture sentinels. Real resolution must use the live
            // TextureStore after the owner-thread configuration is established.
            prepared.materials.clear_resolved_cache();
            return prepared;
        });
    }

    Result<void> upload_prepared_scene(RhiScene& scene, const AppConfig& settings) {
        const LoadingProgressCallback progress = [this](std::string_view text, float fraction) {
            loading_frame(text, fraction);
            while (loading_paused_ && !loading_cancelled_) {
                loading_frame(text, fraction);
                SDL_Delay(16);
            }
        };
        try { return upload_staged_scene(scene, *backend_, settings, progress); }
        catch (const std::exception& error) { return std::unexpected(Error{error.what()}); }
    }

    GpuMaterial resolve_scene_material(const std::string& mesh_path,
                                       const std::string& surface_name,
                                       const AppConfig& settings) {
        if (render_test_) return render_test_->resolve_material(mesh_path, surface_name);
        if (!paths_) throw std::logic_error("Cannot resolve scene materials before asset paths are initialized");
        const auto& paths = *paths_;
        GpuMaterial material = materials_.resolve(
            mesh_path, surface_name, paths, *backend_);
        const MaterialDef* definition =
            materials_.get_material_for_surface(
                mesh_path, surface_name);
        const auto policy = resolve_surface_displacement_policy(
            settings, paths, mesh_path, surface_name,
            definition ? definition->name : material.material_name);
        if (policy) {
            material.height =
                backend_->load_texture(policy->height_path);
            material.ground_relief_shading =
                policy->kind == SurfaceDisplacementKind::VillageGround;
            material.geometry_displacement =
                settings.tessellation.enabled && material.height != 0;
            material.displaced_surface_normal =
                material.geometry_displacement &&
                policy->derive_displaced_normal;
            material.tess_length = policy->tessellation_length;
            material.tess_power = policy->displacement_power;
            material.tess_distance = policy->displacement_distance;
            material.tess_threshold = policy->displacement_threshold;
            material.tess_attenuation = policy->displacement_attenuation;
            material.tess_factor = policy->tessellation_factor;
            material.tess_scale = policy->displacement_scale;
        }
        material.geometry_displacement =
            material.geometry_displacement && settings.tessellation.enabled;
        material.displaced_surface_normal =
            material.displaced_surface_normal && material.geometry_displacement;
        return material;
    }

    Result<void> prepare_displacement(const AppConfig& settings) {
        const auto resolve = [this, &settings](const std::string& mesh, const std::string& surface) {
            return resolve_scene_material(mesh, surface, settings);
        };
        const LoadingProgressCallback progress = [this](std::string_view text, float fraction) {
            loading_frame(text, fraction);
            while (loading_paused_ && !loading_cancelled_) {
                loading_frame(text, fraction);
                SDL_Delay(16);
            }
        };
        try { return backend_->prepare_static_displacement(scene_, resolve, progress); }
        catch (const std::exception& error) { return std::unexpected(Error{error.what()}); }
    }

    // These controls are startup actions, separate from saved preferences and
    // benchmark scoring. The sidecar records the actual camera and drawable.
    void initialize_capture_camera() {
        if (!config_.render_test_scene.empty()) {
            const auto scenes = render_test_scene_catalog();
            const auto it = std::ranges::find_if(scenes, [this](const auto& s) {
                return s.id == config_.render_test_scene;
            });
            if (it == scenes.end()) throw std::runtime_error("Unknown render test scene");
            const auto& pose = it->camera_pose;
            camera_.position = {pose[0], pose[1], pose[2]};
            camera_.yaw = pose[3];
            camera_.pitch = pose[4];
            camera_.fov = it->fov;
            camera_collision_.set_enabled(false);
        } else camera_.fov = capture_.fov;
        if (capture_.tour_time) {
            if (camera_path_.keyframes.empty() || *capture_.tour_time > camera_path_.duration) {
                throw std::runtime_error("Camera tour time is outside the loaded path");
            }
            camera_path_.apply_clamped(*capture_.tour_time, camera_);
            config_.time_of_day = camera_path_.time_of_day_clamped(
                *capture_.tour_time, config_.time_of_day);
        } else if (capture_.camera_pose) {
            const auto& pose = *capture_.camera_pose;
            camera_.position = {pose[0], pose[1], pose[2]};
            camera_.yaw = pose[3];
            camera_.pitch = pose[4];
            camera_.fov = capture_.fov;
        }
        if (capture_.fov_explicit) camera_.fov = capture_.fov;
        if (capture_.output) {
            const auto parent = std::filesystem::path(*capture_.output).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            camera_collision_.set_enabled(false);
        }
        if (capture_.profile_output) {
            const auto parent = std::filesystem::path(*capture_.profile_output).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            camera_collision_.set_enabled(false);
        }
    }

    [[nodiscard]] std::string capture_path() const {
        if (!capture_.output) throw std::logic_error("Cannot finish capture without an output path");
        if (capture_index_ == 0) return *capture_.output;
        const std::filesystem::path path(*capture_.output);
        return (path.parent_path() / std::format("{}.{:06d}{}",
            path.stem().string(), capture_index_, path.extension().string())).string();
    }

    void finish_capture() {
        const auto image_path = capture_path();
        const std::string config_path = image_path + ".config.json";
        const auto errors = write_config_file(config_path, config_);
        if (!errors.empty()) throw std::runtime_error(errors.front());
        nlohmann::json metadata{
            {"schema", 1}, {"image", image_path}, {"config", config_path},
            {"render_test_scene", config_.render_test_scene},
            {"sequence_index", capture_index_}, {"sequence_count", capture_.count},
            {"width", window_.width}, {"height", window_.height},
            {"camera_position", {camera_.position.x, camera_.position.y, camera_.position.z}},
            {"camera_yaw", camera_.yaw}, {"camera_pitch", camera_.pitch}, {"camera_fov", camera_.fov},
            {"tour_time", capture_.tour_time ? nlohmann::json(capture_tour_time()) : nlohmann::json(nullptr)},
            {"tour_start_time", capture_.tour_time ? nlohmann::json(*capture_.tour_time) : nlohmann::json(nullptr)},
            {"tour_duration", capture_.tour_time ? nlohmann::json(camera_path_.duration) : nlohmann::json(nullptr)},
            {"tour_motion", capture_.tour_motion},
            {"time_of_day", config_.time_of_day}, {"simulation_time", render_time_},
            {"presented_frames", capture_presented_frames_},
            {"fixed_step", capture_.realtime ? nlohmann::json(nullptr) : nlohmann::json(1.0 / capture_.fps)},
            {"realtime", capture_.realtime},
            {"floating_islands_frozen", !capture_.floating_motion},
            {"overlay_visible", overlay_ui_.microprofiler_open()},
            {"render_api", backend_->render_api_name()}, {"gpu", active_gpu_name_},
            {"driver", active_gpu_info_.driver_name}, {"driver_version", active_gpu_info_.driver_version},
            {"format", config_.hdr ? "P6 diagnostic HDR preview: PQ decoded to nits, Rec.2020 to sRGB, divided by paper white, per-channel Reinhard, sRGB OETF" : "P6 RGB8 SDR, top row first"}, {"benchmark_score", false}
        };
        if (config_.hdr) {
            auto raw_path = std::filesystem::path(image_path);
            raw_path.replace_extension(".pq.pfm");
            metadata["hdr_pq_image"] = raw_path.string();
            metadata["hdr_pq_format"] = "PFM float32 PQ Rec.2020 RGB, bottom-up rows, before swapchain quantization; not physical display calibration";
        }
        append_render_metadata(metadata);
        if (scene_markers_) metadata["scene_start_times"] = scene_markers_->times;
        metadata["scene_objects"] = nlohmann::json::array();
        for (const auto& instance : scene_.instances) {
            if (instance.mesh_index < 0 ||
                !std::cmp_less(instance.mesh_index, scene_.gpu_meshes.size())) continue;
            const auto& mesh = scene_.gpu_meshes[instance.mesh_index];
            const auto center = glm::vec3(instance.transform *
                glm::vec4((mesh.bbox_min + mesh.bbox_max) * 0.5f, 1.0f));
            metadata["scene_objects"].push_back({{"mesh", mesh.path},
                {"center", {center.x, center.y, center.z}}});
        }
        metadata["particle_emitters"] = nlohmann::json::array();
        for (const auto& emitter : scene_.particle_emitters) {
            const auto position = glm::vec3(emitter.transform[3]);
            metadata["particle_emitters"].push_back({{"name", emitter.name},
                {"material", emitter.material_name},
                {"position", {position.x, position.y, position.z}}});
        }
        std::ofstream output(image_path + ".json");
        output << metadata.dump(2) << '\n';
        output.close();
        if (!output) throw std::runtime_error("Cannot write capture metadata");
        ++capture_index_;
        capture_finished_ = capture_index_ == capture_.count;
        capture_requested_ = false;
        std::printf("Capture complete: %s (%d presented frames)\n",
                    image_path.c_str(), capture_presented_frames_);
    }

    void append_render_metadata(nlohmann::json& metadata) {
        const bool authored_test = render_test_scene_uses_assets(config_.render_test_scene);
        if (!config_.render_test_scene.empty()) {
            metadata["render_test_source_kind"] = authored_test ? "authored-assets" : "procedural";
        }
        if (render_test_) {
            metadata["render_test_inputs"] = {
                {"objects", scene_.instances.size()}, {"meshes", scene_.gpu_meshes.size()},
                {"material_records", render_test_->materials.size()},
                {"grounds", render_test_->grounds.size()},
                {"animated_objects", scene_.animated.size()}, {"cloth_bodies", scene_.cloths.size()},
                {"lights", scene_.lights.size()}, {"emitters", scene_.particle_emitters.size()}
            };
        } else if (authored_test) {
            auto bindings = nlohmann::json::array();
            for (const auto& mesh : scene_.gpu_meshes) {
                for (const auto& surface : mesh.surfaces) {
                    if (!surface.handle || !surface.index_count) continue;
                    const auto material = resolve_scene_material(mesh.path, surface.name, config_);
                    if (material.hidden) continue;
                    bindings.push_back({{"mesh", mesh.path}, {"surface", surface.name},
                        {"material", material.material_name}, {"parent_chain", material.parent_chain}});
                }
            }
            metadata["render_test_inputs"] = {
                {"objects", scene_.instances.size()}, {"meshes", scene_.gpu_meshes.size()},
                {"material_records", bindings.size()}, {"grounds", 0},
                {"animated_objects", scene_.animated.size()}, {"cloth_bodies", scene_.cloths.size()},
                {"lights", scene_.lights.size()}, {"emitters", scene_.particle_emitters.size()}
            };
            metadata["render_test_material_bindings"] = std::move(bindings);
            metadata["render_test_asset_sources"] = scene_.source_node_files;
        }
        const auto render_scale = backend_->render_resolution_scale();
        metadata["render_resolution_scale"] = {render_scale.x, render_scale.y};
        metadata["fixture_dynamic_resolution_fixed"] =
            !config_.render_test_scene.empty() && config_.features.dynamic_resolution;
        const auto features = backend_->renderer_feature_stats();
        metadata["renderer_features"] = {
            {"enabled", features.enabled_render_features}, {"total", features.total_render_features},
            {"msaa_samples", features.msaa_samples}, {"refraction_active", features.refraction_active},
            {"msaa_requested", features.msaa_requested}, {"msaa_active", features.msaa_active},
            {"custom_msaa_resolve", features.custom_msaa_resolve},
            {"alpha_to_coverage", features.alpha_to_coverage},
            {"gpu_instancing", features.gpu_instancing}, {"gpu_skinning", features.gpu_skinning},
            {"ibl_prefiltered", features.ibl_prefiltered},
            {"hires_textures", features.hires_textures}, {"original_textures", features.original_textures},
            {"texture_fallbacks", features.texture_fallbacks},
            {"experimental_workflow_surfaces", features.experimental_workflow_surfaces},
            {"color_grading", config_.features.color_grading},
            {"physical_camera", config_.features.physical_camera},
            {"bloom_enabled", features.bloom_enabled},
            {"bloom_strength", features.bloom_strength},
            {"dynamic_resolution", config_.features.dynamic_resolution}
        };
        // CLI-only controls are deliberately absent from saved preferences.
        // Retain them here so an audit can prove the effective render inputs.
        const auto vec3 = [](const glm::vec3& v) {
            return nlohmann::json::array({v.x, v.y, v.z});
        };
        const auto vec4 = [](const glm::vec4& v) {
            return nlohmann::json::array({v.x, v.y, v.z, v.w});
        };
        metadata["runtime_settings"] = {
            {"bloom_strength_requested", tonemap_.bloom_strength},
            {"tone_mapper", to_string(config_.tone_mapper)},
            {"output_tone_mapper", config_.hdr ? "GT7" : to_string(config_.tone_mapper)},
            {"specular_aa_quality", static_cast<int>(config_.quality.specular_aa_level)},
            {"dynamic_resolution_min_scale", config_.quality.dynres_min_scale},
            {"aperture", config_.physical_camera.aperture},
            {"shutter", config_.physical_camera.shutter_speed},
            {"iso", config_.physical_camera.sensitivity},
            {"grading_temperature", config_.grading.temperature},
            {"grading_tint", config_.grading.tint},
            {"grading_contrast", config_.grading.contrast},
            {"grading_saturation", config_.grading.saturation},
            {"grading_night_adaptation", config_.grading.night_adaptation},
            {"grading_mixer_red", vec3(config_.grading.mixer_red)},
            {"grading_mixer_green", vec3(config_.grading.mixer_green)},
            {"grading_mixer_blue", vec3(config_.grading.mixer_blue)},
            {"grading_shadows", vec4(config_.grading.shadows)},
            {"grading_midtones", vec4(config_.grading.midtones)},
            {"grading_highlights", vec4(config_.grading.highlights)},
            {"grading_slope", vec3(config_.grading.cdl_slope)},
            {"grading_offset", vec3(config_.grading.cdl_offset)},
            {"grading_power", vec3(config_.grading.cdl_power)}
        };
    }

    [[nodiscard]] float capture_tour_time() const {
        return std::min(capture_.tour_time.value_or(0.0f) +
            (capture_.tour_motion ? render_time_ : 0.0f), camera_path_.duration);
    }

    [[nodiscard]] GpuRaw active_gpu_raw() const {
        GpuRaw raw;
        raw.model_name = active_gpu_info_.model_name;
        raw.vram_mb = active_gpu_info_.vram_mb;
        raw.driver_name = active_gpu_info_.driver_name;
        raw.driver_version = active_gpu_info_.driver_version;
        raw.driver_info = active_gpu_info_.driver_info;
        raw.driver_check_summary = active_driver_check_.summary;
        raw.driver_check_recommendation = active_driver_check_.recommendation;
        return raw;
    }

    void check_active_graphics_driver() {
        DriverPlatform platform = DriverPlatform::Other;
#ifdef _WIN32
        platform = DriverPlatform::Windows;
#elif defined(__linux__) && !defined(__ANDROID__)
        platform = DriverPlatform::Linux;
#endif
        active_driver_check_ = check_graphics_driver({
            platform, active_gpu_info_.driver_family,
            active_gpu_info_.driver_version, backend_->render_api_name()});
        const std::string description = std::format(
            "{} | {} | {} | {}", active_gpu_name_, backend_->render_api_name(),
            clean_text_field(active_gpu_info_.driver_name, "Unknown driver"),
            clean_text_field(active_gpu_info_.driver_version, "Version unavailable"));
        overlay_ui_.set_driver_check(active_driver_check_, description);
        std::fprintf(stderr, "Graphics driver: %s\nDriver check: %s\n%s\n%s\n",
                     description.c_str(), active_driver_check_.summary.c_str(),
                     active_driver_check_.recommendation.c_str(),
                     active_driver_check_.help_url.c_str());
    }

    [[nodiscard]] Result<void> init() {
        profile::Scope scope("Initialize application", "Loading");
        ScopedStartupTimer startup_total("total init");
        std::optional<RenderTestSceneDefinition> test_definition;
        const bool procedural_test = !config_.render_test_scene.empty() &&
            !render_test_scene_uses_assets(config_.render_test_scene);
        std::printf("Using %s backend\n", std::string(to_string(config_.graphics_api)).c_str());

        // Construct the one logical asset context before any consumer. The
        // renderer, loading screen, material/scene loaders, music, and camera
        // loaders all borrow these same application-owned objects.
        {
            ScopedStartupTimer timer("asset context");
            auto asset_fs = procedural_test
                ? AssetFileSystem::create_memory("render-test-assets")
                : AssetFileSystem::open(config_.data_dir);
            if (!asset_fs) return std::unexpected(asset_fs.error());
            assets_ = std::move(*asset_fs);
            const TextureSetSummary& textures =
                assets_->texture_set_summary();
            config_ui_.texture_set_available = textures.available;
            config_ui_.texture_original_gpu_bytes =
                textures.original_gpu_bytes;
            config_ui_.texture_2k_gpu_bytes = textures.high_2k_gpu_bytes;
            config_ui_.texture_4k_gpu_bytes = textures.ultra_4k_gpu_bytes;
            std::printf(
                "Texture quality: requested=%s archive=%s variants=%llu "
                "estimated_gpu_bytes(original=%llu,2k=%llu,4k=%llu)\n",
                std::string(texture_quality_id(config_.texture_quality)).c_str(),
                textures.available ? "schema-v2" : "original-only",
                static_cast<unsigned long long>(textures.texture_count),
                static_cast<unsigned long long>(textures.original_gpu_bytes),
                static_cast<unsigned long long>(textures.high_2k_gpu_bytes),
                static_cast<unsigned long long>(textures.ultra_4k_gpu_bytes));
            auto resolver = PathResolver::create(assets_->data_root());
            if (!resolver) return std::unexpected(resolver.error());
            paths_ = std::move(*resolver);
            if (procedural_test) {
                auto definition = make_render_test_scene(config_.render_test_scene);
                if (!definition) return std::unexpected(definition.error());
                if (auto mounted = mount_render_test_scene_assets(*definition, *assets_); !mounted)
                    return std::unexpected(mounted.error());
                test_definition = std::move(*definition);
            }
            if (!procedural_test && config_.features.ground_relief) {
                auto generated = ensure_ground_height(*paths_, *assets_);
                if (!generated) {
                    std::fprintf(
                        stderr,
                        "Ground relief generation failed (normal-map fallback): %s\n",
                        generated.error().message.c_str());
                } else if (generated->has_value()) {
                    std::printf("  Generated village ground height in memory\n");
                }
            }
        }

        // Create window for the selected API
        {
            ScopedStartupTimer timer("create window");
            auto win = create_window(
                config_, config_.graphics_api,
                format_application_title(kApplicationBuildInfo));
            if (!win) return std::unexpected(win.error());
            window_ = std::move(*win);
        }

        // Create rendering backend
        {
            ScopedStartupTimer timer("create backend");
            auto backend = create_backend(config_.graphics_api, window_, config_);
            if (!backend) return std::unexpected(backend.error());
            backend_ = std::move(*backend);
            backend_->set_trace_sink(&micro_recorder_);
            backend_->set_trace_frame_context({0, 0, 0, 0, 0, micro_recorder_.session_id()});
            backend_->set_asset_context(std::cref(*paths_),
                                        std::cref(*assets_));
            active_gpu_info_ = backend_->gpu_info();
            active_gpu_name_ =
                clean_text_field(active_gpu_info_.model_name, kUnknownGpu);
            check_active_graphics_driver();
            active_system_info_ = gather_system_info(host_info_source_, active_gpu_raw());
        }

        std::printf("%s\n", backend_->api_version().c_str());
        std::printf("Renderer: %s\n", backend_->device_name().c_str());
        auto show_loading = [this](std::string_view status) {
            if (backend_) {
                backend_->show_loading_screen(config_.data_dir, status,
                                              window_.width, window_.height);
            }
        };
        show_loading("Starting renderer");
        if (!assets_) return std::unexpected(Error{"Asset context is not initialized"});
        config_.tessellation.cache_file = runtime_paths_.tessellation_cache_file(
            config_.tessellation.cache_file, config_.data_dir, assets_->is_zip());

        // Initialize pipelines
        {
            ScopedStartupTimer timer("init pipelines");
            if (auto r = backend_->init_pipelines(config_); !r)
                return std::unexpected(r.error());
        }
        show_loading("Initializing overlay");

        // Initialise the Dear ImGui / filagui overlay now that the engine and
        // pipelines exist. A failure here is non-fatal: the backend logs the
        // cause and leaves the overlay disabled, and the renderer keeps running
        // without the on-screen status.
        {
            ScopedStartupTimer timer("overlay init");
            backend_->overlay_init(config_);
        }
        show_loading("Resolving assets");

        // Scene and audio initialization require the complete asset context.
        if (!paths_ || !assets_) {
            return std::unexpected(Error{"Asset context is not initialized"});
        }
        const PathResolver& paths = *paths_;
        show_loading("Starting audio");

        // Background music subsystem (Music_Player). Constructed and started
        // here, immediately after the Data_Root resolver is available, so it
        // resolves heaven/heaven.oga against the same Data_Root the scene loads
        // from. start() is non-blocking and returns immediately: all
        // fallible work (resolve, decode, device open under a 5 s timeout)
        // happens on the player's own thread and every failure is a logged
        // diagnostic, so a music problem can never abort init()
        // The render loop never calls into the player — there is no
        // per-frame music coordination, so the frame sequence/count and timing
        // are unchanged. Because the player is an Application
        // member started only here, the run_asset_packer path (which branches
        // before constructing Application) never constructs it.
        if (config_.render_test_scene.empty()) {
            ScopedStartupTimer timer("music start async");
            music_player_ = std::make_unique<MusicPlayer>();
            music_player_->start(config_.music, *paths_,
                                 std::cref(*assets_));
        }
        show_loading("Loading materials");

        // Material database
        std::printf("Loading materials from %s...\n", config_.data_dir.c_str());
        show_loading("Loading scene");

        // Scene
        std::printf("Loading scene from %s...\n", config_.data_dir.c_str());
        // Filament has no tessellation shaders, so we subdivide
        // displacement-mapped surfaces on the CPU at load time
        // and cache the results to disk for fast subsequent
        // launches. See engine/cpu_tessellation.cpp.
        const std::string cache_dir = runtime_paths_.tessellation_cache_file(
            config_.tessellation.cache_file, config_.data_dir, assets_->is_zip());
        // Camera collision is built inside the loader from the BASE
        // (pre-tessellation) geometry — see the note in scene_rhi.cpp.

        // The collision radius MUST be larger than the camera's
        // near-clip-plane corner distance, otherwise the camera
        // centre stops `radius` from a wall while the near plane's
        // corners (which reach further out) slice into it — the
        // visible "diagonal clip" when you rotate while touching a
        // surface. The near plane is a rectangle, so its farthest
        // point from the camera is the corner:
        //   corner = near · √(1 + tan²(fovV/2) · (1 + aspect²))
        // For near=0.5, fovV=60°, aspect=16:9 that's ≈ 0.77 m, so
        // we use a radius comfortably above it.
        {
            constexpr float kNear  = 0.5f;
            constexpr float kFovV  = 60.0f;
            const float aspect = safe_aspect(config_.width, config_.height);
            const float t = std::tan(glm::radians(kFovV * 0.5f));
            const float corner = kNear *
                std::sqrt(1.0f + t * t * (1.0f + aspect * aspect));
            // 25 cm margin past the near-plane corner so the whole
            // frustum slab stays clear of the surface.
            camera_collision_.set_radius(corner + 0.25f);
        }
        {
            ScopedStartupTimer timer("scene load + tessellation");
            if (test_definition) {
                auto test = upload_render_test_scene(std::move(*test_definition), *backend_, config_);
                if (!test) return std::unexpected(test.error());
                scene_ = std::move(test->scene);
                render_test_ = std::move(*test);
                std::printf("Render test scene: %s (%zu objects, %zu lights, %zu emitters)\n",
                            config_.render_test_scene.c_str(), scene_.instances.size(),
                            scene_.lights.size(), scene_.particle_emitters.size());
            } else {
                auto prepared = load_scene_in_background(config_, cache_dir);
                if (!prepared) return std::unexpected(prepared.error());
                if (auto uploaded = upload_prepared_scene(prepared->scene, config_); !uploaded)
                    return std::unexpected(uploaded.error());
                scene_ = std::move(prepared->scene);
                materials_ = std::move(prepared->materials);
                camera_collision_ = std::move(prepared->collision);
            }
        }
        show_loading("Preparing scene");
        if (auto r = prepare_displacement(config_); !r) return std::unexpected(r.error());

        // Apply the requested simulation rate to every cloth body
        // we just loaded. Defaults to 60 Hz; debug builds can pass
        // `--cloth-rate 1` to slow each integration step to 1 s
        // and watch how the cloth deforms.
        {
            ScopedStartupTimer timer("post scene setup");
            for (auto& cloth : scene_.cloths) {
                cloth.sim_hz = config_.cloth_sim_hz;
            }
            if (config_.cloth_sim_hz != 60.0f) {
                std::printf("  Cloth sim rate overridden: %.2f Hz\n",
                            config_.cloth_sim_hz);
            }

            // Hand particle emitters to the backend so it can build the
            // GPU resources for rendering them. No-op for backends that
            // don't override set_particle_emitters.
            backend_->set_particle_emitters(scene_.particle_emitters);

            // Hand authored scene lights (lamp posts, accent lights) to the
            // backend. The backend creates persistent light entities once;
            // we update their position each frame for floating-island
            // groups via update_floating_lights() below.
            backend_->set_scene_lights(scene_.lights);
        }
        show_loading("Loading camera path");

        // Camera path + scene markers for benchmark mode. The camera path is
        // the merged camera_*.track spline; the scene markers come from
        // clip.track and drive the "Scene k+1 / n" progress readout. Either may
        // be unusable (missing/short) — BenchmarkSystem::request_start() refuses
        // in that case and the renderer stays in free-fly mode.
        if (config_.render_test_scene.empty()) {
            if (!assets_) return std::unexpected(Error{"Cannot load camera tracks before the asset context is initialized"});
            const auto& assets = *assets_;
            ScopedStartupTimer timer("camera tracks");
            std::string track_dir = paths.track_dir();
            camera_path_ = load_camera_path(
                track_dir, nullptr, std::cref(assets));
            scene_markers_ = load_scene_markers(
                track_dir, nullptr, std::cref(assets));
        }
        show_loading("Finalizing");
        initialize_capture_camera();

        // Build the run lifecycle state machine. It holds non-owning pointers to
        // the scene markers and the camera path (both members that outlive it)
        // plus a frame-pacing probe backed by the backend, so it can refuse to
        // start when pacing cannot be disabled.
        const SceneMarkers* markers_ptr =
            scene_markers_.has_value() ? &*scene_markers_ : nullptr;
        bench_ = std::make_unique<BenchmarkSystem>(
            markers_ptr, &camera_path_,
            [this] { return backend_->frame_pacing_disabled(); },
            // SystemInfoGatherer: compose the selected GPU captured
            // from the live backend with the host OS platform/CPU info. The
            // backend's GpuInfoRaw and run_info's GpuRaw are distinct types, so
            // their selected-device fields are copied across explicitly.
            [this] {
                return gather_system_info(host_info_source_, active_gpu_raw());
            },
            // SettingsInfoGatherer: the live render-API name plus the
            // display mode assembled from the live drawable extent (rather
            // than merely echoing a requested size the compositor might not
            // have supplied), AA samples, window state, and preset label.
            [this] {
                DisplayRaw display_raw;
                display_raw.width      = window_.width;
                display_raw.height     = window_.height;
                display_raw.aa_samples = config_.quality.msaa_samples;
                display_raw.fullscreen = config_.fullscreen;
                return gather_settings_info(backend_->render_api_name(),
                                            display_raw,
                                            std::string(render_preset_id(
                                                config_.render_preset)));
            });

        // The Working_Configuration starts as a copy of the resolved
        // Active_Configuration (config_). The Options_Panels edit this copy;
        // Reload_Settings only advance into config_ on a Benchmark_Reload, while
        // Live_Settings are written to both immediately (in-ui-configuration
        // Seeded here, once config_ is final, so the first overlay
        // frame seeds its controls from the same values the renderer started with.
        working_config_ = config_;
        overlay_ui_.set_application_build_info(kApplicationBuildInfo);
        active_gpu_telemetry_ = backend_->gpu_telemetry();

        // Wire the overlay's per-frame widget builder once. The callback runs
        // inside the backend's render pass (between ImGui NewFrame and Render),
        // so it must not mutate the benchmark system directly — instead it
        // stashes the returned intents into last_intents_, which the main loop
        // applies after render_frame returns.
        // It reads bench_->status() fresh on every invocation and seeds its
        // editable controls from the Working_Configuration snapshot plus the
        // ConfigUiState feedback block (in-ui-configuration).
        backend_->overlay_set_draw([this] {
            profile::Scope scope("Build profiler and application widgets", "UI");
            if (capture_.output || capture_.profile_output) {
                if (microprofile_.visible || overlay_ui_.microprofiler_open())
                    last_intents_ = overlay_ui_.build_microprofiler();
                return;
            }
            overlay_ui_.draw_debug_sidebar(debug_sidebar_info());
            last_intents_ = overlay_ui_.build(bench_->status(), working_config_,
                                              config_ui_);
        });

        // Populate the overlay's Resolution dropdown from the monitor's
        // supported modes + common windowed sizes (gathered via SDL here so the
        // pure OverlayUI stays SDL-free). Done once at init; the list is static
        // for the window's lifetime.
        overlay_ui_.set_available_resolutions(
            gather_window_resolutions(window_.window));

        // Initial lighting from time-of-day.
        lighting_ = lighting_for_time_of_day(config_.time_of_day);

        // Seed the persistent weather state from the parsed CLI config. The
        // WeatherState constructor clamps and applies config.weather.rain_intensity
        // into the commanded intensity, so the default (medium 0.5) run starts the
        // storm and `--rain-intensity V` starts it at V; `--rain-intensity 0`
        // (or `--no-rain`) restores the clear-weather Shipping_Look.
        weather_ = WeatherState(config_.weather);
        std::printf("Time of day: %.1fh — sun=(%.2f,%.2f,%.2f) sun_intensity=%.0f lux "
                    "sky=(%.2f,%.2f,%.2f)\n",
                    config_.time_of_day,
                    lighting_.sun_color.r, lighting_.sun_color.g, lighting_.sun_color.b,
                    lighting_.sun_intensity,
                    lighting_.sky_color.r, lighting_.sky_color.g, lighting_.sky_color.b);

        std::printf("\nRendering... (WASD=move, QE=up/down, Right-Mouse(hold)=look, Shift=fast, "
                    "ESC=cancel/quit, B=start benchmark)\n");

        // Interactive launch refusals retain free-fly mode; automated one-shot
        // runs must fail instead of waiting indefinitely without a result.
        if (config_.benchmark_mode) {
            if (!request_benchmark_start()) {
                if (const char* exit_once = std::getenv("HEAVEN_BENCHMARK_EXIT");
                    exit_once && std::strcmp(exit_once, "0") != 0) {
                    return std::unexpected(Error{"Benchmark auto-start refused"});
                }
                std::printf("Benchmark auto-start refused; staying in free-fly mode.\n");
            }
        }

        return {};
    }

    void main_loop() {
        bool running = true;
        bool printed_tex_stats = false;
        bool advance_capture = true;
#ifdef __ANDROID__
        // Modal reload consumes the same lifecycle stream as ordinary frames.
        // Share pause/resume state so cancellation cannot resume an old scene
        // after a background event was already consumed by the loading pump.
        bool& android_paused = loading_paused_;
        bool& android_resume_pending = loading_resume_pending_;
        PreviewFramePacer preview_pacer;
#endif

        while (running) {
            if (exit_requested_) break;
            if (microprofile_start_requested_) {
                microprofile_start_requested_ = false;
                micro_recorder_.begin_capture(microprofile_history_ns_);
                update_microprofile_metadata("runtime");
                bench_->set_microprofile_recording(true);
            }
            // Deferred grass/entity creation belongs to startup, including the
            // first accepted scene frame. Change capture epochs only between
            // iterations, after every old CPU scope has closed.
            if (microprofile_.output_base && microprofile_scene_ready_ && !microprofile_runtime_started_) {
                save_microprofile(*microprofile_.output_base + ".startup");
                micro_recorder_.begin_capture(static_cast<uint64_t>((microprofile_.seconds + 1.0) * 1e9));
                micro_recorder_.set_thread_name("Main");
                microprofile_runtime_started_ = true;
                microprofile_runtime_begin_ns_ = profile::Recorder::steady_now_ns();
                update_microprofile_metadata("runtime");
                bench_->set_microprofile_recording(true);
                const auto info = debug_sidebar_info();
                micro_recorder_.counter("Scene vertices total", static_cast<double>(info.vertices), "Scene totals");
                micro_recorder_.counter("Scene triangles total", static_cast<double>(info.triangles), "Scene totals");
                micro_recorder_.counter("Scene instances total", static_cast<double>(info.instance_count), "Scene totals");
                micro_recorder_.counter("Texture bytes selected", static_cast<double>(info.selected_texture_bytes), "Scene totals");
            }
            profile::Context trace_context{++microprofile_attempt_id_, 0, 0, 0, 0, micro_recorder_.session_id()};
            profile::ContextScope iteration_context(micro_recorder_, trace_context);
            TraceIteration trace_iteration(micro_recorder_, trace_context);
            profile::Scope frame_scope("Main loop", "Frame");
            std::optional<profile::Scope> task_scope;
            task_scope.emplace("Preview pacing", "Wait");
#ifdef __ANDROID__
            // Pace submissions before sampling the frame clock, so raw wall
            // time includes the wait. Scored/capture/profile paths never wait.
            const bool pace_preview = preview_pacing_allowed(
                bench_->is_running(), capture_.output.has_value(),
                capture_.profile_output.has_value(), android_paused);
            for (auto wait = preview_pacer.wait_before_frame(
                     PreviewFramePacer::Clock::now(),
                     config_.android_preview_fps, pace_preview);
                 wait > PreviewFramePacer::Clock::duration::zero();
                 wait = preview_pacer.wait_before_frame(
                     PreviewFramePacer::Clock::now(),
                     config_.android_preview_fps, pace_preview)) {
                SDL_Delay(static_cast<Uint32>(
                    std::chrono::ceil<std::chrono::milliseconds>(wait).count()));
            }
#endif
            task_scope.reset();
            task_scope.emplace("Frame clock and input", "Input");
            float dt = stats_.begin_frame();
            if (capture_.output && !capture_.realtime) {
                dt = advance_capture ? 1.0f / capture_.fps : 0.0f;
                render_time_ = static_cast<float>(capture_presented_frames_ + 1) / capture_.fps;
                backend_->set_capture_time(render_time_);
            } else {
                render_time_ += dt;
            }

            // Begin the overlay's ImGui frame before polling input so the
            // helper has up-to-date display size and delta, and so events can be
            // routed into ImGui IO this frame.
            backend_->overlay_begin_frame(dt, window_.width, window_.height);

            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) running = false;

#ifdef __ANDROID__
                // SDL delivers the pause events before its default Android
                // event pump blocks. Invalidate a scored run and submit no
                // more frames while the app is paused.
                if (ev.type == SDL_APP_WILLENTERBACKGROUND) {
                    preview_pacer.reset();
                    submission_retry_.reset();
                    android_paused = true;
                    android_resume_pending = false;
                    if (bench_->is_running()) {
                        cancel_active_benchmark();
                        std::fprintf(
                            stderr,
                            "Benchmark cancelled: Android app entered "
                            "the background.\n");
                    }
                    benchmark_config_snapshot_.reset();
                    perf_frame_times_.clear();
                    perf_timing_sum_ = {};
                    perf_timing_samples_ = 0;
                    perf_gpu_ms_sum_ = 0.0;
                    perf_gpu_samples_ = 0;
                    continue;
                }
                if (ev.type == SDL_APP_DIDENTERFOREGROUND) {
                    preview_pacer.reset();
                    submission_retry_.reset();
                    // surfaceCreated replaces SDL's ANativeWindow before
                    // nativeResume signals this event. Recreate after the
                    // entire event batch, even when its extent is unchanged.
                    android_paused = true;
                    android_resume_pending = true;
                    continue;
                }
                if (android_paused) continue;
#endif

                // Keep the cached drawable size and the swap chain in sync with
                // the actual window size on any resize (an in-UI resolution
                // apply, a fullscreen toggle's async resize, or a
                // window-manager resize). Only acts on a real change, so the
                // direct resize already done by rebuildFromActiveConfig is not
                // redundantly repeated here.
                if (ev.type == SDL_WINDOWEVENT &&
                    (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                     ev.window.event == SDL_WINDOWEVENT_RESIZED)) {
                    int logical_w = 0, logical_h = 0;
                    SDL_GetWindowSize(window_.window, &logical_w, &logical_h);
                    int dw = window_.width, dh = window_.height;
                    get_drawable_size(window_.window, dw, dh);
                    std::printf("Window: resize event client=%dx%d drawable=%dx%d\n",
                                logical_w, logical_h, dw, dh);
                    if (dw > 0 && dh > 0 &&
                        (dw != window_.width || dh != window_.height)) {
#ifdef __ANDROID__
                        preview_pacer.reset();
                        // A scored run must keep one Surface extent throughout.
                        // Discard it before submitting a differently sized frame,
                        // then let an idle run request the new Android extent.
                        if (bench_->is_running()) {
                            cancel_active_benchmark();
                            std::fprintf(
                                stderr,
                                "Benchmark cancelled: Android surface changed "
                                "from %dx%d to %dx%d.\n",
                                window_.width, window_.height, dw, dh);
                        }
                        config_.width = dw;
                        config_.height = dh;
                        working_config_.width = dw;
                        working_config_.height = dh;
                        config_ui_.dirty = is_dirty(working_config_, config_);
                        overlay_ui_.set_available_resolutions(
                            gather_window_resolutions(window_.window));
#endif
                        window_.width  = dw;
                        window_.height = dh;
                        backend_->resize(dw, dh);
                        submission_retry_.reset();
                    }
                }

                // Offer every event to the overlay first so ImGui can capture
                // input destined for its widgets.
                bool consumed = backend_->overlay_handle_event(&ev);

                // Dismiss Credits before applying the usual benchmark control.
                // Outside that dialog, Escape still cancels a run or quits,
                // regardless of ImGui capture or suppressed free-fly input.
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE) {
                    if (ev.key.repeat) continue;
                    if (overlay_ui_.dismiss_credits()) continue;
                    if (bench_->is_running()) cancel_active_benchmark();
                    else running = false;
                    continue;
                }
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F6 && !ev.key.repeat) {
                    overlay_ui_.toggle_microprofiler();
                    continue;
                }

                // ImGui ate this event — do not let it fall through to the
                // camera/app handling.
                if (consumed || capture_.output || capture_.profile_output) continue;

                // Fall-through: free-fly camera / app input. Suppressed while a
                // run is active so the authored path is never perturbed

                if (!bench_->is_running()) {
                    // B is a convenience manual start; --benchmark handles
                    // auto-start separately. This preserves the documented
                    // control. request_start() logs and refuses if assets or
                    // pacing are unusable.
                    if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_b)
                        request_benchmark_start();
                    // C toggles camera collision (free-fly noclip when off).
                    if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_c) {
                        camera_collision_.set_enabled(!camera_collision_.enabled());
                        std::printf("Camera collision: %s\n",
                                    camera_collision_.enabled() ? "ON" : "OFF");
                    }
                    // Free-fly look is a hold-to-engage action so the OS cursor
                    // stays free for the overlay menu the rest of the time.
                    // Holding the right mouse button locks + hides the cursor
                    // (relative mode) and feeds motion to the camera; releasing
                    // it frees the cursor again.
                    if (ev.type == SDL_MOUSEBUTTONDOWN &&
                        ev.button.button == SDL_BUTTON_RIGHT) {
                        mouse_look_ = true;
                        SDL_SetRelativeMouseMode(SDL_TRUE);
                    }
                    if (ev.type == SDL_MOUSEBUTTONUP &&
                        ev.button.button == SDL_BUTTON_RIGHT) {
                        mouse_look_ = false;
                        SDL_SetRelativeMouseMode(SDL_FALSE);
                    }
                    if (ev.type == SDL_MOUSEMOTION && mouse_look_)
                        camera_.process_mouse(static_cast<float>(ev.motion.xrel),
                                              static_cast<float>(ev.motion.yrel));
                }
            }

#ifdef __ANDROID__
            if (android_paused) {
                if (!running) break;
                if (!android_resume_pending) continue;

                int dw = 0;
                int dh = 0;
                get_drawable_size(window_.window, dw, dh);
                SDL_SysWMinfo native_info{};
                SDL_VERSION(&native_info.version);
                if (dw <= 0 || dh <= 0 ||
                    SDL_GetWindowWMInfo(window_.window, &native_info) != SDL_TRUE ||
                    native_info.subsystem != SDL_SYSWM_ANDROID ||
                    native_info.info.android.window == nullptr) {
                    // The normal SDL path resumes only after Surface readiness.
                    // If it is not ready yet, retain the paused state and let
                    // the next SDL pump process lifecycle events before retry.
                    SDL_Delay(10);
                    continue;
                }

                backend_->resize(dw, dh);
                submission_retry_.reset();
                window_.width = dw;
                window_.height = dh;
                config_.width = dw;
                config_.height = dh;
                working_config_.width = dw;
                working_config_.height = dh;
                config_ui_.dirty = is_dirty(working_config_, config_);
                overlay_ui_.set_available_resolutions(
                    gather_window_resolutions(window_.window));
                android_resume_pending = false;
                android_paused = false;
                std::printf("Android: resumed with recreated Surface %dx%d.\n",
                            dw, dh);
            }
#endif

            // A run takes over the camera: drop any held free-fly look so the
            task_scope.reset();
            task_scope.emplace("Camera and benchmark playback", "Simulation");
            // cursor is freed for the overlay (the run ignores look input
            // anyway) and the menu / result report stay clickable.
            if (bench_->is_running() && mouse_look_) {
                mouse_look_ = false;
                SDL_SetRelativeMouseMode(SDL_FALSE);
            }

            // Capture the pre-input position so we can resolve the
            // free-fly movement against the collision world below.
            glm::vec3 prev_cam_pos = camera_.position;

            // Free-fly keyboard movement is gated off while a run drives the
            // camera.
            if (!bench_->is_running() && !capture_.output && !capture_.profile_output)
                camera_.process_keys(SDL_GetKeyboardState(nullptr), dt);

            // Advance the benchmark state machine. While Running it advances the
            // playback clock 1:1, updates the scene index, and drives the camera
            // along the authored path. Stats are committed below only after
            // Filament accepts the frame; in every other state this leaves the
            // camera untouched.
            const bool was_running = bench_->is_running();
            if (!was_running) {
                // Authored DOF belongs to camera-track playback. Clear the
                // previous path sample as soon as the run is no longer active
                // so free-fly mode returns to the configured physical-camera
                // behavior instead of retaining the final benchmark focus.
                if (!capture_.tour_time) camera_.authored_dof = {};
            }
            bench_->tick(
                capture_.output && !capture_.realtime
                    ? dt : stats_.raw_frame_delta(),
                camera_, /*record_as_presented=*/false);
            if (capture_.tour_motion) {
                const float time = capture_tour_time();
                camera_path_.apply_clamped(time, camera_);
                config_.time_of_day = camera_path_.time_of_day_clamped(time, config_.time_of_day);
                if (capture_.fov_explicit) camera_.fov = capture_.fov;
            }

            // Animate floating islands (sine wave bobbing).
            task_scope.reset();
            task_scope.emplace("Floating islands and bridges", "Simulation");
            // Also build per-group Z offset map for bridge endpoint updates.

            // While a benchmark run is driving the authored camera path the
            // islands (and the rocks/bridges/lights anchored to them) are
            // frozen at their rest positions: the run must compare frame rates
            // against a deterministic, motionless scene, so we snap every
            // floating group back to z_offset = 0 and leave it there until the
            // run completes. Free-fly mode keeps the bobbing animation.
            const bool freeze_floating = bench_->is_running() ||
                (capture_.output && !capture_.floating_motion);
            std::map<int, float> group_offsets;
            moving_instance_indices_.clear();
            for (auto& fi : scene_.floating_islands) {
                float z_offset = 0.0f;
                if (!freeze_floating) {
                    float time = render_time_ * fi.speed;
                    z_offset = fi.amplitude - std::cos(time) * fi.amplitude;
                }
                glm::mat4 anim = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z_offset));
                const glm::mat4 next_transform = anim * fi.base_transform;
                if (scene_.instances[fi.instance_index].transform != next_transform) {
                    scene_.instances[fi.instance_index].transform = next_transform;
                    moving_instance_indices_.push_back(fi.instance_index);
                }
                // Store the offset for this group (all islands in same group have same params)
                group_offsets[fi.group] = z_offset;
            }

            // Animate small floating rocks (`stone_noise` WorldExpression).
            // The original script overwrites each stone's parent
            // ObjectDummy Z every tick:

            //     dummy.z = sin(time*0.5 + seed) * 6 * sin(seed)

            // where `seed = dummy.x*13 + dummy.y` is fixed per
            // stone. The rendered mesh then sits at `child_local_z`
            // above/below the dummy (some rocks are 30+ metres off
            // their dummy on the Z axis), so we add that offset
            // back when computing the world-space Z. Without it
            // every rock collapses to ±5m around z=0 and clips
            // straight into the islands.
            for (auto& sa : scene_.stone_anims) {
                float dummy_z = 0.0f;
                if (!freeze_floating) {
                    dummy_z = std::sin(render_time_ * sa.speed + sa.seed)
                            * sa.amplitude * std::sin(sa.seed);
                }
                glm::mat4 m = sa.base_transform;
                m[3].z = dummy_z + sa.child_local_z;
                if (scene_.instances[sa.instance_index].transform != m) {
                    scene_.instances[sa.instance_index].transform = m;
                    moving_instance_indices_.push_back(sa.instance_index);
                }
            }

            // Keep the collision world's moving bodies (floating
            // islands, stones) in sync with their animated transforms
            // every frame, regardless of mode, so the cost is
            // represented in benchmark numbers and the world is
            // always correct. The actual collide-and-slide of the
            // camera only runs in free-fly mode — the authored
            // benchmark camera path is known-good and must not be
            // perturbed.
            if (camera_collision_.enabled()) {
                profile::Scope collision_scope("Synchronize camera collision", "Simulation");
                camera_collision_.update_moving(scene_.instances,
                                                moving_instance_indices_,
                                                camera_.position);
            }
            if (!bench_->is_running() && camera_collision_.enabled()) {
                profile::Scope collision_scope("Resolve camera collision", "Simulation");
                camera_.position =
                    camera_collision_.resolve(prev_cam_pos, camera_.position);
            }


            // Simulate bridge rope chains with per-group endpoint offsets
            if (scene_.bridge_system.update(dt, group_offsets)) {
                profile::Scope rope_scope("Apply rope simulation", "Simulation");
                scene_.bridge_system.apply_to_instances(
                    scene_.instances, &moving_instance_indices_);
            }

            std::sort(moving_instance_indices_.begin(),
                      moving_instance_indices_.end());
            moving_instance_indices_.erase(
                std::unique(moving_instance_indices_.begin(),
                            moving_instance_indices_.end()),
                moving_instance_indices_.end());

            // Translate authored scene lights with their floating group's
            // Z offset. Backends that don't support point/spot lights see
            // a no-op default. Convert the std::map to a flat
            // pair-vector so the backend doesn't need to depend on <map>.
            if (!group_offsets.empty()) {
                std::vector<std::pair<int, float>> off_pairs;
                off_pairs.reserve(group_offsets.size());
                for (auto& [g, z] : group_offsets) off_pairs.emplace_back(g, z);
                backend_->update_floating_lights(off_pairs);
            }

            // Animate skinned meshes (mill wings, propeller, weathercock, gears).
            task_scope.reset();
            task_scope.emplace("Skeletal animation", "Simulation");

            // Per-frame CPU skinning is the hot path's most expensive
            // step (lab_tourbines alone is ~12k tessellated vertices,
            // ~500us/frame to skin). The underlying animation is 30 fps
            // — re-skinning every render frame at 1500+ fps is pure
            // waste. Skip the work when frame_f hasn't advanced enough
            // for the new pose to differ visibly from the cached one.

            // The threshold is in source frames; with `kReskinThreshold
            // = 0.05` we re-skin at most 30/0.05/30 = 20 times per
            // source-frame advance, i.e. ~600 Hz update rate. Plenty
            // smooth for monitor refresh, ~5x cheaper than per-frame.
            constexpr float kReskinThreshold = 0.05f;
            // GPU skinning (default ON) skins each Animated_Object on the
            // GPU from a static rest-pose buffer and per-frame bone
            // matrices, so the per-frame CPU linear-blend skin, vertex
            // re-upload, and tangent recompute are all eliminated
            // With --no-gpu-skinning the existing CPU
            // path runs byte-for-byte unchanged.
            const bool gpu_skinning = config_.features.gpu_skinning;
            for (auto& anim : scene_.animated) {
                // Guard against a zero frame count: fmod by zero is
                // undefined (NaN). A skinned mesh with no animation
                // frames can never advance, so pin it to frame 0. For
                // any num_frames > 0 this is identical to the bare
                // fmod computation. The SAME frame_f drives both the
                // GPU and CPU paths, so the interpolated pose per
                // wall-clock time is identical.
                float frame_f = anim.smesh.num_frames > 0
                    ? std::fmod(render_time_ * anim.anim_speed,
                                static_cast<float>(anim.smesh.num_frames))
                    : 0.0f;
                auto& gpu_mesh = scene_.gpu_meshes[anim.mesh_index];

                // Resolve the per-object GPU-skinning decision once, after the
                // persistent scene is built. When GPU
                // skinning is requested globally, an Animated_Object whose
                // skinned upload/enable did not take effect (e.g. its bone
                // count exceeded Filament's 255-bone limit) has no
                // skinning-enabled renderable and would freeze at its rest pose
                // on the GPU branch — it must animate through the
                // CPU_Skinning_Path instead. The backend reports
                // std::nullopt until the scene exists, so we defer skinning that
                // object for the frame; once resolved the choice is cached and a
                // one-time diagnostic is emitted on fallback. With
                // --no-gpu-skinning this block is skipped entirely and the CPU
                // path below runs byte-for-byte unchanged.
                bool use_gpu_skin = false;
                if (gpu_skinning) {
                    if (!anim.gpu_skin_resolved) {
                        auto active = backend_->gpu_skinning_active(anim.mesh_index);
                        if (!active) {
                            // Scene not built yet — decide on a later frame.
                            continue;
                        }
                        anim.gpu_skin_resolved = true;
                        anim.gpu_skin_active   = *active;
                        if (!anim.gpu_skin_active) {
                            std::fprintf(stderr,
                                "  GPU skinning unavailable for animated mesh "
                                "'%s' (index %d) — falling back to CPU skinning "
                                "for this object\n",
                                anim.smesh.name.c_str(), anim.mesh_index);
                        }
                    }
                    use_gpu_skin = anim.gpu_skin_active;
                }

                if (use_gpu_skin) {
                    // GPU skinning path: compute the per-bone matrices
                    // for the interpolated frame once per object (≤6
                    // bones — cheap) and push them to the renderable via
                    // setBones. No skin_interpolated / update_mesh_vertices
                    // for Animated_Objects.

                    // Retain the high-FPS re-skin throttle: the animation
                    // is authored at 30 fps, so only recompute and push
                    // bones once frame_f has advanced past the threshold
                    // (or wrapped). The throttle is optional for the
                    // matrix path but keeps per-frame CPU work minimal.
                    if (!anim.last_frame_f.empty()) {
                        float prev = anim.last_frame_f[0];
                        float delta = std::abs(frame_f - prev);
                        bool wrapped = delta > static_cast<float>(anim.smesh.num_frames) * 0.5f;
                        if (!wrapped && delta < kReskinThreshold) continue;
                    }
                    auto mats = anim.smesh.bone_matrices_interpolated(frame_f);
                    auto ub = backend_->update_skinned_bones(anim.mesh_index, mats);
                    if (!ub) {
                        std::fprintf(stderr,
                            "skinned bone update skipped: %s\n",
                            ub.error().message.c_str());
                    } else {
                        // Record the pushed frame so the throttle has a
                        // reference for the next frame's delta.
                        for (auto& f : anim.last_frame_f) f = frame_f;
                    }
                    continue;
                }

                // CPU skinning path: runs when GPU skinning is disabled
                // (--no-gpu-skinning) OR when this specific Animated_Object
                // fell back from GPU skinning. The object was
                // uploaded via upload_skinned (rest-pose buffer 0 in the PosUV
                // layout), so update_mesh_vertices re-uploads the CPU-skinned
                // positions into that same buffer with a matching vertex count.
                for (size_t si = 0; si < anim.smesh.surfaces.size() && si < gpu_mesh.surfaces.size(); si++) {
                    // Skip if the cached upload still represents this
                    // frame_f closely enough. The wraparound check
                    // forces a re-skin when the animation cycle wraps
                    // (frame_f jumps from ~num_frames back to ~0).
                    if (si < anim.last_frame_f.size()) {
                        float prev = anim.last_frame_f[si];
                        float delta = std::abs(frame_f - prev);
                        bool wrapped = delta > static_cast<float>(anim.smesh.num_frames) * 0.5f;
                        if (!wrapped && delta < kReskinThreshold) continue;
                    }
                    auto skinned = anim.smesh.skin_interpolated(static_cast<int>(si), frame_f);
                    if (!skinned.empty()) {
                        auto uv = backend_->update_mesh_vertices(
                            gpu_mesh.surfaces[si].handle,
                            skinned.data(),
                            static_cast<uint32_t>(skinned.size()));
                        if (!uv) {
                            std::fprintf(stderr,
                                "skinned re-upload skipped: %s\n",
                                uv.error().message.c_str());
                        } else if (si < anim.last_frame_f.size()) {
                            anim.last_frame_f[si] = frame_f;
                        }
                    }
                }
            }

            // Step cloth bodies (flags). Particles are authored
            task_scope.reset();
            task_scope.emplace("Cloth simulation and upload", "Simulation");
            // in body-local space; the GpuMesh's instance transform
            // converts local → world at render time, so we push
            // particle positions directly without any extra
            // transform.
            for (auto& cloth : scene_.cloths) {
                bool stepped = step_cloth(cloth, dt, render_time_);
                // At 3000 FPS the simulator only ticks every ~50
                // render frames; only re-upload the vertex buffer
                // when something actually changed.
                if (!stepped) continue;
                static thread_local std::vector<Vertex> tmp;
                emit_vertices(cloth, tmp);
                if (cloth.mesh_index >= 0 &&
                    static_cast<std::size_t>(cloth.mesh_index) <
                        scene_.gpu_meshes.size()) {
                    auto& gm = scene_.gpu_meshes[cloth.mesh_index];
                    if (!gm.surfaces.empty() && !tmp.empty()) {
                        auto uv = backend_->update_mesh_vertices(
                            gm.surfaces[0].handle,
                            tmp.data(),
                            static_cast<uint32_t>(tmp.size()));
                        if (!uv) {
                            std::fprintf(stderr,
                                "cloth re-upload skipped: %s\n",
                                uv.error().message.c_str());
                        }
                    }
                }
            }

            // Wind debug arrows — one per cloth body, pointing in
            // world-space along the authored wind vector. Lets us
            // verify visually that the simulation sees the right
            // direction. Each arrow's length is wind speed in
            // metres × an extra visualisation factor so it shows
            // up clearly in the scene.
            if (config_.wind_debug && !scene_.cloths.empty()) {
                std::vector<IRhiBackend::WindArrow> arrows;
                arrows.reserve(scene_.cloths.size());
                constexpr float kArrowScale = 0.5f;  // 1 m/s wind = 0.5 m arrow
                for (const auto& cloth : scene_.cloths) {
                    if (cloth.mesh_index < 0 ||
                        static_cast<std::size_t>(cloth.mesh_index) >=
                            scene_.instances.size()) {
                        continue;
                    }
                    // Anchor centroid in body-local space.
                    glm::vec3 anc(0);
                    int n = 0;
                    for (int idx : cloth.anchor_idx) {
                        if (idx < 0 ||
                            static_cast<std::size_t>(idx) >=
                                cloth.rest_pos.size()) {
                            continue;
                        }
                        anc += cloth.rest_pos[idx]; n++;
                    }
                    if (n > 0) {
                        anc /= static_cast<float>(n);
                    }
                    // Convert body-local anchor centroid to world
                    // via the cloth instance's transform. The
                    // ClothBody stores `world_transform` set at
                    // load time — but flags are static-pole bound
                    // (no per-frame instance movement), so this
                    // is fine.
                    glm::vec4 world_anchor =
                        cloth.world_transform * glm::vec4(anc, 1.0f);
                    IRhiBackend::WindArrow a;
                    a.origin    = glm::vec3(world_anchor);
                    a.direction = cloth.wind_velocity_world * kArrowScale;
                    arrows.push_back(a);
                }
                backend_->set_wind_debug_arrows(arrows);
            }

            // Material resolver lambda — captures backend for texture loading
            auto resolver = [this](const std::string& mesh_path,
                                   const std::string& surface_name) -> GpuMaterial {
                return resolve_scene_material(mesh_path, surface_name, config_);
            };

            // The overlay draw callback (set in init) runs inside render_frame
            // and writes this frame's intents into last_intents_. Reset it first
            // so a disabled overlay (callback never runs) leaves no stale intent.
            last_intents_ = OverlayIntents{};

            // Advance the persistent weather state, then derive this frame's
            // lighting from it. The WeatherState constructor (in init()) already
            // applied config.weather.rain_intensity, so the main loop only needs
            // to advance the dry-down transition. `dt` (from stats_.begin_frame())
            // is the same frame-rate-independent wall-clock delta that drives
            // every other animated system in this loop, so an in-progress dry-down
            // completes in the same elapsed time at any frame rate.
            task_scope.reset();
            task_scope.emplace("Weather and lighting", "Simulation");
            weather_.advance(dt);

            // Push the base time-of-day lighting through the overcast tint so
            // sun intensity, sky, and ambient all shift toward stormy together
            // with the precipitation, wetness, and fog responses.
            // At rain intensity 0 overcast_lighting returns the base unchanged,
            // preserving the Shipping_Look. The base is recomputed fresh
            // each frame so the tint is never applied on top of itself.
            const float frame_time_of_day = was_running
                ? bench_->time_of_day(config_.time_of_day)
                : config_.time_of_day;
            lighting_ = weather_.overcast_lighting(
                lighting_for_time_of_day(frame_time_of_day));

            // A score must represent the requested drawable resolution.
            // Synchronize the backend immediately before submission so the
            // first through last measured frames have adaptive resolution off.
            backend_->set_benchmark_measurement(was_running);
            if (capture_.output && !capture_requested_ &&
                capture_presented_frames_ + 1 == capture_.frames + capture_index_) {
                auto requested = backend_->request_screenshot(capture_path());
                if (!requested) throw std::runtime_error(requested.error().message);
                capture_requested_ = true;
            }

            // Drive the frame with the one persistent weather state: overcast
            // lighting (above), the fog-density override and wetness/precipitation
            // responses (inside the backend / features) all read this same state.
            tonemap_.bloom_strength = config_.bloom_strength;
            task_scope.reset();
            task_scope.emplace("Rendering", "Rendering");
            auto submission_context = trace_context;
            submission_context.submission_id = microprofile_submission_id_ + 1;
            backend_->set_trace_frame_context(submission_context);
            const FrameResult frame_result = backend_->render_frame(
                scene_.instances, moving_instance_indices_, scene_.gpu_meshes,
                resolver, camera_, lighting_, weather_, tonemap_,
                window_.width, window_.height);
            latest_frame_timings_ = frame_result.timings;
            submission_retry_.observe_gpu_ms(frame_result.timings.gpu_ms);
            if (frame_result.presented) microprofile_scene_ready_ = true;
            trace_iteration.finish(frame_result.presented ? profile::FrameOutcome::Accepted
                : profile::FrameOutcome::Rejected,
                frame_result.presented ? ++microprofile_submission_id_ : 0);
            task_scope.reset();
            task_scope.emplace("Frame statistics and diagnostics", "Application");
            if (capture_.output) {
                advance_capture = frame_result.presented;
                if (frame_result.presented) ++capture_presented_frames_;
                if (capture_requested_) {
                    auto complete = backend_->screenshot_complete();
                    if (!complete) throw std::runtime_error(complete.error().message);
                    if (*complete) {
                        finish_capture();
                        if (capture_finished_) running = false;
                    }
                }
            }
            // Filament presents internally via endFrame() — no
            // explicit swap_buffers call needed.
            const float presented_interval = frame_result.presented
                ? stats_.record_presented_frame()
                : 0.0f;
            // Keep hardware polling far out of the hot path: the accepted-frame
            // statistics tick at most once per second, which is also a useful
            // cadence for DRM sensors and Vulkan heap-budget estimates.
            if (stats_.fps_updated()) {
                active_gpu_telemetry_ = backend_->gpu_telemetry();
                if (micro_recorder_.active()) {
                    micro_recorder_.counter("GPU utilization percent", active_gpu_telemetry_.utilization_percent, "Telemetry");
                    micro_recorder_.counter("GPU temperature C", active_gpu_telemetry_.temperature_c, "Telemetry");
                }
            }

            if (capture_.profile_output && frame_result.presented && presented_interval > 0.0f) {
                if (record_profile_frame(*capture_.profile_output, presented_interval, frame_result.timings)) running = false;
            }

            // Benchmark throughput is based on frames accepted by Filament,
            // not loop iterations. BenchmarkStats measures the wall-clock
            // interval directly between accepted endFrame completions, so
            // rejected beginFrame attempts and long hitches are included
            // without the simulation-delta clamp distorting the score.
            if (was_running) {
                if (frame_result.presented && presented_interval > 0.0f) {
                    if (bench_->will_complete_on_present()) {
                        bench_->set_gpu_benchmark_times(
                            backend_->finalize_benchmark_gpu_times());
                    }
                    bench_->record_presented_frame(
                        presented_interval);
                    perf_frame_times_.push_back(presented_interval);
                    perf_timing_sum_.camera_ms += frame_result.timings.camera_ms;
                    perf_timing_sum_.scene_sync_ms += frame_result.timings.scene_sync_ms;
                    perf_timing_sum_.feature_ms += frame_result.timings.feature_ms;
                    perf_timing_sum_.render_submit_ms +=
                        frame_result.timings.render_submit_ms;
                    if (frame_result.timings.gpu_ms >= 0.0) {
                        perf_gpu_ms_sum_ += frame_result.timings.gpu_ms;
                        ++perf_gpu_samples_;
                    }
                    ++perf_timing_samples_;
                }
            }
            if (was_running && !bench_->is_running()) {
                if (bench_->state() == BenchmarkState::Complete) {
                    save_completed_benchmark_report();
                }
                report_perf_window();
                if (const char* exit_once =
                        std::getenv("HEAVEN_BENCHMARK_EXIT");
                    exit_once && std::strcmp(exit_once, "0") != 0) {
                    running = false;
                }
            }

            // Service the overlay's configuration edits/intents captured during
            // the draw callback BEFORE the benchmark intents, in the fixed order
            // edits → save → load → reload, and before the next render_frame call
            // (in-ui-configuration). This applies Live_Settings
            // to the running renderer, services save/load through the Config_File
            // module, and runs any Benchmark_Reload.
            applyConfigIntents();
            service_microprofiler();
            if (retired_reload_error_) break;

            // Apply the overlay intents captured during the draw callback. Quit
            // is honoured BEFORE start so a simultaneous quit+start quits without
            // beginning a run. dismiss/start map onto the
            // state machine; Cancel is available in the toolbar during playback.
            if (last_intents_.quit) {
                running = false;
            } else if (last_intents_.cancel) {
                cancel_active_benchmark();
            } else if (last_intents_.dismiss_result) {
                bench_->dismiss_result();
            } else if (last_intents_.start_benchmark) {
                request_benchmark_start();
            }

            if (!printed_tex_stats && stats_.frame_count() > 1) {
                printed_tex_stats = true;
                std::printf("  Textures: %d matched, %d unmatched\n",
                            materials_.hits(), materials_.misses());
                if (!materials_.missed_surfaces().empty()) {
                    std::printf("  Unmatched surfaces:\n");
                    for (const auto& s : materials_.missed_surfaces())
                        std::printf("    %s\n", s.c_str());
                }
            }

            if (micro_recorder_.active() && (window_.width != microprofile_resolution_width_ ||
                    window_.height != microprofile_resolution_height_)) {
                microprofile_configuration_changed();
                microprofile_resolution_width_ = window_.width;
                microprofile_resolution_height_ = window_.height;
            }
            if (microprofile_.output_base && microprofile_runtime_started_ && !microprofile_finished_) {
                const auto now = profile::Recorder::steady_now_ns();
                if (microprofile_capture_stop_ns_ == 0 &&
                    now - microprofile_runtime_begin_ns_ >= static_cast<uint64_t>(microprofile_.seconds * 1e9)) {
                    micro_recorder_.set_active(false);
                    bench_->set_microprofile_recording(false);
                    microprofile_capture_stop_ns_ = now;
                }
                // Keep rendering and servicing input during a bounded query
                // drain. No query wait, finish, readback, or GPU flush is added.
                const auto export_status = micro_writer_.status();
                const auto export_state = export_status.state;
                if (export_state == profile::ReportState::Error) {
                    microprofile_export_error_ = true;
                    microprofile_export_error_message_ = export_status.error;
                }
                if (microprofile_capture_stop_ns_ != 0 && now - microprofile_capture_stop_ns_ >= 500'000'000ull &&
                    export_state != profile::ReportState::Queued && export_state != profile::ReportState::Writing) {
                    save_microprofile(*microprofile_.output_base);
                    microprofile_finished_ = true;
                    if (!capture_.output && !capture_.profile_output && !bench_->is_running()) running = false;
                }
            }

            // Filament can reject submissions while retiring GPU work or
            // enforcing its skip policy.
            // Retry after a short scheduler wait rather than flooding its
            // command stream and the recorder with unchanged render attempts.
            // Accepted frames stay uncapped. Service input-derived intents and
            // exports above first; the next frame clock includes this wait and
            // fixed-step captures still advance only after an accepted frame.
            if (!frame_result.presented && running && !exit_requested_) {
                task_scope.reset();
                task_scope.emplace("Rejected submission retry", "Wait");
                // Scale retries to the last asynchronously completed GPU
                // duration: about sixteen checks per GPU frame, with an
                // eight-millisecond ceiling for responsive controls. No query
                // is waited on; unavailable timing uses a one-millisecond retry.
                std::this_thread::sleep_for(submission_retry_.delay());
            }

        }
    }

    // Concrete RendererEffects over the live renderer subsystems
    // (in-ui-configuration). The configuration controller drives these
    // when a Live_Setting changes or a Benchmark_Reload is requested; each method
    // forwards to a subsystem the Application already owns. Nested so it can reach
    // the Application's private members directly.
    struct AppRendererEffects final : RendererEffects {
        Application& app;
        explicit AppRendererEffects(Application& a) : app(a) {}

        // Live feature toggle → the supported runtime feature API. An unmatched
        // name is a safe no-op inside the backend.
        void set_feature_enabled(const char* name, bool on) override {
            if (app.backend_) app.backend_->set_feature_enabled(name, on);
        }

        // Apply a single non-feature Live_Setting. The controller has already
        // written the new value into `active` (== config_). Most settings are
        // consumed directly from that application-owned object; physical-camera
        // controls are explicitly forwarded because the backend owns a separate
        // startup configuration copy.
        void apply_live(SettingId id, const BenchConfig& active) override {
            switch (id) {
                case SettingId::PhysicalCameraEnabled:
                case SettingId::CameraAperture:
                case SettingId::CameraShutter:
                case SettingId::CameraIso:
                    if (app.backend_) {
                        app.backend_->set_camera_config(
                            active.features.physical_camera,
                            active.physical_camera);
                    }
                    break;
                case SettingId::FeatureSoftShadows:
                case SettingId::FeatureContactShadows:
                case SettingId::FeaturePointLightShadows:
                case SettingId::FeatureReflections:
                case SettingId::SoftShadowLevel:
                case SettingId::ContactShadowLevel:
                case SettingId::ShadowLightBudget:
                    if (app.backend_) {
                        app.backend_->set_live_render_config(active.features, active.quality);
                    }
                    break;
                case SettingId::WindDebug:
                    if (app.backend_) {
                        app.backend_->set_feature_enabled("wind-arrows", active.wind_debug);
                    }
                    break;
                case SettingId::MusicVolume:
                    if (app.music_player_)
                        app.music_player_->set_volume(active.music.volume);
                    break;
                case SettingId::RainIntensity:
                    app.weather_.set_intensity(active.weather.rain_intensity);
                    break;
                case SettingId::DryDownSeconds:
                    app.weather_.cfg.dry_down_seconds =
                        active.weather.dry_down_seconds;
                    break;
                default:
                    // Time-of-day and application-owned per-frame knobs are
                    // consumed straight from config_ next frame.
                    break;
            }
        }

        // Cancel any in-flight run before a Benchmark_Reload rebuilds the scene

        void cancel_active_run() override {
            app.cancel_active_benchmark();
        }

        // Rebuild the scene/pipeline from the about-to-become-active
        // Working_Configuration.
        Result<void> rebuild() override { return app.rebuildFromActiveConfig(); }
    };

    // Service one frame's overlay configuration edits/intents (in-ui-configuration
    // Delegates to the GPU-free controller, which applies edits
    // → save → load → reload over the Working/Active configs, drives Live_Settings
    // and the Benchmark_Reload through AppRendererEffects, and surfaces diagnostics
    // into config_ui_. Called right after render_frame returns and before the
    // existing benchmark-intent block.
    void applyConfigIntents() {
        AppRendererEffects fx(*this);
        OverlayIntents intents = last_intents_;
        if (!intents.edits.empty() || intents.load || intents.reload)
            submission_retry_.reset();
        const bool trace_change = micro_recorder_.active() &&
            (!intents.edits.empty() || intents.load || intents.reload);
        const auto previous_settings = trace_change ? serialize_config(config_) : std::string{};
        resolve_config_paths_for_ui(intents);
        apply_config_intents(working_config_, config_, config_ui_, intents,
                             fx);
        if (trace_change && previous_settings != serialize_config(config_))
            microprofile_configuration_changed();

        // The controller does not handle the diagnostics-dismiss intent: it is a
        // pure UI-feedback action with no configuration effect, so the Main_Loop
        // clears the surfaced feedback here.
        if (last_intents_.dismiss_diagnostics) {
            config_ui_.diagnostics.clear();
            config_ui_.success = false;
            config_ui_.success_detail.clear();
        }
    }

    [[nodiscard]] std::string runtime_relative_config_path(
            const std::string& path) const {
        return runtime_paths_.resolve_ui_config_path(path).string();
    }

    void resolve_config_paths_for_ui(OverlayIntents& intents) const {
        if (intents.save) {
            intents.save_path = runtime_relative_config_path(intents.save_path);
        }
        if (intents.load) {
            intents.load_path = runtime_relative_config_path(intents.load_path);
        }
    }

    // Benchmark_Reload: rebuild the scene and the Reload-consuming setup from the
    // Working_Configuration (in-ui-configuration). The
    // controller advances config_ (Active) only on success. That configuration
    // guarantee does not restore retired GPU resources. Generated CPU inputs
    // are prepared first; a failure after context replacement or backend reload
    // stops the main loop before it can render the old scene's stale handles.
    [[nodiscard]] Result<void> rebuildFromActiveConfig() {
        submission_retry_.reset();
        struct ResetLoadingClock {
            BenchmarkStats& stats;
            ~ResetLoadingClock() { stats.reset_frame_clock(); }
        } reset_clock{stats_};
        loading_cancelled_ = false;
        if (!assets_) {
            return std::unexpected(Error{"Cannot reload before the asset context is initialized"});
        }
#ifdef __ANDROID__
        // A loaded desktop configuration cannot resize the Android Surface.
        // Normalize the pending request before reload consumes it; the
        // controller advances Active from this Working copy only on success.
        const char* driver = SDL_GetCurrentVideoDriver();
        if (window_.window && driver && SDL_strcmp(driver, "android") == 0) {
            int width = 0;
            int height = 0;
            get_drawable_size(window_.window, width, height);
            if (width > 0 && height > 0) {
                working_config_.width = width;
                working_config_.height = height;
                working_config_.fullscreen = true;
                overlay_ui_.set_available_resolutions(
                    gather_window_resolutions(window_.window));
            }
        }
#endif

        // Cancel any active run before rebuilding. The
        // controller also calls cancel_active_run() first; this is idempotent.
        if (bench_ && bench_->is_running()) cancel_active_benchmark();

        const bool switching_scene =
            working_config_.render_test_scene != config_.render_test_scene;
        const bool changing_asset_root = working_config_.data_dir != config_.data_dir;
        const bool previous_collision_enabled = camera_collision_.enabled();
        bool resources_retired = false;
        const auto fail = [&](std::string_view stage, const Error& error) -> Result<void> {
            Error failure{"Reload failed at " + std::string(stage) + ": " + error.message +
                (resources_retired
                    ? "; rendering stopped after resource/context retirement"
                    : "; previous scene remains active")};
            if (resources_retired) retired_reload_error_ = failure;
            else std::fprintf(stderr, "%s\n", failure.message.c_str());
            return std::unexpected(std::move(failure));
        };

        if (!working_config_.render_test_scene.empty() &&
            !render_test_scene_uses_assets(working_config_.render_test_scene)) {
            auto definition = make_render_test_scene(working_config_.render_test_scene);
            if (!definition) return fail("procedural scene generation", definition.error());
            auto generated = AssetFileSystem::create_memory("render-test-assets");
            if (!generated) return fail("generated asset namespace", generated.error());
            if (auto r = mount_render_test_scene_assets(*definition, *generated); !r)
                return fail("generated texture mount", r.error());
            auto paths = PathResolver::create(generated->data_root());
            if (!paths) return fail("generated path resolver", paths.error());
            // All generated inputs are ready before retiring feature borrowers.
            // Reload can fail after partial teardown, so mark the boundary first.
            // Audio startup borrows the current namespace. Join its worker
            // before replacing that namespace, while the borrowed assets live.
            if (music_player_) music_player_->stop();
            resources_retired = true;
            if (auto r = backend_->reconfigure_for_reload(working_config_); !r)
                return fail("procedural backend reload", r.error());
            // The backend has removed the old scene/depth borrowers. Release
            // only their mesh owners before uploading the generated scene.
            for (const auto& mesh : scene_.gpu_meshes) backend_->release_mesh(mesh);
            scene_.gpu_meshes.clear();
            assets_ = std::move(*generated);
            paths_ = std::move(*paths);
            backend_->set_asset_context(std::cref(*paths_), std::cref(*assets_));
            auto test = upload_render_test_scene(std::move(*definition), *backend_, working_config_);
            if (!test) return fail("procedural scene upload", test.error());
            scene_ = std::move(test->scene);
            render_test_ = std::move(*test);
            camera_collision_.set_enabled(false);
        } else {
            if (!config_.render_test_scene.empty() || changing_asset_root) {
                auto real_assets = AssetFileSystem::open(working_config_.data_dir);
                if (!real_assets) return fail("Heaven asset namespace", real_assets.error());
                auto real_paths = PathResolver::create(working_config_.data_dir);
                if (!real_paths) return fail("Heaven path resolver", real_paths.error());
                if (working_config_.features.ground_relief) {
                    auto generated = ensure_ground_height(*real_paths, *real_assets);
                    if (!generated) {
                        std::fprintf(stderr,
                            "Ground relief generation failed (normal-map fallback): %s\n",
                            generated.error().message.c_str());
                    } else if (generated->has_value()) {
                        std::printf("  Generated village ground height in memory\n");
                    }
                }
                if (music_player_) music_player_->stop();
                resources_retired = true;
                assets_ = std::move(*real_assets);
                paths_ = std::move(*real_paths);
                backend_->set_asset_context(std::cref(*paths_), std::cref(*assets_));
                // build() is additive; a new namespace must not retain the
                // previous root's definitions or borrowed GPU texture handles.
                materials_ = RhiMaterialDatabase{};
            }

            // Re-run the scene load that consumes the Reload_Settings (resolution,
            // tessellation geometry, scene-build quality, material gates from
            // features) from the new configuration. A local keeps scene_ until
            // success, but cannot undo an already replaced asset context.
            if (!assets_) {
                return fail("Heaven asset namespace", Error{"No active assets to reload"});
            }
            const std::string cache_dir = runtime_paths_.tessellation_cache_file(
                working_config_.tessellation.cache_file, working_config_.data_dir,
                assets_->is_zip());
            working_config_.tessellation.cache_file = cache_dir;
            auto prepared = load_scene_in_background(working_config_, cache_dir);
            if (!prepared) return fail("Heaven scene preparation", prepared.error());
            // GPU upload remains on the video owner, pumping progress/events
            // between meshes. A failed/cancelled upload retires only its owners.
            if (auto uploaded = upload_prepared_scene(prepared->scene, working_config_); !uploaded)
                return fail("Heaven scene upload", uploaded.error());

            // The local scene has loaded successfully, so it is now safe to tear
            // down persistent renderables and texture-backed material instances.
            // A failed reload can already have cleared those objects. Stop
            // rendering on failure rather than treating config rollback as
            // restoration of the old TextureStore/material references.
            resources_retired = true;
            if (auto r = backend_->reconfigure_for_reload(working_config_); !r) {
                // The replacement has no borrowers even if reconfiguration
                // fails; its local RhiScene does not itself own GPU resources.
                for (const auto& mesh : prepared->scene.gpu_meshes) backend_->release_mesh(mesh);
                return fail("Heaven backend reload", r.error());
            }
            materials_.clear_resolved_cache();

            // New meshes were uploaded while the old scene was still active.
            // Its renderable/depth borrowers are now gone; retain the new mesh
            // owners and retire only the old handles before replacing scene_.
            for (const auto& mesh : scene_.gpu_meshes) backend_->release_mesh(mesh);

            // Success: swap in the freshly built scene and re-apply the rest of the
            // Reload-consuming init sequence from working_config_.
            scene_ = std::move(prepared->scene);
            materials_ = std::move(prepared->materials);
            camera_collision_ = std::move(prepared->collision);
            render_test_.reset();
        }

        if (auto r = prepare_displacement(working_config_); !r)
            return fail("static displacement preparation", r.error());

        if (switching_scene) {
            render_time_ = 0.0f;
            if (!working_config_.render_test_scene.empty()) {
                if (config_.render_test_scene.empty())
                    ordinary_scene_collision_enabled_ = previous_collision_enabled;
                camera_collision_.set_enabled(false);
                const auto scenes = render_test_scene_catalog();
                const auto it = std::ranges::find(scenes, working_config_.render_test_scene, &RenderTestSceneInfo::id);
                if (it != scenes.end()) {
                    camera_.position = {it->camera_pose[0], it->camera_pose[1], it->camera_pose[2]};
                    camera_.yaw = it->camera_pose[3];
                    camera_.pitch = it->camera_pose[4];
                    camera_.fov = it->fov;
                }
            } else {
                camera_collision_.set_enabled(
                    ordinary_scene_collision_enabled_ &&
                    !capture_.output && !capture_.profile_output);
            }
        }

        if (switching_scene || changing_asset_root) {
            if (bench_) bench_->set_scene_markers(nullptr);
            scene_markers_.reset();
            camera_path_ = CameraPath{};
            if (working_config_.render_test_scene.empty()) {
                if (!paths_ || !assets_)
                    return fail("camera tracks", Error{"Asset context and paths are not initialized"});
                const auto& paths = *paths_;
                const auto& assets = *assets_;
                camera_path_ = load_camera_path(paths.track_dir(), nullptr, std::cref(assets));
                scene_markers_ = load_scene_markers(paths.track_dir(), nullptr, std::cref(assets));
            }
            if (bench_) bench_->set_scene_markers(
                scene_markers_ ? &*scene_markers_ : nullptr);
        }

        for (auto& cloth : scene_.cloths) cloth.sim_hz = working_config_.cloth_sim_hz;
        backend_->set_particle_emitters(scene_.particle_emitters);
        backend_->set_scene_lights(scene_.lights);

        // Restart the background music with the (possibly changed) music config.
        // music.enabled is a Reload_Setting, so a restart is how a toggle takes
        // effect. stop() is idempotent and joins the prior thread before the new
        // player starts.
        if (music_player_) music_player_->stop();
        if (paths_ && working_config_.render_test_scene.empty()) {
            if (!assets_) return fail("music restart", Error{"Asset context is not initialized"});
            const auto& paths = *paths_;
            const auto& assets = *assets_;
            music_player_ = std::make_unique<MusicPlayer>();
            music_player_->start(working_config_.music, paths,
                                 std::cref(assets));
        }

        // Re-seed the persistent weather + lighting from the new configuration so
        // the next frame reads consistent state.
        weather_  = WeatherState(working_config_.weather);
        lighting_ = lighting_for_time_of_day(working_config_.time_of_day);
        // Apply the window resolution / fullscreen (both Reload settings) to the
        // live SDL window so the change actually resizes the window, then
        // recreate the backend swap chain and refresh the cached drawable size
        // the per-frame viewport is taken from. Without this the new width/height
        // would only live in config_ and never reach the window or the renderer.
        if (window_.window) {
            if (working_config_.fullscreen) {
                SDL_SetWindowFullscreen(window_.window,
                                        SDL_WINDOW_FULLSCREEN_DESKTOP);
            } else {
                SDL_SetWindowFullscreen(window_.window, 0);
                window_.set_drawable_size(
                    working_config_.width, working_config_.height);
            }
            // Re-read the actual drawable size (HiDPI / fullscreen-desktop may
            // differ from the requested logical size) and rebuild the swap chain.
            get_drawable_size(window_.window, window_.width, window_.height);
            backend_->resize(window_.width, window_.height);
        }

        return {};
    }

    void cancel_active_benchmark() {
        submission_retry_.reset();
        if (bench_) bench_->cancel();
        benchmark_config_snapshot_.reset();
        perf_frame_times_.clear();
        perf_timing_sum_ = {};
        perf_timing_samples_ = 0;
        perf_gpu_ms_sum_ = 0.0;
        perf_gpu_samples_ = 0;
    }

    bool request_benchmark_start() {
        submission_retry_.reset();
        if (!config_.render_test_scene.empty()) return false;
        if (!bench_ || bench_->state() != BenchmarkState::Idle) return false;
        bench_->set_microprofile_recording(micro_recorder_.active());
        profile::Scope cpu_score_scope("CPU benchmark preparation", "Benchmark");
        if (window_.width != config_.width ||
            window_.height != config_.height) {
            std::fprintf(
                stderr,
                "Benchmark start refused: configured resolution is %dx%d, "
                "but the live drawable is %dx%d. Use a display mode "
                "and scaling configuration that can provide the requested "
                "pixel extent.\n",
                config_.width, config_.height,
                window_.width, window_.height);
            return false;
        }
        backend_->show_loading_screen(
            config_.data_dir, "Benchmarking CPU",
            window_.width, window_.height);
        std::printf("\n=== CPU Benchmark Phase ===\n");
        const std::string cache_dir =
            runtime_paths_.user_directory()
                ? runtime_paths_.tessellation_cache_file(
                      config_.tessellation.cache_file, config_.data_dir,
                      assets_ && assets_->is_zip())
                : (paths_ ? paths_->cache_dir() : std::string{});
        auto measured = run_cpu_benchmark(
            config_, materials_, cache_dir, camera_path_, 3.0,
            assets_ ? AssetFileSystemRef{std::cref(*assets_)} : std::nullopt);
        if (!measured) {
            std::fprintf(stderr, "CPU benchmark failed: %s\n",
                         measured.error().message.c_str());
            return false;
        }
        cpu_metrics_ = *measured;
        bench_->set_cpu_benchmark_metrics(cpu_metrics_);
        std::printf(
            "CPU: %.1f updates/s | avg %.3f ms | p95 %.3f ms | "
            "p99 %.3f ms | score %.0f\n",
            cpu_metrics_.updates_per_second,
            cpu_metrics_.timing.average_ms,
            cpu_metrics_.timing.p95_ms,
            cpu_metrics_.timing.p99_ms,
            score_from_cpu_rate(cpu_metrics_.updates_per_second));
        if (config_.cpu_stress.enabled) {
            backend_->show_loading_screen(
                config_.data_dir, "CPU Torture: all cores",
                window_.width, window_.height);
            auto stressed = run_cpu_stress_benchmark(
                config_, materials_, cache_dir, camera_path_,
                assets_ ? AssetFileSystemRef{std::cref(*assets_)} : std::nullopt);
            if (!stressed) {
                std::fprintf(stderr, "CPU stress phase failed: %s\n",
                             stressed.error().message.c_str());
                return false;
            }
            cpu_stress_metrics_ = std::move(*stressed);
            bench_->set_cpu_stress_metrics(cpu_stress_metrics_);
            std::printf(
                "CPU stress: %d workers | %.1f aggregate updates/s | %.1f s\n",
                cpu_stress_metrics_.workers,
                cpu_stress_metrics_.aggregate_updates_per_second,
                cpu_stress_metrics_.duration_seconds);
        }
        if (!bench_->request_start()) {
            return false;
        }
        stats_.reset_frame_clock();
        benchmark_config_snapshot_ = config_;
        return true;
    }

    void save_completed_benchmark_report() {
        if (!benchmark_config_snapshot_) {
            std::fprintf(
                stderr,
                "[benchmark] Result export skipped: start configuration is "
                "unavailable\n");
            return;
        }

        bench_->set_renderer_feature_stats(backend_->renderer_feature_stats());
        const BenchmarkStatus completed = bench_->status();
        if (!completed.has_result ||
            completed.state != BenchmarkState::Complete) {
            return;
        }

        const BenchmarkReportWriteResult report = write_benchmark_report(
            completed.result, *benchmark_config_snapshot_,
            runtime_paths_.results_directory(), kApplicationBuildInfo);
        if (report.success()) {
            std::fprintf(stderr,
                         "[benchmark] JSON result saved: %s\n",
                         report.json_path.string().c_str());
            std::fprintf(stderr,
                         "[benchmark] HTML result saved: %s\n",
                         report.html_path.string().c_str());
        } else {
            for (const std::string& diagnostic : report.diagnostics) {
                std::fprintf(stderr,
                             "[benchmark] Result export failed: %s\n",
                             diagnostic.c_str());
            }
        }
        benchmark_config_snapshot_.reset();
    }

    [[nodiscard]] DebugSidebarInfo debug_sidebar_info() const {
        DebugSidebarInfo info;

        // Presented-frame rates only: rejected frame-skipper iterations never
        // inflate these values. Stage timings are the latest submitted frame;
        // the completed GPU timestamp can naturally lag by a few frames.
        info.fps = stats_.current_fps();
        info.frame_time_ms = stats_.current_frame_time_ms();
        info.one_percent_low_fps = stats_.current_one_percent_low_fps();
        info.gpu_frame_ms = latest_frame_timings_.gpu_ms;
        info.camera_ms = latest_frame_timings_.camera_ms;
        info.scene_sync_ms = latest_frame_timings_.scene_sync_ms;
        info.feature_ms = latest_frame_timings_.feature_ms;
        info.render_submit_ms = latest_frame_timings_.render_submit_ms;

        info.gpu_name = active_gpu_name_;
        info.gpu_driver = active_gpu_info_.driver_name;
        info.gpu_driver_info = active_gpu_info_.driver_info;
        info.gpu_driver_version = active_gpu_info_.driver_version;
        info.graphics_api = backend_ ? backend_->render_api_name() : std::string{};
        info.graphics_api_version = active_gpu_info_.api_version;
        if (active_gpu_info_.pci_domain >= 0 && active_gpu_info_.pci_bus >= 0 &&
            active_gpu_info_.pci_device >= 0 &&
            active_gpu_info_.pci_function >= 0) {
            info.pci_address = std::format(
                "{:04x}:{:02x}:{:02x}.{:x}", active_gpu_info_.pci_domain,
                active_gpu_info_.pci_bus, active_gpu_info_.pci_device,
                active_gpu_info_.pci_function);
        }
        info.gpu_utilization_percent =
            active_gpu_telemetry_.utilization_percent;
        info.memory_utilization_percent =
            active_gpu_telemetry_.memory_utilization_percent;
        info.process_vram_used_mb =
            active_gpu_telemetry_.process_vram_used_mb;
        info.process_vram_budget_mb =
            active_gpu_telemetry_.process_vram_budget_mb;
        info.device_vram_used_mb =
            active_gpu_telemetry_.device_vram_used_mb;
        info.device_vram_total_mb =
            active_gpu_telemetry_.device_vram_total_mb;
        info.gpu_temperature_c = active_gpu_telemetry_.temperature_c;
        info.gpu_power_w = active_gpu_telemetry_.power_w;
        info.gpu_core_clock_mhz = active_gpu_telemetry_.core_clock_mhz;
        info.gpu_memory_clock_mhz = active_gpu_telemetry_.memory_clock_mhz;
        info.gpu_fan_rpm = active_gpu_telemetry_.fan_rpm;

        info.vertices = scene_.total_vertices;
        info.triangles = scene_.total_triangles;
        info.base_vertices = scene_.base_vertices;
        info.base_triangles = scene_.base_triangles;
        info.mesh_count = scene_.gpu_meshes.size();
        info.instance_count = scene_.instances.size();

        if (backend_) {
            const RendererFeatureStats features =
                backend_->renderer_feature_stats();
            info.selected_texture_bytes = features.selected_texture_bytes;
            info.texture_count = features.srgb_textures +
                features.scalar_textures + features.normal_textures +
                features.hdr_textures;
            info.enabled_render_features = features.enabled_render_features;
            info.total_render_features = features.total_render_features;
            info.msaa_samples = features.msaa_samples;
            info.texture_quality =
                std::string(texture_quality_id(features.texture_quality_active));
        }

        info.width = window_.width;
        info.height = window_.height;
        info.fullscreen = config_.fullscreen;
        info.render_preset = std::string(render_preset_id(config_.render_preset));
        info.platform = active_system_info_.platform;
        info.cpu = active_system_info_.cpu;
        info.application_version = kApplicationBuildInfo.version;
        info.build_architecture = kApplicationBuildInfo.architecture;
        return info;
    }

    void print_texture_stats() const {
        if (materials_.misses() > 0) {
            std::printf("\nTexture resolution: %d/%d surfaces matched (%.0f%%)\n",
                        materials_.hits(), materials_.hits() + materials_.misses(),
                        100.0f * static_cast<float>(materials_.hits()) /
                            static_cast<float>(materials_.hits() + materials_.misses()));
        }
        if (backend_) {
            const RendererFeatureStats s = backend_->renderer_feature_stats();
            std::printf(
                "Texture quality: requested=%s active=%s hi-res=%llu "
                "original=%llu fallbacks=%llu (missing=%llu unsupported=%llu "
                "invalid/corrupt=%llu) "
                "selected_mip_bytes=%llu max_dimension=%u\n",
                std::string(texture_quality_id(s.texture_quality_requested)).c_str(),
                std::string(texture_quality_id(s.texture_quality_active)).c_str(),
                static_cast<unsigned long long>(s.hires_textures),
                static_cast<unsigned long long>(s.original_textures),
                static_cast<unsigned long long>(s.texture_fallbacks),
                static_cast<unsigned long long>(
                    s.texture_missing_variant_fallbacks),
                static_cast<unsigned long long>(
                    s.texture_unsupported_format_fallbacks),
                static_cast<unsigned long long>(
                    s.texture_invalid_variant_fallbacks),
                static_cast<unsigned long long>(s.selected_texture_bytes),
                s.effective_texture_max_dimension);
        }
    }

    bool record_profile_frame(const std::string& path, float interval, const FrameTimings& timings) {
        profile_elapsed_ += static_cast<double>(interval);
        profile_intervals_.push_back(interval);
        profile_samples_.push_back({interval, timings.camera_ms, timings.scene_sync_ms,
            timings.feature_ms, timings.render_submit_ms, timings.gpu_ms});
        if (profile_elapsed_ < capture_.profile_seconds) return false;
        const auto window = measure_perf_window(profile_intervals_);
        if (!window.valid) throw std::runtime_error("Profile has no complete 60-second measurement window");
        const auto errors = write_config_file(path + ".config.json", config_);
        if (!errors.empty()) throw std::runtime_error(errors.front());
        nlohmann::json report{
            {"schema", 1}, {"benchmark_score", false}, {"screenshot_readback", false},
            {"overlay_visible", overlay_ui_.microprofiler_open()}, {"floating_islands_frozen", false},
            {"api", backend_->render_api_name()}, {"gpu", active_gpu_name_},
            {"driver", active_gpu_info_.driver_name}, {"driver_version", active_gpu_info_.driver_version},
            {"width", window_.width}, {"height", window_.height},
            {"camera_position", {camera_.position.x, camera_.position.y, camera_.position.z}},
            {"camera_yaw", camera_.yaw}, {"camera_pitch", camera_.pitch}, {"camera_fov", camera_.fov},
            {"warmup_seconds", window.warmup_seconds}, {"measured_seconds", window.window_seconds},
            {"measured_frames", window.frames_in_window}, {"average_fps", window.mean_fps},
            {"columns", {"presented_interval_seconds", "camera_ms", "scene_sync_ms", "features_ms",
                         "render_submit_ms", "last_completed_gpu_ms"}},
            {"gpu_timing_note", "Asynchronous last-completed whole-frame samples; may lag or repeat, not per-pass timings"},
            {"samples_including_warmup", profile_samples_}};
        append_render_metadata(report);
        std::ofstream output(path);
        output << report.dump(2) << '\n';
        output.close();
        if (!output) throw std::runtime_error("Cannot write frame profile");
        profile_finished_ = true;
        std::printf("Profile complete: %.2f FPS over %.2f seconds after %.2f-second warmup: %s\n",
            window.mean_fps, window.window_seconds, window.warmup_seconds, path.c_str());
        return true;
    }

    // Report the camera-path measurement after excluding the warm-up. FPS is
    // informational: performance comparisons need a calibrated baseline with
    // matching hardware, drivers, output size, and effective settings. Clear
    // the series so the next run starts with a fresh measurement window.
    void report_perf_window() {
        const PerfWindow w = measure_perf_window(perf_frame_times_);
        perf_frame_times_.clear();

        if (perf_timing_samples_ > 0) {
            const double n = static_cast<double>(perf_timing_samples_);
            std::printf(
                "\n=== Render Stage Timing (presented frames) ===\n"
                "Camera %.3f ms | scene sync %.3f ms | features %.3f ms | "
                "render submission %.3f ms",
                perf_timing_sum_.camera_ms / n,
                perf_timing_sum_.scene_sync_ms / n,
                perf_timing_sum_.feature_ms / n,
                perf_timing_sum_.render_submit_ms / n);
            if (perf_gpu_samples_ > 0) {
                std::printf(" | whole-frame GPU %.3f ms",
                            perf_gpu_ms_sum_ /
                                static_cast<double>(perf_gpu_samples_));
            }
            std::printf("\n");
        }
        perf_timing_sum_ = {};
        perf_timing_samples_ = 0;
        perf_gpu_ms_sum_ = 0.0;
        perf_gpu_samples_ = 0;

        std::printf("\n=== Performance Window%s ===\n"
                    "API %s | output %dx%d\n",
                    w.valid ? "" : ": INCOMPLETE",
                    backend_->render_api_name().c_str(),
                    window_.width, window_.height);

        if (!w.valid) {
            std::printf(
                "Measured window %.1f s (warm-up %.1f s, %zu frames) is shorter "
                "than the required %.0f s window after the %.0f s warm-up "
                "period. Run the full --benchmark camera path for a complete "
                "measurement.\n",
                w.window_seconds, w.warmup_seconds, w.frames_in_window,
                kPerfMinWindowSeconds, kPerfWarmupSeconds);
            return;
        }

        std::printf("Mean FPS %.1f over %.1f s window "
                    "(warm-up %.1f s, %zu frames).\n",
                    w.mean_fps, w.window_seconds, w.warmup_seconds,
                    w.frames_in_window);
    }

    BenchConfig                    config_;
    RuntimePaths                  runtime_paths_;
    VisualCaptureOptions          capture_;
    MicroprofileOptions           microprofile_;
    profile::Recorder             micro_recorder_;
    profile::ReportWriter         micro_writer_;
    uint64_t                      microprofile_attempt_id_ = 0;
    uint64_t                      microprofile_submission_id_ = 0;
    uint64_t                      microprofile_flow_id_ = 0;
    uint64_t                      microprofile_snapshot_ns_ = 0;
    uint64_t                      microprofile_metadata_capture_id_ = 0;
    int                           microprofile_resolution_width_ = 0;
    int                           microprofile_resolution_height_ = 0;
    uint64_t                      microprofile_runtime_begin_ns_ = 0;
    uint64_t                      microprofile_capture_stop_ns_ = 0;
    uint64_t                      microprofile_history_ns_ = 10'000'000'000ull;
    bool                          microprofile_start_requested_ = false;
    bool                          microprofile_runtime_started_ = false;
    bool                          microprofile_scene_ready_ = false;
    bool                          microprofile_finished_ = false;
    bool                          microprofile_export_error_ = false;
    bool                          microprofile_initial_overlay_visible_ = false;
    bool                          microprofile_overlay_mixed_ = false;
    std::string                   microprofile_export_error_message_;
    std::string                   microprofile_feedback_;
    int                           capture_presented_frames_ = 0;
    int                           capture_index_ = 0;
    bool                          capture_requested_ = false;
    bool                          capture_finished_ = false;
    bool                          profile_finished_ = false;
    double                        profile_elapsed_ = 0.0;
    std::vector<float>             profile_intervals_;
    nlohmann::json                 profile_samples_ = nlohmann::json::array();
    // Asset path resolver — owned here (Application is the single owner) and
    // borrowed by collaborators via a non-owning const reference / pointer.
    // std::optional because PathResolver has only a private ctor + static
    // create(); it is assigned once in init() once the Data_Root is known.
    std::optional<PathResolver>    paths_;
    std::optional<AssetFileSystem> assets_;
    WindowHandle                   window_;
    std::unique_ptr<IRhiBackend>   backend_;
    IRhiBackend::GpuInfoRaw        active_gpu_info_;
    DriverCheck                   active_driver_check_;
    IRhiBackend::GpuTelemetry      active_gpu_telemetry_;
    std::string                    active_gpu_name_;
    SystemInfo                     active_system_info_;
    Camera                         camera_;
    // Background music subsystem (Music_Player). Owned here via unique_ptr and
    // started in init() after paths_ is set; stopped in ~Application() before
    // window_/SDL teardown. Declared after window_/backend_ so, even absent the
    // explicit stop() in the destructor body, member-destruction order (reverse
    // of declaration) destroys it — and thus calls its stop() — before window_
    // runs SDL_Quit(). GPU-free, never touched by the render loop.
    std::unique_ptr<MusicPlayer>   music_player_;
    RhiMaterialDatabase            materials_;
    RhiScene                       scene_;
    std::optional<RenderTestScene> render_test_;
    CameraPath                     camera_path_;
    std::optional<SceneMarkers>    scene_markers_;
    // Host OS info source for the benchmark Result_Report's System block
    // (Platform_Description / CPU_Description via uname / sysconf / proc).
    // Declared before bench_ so it outlives the BenchmarkSystem whose
    // SystemInfoGatherer lambda references it (the gatherer runs at run
    // completion, while this app instance is alive).
    LinuxHostInfoSource            host_info_source_;
    std::unique_ptr<BenchmarkSystem> bench_;
    OverlayUI                      overlay_ui_;
    OverlayIntents                 last_intents_;
    // In-UI configuration state. The
    // Working_Configuration is the mutable copy the Options_Panels edit (seeded
    // from config_ in init()); config_ui_ is the feedback block (dirty / error /
    // success / diagnostics) the controller writes and OverlayUI::build() reads.
    BenchConfig                    working_config_;
    std::optional<BenchConfig>     benchmark_config_snapshot_;
    ConfigUiState                  config_ui_;
    std::optional<Error>            retired_reload_error_;
    CameraCollision                camera_collision_;
    bool                           ordinary_scene_collision_enabled_ = true;
    bool                           loading_cancelled_ = false;
    bool                           loading_paused_ = false;
#ifdef __ANDROID__
    bool                           loading_resume_pending_ = false;
#endif
    bool                           exit_requested_ = false;
    uint64_t                       loading_frames_ = 0;
    std::vector<int>               moving_instance_indices_;
    bool                           mouse_look_ = false;
    float                          render_time_ = 0.0f;
    LightingParams                 lighting_;
    // Persistent weather state — the single source of truth for rain
    // intensity, the dry-down transition, and the overcast/wetness mapping.
    // Seeded from config.weather in init(), advanced once per frame in the
    // main loop, and threaded into render_frame so precipitation, surface
    // wetness, the fog override, and overcast lighting all read one state
    // Persists across frames so the dry-down can run.
    WeatherState                   weather_;
    TonemapParams                  tonemap_;
    BenchmarkStats                 stats_;
    FrameTimings                   latest_frame_timings_{};
    SubmissionRetryDelay           submission_retry_;
    CpuBenchmarkMetrics            cpu_metrics_{};
    CpuStressMetrics               cpu_stress_metrics_{};
    // Per-frame wall-clock deltas recorded over the active benchmark run, used
    // for the performance-window summary. Populated only while the
    // run drives the authored camera path and consumed by report_perf_window()
    // when the run completes.
    std::vector<float>             perf_frame_times_;
    FrameTimings                   perf_timing_sum_{};
    uint64_t                       perf_timing_samples_ = 0;
    double                         perf_gpu_ms_sum_ = 0.0;
    uint64_t                       perf_gpu_samples_ = 0;
};

// ── Entry point ─────────────────────────────────────────────────

int build_tessellation_cache_headless(
        const BenchConfig& config, const std::optional<std::string>& target) {
    auto assets = AssetFileSystem::open(config.data_dir);
    if (!assets) {
        std::fprintf(stderr, "Tessellation cache: %s\n",
                     assets.error().message.c_str());
        return 1;
    }
    auto paths = PathResolver::create(config.data_dir);
    if (!paths) {
        std::fprintf(stderr, "Tessellation cache: %s\n",
                     paths.error().message.c_str());
        return 1;
    }
    const std::string cache_file = target.value_or(
        config.tessellation.cache_file.empty()
            ? default_tessellation_cache_archive(
                  config.data_dir, assets->is_zip())
            : config.tessellation.cache_file);
    if (config.features.ground_relief) {
        auto generated = ensure_ground_height(*paths, *assets);
        if (!generated) {
            std::fprintf(
                stderr,
                "Ground relief generation failed (normal-map fallback): %s\n",
                generated.error().message.c_str());
        }
    }

    NullBackend backend;
    RhiMaterialDatabase materials;
    materials.build(*paths, nullptr, std::cref(*assets));
    auto scene = load_scene_rhi(config, backend, materials, cache_file,
                                nullptr, std::cref(*assets));
    if (!scene) {
        std::fprintf(stderr, "Tessellation cache build failed: %s\n",
                     scene.error().message.c_str());
        return 1;
    }
    std::printf(
        "Tessellation cache ready: %s (%llu vertices, %llu triangles)\n",
        cache_file.c_str(),
        static_cast<unsigned long long>(scene->total_vertices),
        static_cast<unsigned long long>(scene->total_triangles));
    return 0;
}

} // namespace

#ifdef __ANDROID__
// The separate SDL_main bridge calls this CMake-renamed entry point.
extern int heaven_main(int argc, char* argv[]);
#endif

int main(int argc, char* argv[]) try {
    // Resolve the Effective_Configuration first. resolve_config owns all CLI /
    // config diagnostics and the --help / --write-config early exits, and also
    // surfaces the used-asset packer activation intent below.
    std::vector<std::string_view> cli_args;
    cli_args.reserve(static_cast<std::size_t>(std::max(argc, 0)));
    for (int i = 0; i < argc; ++i) {
        cli_args.emplace_back(argv[i]);
    }

    std::optional<std::filesystem::path> executable_invocation;
    if (!cli_args.empty() && !cli_args.front().empty()) {
        executable_invocation = std::filesystem::path(cli_args.front());
    }

    const std::filesystem::path executable_dir =
        executable_dir_or_cwd(executable_invocation);
    const RuntimePaths runtime_paths(executable_dir, runtime_user_directory());
    ResolvedConfig resolved = resolve_config(cli_args, runtime_paths);
    if (runtime_paths.user_directory()) {
        std::filesystem::create_directories(*runtime_paths.user_directory());
    }

    // Used-asset packer hook (used-asset-packer). When the
    // --pack-assets token appeared on the command line we branch to the packer
    // BEFORE constructing Application, so the renderer, window, and Filament
    // backend are never initialized.
    //   • Target present: run the packer into that directory.
    //   • Target absent (--pack-assets alone, as the final token, or followed
    //     by another flag): default the Pack_Target to "assets/assets.zip". No
    //     error, no diagnostic.
    //   • Token absent: fall through to the normal renderer path. The
    //     packer resolves the Source_Tree from the same config Application would
    //     use, so the Used_Asset_Set matches a render run.
    if (resolved.pack.requested) {
        heaven::PackOptions opts;
        opts.target    = resolved.pack.target.value_or("assets/assets.zip");
        opts.move      = resolved.pack.move;
        opts.overwrite = resolved.pack.overwrite;
        opts.texture_set_manifest = resolved.pack.texture_set_manifest;
        return heaven::run_asset_packer(resolved.config, opts);
    }
    if (resolved.build_tessellation_cache) {
        return build_tessellation_cache_headless(
            resolved.config,
            resolved.tessellation_cache_target);
    }
    if (resolved.benchmark_loading) {
        const std::filesystem::path output = runtime_paths.loading_results_directory(
            resolved.loading_benchmark_output.value_or("loading-results"));
        auto result = run_loading_benchmark(resolved.config, output);
        if (!result) {
            std::fprintf(stderr, "Loading benchmark failed: %s\n",
                         result.error().message.c_str());
            return 1;
        }
        std::printf("Loading benchmark reports: %s\n",
                    output.string().c_str());
        return 0;
    }

    return Application(std::move(resolved.config), runtime_paths,
                       std::move(resolved.config_path), std::move(resolved.capture),
                       std::move(resolved.microprofile)).run();
} catch (const std::exception& error) {
    std::fprintf(stderr, "HeavenBench failed: %s\n", error.what());
    return 1;
} catch (...) {
    std::fprintf(stderr, "HeavenBench failed with an unknown exception\n");
    return 1;
}
