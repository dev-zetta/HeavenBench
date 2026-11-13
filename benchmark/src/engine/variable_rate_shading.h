/* GPU-independent VRS request and effective shading-rate status. */
#ifndef HEAVENBENCH_SRC_ENGINE_VARIABLE_RATE_SHADING_H_
#define HEAVENBENCH_SRC_ENGINE_VARIABLE_RATE_SHADING_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace heaven {

enum class VariableRateShadingMode : std::uint8_t { Off, Rate2x1, Rate2x2 };

[[nodiscard]] std::string_view variable_rate_shading_mode_id(VariableRateShadingMode mode) noexcept;
[[nodiscard]] std::optional<VariableRateShadingMode> parse_variable_rate_shading_mode(std::string_view id) noexcept;

// Capability is supplied by the active backend for its actual sample count.
// Requested coarse shading must never be presented as effective on a fallback.
struct VariableRateShadingStatus {
    VariableRateShadingMode requested = VariableRateShadingMode::Off;
    VariableRateShadingMode effective = VariableRateShadingMode::Off;
    bool supported_2x1 = false;
    bool supported_2x2 = false;
    std::string reason = "Disabled";
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_VARIABLE_RATE_SHADING_H_
