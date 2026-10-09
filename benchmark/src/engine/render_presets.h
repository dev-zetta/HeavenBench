/*
 * HeavenBench — pure rendering-preset model
 */
#ifndef HEAVENBENCH_SRC_ENGINE_RENDER_PRESETS_H_
#define HEAVENBENCH_SRC_ENGINE_RENDER_PRESETS_H_

#include "engine/types.h"

#include <optional>
#include <string_view>

namespace heaven {

[[nodiscard]] std::string_view render_preset_id(RenderPreset preset) noexcept;
[[nodiscard]] std::string_view render_preset_label(RenderPreset preset) noexcept;
[[nodiscard]] std::optional<RenderPreset> parse_render_preset(
    std::string_view value);

// Applies only rendering and CPU-stress fields. Display, HDR, scene, weather,
// audio, benchmark mode, paths and output ownership are intentionally untouched.
void apply_render_preset(AppConfig& c, RenderPreset preset);

// Returns a named preset only when every preset-owned field matches it.
[[nodiscard]] RenderPreset detect_render_preset(
    const AppConfig& config);

[[nodiscard]] std::string_view material_workflow_id(
    MaterialWorkflow workflow) noexcept;
[[nodiscard]] std::optional<MaterialWorkflow> parse_material_workflow(
    std::string_view value);

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_RENDER_PRESETS_H_
