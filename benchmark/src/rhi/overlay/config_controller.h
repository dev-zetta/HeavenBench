/*
 * HeavenBench — In-UI configuration controller (GPU-free, host-testable)
 *
 * This is the pure logic core of the In-UI Configuration feature: it applies a
 * frame's worth of OverlayIntents (edits + save/load/reload) onto the
 * Working_Configuration and Active_Configuration, surfaces diagnostics into the
 * ConfigUiState feedback block, and drives renderer side effects through the
 * abstract RendererEffects interface below. The Main_Loop owns the concrete
 * RendererEffects (over FilamentBackend / MusicPlayer / the Benchmark system);
 * this translation unit never names a Filament or GPU symbol, so it links into
 * the Filament-free host test target and is fully property-testable.
 *
 * The split of responsibility mirrors the implementation: OverlayUI describes intent,
 * the Main_Loop owns state and effects, and this controller is the seam that
 * turns one into the other in the fixed order edits → save → load → reload
 *
 *
 * It depends only on the GPU-free value header (config_edit.h), the Settings
 * Catalog (settings_catalog.h), the Config_File module (engine/config_file.h),
 * and engine/types.h (Result/Error). No Filament include, no I/O of its own
 * beyond the Config_File module's save/load.
 */
#ifndef HEAVENBENCH_SRC_RHI_OVERLAY_CONFIG_CONTROLLER_H_
#define HEAVENBENCH_SRC_RHI_OVERLAY_CONFIG_CONTROLLER_H_

#include "engine/config_file.h"           // load_config_file / write_config_file / ConfigLoadResult
#include "engine/types.h"                  // heaven::Result<void> / heaven::Error
#include "rhi/overlay/config_edit.h"       // OverlayIntents / ConfigUiState / SettingId / SettingValue
#include "rhi/overlay/settings_catalog.h"  // settings_catalog / catalog_entry / Apply

#include <vector>

namespace heaven {

// Abstract renderer side effects the configuration controller drives when a
// Live_Setting changes or a Benchmark_Reload is requested. It is deliberately
// GPU-free so the controller and the host tests can substitute a recording fake
// (tests) or a concrete implementation over the renderer subsystems (main.cpp).

// Downstream tasks 4.3 (test fake) and 8.1 (main.cpp implementation) implement
// these methods VERBATIM — the interface is stable.
struct RendererEffects {
    virtual ~RendererEffects() = default;

    // Toggle a registered IRenderFeature on/off by its name() string. An
    // unmatched name is a safe no-op.
    virtual void set_feature_enabled(const char* name, bool on) = 0;

    // Apply a single Live_Setting (time-of-day, weather, exposure, music volume,
    // a per-frame quality knob, …) that is not a feature toggle, reading the new
    // value from the just-updated Active_Configuration.
    virtual void apply_live(SettingId id, const BenchConfig& active) = 0;

    // Cancel any in-flight benchmark before changing its live configuration,
    // loading a configuration or rebuilding the scene/pipeline.
    virtual void cancel_active_run() = 0;

    // Rebuild the scene and render pipeline from the Active_Configuration. A
    // success result advances Active to Working; a failure leaves Active intact

    virtual Result<void> rebuild() = 0;
};

// Operation log recorded by apply_config_intents(), in the order operations ran
// this frame. Sufficient to assert the Requirement-7.5 ordering invariant
// (every Edit precedes every Save, Load, and Reload).  checks the
// log orders edits < save < load < reload.
enum class ConfigOp { Edit, Save, Load, Reload };
using ConfigOpLog = std::vector<ConfigOp>;

// Dirty_State predicate: true iff `working` and `active` differ in ANY
// Apply::Reload catalog entry. Live_Settings are excluded because they are
// applied to both configs immediately and so never diverge

[[nodiscard]] bool is_dirty(const BenchConfig& working, const BenchConfig& active);

// Map a Live feature-toggle SettingId to the exact IRenderFeature name() the
// backend registers, or nullptr for any setting that has no runtime feature
// (so the controller dispatches it through RendererEffects::apply_live instead).
// The strings match the feature classes under rhi/features/*.h and their
// registration in FilamentBackend::init_features.
[[nodiscard]] const char* live_feature_name(SettingId id);

// Apply one frame's OverlayIntents onto the Working_Configuration and
// Active_Configuration in the fixed order edits → save → load → reload,
// surfacing diagnostics/success/dirty/reload_error into `ui` and driving Live
// applies and the Benchmark_Reload through `fx`. Returns the operation log in
// execution order (Properties 5–13).
ConfigOpLog apply_config_intents(BenchConfig&          working,
                                 BenchConfig&          active,
                                 ConfigUiState&        ui,
                                 const OverlayIntents&  intents,
                                 RendererEffects&       fx);

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_OVERLAY_CONFIG_CONTROLLER_H_
