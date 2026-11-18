#ifndef HEAVENBENCH_SRC_RHI_OPAQUE_DEPTH_PREPASS_POLICY_H_
#define HEAVENBENCH_SRC_RHI_OPAQUE_DEPTH_PREPASS_POLICY_H_

#include "engine/gpu_driver_info.h"
#include "engine/types.h"
#include "render_tuning.h"

#include <cmath>
#include <cstdint>
#include <string_view>

namespace heaven {

struct OpaqueDepthPrepassDevice {
    bool vulkan = false;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    DriverFamily driver_family = DriverFamily::Unknown;
    std::string_view driver_version;
};

// Only this exact device/driver is eligible. Other combinations require
// independent native quality and timing qualification before extending scope.
[[nodiscard]] constexpr bool qualifies_opaque_depth_prepass_device(
        OpaqueDepthPrepassDevice const& device) noexcept {
    return device.vulkan && device.vendor_id == 0x8086u && device.device_id == 0xe223u &&
           device.driver_family == DriverFamily::Mesa &&
           device.driver_version == "26.0.8-1ubuntu0.3";
}

// This optional execution policy changes no rendering-feature settings. Read
// the current per-frame lighting/settings so day/night and live edits cannot
// retain a previously enabled prepass after its qualified workload has ended.
[[nodiscard]] inline bool should_use_opaque_depth_prepass(
        bool device_qualified, bool contact_shadows, QualityLevel contact_quality,
        float sun_elevation, float sun_intensity_lux) noexcept {
    return device_qualified && contact_shadows && contact_quality == QualityLevel::High &&
           std::isfinite(sun_elevation) && std::isfinite(sun_intensity_lux) &&
           sun_elevation > 0.0f &&
           sun_intensity_lux > render_tuning::lighting.moon_intensity_lux;
}

} // namespace heaven
#endif // HEAVENBENCH_SRC_RHI_OPAQUE_DEPTH_PREPASS_POLICY_H_
