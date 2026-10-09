#include "engine/tessellation_levels.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace heaven {

std::string_view tessellation_level_id(TessellationLevel level) noexcept {
    switch (level) {
        case TessellationLevel::Disabled: return "disabled";
        case TessellationLevel::Moderate: return "moderate";
        case TessellationLevel::Normal: return "normal";
        case TessellationLevel::Extreme: return "extreme";
        case TessellationLevel::Custom: return "custom";
    }
    return "custom";
}

std::string_view tessellation_level_label(TessellationLevel level) noexcept {
    switch (level) {
        case TessellationLevel::Disabled: return "Disabled";
        case TessellationLevel::Moderate: return "Moderate";
        case TessellationLevel::Normal: return "Normal";
        case TessellationLevel::Extreme: return "Extreme";
        case TessellationLevel::Custom: return "Custom";
    }
    return "Custom";
}

std::optional<TessellationLevel> parse_tessellation_level(std::string_view value) {
    std::string name(value);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto level : kSelectableTessellationLevels) {
        if (name == tessellation_level_id(level)) return level;
    }
    return std::nullopt;
}

TessellationLevel detect_tessellation_level(const TessellationConfig& config) noexcept {
    if (!config.enabled) return TessellationLevel::Disabled;
    if (config.scale != config.factor || config.scale != config.distance)
        return TessellationLevel::Custom;
    if (config.scale == 0.75f) return TessellationLevel::Moderate;
    if (config.scale == 1.0f) return TessellationLevel::Normal;
    if (config.scale == 1.5f) return TessellationLevel::Extreme;
    return TessellationLevel::Custom;
}

void apply_tessellation_level(TessellationConfig& config, TessellationLevel level) noexcept {
    float multiplier = 1.0f;
    switch (level) {
        case TessellationLevel::Disabled:
            config.enabled = false;
            return;
        case TessellationLevel::Moderate: multiplier = 0.75f; break;
        case TessellationLevel::Normal: multiplier = 1.0f; break;
        case TessellationLevel::Extreme: multiplier = 1.5f; break;
        case TessellationLevel::Custom:
        default: return;
    }
    // Heaven's original setTessellationScale/Factor/Distance apply these same
    // multipliers to the panel's default 100% controls.
    config.enabled = true;
    config.scale = multiplier;
    config.factor = multiplier;
    config.distance = multiplier;
}

} // namespace heaven
