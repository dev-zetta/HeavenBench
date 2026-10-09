/*
 * HeavenBench — frame-pacing smoke case (renderer-linked, GATED)
 *
 * after the Filament backend is initialised,
 * `IRhiBackend::frame_pacing_disabled()` must report `true` — the renderer
 * presents frames with vsync off and no frame-rate cap, so the measured
 * frame rate is not constrained by the display refresh rate.
 *
 * This file used to carry the harness main(); it now provides ONLY the
 * frame-pacing CASE FUNCTION. The shared gate (HEAVEN_SMOKE=1 + display) and
 * the window/backend bring-up live in smoke_main.cpp, which constructs the
 * backend and invokes this case (and the overlay-render case in
 * smoke_overlay_render.cpp). Keeping exactly one main() across the smoke
 * sources lets the GLOBbed `heaven_benchmark_smoke` target link cleanly.
 *
 * The assertion is unchanged from: query frame_pacing_disabled() on
 * the freshly-constructed backend and PASS iff it is true. No window/backend
 * setup happens here, so there is no headless SKIP path inside the case — the
 * harness already gated and brought the backend up before calling us.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"

#include <cstdio>

namespace heaven::smoke {

CaseResult run_frame_pacing_case(const CaseContext& ctx) {
    constexpr const char* kTag = "[smoke_frame_pacing]";

    // The constructor configures IMMEDIATE present, refreshRate=0, interval=0.
    const bool pacing_off = ctx.backend.frame_pacing_disabled();
    if (!pacing_off) {
        std::printf("%s frame_pacing_disabled() returned false after init\n", kTag);
        return CaseResult::Fail;
    }

    std::printf("%s frame_pacing_disabled() == true after init\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
