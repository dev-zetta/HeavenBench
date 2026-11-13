#include "engine/variable_rate_shading.h"

namespace heaven {

std::string_view variable_rate_shading_mode_id(VariableRateShadingMode mode) noexcept {
    switch (mode) {
        case VariableRateShadingMode::Off: return "off";
        case VariableRateShadingMode::Rate2x1: return "2x1";
        case VariableRateShadingMode::Rate2x2: return "2x2";
    }
    return "off";
}

std::optional<VariableRateShadingMode> parse_variable_rate_shading_mode(std::string_view id) noexcept {
    if (id == "off") return VariableRateShadingMode::Off;
    if (id == "2x1") return VariableRateShadingMode::Rate2x1;
    if (id == "2x2") return VariableRateShadingMode::Rate2x2;
    return std::nullopt;
}

} // namespace heaven
