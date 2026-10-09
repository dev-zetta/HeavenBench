/* Native texture appearance after temporary height-sampling View teardown. */
#ifndef HEAVENBENCH_SMOKE_HEIGHT_SAMPLER_LIT_LIFECYCLE_H_
#define HEAVENBENCH_SMOKE_HEIGHT_SAMPLER_LIT_LIFECYCLE_H_

#include "smoke_cases.h"

namespace heaven::smoke {
[[nodiscard]] CaseResult run_height_sampler_lit_lifecycle_case(const CaseContext& ctx);
}

#endif
