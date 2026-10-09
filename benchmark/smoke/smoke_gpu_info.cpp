/*
 * HeavenBench — GPU / render-API smoke case (renderer-linked, GATED)
 *
 * the Result_Report's GPU_Description and
 * Render_API_Description are sourced from the Renderer's ACTIVE rendering
 * backend via two RHI hooks — `IRhiBackend::gpu_info()` (neutral GpuInfoRaw:
 * selected graphics-device model name and VRAM in MB) and
 * `IRhiBackend::render_api_name()` (the live driver path, "Vulkan" or
 * "OpenGL"). On a non-Filament/headless backend these report the undetermined
 * sentinels and the pure RunInfo formatter substitutes the placeholder, so the
 * only way to exercise the genuine GPU/API read is against a live Filament
 * backend on a real GPU. Hence this case is part of the renderer-linked,
 * GATED smoke harness and is skipped in headless CI.
 *
 * Like smoke_frame_pacing.cpp, this file provides ONLY the CASE FUNCTION. The
 * shared gate (HEAVEN_SMOKE=1 + a display present) and the window/backend
 * bring-up live in smoke_main.cpp, which constructs the live backend ONCE and
 * invokes each registered case against it. There is therefore no headless SKIP
 * path inside the case for the *gate itself* — the harness already gated and
 * brought the backend up before calling us.
 *
 * Assertions (against the freshly-constructed live backend):
 *   - render_api_name() is exactly one of "Vulkan" / "OpenGL", matching the
 *     driver Filament actually created for this run.
 *   - gpu_info().model_name is non-empty — a live backend reports either the
 *     selected VkPhysicalDevice name (Vulkan) or the GL renderer string
 *     (OpenGL) for the graphics device model.
 *   - On the Vulkan path: vram_mb >= 0 (non-negative, summed DEVICE_LOCAL
 *     heaps) —.
 *   - On the OpenGL fallback: VRAM is undetermined BY DESIGN (Filament's public
 *     GL path does not expose it), so it stays at its neutral sentinel and the formatter
 *     substitutes the Unknown_Value_Placeholder. We therefore do
 *     NOT fail the strict VRAM contract on OpenGL; we only require the
 *     model name and a matching API name.
 *
 * If render_api_name() comes back empty the backend has no live engine (it was
 * never really brought up — a headless software path), so the case SKIPs
 * rather than faking a pass; the gated harness keeps the process exit code 0.
 */

#include "smoke_cases.h"

#include "rhi/rhi.h"

#include <cstdio>
#include <string>

namespace heaven::smoke {

CaseResult run_gpu_info_case(const CaseContext& ctx) {
    constexpr const char* kTag = "[smoke_gpu_info]";

    // render_api_name() reads engine_->getBackend(); an empty result means no
    // live engine was constructed (headless software path) — SKIP, don't fake.
    const std::string api = ctx.backend.render_api_name();
    if (api.empty()) {
        std::printf("%s SKIP: render_api_name() empty (no live backend)\n", kTag);
        return CaseResult::Skip;
    }

    // the reported API must be the live driver path Filament created.
    const bool api_known = (api == "Vulkan") || (api == "OpenGL");
    if (!api_known) {
        std::printf("%s render_api_name() returned unexpected '%s' "
                    "(expected \"Vulkan\" or \"OpenGL\")\n", kTag, api.c_str());
        return CaseResult::Fail;
    }

    const IRhiBackend::GpuInfoRaw info = ctx.backend.gpu_info();

    // a live backend reports the graphics device model name (the
    // VkPhysicalDevice deviceName on Vulkan, or the GL renderer string).
    if (info.model_name.empty()) {
        std::printf("%s gpu_info().model_name is empty on a live %s backend\n",
                    kTag, api.c_str());
        return CaseResult::Fail;
    }

    // Both backends expose their active graphics API version. OpenGL also
    // guarantees vendor and version strings; Vulkan driver properties are
    // optional on older implementations, so missing optional fields may not
    // turn an otherwise functioning Vulkan backend into a smoke failure.
    if (info.api_version.empty() || (api == "OpenGL" &&
            (info.driver_name.empty() || info.driver_info.empty()))) {
        std::printf("%s incomplete active %s driver identity\n", kTag, api.c_str());
        return CaseResult::Fail;
    }
    const auto family = driver_family_name(info.driver_family);
    std::printf("%s active driver: family='%.*s' name='%s' version='%s' "
                "info='%s' API=%s\n", kTag, static_cast<int>(family.size()),
                family.data(), info.driver_name.c_str(), info.driver_version.c_str(),
                info.driver_info.c_str(), info.api_version.c_str());

    if (api == "Vulkan") {
        // on the Vulkan path VRAM (summed DEVICE_LOCAL heaps → MB)
        // is obtainable and must be non-negative.
        if (info.vram_mb < 0) {
            std::printf("%s gpu_info().vram_mb is negative (%ld) on Vulkan\n",
                        kTag, info.vram_mb);
            return CaseResult::Fail;
        }
        std::printf("%s Vulkan: model='%s' vram=%ldMB\n",
                    kTag, info.model_name.c_str(), info.vram_mb);
        return CaseResult::Pass;
    }

    // OpenGL fallback: VRAM is undetermined by design  — the
    // formatter substitutes the placeholder. We only require the
    // model name (asserted above) and a matching API name (asserted above).
    std::printf("%s OpenGL: model='%s' (VRAM undetermined by design)\n",
                kTag, info.model_name.c_str());
    return CaseResult::Pass;
}

} // namespace heaven::smoke
