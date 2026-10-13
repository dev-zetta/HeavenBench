/*
 * HeavenBench — Overlay UI widget builder
 *
 * OverlayUI is a pure Dear ImGui immediate-mode widget builder. Given an
 * immutable BenchmarkStatus snapshot it emits the on-screen widgets for the
 * current frame and returns the set of user intents triggered this frame
 * (start a run, cancel, dismiss the result, quit, or open an Options_Panel).
 *
 * It issues ONLY ImGui::* calls, holds no engine/Filament types, and performs
 * no state mutation on the benchmark system — the caller (main loop) maps the
 * returned intents onto BenchmarkSystem. This keeps the widget code decoupled
 * from both the backend and the state machine, so it compiles headless (no
 * Filament) into the host test target alongside the vendored Dear ImGui.
 *
 * Layout retains accessible controls throughout playback:
 *   - Menu_Bar: Benchmark (Cancel while running), Settings, Quit, Credits
 *
 *   - Running overlay: "Benchmarking...", Scene k+1 / n, the stats block
 *     (Average FPS / Time / Frames / Min FPS / Max FPS), and the cancel hint
 *
 *   - Result report: Average FPS / Score / Min FPS / Max FPS + dismiss
 *
 */
#ifndef HEAVENBENCH_SRC_RHI_OVERLAY_OVERLAY_UI_H_
#define HEAVENBENCH_SRC_RHI_OVERLAY_OVERLAY_UI_H_

#include "benchmark/build_info.h"
#include "benchmark/benchmark_system.h"
#include "benchmark/driver_check.h"
#include "engine/cli_parser.h"   // heaven::BenchConfig (Working_Configuration snapshot)
#include "rhi/overlay/config_edit.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace heaven {

// GPU/API-free snapshot consumed by the diagnostics sidebar. The application
// gathers these values from the active backend and scene; OverlayUI only
// formats and draws them, preserving the headless-testable UI boundary.
struct DebugSidebarInfo {
    float fps = 0.0f;
    float frame_time_ms = 0.0f;
    float one_percent_low_fps = 0.0f;
    double gpu_frame_ms = -1.0;
    double camera_ms = 0.0;
    double scene_sync_ms = 0.0;
    double feature_ms = 0.0;
    double render_submit_ms = 0.0;

    std::string gpu_name;
    std::string gpu_driver;
    std::string gpu_driver_info;
    std::string gpu_driver_version;
    std::string graphics_api;
    std::string graphics_api_version;
    std::string pci_address;
    double gpu_utilization_percent = -1.0;
    double memory_utilization_percent = -1.0;
    double process_vram_used_mb = -1.0;
    double process_vram_budget_mb = -1.0;
    double device_vram_used_mb = -1.0;
    double device_vram_total_mb = -1.0;
    double gpu_temperature_c = -1.0;
    double gpu_power_w = -1.0;
    double gpu_core_clock_mhz = -1.0;
    double gpu_memory_clock_mhz = -1.0;
    double gpu_fan_rpm = -1.0;

    uint64_t vertices = 0;
    uint64_t triangles = 0;
    uint64_t base_vertices = 0;
    uint64_t base_triangles = 0;
    uint64_t mesh_count = 0;
    uint64_t instance_count = 0;
    uint64_t selected_texture_bytes = 0;
    uint64_t texture_count = 0;
    uint64_t enabled_render_features = 0;
    uint64_t total_render_features = 0;

    int width = 0;
    int height = 0;
    int msaa_samples = 1;
    bool fullscreen = false;
    std::string render_preset;
    std::string texture_quality;
    std::string platform;
    std::string cpu;
    std::string application_version;
    std::string build_architecture;
};

// OverlayIntents, the configuration edit/value types, and the ConfigUiState
// feedback block now live in the GPU-free value header config_edit.h, shared by
// the Settings Catalog, the configuration controller, and the headless host
// test target.

class OverlayUI {
public:
    // Emit all widgets for this frame and return the intents/edits the user
    // triggered. The Options_Panels render real editable controls seeded from
    // the immutable Working_Configuration snapshot `working`; the ConfigUiState
    // `ui_state` feeds back dirty/error/success state and diagnostics so the
    // pure builder can render them without owning any of it. Must be called
    // inside an ImGui frame (between ImGui::NewFrame() and ImGui::Render()).
    // Issues only ImGui calls, performs no I/O, and mutates no engine or
    // configuration state — every request is surfaced through the returned
    // OverlayIntents.
    OverlayIntents build(const BenchmarkStatus& status,
                         const BenchConfig&     working,
                         const ConfigUiState&   ui_state);

    // Draw the collapsible diagnostics sidebar pinned to the left work area.
    // It consolidates the former FPS, GPU-name, and geometry overlays and adds
    // backend/driver telemetry. Unsupported live metrics remain visibly N/A.
    void draw_debug_sidebar(const DebugSidebarInfo& info,
                            const FrameGenerationStatus& frame_generation = {});
    void set_microprofiler_snapshot(std::shared_ptr<const profile::Snapshot> snapshot,
                                    std::string feedback = {}) {
        microprofiler_.set_snapshot(std::move(snapshot), std::move(feedback));
    }
    void set_microprofiler_recording(bool active) { microprofiler_.set_recording(active); }
    void toggle_microprofiler() { microprofiler_.toggle(); }
    void set_microprofiler_open(bool open) { microprofiler_.set_open(open); }
    [[nodiscard]] bool microprofiler_open() const { return microprofiler_.is_open(); }
    // CLI overhead comparisons can show only this panel without the HUD/settings.
    [[nodiscard]] OverlayIntents build_microprofiler() {
        OverlayIntents intents;
        intents.microprofile = microprofiler_.draw();
        return intents;
    }

    // Captured once from the active adapter. Warnings open a non-modal notice;
    // dismissing it leaves the details available from Diagnostics.
    void set_driver_check(const DriverCheck& check, std::string_view description);

    // Supply the list of selectable window resolutions (width, height) for the
    // Display tab's Resolution dropdown. Gathered by the Main_Loop from SDL
    // (the monitor's supported modes plus common windowed sizes) and handed in
    // here so OverlayUI itself stays GPU/SDL-free and headless-testable. When
    // the list is empty the Display tab falls back to plain Width/Height number
    // fields. Safe to call once at startup or whenever the list changes.
    void set_available_resolutions(std::vector<std::pair<int, int>> resolutions);

    // Supply the compile-time application version and selected CPU build
    // architecture shown in the completed benchmark result.
    void set_application_build_info(ApplicationBuildInfo info);

    // The main loop routes Escape here before benchmark cancellation or quit.
    // Closing this nonmodal dialog changes only local presentation state.
    [[nodiscard]] bool dismiss_credits() {
        const bool was_open = credits_open_;
        credits_open_ = false;
        return was_open;
    }
    [[nodiscard]] bool credits_open() const { return credits_open_; }

private:
    void draw_driver_notice();
    void draw_credits();

    DriverCheck driver_check_;
    std::string driver_description_;
    bool driver_notice_open_ = false;
    bool credits_open_ = false;

    // The top Menu_Bar stays available while running, with Cancel replacing
    // Benchmark. The caller services cancellation and configuration changes.
    void build_menu_bar(const BenchmarkStatus& status, OverlayIntents& intents);

    // The single consolidated configuration window. All six setting groups
    // (Graphics, Display, Environment, Camera, Geometry, Audio) live here as
    // tabs in one resizable, sensibly-sized window rather than six separate
    // tiny windows. Each tab iterates its Settings Catalog entries and renders
    // exactly one editable control per governed setting, seeded from `working`;
    // committed changes are surfaced as ConfigurationEdits on `intents`. Reload
    // settings carry a persistent [reload] marker, and tabs that govern Reload
    // settings show the dirty/Apply-Reload/error affordances from `ui_state`.
    // The save/load toolbar and diagnostics area sit below the tab bar, always
    // visible. A menu-bar click opens this window and focuses the matching tab.
    // Pure ImGui: no I/O, no state mutation

    void build_config_window(const BenchConfig&   working,
                             const ConfigUiState& ui_state,
                             OverlayIntents&      intents);

    // The Display-tab Resolution dropdown. Lists the resolutions supplied via
    // set_available_resolutions; selecting one emits Width and Height edits for
    // whichever dimension diverges from the Working_Configuration. Pure ImGui,
    // raises only edits. Rendered only when a resolution
    // list is available; otherwise the Width/Height number fields are shown.
    void build_resolution_combo(const BenchConfig& working,
                                OverlayIntents&    intents);

    // The save/load toolbar and the diagnostics area, hosted in the Settings
    // panel. Pure ImGui: a non-empty path raises a save/load intent,
    // an empty load path is withheld with an inline note, and surfaced
    // diagnostics render in order with a Dismiss control. No I/O, no state
    // mutation.
    void build_config_toolbar(const ConfigUiState& ui_state,
                              OverlayIntents&      intents);

    // The live status overlay shown while a run is active.
    void build_running_overlay(const BenchmarkStatus& status);

    // The final Result_Report shown once a run completes.
    void build_result_report(const BenchmarkStatus& status, OverlayIntents& intents);

    // Open state of the single consolidated configuration window. It stays open
    // across frames until the user closes it; any menu-bar settings control
    // re-opens it.
    bool config_window_open_ = false;

    // The tab to force-select this frame, set when a menu-bar control is
    // activated so the window opens focused on that section. Consumed (reset to
    // None) each frame after the window renders, so the user can freely switch
    // tabs afterwards.
    OverlayIntents::Panel pending_tab_ = OverlayIntents::Panel::None;

    ApplicationBuildInfo application_build_info_{"unknown", "unknown"};

    // Persistent text buffers for the save/load path InputTexts. ImGui edits
    // caller-owned storage in place across frames, so these must outlive a
    // single build() call. They are seeded from ConfigUiState::current_config_name
    // (or config.json when no file was loaded) so Save / Load work with a single
    // click out of the box; the user can still edit either.
    std::string save_path_ = "config.json";
    std::string load_path_ = "config.json";
    std::string path_seed_ = "config.json";

    // Sticky UI-local flag: the user pressed Load with an empty path. The
    // builder withholds the load intent and shows an inline "no file selected"
    // message until a non-empty Load is attempted. This is a
    // UI-local validation note, not a Config_File diagnostic, and involves no
    // I/O.
    bool load_no_file_ = false;

    // Selectable window resolutions for the Display-tab Resolution dropdown
    // (width, height), supplied by the Main_Loop via set_available_resolutions.
    // Empty until set (and on the headless test path), in which case the
    // Display tab shows plain Width/Height number fields instead.
    std::vector<std::pair<int, int>> resolutions_;

    // UI-local presentation state. It intentionally does not enter BenchConfig:
    // collapsing diagnostics changes no renderer or benchmark behaviour.
    bool debug_sidebar_expanded_ = true;
    MicroprofilerUI microprofiler_;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_OVERLAY_OVERLAY_UI_H_
