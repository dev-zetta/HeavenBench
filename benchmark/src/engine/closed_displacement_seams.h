/*
 * HeavenBench — displacement constraints at closed material boundaries
 */
#ifndef HEAVENBENCH_SRC_ENGINE_CLOSED_DISPLACEMENT_SEAMS_H_
#define HEAVENBENCH_SRC_ENGINE_CLOSED_DISPLACEMENT_SEAMS_H_

#include "engine/types.h"
#include "engine/asset_file_system.h"

#include <cstddef>
#include <span>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace heaven {

enum class SeamSurfaceKind { Inactive, Flat, Displaced };

struct ClosedSeamSegment {
    glm::dvec3 a{0};
    glm::dvec3 b{0};
};

struct ClosedDisplacementSeams {
    // Indexed exactly like the original mesh's surfaces. Contains displaced
    // boundaries adjoining flat neighbors or incompatible displaced charts.
    std::vector<std::vector<ClosedSeamSegment>> surfaces;
    double matching_tolerance = 0;
    double chart_matching_tolerance = 0;
    double fade_distance = 0;
    std::size_t matched_segments = 0;
};

struct ClosedSeamFadeReport {
    // Initial smoothstep width and the furthest changed source point after
    // local triangle relaxation are distinct; neither selects a camera.
    double fade_distance = 0;
    double affected_distance = 0;
    double maximum_added_slope = 0;
    std::size_t evaluated_triangles = 0;
    bool slope_limit_met = false;
    std::size_t exact_ratio_relaxations = 0;
    std::size_t rounding_stalls = 0;
    std::size_t unresolved_triangles = 0;
    bool work_limit_exhausted = false;
};

struct ClosedSeamHeightBounds {
    // Maximum |filtered H - 127/255|, before amplitude or normal magnitude.
    std::vector<double> maximum_height_delta;
    std::size_t universal_vertices = 0;
    std::size_t candidate_chains = 0;
};

// Per-load CPU envelope only: it never substitutes the renderer's native H.
// Union original and requested variant, including High2K's selected start mip,
// so device/corrupt-variant fallback does not require a second selection policy.
// Active fade restricts LOD to [meshLod,max(meshLod,1)]. Periodic texel-center
// bilinear extrema include 1/16 texel/LOD phase and FP32 UV error. Unsigned
// BC4/BC5 red interpolants use their complete stored endpoint hull; endpoint
// and explicit 0/1 selectors remain exact. Other compressed formats and
// unsupported/large-coordinate inputs use [0,1] explicitly. Endpoint/filter
// arithmetic allowances still require native sampler qualification.
class ClosedSeamHeightSampler {
public:
    explicit ClosedSeamHeightSampler(AssetFileSystemRef assets = {}) : assets_(assets) {}
    [[nodiscard]] ClosedSeamHeightBounds bounds(const Surface& surface,
        const std::string& original_path, const std::array<float, 4>& transform,
        TextureQuality quality);
private:
    struct Chain;
    [[nodiscard]] std::shared_ptr<const Chain> chain(const std::string& path, std::uint32_t starting_mip);
    AssetFileSystemRef assets_;
    std::map<std::pair<std::string, std::uint32_t>, std::shared_ptr<const Chain>> chains_;
};

// Conservative minimum stretch of the affine spatial matrix. Displacement
// is added in preclip world units, so constant homogeneous W cancels in the
// offset/stretch ratio. Nonfinite, projective, nonpositive-W or singular
// matrices return zero; callers must explicitly retain their original field.
[[nodiscard]] double closed_seam_minimum_spatial_scale(const glm::mat4& transform);

// Collect BEFORE neutral topology changes. Near-coincident endpoint pairs
// qualify. Incompatible displaced chart chains require reciprocal, unique,
// complete coverage; compatible charts and unpaired open edges stay unchanged.
// Distinct displaced surfaces lack a proved common height footprint and are
// constrained conservatively. Within-surface exact P/UV/raw-N copies stay free.
[[nodiscard]] ClosedDisplacementSeams collect_closed_displacement_seams(
    const Mesh& source, std::span<const SeamSurfaceKind> kinds);

// Apply AFTER a neutral cache load/build, before upload. Only the displacement
// magnitude changes; positions, normal directions, tangent frames, UVs and
// topology retain their bits. A segment BVH bounds per-vertex query work.
// A positive source offset bound gives an amplitude-aware initial band. Local
// relaxation bounds the additional raster-displacement derivative by 1.0
// relative to the triangle's minimum-weight-scaled original displacement.
// Exact compatible copies share a weight. A bounded local monotone relaxation
// handles poorly conditioned triangles without widening unrelated charts.
// Degenerate nonzero terms and float rounding stalls use a verified exact
// common dyadic ratio, otherwise zero, only on the affected triangle groups.
// Failure retains all original magnitudes and is exposed through report.
// This bounds the fade term only, not the original height/normal derivatives
// or global self-intersection. Unaffected interior weights remain exactly one.
[[nodiscard]] std::size_t apply_closed_displacement_seams(
    Surface& surface, std::span<const ClosedSeamSegment> segments,
    double fade_distance, double matching_tolerance,
    double maximum_source_offset_per_magnitude = 0,
    ClosedSeamFadeReport* report = nullptr,
    std::span<const double> maximum_source_offsets = {});

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_CLOSED_DISPLACEMENT_SEAMS_H_
