/* HeavenBench — named levels for the existing tessellation controls. */
#ifndef HEAVENBENCH_SRC_ENGINE_TESSELLATION_LEVELS_H_
#define HEAVENBENCH_SRC_ENGINE_TESSELLATION_LEVELS_H_

#include "engine/types.h"

#include <array>
#include <optional>
#include <string_view>

namespace heaven {

// Custom is derived from manual values, never a second stored configuration.
enum class TessellationLevel { Disabled, Moderate, Normal, Extreme, Custom };

inline constexpr std::array kSelectableTessellationLevels = {
    TessellationLevel::Disabled, TessellationLevel::Moderate,
    TessellationLevel::Normal, TessellationLevel::Extreme};

[[nodiscard]] std::string_view tessellation_level_id(TessellationLevel level) noexcept;
[[nodiscard]] std::string_view tessellation_level_label(TessellationLevel level) noexcept;
[[nodiscard]] std::optional<TessellationLevel> parse_tessellation_level(std::string_view value);
[[nodiscard]] TessellationLevel detect_tessellation_level(const TessellationConfig& config) noexcept;

// Disabled preserves the existing controls for re-enabling. The enabled named
// levels set all three controls together; Custom leaves manual values intact.
void apply_tessellation_level(TessellationConfig& config, TessellationLevel level) noexcept;

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_TESSELLATION_LEVELS_H_
