/*
 * Deterministic coverage for `_t`-guided neutral tessellation topology.
 */
#include "engine/cpu_tessellation.h"
#include "engine/massive_foliage_terrain.h"
#include "engine/tessellation_fade.h"
#include "rhi/mesh_displacement.h"
#include "xml0_parser.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};
} // namespace heaven::testing

namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

std::vector<uint8_t> rgba8_dds(uint8_t density) {
    std::vector<uint8_t> out;
    put_u32(out, 0x20534444u);
    put_u32(out, 124);
    put_u32(out, 0);
    put_u32(out, 4);
    put_u32(out, 4);
    put_u32(out, 16);
    put_u32(out, 0);
    put_u32(out, 1);
    for (int i = 0; i < 11; ++i) put_u32(out, 0);
    put_u32(out, 32);
    put_u32(out, 0x41);
    put_u32(out, 0);
    put_u32(out, 32);
    put_u32(out, 0x00ff0000u);
    put_u32(out, 0x0000ff00u);
    put_u32(out, 0x000000ffu);
    put_u32(out, 0xff000000u);
    for (int i = 0; i < 5; ++i) put_u32(out, 0);
    for (int i = 0; i < 16; ++i) {
        out.push_back(density);
        out.push_back(density);
        out.push_back(density);
        out.push_back(255);
    }
    return out;
}

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const std::vector<uint8_t>& bytes) {
        static std::atomic<uint64_t> next{0};
        path = std::filesystem::temp_directory_path() /
               ("heaven_tess_density_" + std::to_string(next++) + ".dds");
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

heaven::Surface square() {
    heaven::Surface out;
    out.name = "square";
    out.vertices.resize(4);
    const float p[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    for (int i = 0; i < 4; ++i) {
        auto& v = out.vertices[i];
        v.px = p[i][0]; v.py = p[i][1]; v.pz = 0;
        v.nx = 0; v.ny = 0; v.nz = 1;
        v.u = p[i][0] * 0.5f; v.v = p[i][1] * 0.5f;
        v.u1 = 0.25f + v.u; v.v1 = 0.5f + v.v;
    }
    out.indices = {0, 1, 2, 0, 2, 3};
    return out;
}

bool same_geometry(const heaven::Surface& a, const heaven::Surface& b) {
    if (a.indices != b.indices || a.vertices.size() != b.vertices.size())
        return false;
    for (size_t i = 0; i < a.vertices.size(); ++i) {
        const auto& x = a.vertices[i];
        const auto& y = b.vertices[i];
        if (x.px != y.px || x.py != y.py || x.pz != y.pz ||
            x.u != y.u || x.v != y.v || x.u1 != y.u1 || x.v1 != y.v1)
            return false;
    }
    return true;
}

bool unit_tessellation_topology() {
    if (heaven::kMaxTessellationSubdivisions != 3) return false;

    XmlNode near_surface{
        .tag = "surface",
        .attrs = {{"folded", "1"}, {"max_visible_distance", "45"}}};
    XmlNode far_surface{
        .tag = "surface",
        .attrs = {{"min_visible_distance", "45"}}};
    XmlNode disabled_surface{
        .tag = "surface",
        .attrs = {{"enabled", "0"}}};
    XmlNode malformed_surface{
        .tag = "surface",
        .attrs = {{"min_visible_distance", "far"}}};
    if (xml0_surface_inactive_in_high_detail_mode(near_surface) ||
        !xml0_surface_inactive_in_high_detail_mode(far_surface) ||
        !xml0_surface_inactive_in_high_detail_mode(disabled_surface) ||
        xml0_surface_inactive_in_high_detail_mode(malformed_surface)) {
        return false;
    }

    if (!heaven::is_massive_foliage_terrain(
            "heaven/meshes/environment/fly_island1.mesh",
            "fly_island1_ground") ||
        !heaven::is_massive_foliage_terrain(
            "/package/heaven/meshes/environment/fly_island3.mesh",
            "fly_island3_ground") ||
        heaven::is_massive_foliage_terrain(
            "heaven/meshes/environment/fly_island1.mesh",
            "fly_island1_grass_mask") ||
        heaven::is_massive_foliage_terrain(
            "heaven/meshes/environment/fort.mesh",
            "fort_ground")) return false;

    const auto terrain_normal =
        heaven::massive_foliage_terrain_params(1.0f);
    const auto terrain_max =
        heaven::massive_foliage_terrain_params(2.0f);
    if (terrain_normal.tessellation_factor != 2.0f ||
        terrain_normal.max_subdivisions != 2 ||
        terrain_normal.displacement_scale != 0.2f ||
        terrain_max.tessellation_factor != 4.0f ||
        terrain_max.max_subdivisions != 3) return false;

    if (!heaven::is_village_ground_surface(
            "heaven/meshes/buildings/fort.mesh", "fort_ground",
            "fort_ground") ||
        !heaven::is_village_ground_surface(
            "/package/heaven/meshes/ruins/island1_ruins.mesh",
            "ruins_ground", "ruins_ground") ||
        heaven::is_village_ground_surface(
            "heaven/meshes/buildings/fort.mesh", "fort_steps",
            "fort_steps") ||
        heaven::is_village_ground_surface(
            "heaven/meshes/ruins/island1_ruins.mesh", "ruins_pavement",
            "ruins_pavement") ||
        heaven::is_village_ground_surface(
            "heaven/meshes/environment/fly_island1.mesh", "island_ground",
            "ruins_ground")) return false;
    heaven::AppConfig ground_config;
    auto ground_paths = heaven::PathResolver::create("Data_Root");
    auto ground_policy = heaven::resolve_surface_displacement_policy(
        ground_config, *ground_paths,
        "heaven/meshes/buildings/fort.mesh", "fort_ground", "fort_ground");
    if (!ground_policy || ground_policy->tessellation_factor != 16.0f ||
        ground_policy->tessellation_length != 0.15f ||
        ground_policy->max_subdivisions != 5 ||
        ground_policy->displacement_scale != 0.03f ||
        ground_policy->displacement_threshold != 2.0f ||
        ground_policy->displacement_distance != 18.0f ||
        ground_policy->boundary_fade != 0.30f ||
        !ground_policy->derive_displaced_normal) return false;

    // The original global distance control scales authored distances rather
    // than imposing a world-space cap. Extreme therefore extends a wall with
    // threshold=4 and range=40 to the same threshold and a range of 60.
    const auto normal_fade = heaven::tessellation_fade(4.0f, 40.0f, 1.0f);
    const auto extreme_fade = heaven::tessellation_fade(4.0f, 40.0f, 1.5f);
    const auto disabled_fade = heaven::tessellation_fade(4.0f, 40.0f, 0.0f);
    if (normal_fade.threshold != 4.0f || normal_fade.range != 40.0f ||
        extreme_fade.threshold != 4.0f || extreme_fade.range != 60.0f ||
        std::abs(extreme_fade.inverse_range() - 1.0f / 60.0f) > 1e-6f ||
        !extreme_fade.enabled() || disabled_fade.enabled()) return false;

    TempFile zero(rgba8_dds(0));
    TempFile full(rgba8_dds(255));

    auto low = square();
    const auto original = low;
    if (!heaven::cpu_tessellate_surface(
            low, zero.path.string(), {15.0f, 1.0f, 3})) return false;
    if (!same_geometry(low, original)) return false;

    auto high_a = square();
    auto high_b = square();
    if (!heaven::cpu_tessellate_surface(
            high_a, full.path.string(), {15.0f, 1.0f, 3}) ||
        !heaven::cpu_tessellate_surface(
            high_b, full.path.string(), {15.0f, 1.0f, 3})) return false;
    if (high_a.indices.size() <= low.indices.size() ||
        !same_geometry(high_a, high_b)) return false;

    // The global quality multiplier belongs in the authored edge factor.
    // Zero disables refinement; increasing it monotonically increases density
    // while the fixed dyadic level cap remains unchanged.
    auto quality_zero = square();
    auto quality_low = square();
    auto quality_high = square();
    if (!heaven::cpu_tessellate_surface(
            quality_zero, full.path.string(), {0.0f, 0.25f, 4}) ||
        !heaven::cpu_tessellate_surface(
            quality_low, full.path.string(), {4.0f, 0.25f, 4}) ||
        !heaven::cpu_tessellate_surface(
            quality_high, full.path.string(), {8.0f, 0.25f, 4})) return false;
    if (!same_geometry(quality_zero, original) ||
        quality_low.indices.size() <= quality_zero.indices.size() ||
        quality_high.indices.size() <= quality_low.indices.size()) return false;

    // Missing `_t` inherits the base material's white density texture.
    auto inherited_white = square();
    if (!heaven::cpu_tessellate_surface(
            inherited_white, "", {15.0f, 1.0f, 3}) ||
        !same_geometry(inherited_white, high_a)) return false;

    // The two original triangles share the diagonal. Every dyadic point on it
    // is represented once, proving shared-edge midpoint reuse (no T-junction).
    int diagonal_midpoints = 0;
    for (const auto& v : high_a.vertices) {
        if (std::abs(v.px - 1.0f) < 1e-6f &&
            std::abs(v.py - 1.0f) < 1e-6f) {
            ++diagonal_midpoints;
        }
        if (std::abs(v.pz) > 1e-6f) return false; // topology stays neutral
        if (std::abs(v.u1 - (0.25f + v.u)) > 1e-6f ||
            std::abs(v.v1 - (0.5f + v.v)) > 1e-6f) return false;
    }
    if (diagonal_midpoints != 1) return false;

    auto faded = square();
    if (!heaven::cpu_tessellate_surface(
            faded, full.path.string(),
            heaven::TessellationTopologyParameters{
                .density_factor = 15.0f,
                .edge_scale = 1.0f,
                .subdivisions = 3,
                .boundary_fade = 0.30f})) return false;
    bool found_interior = false;
    for (const auto& vertex : faded.vertices) {
        const bool boundary = vertex.px == 0.0f || vertex.px == 2.0f ||
                              vertex.py == 0.0f || vertex.py == 2.0f;
        if (boundary && vertex.displacement_normal_scale != 0.0f) return false;
        if (std::abs(vertex.px - 1.0f) < 1e-6f &&
            std::abs(vertex.py - 1.0f) < 1e-6f) {
            found_interior = true;
            if (vertex.displacement_normal_scale < 0.99f) return false;
        }
    }
    if (!found_interior) return false;

    // Decode failure is atomic: neither payload changes.
    TempFile malformed({0, 1, 2, 3});
    auto unchanged = square();
    if (heaven::cpu_tessellate_surface(
            unchanged, malformed.path.string(), {15.0f, 1.0f, 3}) ||
        !same_geometry(unchanged, square())) {
        std::printf("[unit_tessellation_topology] malformed input was not atomic\n");
        return false;
    }

    heaven::SkinnedSurface skinned;
    skinned.name = "skinned";
    for (const auto& v : square().vertices) {
        heaven::SkinnedVertex sv{};
        sv.px = v.px; sv.py = v.py; sv.pz = v.pz;
        sv.nx = v.nx; sv.ny = v.ny; sv.nz = v.nz;
        sv.u = v.u; sv.v = v.v; sv.u1 = v.u1; sv.v1 = v.v1;
        sv.bone_indices[0] = 3;
        sv.bone_weights[0] = 1.0f;
        skinned.vertices.push_back(sv);
    }
    skinned.indices = square().indices;
    if (!heaven::cpu_tessellate_skinned_surface(
            skinned, full.path.string(), {15.0f, 1.0f, 2})) {
        std::printf("[unit_tessellation_topology] skinned tessellation failed\n");
        return false;
    }
    for (const auto& v : skinned.vertices) {
        if (v.bone_indices[0] != 3 ||
            std::abs(v.bone_weights[0] - 1.0f) > 1e-6f ||
            std::abs(v.u1 - (0.25f + v.u)) > 1e-6f) {
            std::printf("[unit_tessellation_topology] skinned interpolation mismatch\n");
            return false;
        }
    }
    std::printf("[unit_tessellation_topology] PASSED\n");
    return true;
}

const heaven::testing::Register g_register_tessellation_topology{
    &unit_tessellation_topology};

bool unit_reversed_duplicate_tessellation_edge() {
    // The house2 source edge 265--447 is repeated as 448--264. Its endpoints
    // are bit-identical in position and UV0, but the tangent frames differ.
    // These exact float payloads reproduce the quarter-edge rounding failure.
    heaven::Vertex a{};
    a.px = std::bit_cast<float>(0x3f2ee009u);
    a.py = std::bit_cast<float>(0xc02a5de6u);
    a.pz = std::bit_cast<float>(0x4069b949u);
    a.u = std::bit_cast<float>(0x3e930d3bu);
    a.v = std::bit_cast<float>(0x3f5baedcu);
    a.u1 = a.u;
    a.nz = 1.0f;
    a.tx = 1.0f;
    a.tw = 1.0f;
    heaven::Vertex b = a;
    b.px = std::bit_cast<float>(0xbefa072du);
    b.py = std::bit_cast<float>(0xc02a5d9au);
    b.pz = std::bit_cast<float>(0x40698f82u);
    b.u = std::bit_cast<float>(0x3e501698u);
    b.u1 = b.u;
    heaven::Vertex c = a;
    c.py += 1.0f;
    c.v += 0.2f;
    c.v1 = 1.0f;
    heaven::Vertex reverse_b = b;
    heaven::Vertex reverse_a = a;
    heaven::Vertex d = a;
    d.py -= 1.0f;
    d.v += 0.4f; // Different incident UV footprints must still be shared.
    d.v1 = -1.0f;
    for (auto* vertex : {&reverse_b, &reverse_a, &d}) {
        vertex->tx = 0.0f;
        vertex->ty = 1.0f;
        vertex->tw = -1.0f;
    }
    heaven::Surface surface;
    surface.name = "reversed-tangent-seam";
    surface.vertices = {a, b, c, reverse_b, reverse_a, d};
    surface.indices = {0, 1, 2, 3, 4, 5};
    if (!heaven::cpu_tessellate_surface(
            surface, "", {.density_factor = 15.0f,
                          .edge_scale = 16.0f, .subdivisions = 3})) return false;

    std::array<std::vector<uint32_t>, 2> seams;
    for (uint32_t index = 0; index < surface.vertices.size(); ++index) {
        const auto& vertex = surface.vertices[index];
        // UV1.v marks only the duplicated edge; the two disconnected triangle
        // interiors retain positive/negative values after subdivision.
        if (vertex.v1 == 0.0f) {
            if (vertex.tw != 1.0f && vertex.tw != -1.0f) return false;
            seams[vertex.tw > 0.0f ? 0 : 1].push_back(index);
        }
    }
    // Eight segments on each independent copy. Welding the tangent seam must
    // not make this test pass by hiding the duplicated geometry.
    if (seams[0].size() != 9 || seams[1].size() != 9) return false;
    for (auto& seam : seams) {
        std::sort(seam.begin(), seam.end(), [&](uint32_t x, uint32_t y) {
            return surface.vertices[x].u < surface.vertices[y].u;
        });
    }
    const auto bits = [](const heaven::Vertex& vertex) {
        return std::array<uint32_t, 7>{
            std::bit_cast<uint32_t>(vertex.px),
            std::bit_cast<uint32_t>(vertex.py),
            std::bit_cast<uint32_t>(vertex.pz),
            std::bit_cast<uint32_t>(vertex.u),
            std::bit_cast<uint32_t>(vertex.v),
            std::bit_cast<uint32_t>(vertex.u1),
            std::bit_cast<uint32_t>(vertex.v1)};
    };
    const auto footprints =
        heaven::mesh_displacement::calculate_footprints<heaven::Vertex>(
            surface.vertices, surface.indices);
    for (std::size_t index = 0; index < seams[0].size(); ++index) {
        const auto forward = seams[0][index];
        const auto reverse = seams[1][index];
        const auto& x = surface.vertices[forward];
        const auto& y = surface.vertices[reverse];
        if (bits(x) != bits(y) || footprints[forward] != footprints[reverse] ||
            x.tx != 1.0f || x.ty != 0.0f || x.tw != 1.0f ||
            y.tx != 0.0f || y.ty != 1.0f || y.tw != -1.0f) {
            std::printf("[reversed_duplicate_tessellation_edge] mismatch at %zu\n",
                        index);
            return false;
        }
    }
    return true;
}

const heaven::testing::Register g_register_reversed_duplicate_edge{
    &unit_reversed_duplicate_tessellation_edge};

bool unit_truncated_tessellation_cache_header() {
    // Use the normal saver so every malformed binary retains valid metadata
    // and reaches header parsing instead of taking the metadata-missing exit.
    const TempFile anchor({});
    struct CacheDirectory {
        std::filesystem::path path;
        ~CacheDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };
    const CacheDirectory cache{anchor.path.string() + ".cache"};
    const auto original = square();
    const auto save = [&] {
        heaven::save_tessellation_cache(original, "test.mesh", "square", "",
                                        1.0f, 1.0f, 1, cache.path.string());
    };
    const auto load = [&](heaven::Surface& surface) {
        return heaven::load_tessellation_cache(surface, "test.mesh", "square", "",
                                               1.0f, 1.0f, 1, cache.path.string());
    };
    save();
    heaven::Surface loaded;
    if (!load(loaded) || !same_geometry(loaded, original)) return false;

    std::filesystem::path binary;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(cache.path)) {
        if (entry.path().extension() == ".bin") binary = entry.path();
    }
    if (binary.empty()) return false;
    auto metadata = binary;
    metadata.replace_extension(".json");
    if (!std::filesystem::exists(metadata)) return false;

    std::array<char, 8> header{};
    {
        std::ifstream input(binary, std::ios::binary);
        if (!input.read(header.data(), static_cast<std::streamsize>(header.size()))) return false;
    }
    for (std::streamsize length = 0; length < 8; ++length) {
        std::ofstream output(binary, std::ios::binary | std::ios::trunc);
        output.write(header.data(), length);
        output.close();
        if (!output) return false;
        auto unchanged = original;
        if (load(unchanged) || !same_geometry(unchanged, original)) return false;
    }
    save();
    return load(loaded) && same_geometry(loaded, original);
}

const heaven::testing::Register g_register_truncated_cache_header{
    &unit_truncated_tessellation_cache_header};

} // namespace
