/*
 * HeavenBench — CPU-side grass blade generation
 *
 * Ports the compute shader grass generation (grass_generate.comp) to C++.
 * Generates blade geometry from root positions with wind animation,
 * producing vertex data suitable for upload to any graphics API.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_GRASS_CPU_H_
#define HEAVENBENCH_SRC_ENGINE_GRASS_CPU_H_

#include "engine/asset_file_system.h"
#include "engine/types.h"
#include <glm/glm.hpp>
#include <vector>

namespace heaven {

// ── Grass vertex (matches what Filament expects) ────────────────

struct GrassVertex {
    float px, py, pz;       // world position
    float nx, ny, nz;       // normal
    float u, v;             // UV (u=width, v=height 0-1)
    float r, g, b, a;       // pre-computed color + alpha
};

// ── Grass blade root ────────────────────────────────────────────

struct GrassRoot {
    glm::vec3 position;     // world-space root position (Z-up)
};

// ── Grass generation parameters ─────────────────────────────────

struct GrassParams {
    // Blades are narrower and a touch taller than before so the
    // meadow reads as fine wispy stems rather than broad flat
    // cards. The original Heaven grass is a dense field of thin
    // leaves; wide blades made ours look like a sparse succulent
    // patch. Lower bend keeps tall blades upright (less of the
    // "flat face turned to camera" look that flattened the field).
    float blade_width          = 0.062f;
    float blade_width_variance = 0.03f;
    float blade_height         = 0.88f;
    float blade_height_variance= 0.68f;
    float blade_bend_max       = 0.45f;
    float blade_curvature      = 1.0f;
    int   blade_segments       = 4;
    float wind_amplitude       = 0.65f * 0.3f;
    glm::vec2 wind_direction   = {1.0f, 0.0f};
};

// Per-blade scratch capacity. The generator writes `segments*2 + 1` points
// per blade; sizing the fixed pts/uvs arrays from this capacity (rather than a
// hard-coded 4-segment assumption) means a configured `blade_segments` value up
// to kMaxBladeSegments cannot overflow the stack arrays. The effective segment
// count is clamped to kMaxBladeSegments inside generate_grass_blades.
inline constexpr int kMaxBladeSegments =
    render_tuning::shader_limits.grass_blade_segments;
inline constexpr int kMaxBladePts = kMaxBladeSegments * 2 + 1;    // 17

// Per-blade triangle/vertex counts for the Shipping_Look default of 4 segments.
// These describe the default output layout used by the grass features to size
// their GPU vertex buffers. Inside the generator the counts are derived from the
// clamped segment count instead; at 4 segments they equal these constants.
static constexpr int GRASS_TRIS_PER_BLADE = 7;   // 4 segments * 2 - 1
static constexpr int GRASS_VERTS_PER_BLADE = 21;  // 7 * 3
static constexpr int FLOWER_VERTS_PER_CARD = 12;  // two crossed quads

// Dimensions for one authored flowering-plant card. Variation is derived
// deterministically from the root position so archive and loose runs match.
struct FlowerCardParams {
    float width = render_tuning::grass.flower_card_shape.x;
    float width_variance = render_tuning::grass.flower_card_shape.y;
    float height = render_tuning::grass.flower_card_shape.z;
    float height_variance = render_tuning::grass.flower_card_shape.w;
};

// ── Generate grass blade roots from mesh surfaces ───────────────

//Extract grass root positions from surfaces whose name contains the filter string.
//Uses area-weighted random sampling to distribute roots evenly.
std::vector<GrassRoot> generate_grass_roots(
    const std::string& mesh_path,
    int target_blade_count = 120000,
    const std::string& surface_filter = "grass",
    AssetFileSystemRef assets = std::nullopt,
    std::uint32_t random_seed = 12345u);

// ── Generate grass blade geometry ───────────────────────────────

//Generate grass blade vertices once for upload. Runtime wind animation is
//applied by heaven_grass.mat; `time` only selects the generated rest pose.
//Returns vertex data ready for upload to a vertex buffer.

//@param roots       Blade root positions
//@param time        Current time in seconds (for wind animation)
//@param params      Blade generation parameters
std::vector<GrassVertex> generate_grass_blades(
    const std::vector<GrassRoot>& roots,
    float time,
    const GrassParams& params = {});

// Generate two alpha-masked crossed cards per root for the flowering plant in
// grass_variation_d.dds. UVs remain normalized across each card; the material
// instance maps them into the authored atlas region so UV.y can also drive wind.
std::vector<GrassVertex> generate_flower_cards(
    const std::vector<GrassRoot>& roots,
    const FlowerCardParams& params = {});

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_GRASS_CPU_H_
