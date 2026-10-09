/* HeavenBench — startup-only native-height boundary for bounded static geometry. */
#ifndef HEAVENBENCH_SRC_RHI_DISPLACEMENT_SURFACE_PREPARER_H_
#define HEAVENBENCH_SRC_RHI_DISPLACEMENT_SURFACE_PREPARER_H_

#include "engine/bounded_displacement_surface.h"
#include "rhi/geometry_displacement.h"
#include "rhi/height_sample_batch.h"
#include "rhi/packed_static_mesh.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace heaven {

enum class DisplacementHeightRoute { TextureLod, ManualWindow };
enum class DisplacementPreparationDisposition {
    Prepared, SourceSampled, Disabled, NonStatic, UnsupportedHeightRoute,
    InvalidPackedSource, UnsupportedInstanceMetric, IncompatibleInstanceScale,
    MeshLodBelowOne, NativeHeightFailed, OperatorRejected, BudgetLimited,
};

struct DisplacementPreparationPolicy {
    BoundedDisplacementPolicy geometry{}; // default 20mm world correspondence bound
    std::uint32_t maximum_source_vertices = 1'048'576;
    // Only float roundoff around a proper uniform spatial transform is accepted.
    // A constant positive homogeneous W is retained; the spatial bottom-row
    // terms must be exactly zero. General projectivity/shear are unsupported.
    double uniform_metric_tolerance = 2e-6;
    double sharing_scale_tolerance = 2e-6;
};

struct StaticDisplacementSurfaceRequest {
    std::span<const Vertex> vertices;
    std::span<const std::uint32_t> indices;
    // Actual original arrays used by the first upload. The caller retains them
    // until sample_source() returns; preparing never recomputes their quats.
    const PackedStaticSurface* uploaded_source = nullptr;
    std::span<const glm::mat4> instance_transforms;
    bool static_vertices = false; // explicit scene ownership, not bind-pose inference
    DisplacementHeightRoute height_route = DisplacementHeightRoute::TextureLod;
    filament::Texture* selected_height = nullptr; // borrowed, actual TextureStore selection
    GeometryDisplacementDescriptor displacement{};
    std::array<float, 4> base_transform{1, 1, 0, 0};
};

struct PreparedDisplacementSurface {
    DisplacementPreparationDisposition disposition = DisplacementPreparationDisposition::Disabled;
    std::string reason;
    // Retained for a well-formed source within the source budget, including
    // eligibility fallback. Over-budget inputs remain owned by the caller.
    PackedStaticSurface original_packed;
    std::vector<BoundedDisplacementVertex> source_vertices;
    std::vector<std::array<std::uint32_t, 3>> source_triangles;
    std::vector<HeightSampleValue> native_height;
    BoundedDisplacementSurface geometry;
    GeometryDisplacementDescriptor displacement;
    double uniform_scale = 1.0;
    double maximum_uniform_metric_error = 0.0;
    // Raw spatial scale/amplitude remain in the shader's preclip metric. The
    // smallest positive constant W bounds motion after the homogeneous divide.
    double minimum_homogeneous_w = 1.0;
    double effective_correspondence_bound = 0.020;
    double effective_initial_node_bound = 0.00999;
    double effective_position_error = 0.002;
    bool source_ready = false;
    bool geometry_integrity_valid = false;
    bool requested_quality_met = false;
    // This checks representation integrity, not quality/visual acceptance.
    // The owner must explicitly retain/report BudgetLimited's unmet criterion.
    [[nodiscard]] bool ready() const noexcept {
        return source_ready && geometry_integrity_valid &&
                (disposition == DisplacementPreparationDisposition::Prepared ||
                 disposition == DisplacementPreparationDisposition::BudgetLimited);
    }
};

// Native sampling runs on the Engine owner, outside beginFrame/endFrame. The
// batch and selected texture are borrowed; this module owns CPU outputs only.
// Frozen-source refinement reads only CPU arrays and policy and can run on a
// worker while the preparer/batch stay alive. No per-frame generation.
class DisplacementSurfacePreparer final {
public:
    explicit DisplacementSurfacePreparer(HeightSampleBatch& height_batch,
            DisplacementPreparationPolicy policy = {})
        : height_batch_(height_batch), policy_(policy) {}
    // Freeze validated uploaded source fields and measured native H without
    // building a target or claiming geometry integrity. The returned arrays
    // have no borrowed request/texture lifetime and can be ranked by the caller.
    [[nodiscard]] Result<PreparedDisplacementSurface> sample_source(
            const StaticDisplacementSurfaceRequest& r) const;
    // Consume a frozen source with this preparer's resource/quality policy.
    // This phase neither samples the GPU nor recomputes original packing.
    [[nodiscard]] Result<PreparedDisplacementSurface> refine_source(
            PreparedDisplacementSurface result) const;
    [[nodiscard]] Result<PreparedDisplacementSurface> prepare(
            const StaticDisplacementSurfaceRequest& request) const;
private:
    HeightSampleBatch& height_batch_;
    DisplacementPreparationPolicy policy_;
};

} // namespace heaven
#endif
