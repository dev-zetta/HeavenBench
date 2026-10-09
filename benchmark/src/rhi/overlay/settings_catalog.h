/*
 * HeavenBench — Settings Catalog (GPU-free single source of truth)
 *
 * The catalog binds every editable BenchConfig setting to: its SettingId, the
 * Options_Panel that governs it, its Live/Reload classification, the ImGui
 * widget kind that edits it, a human-readable label, its bounds (sourced from
 * the shared engine/types.h validation helpers), and a pair of generic
 * accessors:
 *
 *   read(cfg)       — pull the governed field's current value out of a
 *                     BenchConfig so a panel control can seed itself from the
 *                     Working_Configuration.
 *   apply_to(cfg,v) — validate `v` through the SAME shared engine/types.h
 *                     helper the CLI and the Config_File module use, write the
 *                     field, and return the value actually stored (post
 *                     clamp/reject) so the caller can detect a rejection
 *
 *
 * Because validation reuses the shared helpers, a value validates identically
 * regardless of source (UI edit, CLI flag, or loaded Config_File). Persisted
 * settings (those present in config_schema()) map 1:1 to their schema key, so
 * save/load and live editing stay consistent.
 *
 * This translation unit is GPU-free: it depends only on engine/types.h,
 * engine/cli_parser.h (for the BenchConfig value struct), the new GPU-free
 * value header config_edit.h, and the standard library. It references no
 * Filament symbol, so OverlayUI, the configuration controller, and the headless
 * host test target can all share it.
 */
#ifndef HEAVENBENCH_SRC_RHI_OVERLAY_SETTINGS_CATALOG_H_
#define HEAVENBENCH_SRC_RHI_OVERLAY_SETTINGS_CATALOG_H_

#include "engine/cli_parser.h"   // heaven::BenchConfig (AppConfig + benchmark fields)
#include "engine/types.h"
#include "rhi/overlay/config_edit.h"

#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace heaven {

// The six Options_Panels the Menu_Bar opens. Each editable setting belongs to
// exactly one panel whose category matches the setting's category

enum class Panel {
    Camera,
    Environment,
    Tessellation,
    Settings,
    Quality,
    Sound,
};

// Where the renderer consumes a setting decides how a change is applied:
//   Live   — re-read every frame or applied through a runtime
//            IRenderFeature::set_enabled(); takes effect without a rebuild.
//   Reload — consumed only during scene build / pipeline construction; needs a
//            Benchmark_Reload to take effect.
enum class Apply {
    Live,
    Reload,
};

// The ImGui control kind OverlayUI renders for a setting. Bounded numerics use
// the slider widgets (with the entry's [min,max]); unbounded numerics use the
// input widgets; QualityLevel/fixed-set enums use Combo; bools use Checkbox;
// strings use InputText.
enum class Widget {
    Checkbox,
    SliderInt,
    SliderFloat,
    InputInt,
    InputFloat,
    Combo,
    InputText,
};

// One editable setting. `min_value`/`max_value` are meaningful for the bounded
// numeric widgets (SliderInt/SliderFloat), the QualityLevel combos (which carry
// [0,2] == Low..High), and the fixed-set MsaaSamples combo (which carries [1,4]
// bracketing the accepted set {1,2,4}); they are 0 for unbounded numerics
// (InputInt/InputFloat), bools (Checkbox), and strings (InputText).
struct CatalogEntry {
    SettingId   id;
    Panel       panel;
    Apply       apply;
    Widget      widget;
    const char* label;
    float       min_value;
    float       max_value;

    // Read the governed field's current value out of a BenchConfig.
    SettingValue (*read)(const BenchConfig& cfg);

    // Validate `v` through the matching shared helper and write it into cfg;
    // return the value actually stored so the caller can detect a clamp/reject.
    SettingValue (*apply_to)(BenchConfig& cfg, const SettingValue& v);
};

// The full catalog: exactly one CatalogEntry per editable SettingId, built once
// and returned by reference. Every SettingId appears in exactly one entry

[[nodiscard]] const std::vector<CatalogEntry>& settings_catalog();

// Look up the single CatalogEntry governing `id`. The catalog is total over
// SettingId, so this always resolves; an unknown id is a programming error.
[[nodiscard]] const CatalogEntry& catalog_entry(SettingId id);

// User-facing section for an entry in the consolidated settings window.
// Entries are ordered so equal section names are contiguous within each panel.
[[nodiscard]] std::string_view catalog_section(SettingId id);

// Qualitative guidance for an enabled workload, not an FPS prediction or a
// measurement of the current frame. Keep the reason beside the estimate so
// different UI presentations cannot separate a cost from its conditions.
struct PerformanceImpact {
    std::string_view estimate;
    std::string_view explanation;
};

[[nodiscard]] std::optional<PerformanceImpact> catalog_performance_impact(SettingId id) noexcept;

// These implemented features currently have no editable catalog control.
// Describe their impact without introducing a second configuration path.
struct AdditionalFeatureImpact {
    std::string_view feature;
    PerformanceImpact impact;
};

[[nodiscard]] std::span<const AdditionalFeatureImpact> additional_feature_impacts() noexcept;

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_OVERLAY_SETTINGS_CATALOG_H_
