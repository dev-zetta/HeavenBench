#ifndef HEAVENBENCH_SRC_RHI_VARIABLE_RATE_SHADING_H_
#define HEAVENBENCH_SRC_RHI_VARIABLE_RATE_SHADING_H_

#include "engine/variable_rate_shading.h"
#include "rhi/backend_core.h"

#include <cstdint>

namespace heaven {

// Owns no graphics resources. The backend owns the main View and platform for
// longer than this collaborator; all shadow/depth/composite views stay full-rate.
class VariableRateShadingController {
public:
    explicit VariableRateShadingController(BackendCore core);
    void apply(VariableRateShadingMode requested);
    [[nodiscard]] VariableRateShadingStatus status() const { return status_; }

private:
    BackendCore core_;
    VariableRateShadingStatus status_;
    std::uint32_t supported_2x1_samples_ = 0;
    std::uint32_t supported_2x2_samples_ = 0;
    std::uint32_t last_samples_ = 0;
    bool configured_ = false;
    bool enabled_ = false;
    std::string capability_reason_;
};

} // namespace heaven
#endif
