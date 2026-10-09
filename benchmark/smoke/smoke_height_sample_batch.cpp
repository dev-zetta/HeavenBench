#include "smoke_cases.h"
#include "height_sample_batch_fixture.h"
#include "rhi/filament_backend.h"
#include "rhi/materials_data.h"
#include <cstdio>

namespace heaven::smoke {
CaseResult run_height_sample_batch_case(const CaseContext& ctx) {
    if (ctx.backend.api() != GraphicsApi::Filament) return CaseResult::Skip;
    if (auto initialized = ctx.backend.init_pipelines(ctx.config); !initialized) {
        std::printf("[smoke_height_sample_batch] init_pipelines: %s\n", initialized.error().message.c_str());
        return CaseResult::Fail;
    }
    auto* engine = static_cast<FilamentBackend&>(ctx.backend).debug_render_context().engine;
    if (!engine) {
        std::printf("[smoke_height_sample_batch] initialized Engine is missing\n");
        return CaseResult::Fail;
    }
    auto result = verify_height_sample_batch_fixture(*engine,
        {k_heaven_height_sample_filamat, k_heaven_height_sample_filamat_size});
    if (!result) {
        std::printf("[smoke_height_sample_batch] %s\n", result.error().message.c_str());
        return CaseResult::Fail;
    }
    std::printf("[smoke_height_sample_batch] actual R8/RGBA8 textures, UV/LOD/filter/wrap/IDs/chunks/one-level/width1 and invalid input checks passed\n");
    return CaseResult::Pass;
}
} // namespace heaven::smoke
