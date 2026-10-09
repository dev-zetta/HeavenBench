/* HeavenBench — persistent CPU output keyed by frozen native displacement input. */
#ifndef HEAVENBENCH_SRC_RHI_PREPARED_DISPLACEMENT_CACHE_H_
#define HEAVENBENCH_SRC_RHI_PREPARED_DISPLACEMENT_CACHE_H_

#include "rhi/displacement_surface_preparer.h"

#include <filesystem>

namespace heaven {

// Empty paths disable persistence. Every hit is tied to actual frozen packing,
// native height/LOD values, instance metric, displacement and complete policy.
// No GPU/Engine access; immutable source arrays are never replaced by the cache.
[[nodiscard]] std::filesystem::path default_prepared_displacement_cache_directory(
    const std::filesystem::path& archive);

// false is a normal miss; Error is corrupt/incompatible IO and must be reported
// nonfatally by the owner before recomputation. Commit geometry/flags only after
// payload hash, bounds, ancestry, winding and source-chart coverage validation.
[[nodiscard]] Result<bool> load_prepared_displacement_cache(
    const std::filesystem::path& directory, PreparedDisplacementSurface& source,
    const DisplacementPreparationPolicy& policy);

// Only integrity-bearing Prepared/BudgetLimited outputs are persisted. Quality
// failures, metrics and exact component fallback records remain explicit.
[[nodiscard]] Result<void> store_prepared_displacement_cache(
    const std::filesystem::path& directory, const PreparedDisplacementSurface& refined,
    const DisplacementPreparationPolicy& policy);

} // namespace heaven
#endif
