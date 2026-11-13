#include "rhi/variable_rate_shading.h"

#include <backend/platforms/VulkanPlatform.h>
#include <filament/Engine.h>
#include <filament/View.h>

#include <string>

namespace heaven {

VariableRateShadingController::VariableRateShadingController(BackendCore core) : core_(core) {
    if (!core_.engine || core_.engine->getBackend() != filament::Engine::Backend::VULKAN) {
        capability_reason_ = "Variable-rate shading requires the Vulkan backend";
        return;
    }
    const auto* platform = static_cast<const filament::backend::VulkanPlatform*>(core_.engine->getPlatform());
    const auto capability = platform->getFragmentShadingRateCapabilities();
    enabled_ = capability.enabled;
    supported_2x1_samples_ = capability.sampleCounts2x1;
    supported_2x2_samples_ = capability.sampleCounts2x2;
    capability_reason_ = capability.reason ? capability.reason : "VRS logical-device capability is unavailable";
}

void VariableRateShadingController::apply(VariableRateShadingMode requested) {
    if (!core_.view) return;
    const auto& multisampling = core_.view->getMultiSampleAntiAliasingOptions();
    const std::uint32_t samples = multisampling.enabled ? multisampling.sampleCount : 1;
    if (configured_ && status_.requested == requested && last_samples_ == samples) return;
    configured_ = true;
    last_samples_ = samples;
    status_.requested = requested;
    status_.effective = VariableRateShadingMode::Off;
    status_.supported_2x1 = enabled_ && (supported_2x1_samples_ & samples) != 0;
    status_.supported_2x2 = enabled_ && (supported_2x2_samples_ & samples) != 0;
    auto rate = filament::backend::FragmentShadingRate::FULL;
    if (requested == VariableRateShadingMode::Off) {
        status_.reason = "Disabled";
    } else if (!enabled_) {
        status_.reason = capability_reason_;
    } else if ((requested == VariableRateShadingMode::Rate2x1 && status_.supported_2x1) ||
               (requested == VariableRateShadingMode::Rate2x2 && status_.supported_2x2)) {
        rate = requested == VariableRateShadingMode::Rate2x1
            ? filament::backend::FragmentShadingRate::RATE_2X1
            : filament::backend::FragmentShadingRate::RATE_2X2;
        status_.effective = requested;
        status_.reason = "Coarse shading for eligible opaque lit main-color geometry; other draws remain full-rate";
    } else {
        status_.reason = "Requested VRS rate is unsupported at " + std::to_string(samples) + "x MSAA; using full-rate shading";
    }
    core_.view->setFragmentShadingRate(rate);
}

} // namespace heaven
