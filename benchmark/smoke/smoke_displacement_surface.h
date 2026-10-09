#ifndef HEAVENBENCH_SMOKE_DISPLACEMENT_SURFACE_H_
#define HEAVENBENCH_SMOKE_DISPLACEMENT_SURFACE_H_

#include "smoke_cases.h"

namespace heaven::smoke {
// Actual uploaded height, the reusable static preparer, and shipping parent
// materials. Registration is kept separate until the native run is reviewed.
[[nodiscard]] CaseResult run_displacement_surface_case(const CaseContext& ctx);
}

#endif
