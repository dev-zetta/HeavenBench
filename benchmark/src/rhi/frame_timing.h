#ifndef HEAVENBENCH_SRC_RHI_FRAME_TIMING_H_
#define HEAVENBENCH_SRC_RHI_FRAME_TIMING_H_

#include <filament/Renderer.h>

namespace heaven {

struct FrameTimingOptions {
    filament::Renderer::DisplayInfo display;
    filament::Renderer::FrameRateOptions frame_rate;
};

[[nodiscard]] inline FrameTimingOptions frame_timing_options(bool adaptive_resolution) {
    // Filament divides interval by refreshRate to obtain the adaptive GPU
    // budget. Zero produces an infinite target and prevents any downscaling.
    // This interactive option targets 60 FPS; fixed/scored rendering keeps
    // the previous unpaced display configuration. Presentation remains
    // IMMEDIATE independently of this controller input.
    // Construct the returned aggregate directly so no implicit DisplayInfo
    // move/copy accesses its deprecated, unused timing fields.
    return {
        .display = {.refreshRate = adaptive_resolution ? 60.0f : 0.0f},
        // Filament clamps this to at least one; zero never disabled pacing.
        .frame_rate = {.interval = 1}
    };
}

inline void configure_frame_timing(filament::Renderer& renderer, bool adaptive_resolution) {
    const auto options = frame_timing_options(adaptive_resolution);
    renderer.setDisplayInfo(options.display);
    renderer.setFrameRateOptions(options.frame_rate);
}

} // namespace heaven

#endif
