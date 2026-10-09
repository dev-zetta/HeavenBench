/*
 * HeavenBench — CPU-side grass blade generation
 *
 * Ported from grass_generate.comp. Generates blade geometry on the CPU
 * with wind animation, producing colored vertices for Filament rendering.
 */
#include "engine/grass_cpu.h"
#include "render_tuning.h"
#include "engine/mesh.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <numeric>
#include <numbers>

namespace heaven {
namespace {

// ── Noise functions (ported from the compute shader) ────────────

float snoise(glm::vec2 v) {
    const glm::vec4 C(0.211324865405187f, 0.366025403784439f,
                      -std::numbers::inv_sqrt3_v<float>, 0.024390243902439f);
    glm::vec2 i = glm::floor(v + glm::dot(v, glm::vec2(C.y)));
    glm::vec2 x0 = v - i + glm::dot(i, glm::vec2(C.x));
    glm::vec2 i1 = (x0.x > x0.y) ? glm::vec2(1, 0) : glm::vec2(0, 1);
    glm::vec4 x12 = glm::vec4(x0.x, x0.y, x0.x, x0.y) + glm::vec4(C.x, C.x, C.z, C.z);
    x12.x -= i1.x; x12.y -= i1.y;
    i = glm::mod(i, glm::vec2(289.0f));

    auto permute3 = [](glm::vec3 x) -> glm::vec3 {
        return glm::mod(((x * 34.0f) + 1.0f) * x, glm::vec3(289.0f));
    };
    glm::vec3 p = permute3(permute3(glm::vec3(i.y, i.y + i1.y, i.y + 1.0f))
                  + glm::vec3(i.x, i.x + i1.x, i.x + 1.0f));
    glm::vec3 m = glm::max(glm::vec3(0.5f) - glm::vec3(
        glm::dot(x0, x0),
        glm::dot(glm::vec2(x12.x, x12.y), glm::vec2(x12.x, x12.y)),
        glm::dot(glm::vec2(x12.z, x12.w), glm::vec2(x12.z, x12.w))),
        glm::vec3(0.0f));
    m = m * m; m = m * m;
    glm::vec3 x = 2.0f * glm::fract(p * C.w) - 1.0f;
    glm::vec3 h = glm::abs(x) - 0.5f;
    glm::vec3 ox = glm::floor(x + 0.5f);
    glm::vec3 a0 = x - ox;
    m *= 1.79284291400159f - 0.85373472095314f * (a0 * a0 + h * h);
    glm::vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.y = a0.y * x12.x + h.y * x12.y;
    g.z = a0.z * x12.z + h.z * x12.w;
    return 130.0f * glm::dot(m, g);
}

float random1(glm::vec2 c) {
    float j = 4096.0f * std::sin(glm::dot(c, glm::vec2(17.0f, 59.4f)));
    return snoise(c * 512.0f + j);
}

glm::mat4 rotate_around_axis(glm::vec3 axis, float angle) {
    axis = glm::normalize(axis);
    float s = std::sin(angle), c = std::cos(angle), oc = 1.0f - c;
    return {
        oc*axis.x*axis.x+c,          oc*axis.x*axis.y-axis.z*s, oc*axis.z*axis.x+axis.y*s, 0,
        oc*axis.x*axis.y+axis.z*s,   oc*axis.y*axis.y+c,        oc*axis.y*axis.z-axis.x*s, 0,
        oc*axis.z*axis.x-axis.y*s,   oc*axis.y*axis.z+axis.x*s, oc*axis.z*axis.z+c,        0,
        0, 0, 0, 1};
}

} // namespace

// ── Root generation ─────────────────────────────────────────────

std::vector<GrassRoot> generate_grass_roots(
    const std::string& mesh_path,
    int target_blade_count,
    const std::string& surface_filter,
    AssetFileSystemRef assets,
    std::uint32_t random_seed)
{
    std::vector<GrassRoot> roots;

    auto mesh_result = load_mi08(mesh_path, assets);
    if (!mesh_result) return roots;

    struct Triangle { glm::vec3 v0, v1, v2; float area; };
    std::vector<Triangle> grass_tris;
    float total_area = 0;

    for (const auto& surf : mesh_result->surfaces) {
        if (surf.name.find(surface_filter) == std::string::npos) continue;
        for (size_t i = 0; i + 2 < surf.indices.size(); i += 3) {
            auto& a = surf.vertices[surf.indices[i]];
            auto& b = surf.vertices[surf.indices[i+1]];
            auto& c = surf.vertices[surf.indices[i+2]];
            glm::vec3 p0(a.px, a.py, a.pz);
            glm::vec3 p1(b.px, b.py, b.pz);
            glm::vec3 p2(c.px, c.py, c.pz);
            float area = 0.5f * glm::length(glm::cross(p1 - p0, p2 - p0));
            if (area > 0.001f) {
                grass_tris.push_back({p0, p1, p2, area});
                total_area += area;
            }
        }
    }

    if (grass_tris.empty()) return roots;

    // Build CDF for area-weighted sampling
    std::vector<float> cdf(grass_tris.size());
    float running = 0;
    for (size_t i = 0; i < grass_tris.size(); i++) {
        running += grass_tris[i].area;
        cdf[i] = running;
    }

    std::srand(random_seed);
    roots.reserve(target_blade_count);
    constexpr float kRandMax = static_cast<float>(RAND_MAX);

    for (int i = 0; i < target_blade_count; i++) {
        float r = static_cast<float>(std::rand()) / kRandMax * total_area;
        auto it = std::lower_bound(cdf.begin(), cdf.end(), r);
        size_t ti = std::min(static_cast<size_t>(it - cdf.begin()), grass_tris.size() - 1);
        const auto& tri = grass_tris[ti];

        float u = static_cast<float>(std::rand()) / kRandMax;
        float v = static_cast<float>(std::rand()) / kRandMax;
        if (u + v > 1.0f) { u = 1.0f - u; v = 1.0f - v; }
        glm::vec3 p = tri.v0 + u * (tri.v1 - tri.v0) + v * (tri.v2 - tri.v0);
        roots.push_back({p});
    }

    std::printf("  Grass: %d roots generated from %zu triangles (area=%.0f)\n",
                target_blade_count, grass_tris.size(), total_area);
    return roots;
}

// ── Blade generation (per-frame) ────────────────────────────────

namespace {

// Per-blade variant kinds. Choosing a kind is deterministic from
// the root's world position, so the same blade always gets the
// same shape across runs.
enum class BladeKind : uint8_t {
    Wide      = 0,  // ~65% — broad blade, current default geometry
    Strand    = 1,  // ~30% — tall thin wispy stem
    Seedhead  = 2,  // ~5%  — short stalk topped with a wider pale tip;
                    //         true flowers use authored crossed cards in
                    //         generate_flower_cards().
};

BladeKind pick_blade_kind(glm::vec2 hashSeed) {
    // Independent hash from seed: random1 returns roughly [-1, 1].
    // Shift to [0, 1] and partition into kind buckets. The +9999
    // offset is just a different sample point from height/width
    // hashes used elsewhere so kinds don't correlate with size.

    // Distribution favours thin wispy strands (60%) over broad
    // blades (35%) to match the original Heaven meadow, which is a
    // dense field of fine leaves rather than wide cards. A small
    // fraction (5%) are taller seedhead stalks for silhouette
    // variety against the sky.
    float h = random1(hashSeed + glm::vec2(9999.0f));
    h = h * 0.5f + 0.5f;
    if (h < render_tuning::grass.kind_thresholds.x)
        return BladeKind::Seedhead;
    if (h < render_tuning::grass.kind_thresholds.y)
        return BladeKind::Strand;
    return BladeKind::Wide;
}

} // namespace

std::vector<GrassVertex> generate_grass_blades(
    const std::vector<GrassRoot>& roots,
    float time,
    const GrassParams& params)
{
    const std::size_t blade_count = roots.size();

    const float PI = std::numbers::pi_v<float>;

    // Clamp the configured segment count to the per-blade scratch capacity so a
    // blade_segments value greater than kMaxBladeSegments cannot overflow the
    // fixed-size pts/uvs arrays. At the Shipping_Look default
    // of 4 segments this clamp is a no-op, keeping geometry byte-for-byte
    // identical.
    const int segments = std::clamp(params.blade_segments, 1, kMaxBladeSegments);
    const int max_pts = segments * 2 + 1;

    // Per-blade triangle/vertex counts derived from the clamped segment count
    // rather than the hard-coded 4-segment constants. For
    // segments == 4 these equal GRASS_TRIS_PER_BLADE (7) / GRASS_VERTS_PER_BLADE
    // (21), so the output layout and total size match the current output.
    const int tris_per_blade  = segments * 2 - 1;
    const int verts_per_blade = tris_per_blade * 3;

    std::vector<GrassVertex> vertices(blade_count * verts_per_blade);

    // Vertex colour carries only per-kind albedo tint. Texture sampling,
    // height-based ambient occlusion, direct/indirect lighting, and shadows
    // happen in heaven_grass.mat / Filament at render time.

    for (std::size_t id = 0; id < blade_count; id++) {
        // Root position is in Z-up; convert to Y-up for blade generation
        glm::vec3 rootZUp = roots[id].position;
        glm::vec3 posWS(rootZUp.x, rootZUp.z, rootZUp.y); // Y-up

        // Per-blade kind — wide / wispy strand / flower stalk.
        // Picked deterministically from the world XZ position so
        // the same blade is always the same shape.
        BladeKind kind = pick_blade_kind(glm::vec2(posWS.x, posWS.z));

        // Per-kind geometry tweaks. The `width_mul` shrinks the
        // blade horizontally for strands; `height_mul` makes
        // strands taller and flowers shorter; `bend_mul` keeps
        // strands more upright (they'd droop into the ground if
        // they bent as much as wide blades) and gives flowers a
        // slight tilt for character; `curvature_mul` controls how
        // quickly the bend ramps along the height — strands curve
        // gently from the start, flowers stay straight then bend
        // sharply at the top.
        float width_mul     = 1.0f;
        float height_mul    = 1.0f;
        float bend_mul      = 1.0f;
        float curvature_mul = 1.0f;
        switch (kind) {
            case BladeKind::Wide:
                break;
            case BladeKind::Strand:
                width_mul     = render_tuning::grass.strand_shape.x;
                height_mul    = render_tuning::grass.strand_shape.y;
                bend_mul      = render_tuning::grass.strand_shape.z;
                curvature_mul = render_tuning::grass.strand_shape.w;
                break;
            case BladeKind::Seedhead:
                width_mul     = render_tuning::grass.seedhead_shape.x;
                height_mul    = render_tuning::grass.seedhead_shape.y;
                bend_mul      = render_tuning::grass.seedhead_shape.z;
                curvature_mul = render_tuning::grass.seedhead_shape.w;
                break;
        }

        // Height/width variation
        float grassHeightNoise = snoise(
            glm::vec2(posWS.x, posWS.z) *
            render_tuning::grass.height_noise_scale);
        float grassHeight = std::pow(std::clamp(
            std::abs(grassHeightNoise) * 0.5f + 0.5f, 0.0f, 1.0f),
            render_tuning::grass.height_noise_power);

        float height = (params.blade_height + params.blade_height_variance * grassHeight) * height_mul;
        float width = (params.blade_width + params.blade_width_variance *
                      random1(glm::vec2(posWS.x, posWS.z) +
                              render_tuning::grass.width_noise_seed)) * width_mul;
        float maxBend = params.blade_bend_max * PI / 2.0f *
                        random1(glm::vec2(posWS.x, posWS.z) +
                                render_tuning::grass.bend_noise_seed) * bend_mul;

        // Matrices
        glm::mat4 bendMatrix = rotate_around_axis(glm::vec3(1,0,0), maxBend);
        glm::mat4 rotMatrix = rotate_around_axis(glm::vec3(0,1,0),
            snoise(glm::vec2(posWS.x, posWS.z) *
                   render_tuning::grass.rotation_noise_scale) * 2.0f * PI);
        glm::mat4 scaleMatrix(1.0f);
        scaleMatrix[0][0] = width; scaleMatrix[1][1] = height;
        glm::mat4 toWS = rotMatrix * bendMatrix * scaleMatrix;

        // Generate blade points
        glm::vec3 pts[kMaxBladePts];
        glm::vec2 uvs[kMaxBladePts];

        for (int i = 0; i < segments; i++) {
            float v = static_cast<float>(i) / static_cast<float>(segments);
            float u = v / 2.0f;
            glm::mat4 bendSeg = rotate_around_axis(glm::vec3(1,0,0),
                maxBend * std::pow(v, params.blade_curvature * curvature_mul));
            glm::mat4 toWSSeg = rotMatrix * bendSeg * scaleMatrix;

            const auto point_index = static_cast<std::size_t>(i) * 2u;
            pts[point_index]   = glm::vec3(toWSSeg * glm::vec4(2.0f*(u-0.5f), v, 0, 1)) + posWS;
            pts[point_index + 1] = glm::vec3(toWSSeg * glm::vec4(2.0f*(0.5f-u), v, 0, 1)) + posWS;
            uvs[point_index]   = glm::vec2(u, v);
            uvs[point_index + 1] = glm::vec2(1.0f-u, v);
        }
        const auto tip_index = static_cast<std::size_t>(segments) * 2u;
        pts[tip_index] = glm::vec3(toWS * glm::vec4(0, 1, 0, 1)) + posWS;
        uvs[tip_index] = glm::vec2(0.5f, 1.0f);

        // Wind
        float idHash = snoise(
            (glm::vec2(posWS.x, posWS.z) +
             render_tuning::grass.bend_noise_seed) *
            render_tuning::grass.identity_noise_scale);
        float windFreq = (idHash >= 0.5f)
            ? render_tuning::grass.wind_frequency.x
            : render_tuning::grass.wind_frequency.y;
        float wind = std::cos(
            time * (windFreq * (1.0f + grassHeight / 2.0f)) +
            idHash * render_tuning::grass.wind_phase_scale);
        wind = wind * wind * params.wind_amplitude *
               (1.0f + grassHeight / 2.0f) *
               render_tuning::grass.wind_response;

        for (int i = 0; i < max_pts; i++) {
            float vy = uvs[i].y;
            pts[i] += wind * vy * glm::vec3(params.wind_direction.x, 0, params.wind_direction.y);
        }

        // Blade cards are nearly vertical, so their geometric normals are
        // nearly horizontal. That makes an overhead sun contribute almost no
        // diffuse light and leaves the meadow unnaturally black. Vegetation
        // renderers conventionally bend card normals toward the canopy normal:
        // retain enough horizontal component for directional variation while
        // giving both card faces a stable upward response.
        glm::vec3 bladeRight = pts[1] - pts[0];
        glm::vec3 bladeUp = pts[2] - pts[0];
        glm::vec3 bladeNormal = glm::normalize(glm::cross(bladeRight, bladeUp));

        // Convert normal to Z-up
        glm::vec3 normalZUp(bladeNormal.x, bladeNormal.z, bladeNormal.y);
        normalZUp = glm::normalize(glm::vec3(
            normalZUp.x * render_tuning::grass.normal_bend.x,
            normalZUp.y * render_tuning::grass.normal_bend.x,
            render_tuning::grass.normal_bend.y));

        // Generate triangles
        const std::size_t base = id * verts_per_blade;
        int tri = 0;

        for (int i = 0; i <= segments * 2 - 2 && tri < tris_per_blade; i++) {
            for (int k = 0; k < 3; k++) {
                int vi = i + k;
                if (vi >= max_pts) vi = max_pts - 1;

                // Convert from Y-up to Z-up
                glm::vec3 p(pts[vi].x, pts[vi].z, pts[vi].y);
                float h = uvs[vi].y;

                // Per-kind colour tweak. Strands get a touch
                // yellow-warm at the tips (dry seedhead look) and
                // flowers get a brighter tip overall (so the
                // monochrome diffuse reads as a stalk topped with
                // a paler bloom). We multiply the lighting term
                // by the tint so the shader's `diffuse * lighting`
                // path picks it up automatically.
                glm::vec3 kindTint(1.0f);
                if (kind == BladeKind::Strand) {
                    // Yellower at the tip (linear blend on h)
                    const auto tint = render_tuning::grass.strand_tip_tint;
                    kindTint = glm::mix(
                        glm::vec3(1.0f),
                        glm::vec3(tint.x, tint.y, tint.z), h);
                } else if (kind == BladeKind::Seedhead) {
                    // Whiter overall, strongest near the top so it
                    // reads as a small bloom.
                    const auto tint = render_tuning::grass.seedhead_tip_tint;
                    kindTint = glm::mix(
                        glm::vec3(1.0f),
                        glm::vec3(tint.x, tint.y, tint.z), h);
                }
                // Distance fade (will be handled by alpha)
                float fade = 1.0f; // Filament handles distance culling

                auto& gv = vertices[base + static_cast<std::size_t>(tri) * 3 + k];
                gv.px = p.x; gv.py = p.y; gv.pz = p.z;
                gv.nx = normalZUp.x; gv.ny = normalZUp.y; gv.nz = normalZUp.z;
                gv.u = uvs[vi].x; gv.v = uvs[vi].y;
                gv.r = kindTint.x; gv.g = kindTint.y; gv.b = kindTint.z;
                gv.a = (h < render_tuning::grass.base_alpha_cutoff)
                    ? 0.0f : fade;
            }
            tri++;
        }

        // Fill remaining with degenerate
        glm::vec3 rootOut(rootZUp.x, rootZUp.y, rootZUp.z);
        for (int i = tri; i < tris_per_blade; i++) {
            for (int k = 0; k < 3; k++) {
                auto& gv = vertices[base + static_cast<std::size_t>(i) * 3 + k];
                gv.px = rootOut.x; gv.py = rootOut.y; gv.pz = rootOut.z;
                gv.nx = 0; gv.ny = 0; gv.nz = 1;
                gv.u = 0; gv.v = 0;
                gv.r = 0; gv.g = 0; gv.b = 0; gv.a = 0;
            }
        }
    }

    return vertices;
}

std::vector<GrassVertex> generate_flower_cards(
    const std::vector<GrassRoot>& roots,
    const FlowerCardParams& params)
{
    std::vector<GrassVertex> vertices;
    vertices.reserve(roots.size() * FLOWER_VERTS_PER_CARD);

    constexpr std::array<std::array<int, 3>, 2> triangles{{
        {{0, 1, 2}}, {{1, 3, 2}},
    }};
    constexpr std::array<glm::vec2, 4> uvs{{
        glm::vec2{0.0f, 0.0f}, glm::vec2{1.0f, 0.0f},
        glm::vec2{0.0f, 1.0f}, glm::vec2{1.0f, 1.0f},
    }};

    for (const GrassRoot& root : roots) {
        const glm::vec2 seed(root.position.x, root.position.y);
        const float width_noise =
            random1(seed + glm::vec2(413.0f)) * 0.5f + 0.5f;
        const float height_noise =
            random1(seed + glm::vec2(719.0f)) * 0.5f + 0.5f;
        const float width = params.width + params.width_variance * width_noise;
        const float height = params.height + params.height_variance * height_noise;
        const float angle =
            (random1(seed + glm::vec2(977.0f)) * 0.5f + 0.5f) *
            std::numbers::pi_v<float>;
        const float palette =
            random1(seed + glm::vec2(1237.0f)) * 0.5f + 0.5f;
        auto tint = render_tuning::grass.flower_tint_yellow;
        if (palette >= render_tuning::grass.flower_palette_split.y) {
            tint = render_tuning::grass.flower_tint_red;
        } else if (palette >= render_tuning::grass.flower_palette_split.x) {
            tint = render_tuning::grass.flower_tint_blue;
        }

        for (int plane = 0; plane < 2; ++plane) {
            const float a = angle + static_cast<float>(plane) *
                                      1.57079632679490f;
            const glm::vec3 right(std::cos(a) * width * 0.5f,
                                  std::sin(a) * width * 0.5f, 0.0f);
            const std::array card{
                root.position - right,
                root.position + right,
                root.position - right + glm::vec3(0.0f, 0.0f, height),
                root.position + right + glm::vec3(0.0f, 0.0f, height),
            };
            const glm::vec3 horizontal_normal(
                -std::sin(a), std::cos(a), 0.0f);
            const glm::vec3 normal = glm::normalize(
                horizontal_normal * render_tuning::grass.normal_bend.x +
                glm::vec3(0.0f, 0.0f,
                          render_tuning::grass.normal_bend.y));

            for (const auto& triangle : triangles) {
                for (const int index : triangle) {
                    const glm::vec3& position = card[index];
                    const glm::vec2& uv = uvs[index];
                    vertices.push_back({
                        position.x, position.y, position.z,
                        normal.x, normal.y, normal.z,
                        uv.x, uv.y,
                        tint.x, tint.y, tint.z, 1.0f,
                    });
                }
            }
        }
    }
    return vertices;
}

} // namespace heaven
