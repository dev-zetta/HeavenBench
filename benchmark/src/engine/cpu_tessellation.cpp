#include "benchmark/microprofiler.h"
/* HeavenBench — CPU tessellation topology generation.*/
#include "engine/cpu_tessellation.h"
#include "engine/loader_validation.h"
#include "engine/dds_block_codec.h"
#include "dds_loader_cpu.h"
#include "json.hpp"

#include <cstddef>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace heaven {
namespace {

// ── Height map sampling ─────────────────────────────────────────

// Simple BC1 block decompression (RGB, take R channel)
void decompress_bc1_block(const uint8_t* block, uint8_t out_r[16]) {
    uint16_t c0 = block[0] | (block[1] << 8);
    uint16_t c1 = block[2] | (block[3] << 8);
    uint8_t r[4];
    r[0] = ((c0 >> 11) & 0x1F) * 255 / 31;
    r[1] = ((c1 >> 11) & 0x1F) * 255 / 31;
    if (c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2;
        r[3] = 0;
    }
    uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (block[7] << 24);
    for (int i = 0; i < 16; i++)
        out_r[i] = r[(indices >> (i * 2)) & 3];
}

// Decompress a BC1 texture to a flat  buffer (red channel only). `src_size`
// bounds the backing buffer; per block we verify [block_off, block_off + 8)
// fits before reading (BC1 blocks are 8 bytes). On an out-of-range block we
// stop, leave the already-decoded output unchanged, and set `ok = false`.
std::vector<uint8_t> decompress_bc1_r(const uint8_t* src, std::size_t src_size,
                                             uint32_t w, uint32_t h, bool& ok) {
    std::vector<uint8_t> out(static_cast<std::size_t>(w) * h, 0);
    ok = true;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            std::uint64_t block_off =
                (static_cast<std::uint64_t>(by) * bw + bx) * 8;
            if (!loadval::fits(block_off, 8, src_size)) {
                ok = false;
                return out;
            }
            uint8_t block_r[16];
            decompress_bc1_block(src + block_off, block_r);
            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    uint32_t x = bx * 4 + px, y = by * 4 + py;
                    if (x < w && y < h)
                        out[y * w + x] = block_r[py * 4 + px];
                }
            }
        }
    }
    return out;
}

// Load a single-channel density map and decompress its available mip levels.
struct HeightMip {
    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
};
struct HeightChain {
    std::vector<HeightMip> mips;
    bool empty() const { return mips.empty(); }
};

bool load_heightmap_chain(const std::string& path, HeightChain& chain,
                                 AssetFileSystemRef assets = std::nullopt) {
    auto dds = load_dds_cpu(path, assets);
    if (!dds || dds.mip_data.empty()) return false;

    chain.mips.clear();
    chain.mips.reserve(dds.mip_data.size());
    for (size_t mi = 0; mi < dds.mip_data.size(); ++mi) {
        uint32_t w = std::max<uint32_t>(1, dds.width  >> mi);
        uint32_t h = std::max<uint32_t>(1, dds.height >> mi);
        const std::span<const std::uint8_t> src = dds.mip_data[mi].bytes();
        HeightMip out;
        out.w = w; out.h = h;
        switch (dds.format) {
            case DDSFormatCPU::BC4: {
                auto decoded = decode_bc4_channel(src, w, h);
                if (!decoded) continue;
                out.pixels = std::move(decoded->pixels);
                break;
            }
            case DDSFormatCPU::BC1: {
                bool ok = false;
                out.pixels = decompress_bc1_r(src.data(), src.size(), w, h, ok);
                if (!ok) continue; // OOB block -> skip this mip
                break;
            }
            case DDSFormatCPU::BC5: {
                auto decoded = decode_bc4_channel(src, w, h, 16, 0);
                if (!decoded) continue;
                out.pixels = std::move(decoded->pixels);
                break;
            }
            case DDSFormatCPU::RGBA8: {
                const uint64_t pixel_count = static_cast<uint64_t>(w) * h;
                if (!loadval::fits(0, pixel_count * 4u, src.size())) continue;
                out.pixels.resize(static_cast<std::size_t>(pixel_count));
                for (std::size_t i = 0; i < out.pixels.size(); ++i)
                    out.pixels[i] = src[i * 4]; // R channel
                break;
            }
            default:
                continue;
        }
        if (!out.pixels.empty()) chain.mips.push_back(std::move(out));
    }
    return !chain.mips.empty();
}

// Backward-compatible wrapper: load only mip 0 (used by older callers).
bool load_heightmap(const std::string& path, std::vector<uint8_t>& pixels,
                           uint32_t& w, uint32_t& h,
                           AssetFileSystemRef assets = std::nullopt) {
    HeightChain chain;
    if (!load_heightmap_chain(path, chain, assets)) return false;
    pixels = std::move(chain.mips[0].pixels);
    w = chain.mips[0].w;
    h = chain.mips[0].h;
    return true;
}

float sample_height(const std::vector<uint8_t>& pixels, uint32_t w, uint32_t h,
                           float u, float v) {
    if (pixels.empty() || w == 0 || h == 0) return 0.0f;
    // Wrap UVs into [0, 1].
    u = u - std::floor(u);
    v = v - std::floor(v);
    if (u < 0) u += 1.0f;
    if (v < 0) v += 1.0f;

    // Bilinear sampling: nearby vertices sample a smoothly-varying height
    // instead of stepping between adjacent texels. Without this, a 64×
    // subdivision can put many vertices at slightly different UVs that all
    // pick different texel values, creating thin near-vertical "wall"
    // triangles between them that show as black backfaces.
    float fx = u * static_cast<float>(w) - 0.5f;
    float fy = v * static_cast<float>(h) - 0.5f;
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    float tx = fx - static_cast<float>(x0);
    float ty = fy - static_cast<float>(y0);

    auto wrap = [](int i, int n) -> int {
        i %= n;
        if (i < 0) i += n;
        return i;
    };
    int xi0 = wrap(x0,   static_cast<int>(w));
    int xi1 = wrap(x0+1, static_cast<int>(w));
    int yi0 = wrap(y0,   static_cast<int>(h));
    int yi1 = wrap(y0+1, static_cast<int>(h));

    float h00 = static_cast<float>(pixels[yi0 * w + xi0]) / 255.0f;
    float h10 = static_cast<float>(pixels[yi0 * w + xi1]) / 255.0f;
    float h01 = static_cast<float>(pixels[yi1 * w + xi0]) / 255.0f;
    float h11 = static_cast<float>(pixels[yi1 * w + xi1]) / 255.0f;

    float h0 = h00 + (h10 - h00) * tx;
    float h1 = h01 + (h11 - h01) * tx;
    return h0 + (h1 - h0) * ty;
}

// ── Subdivision ─────────────────────────────────────────────────

Vertex midpoint_vertex(const Vertex& a, const Vertex& b) {
    constexpr float t = 0.5f;
    Vertex r;
    // Separate tangent-seam copies can traverse the same source edge in
    // opposite directions. A subtraction-based lerp rounds differently and
    // prevents exact position/UV matches from sharing their height footprint.
    r.px = std::midpoint(a.px, b.px);
    r.py = std::midpoint(a.py, b.py);
    r.pz = std::midpoint(a.pz, b.pz);
    const glm::vec3 raw_a{a.nx * a.displacement_normal_scale,
                          a.ny * a.displacement_normal_scale,
                          a.nz * a.displacement_normal_scale};
    const glm::vec3 raw_b{b.nx * b.displacement_normal_scale,
                          b.ny * b.displacement_normal_scale,
                          b.nz * b.displacement_normal_scale};
    const glm::vec3 raw = glm::mix(raw_a, raw_b, t);
    r.displacement_normal_scale = glm::length(raw);
    const glm::vec3 normal = r.displacement_normal_scale > 1e-6f
        ? raw / r.displacement_normal_scale : glm::vec3{0.0f, 0.0f, 1.0f};
    r.nx = normal.x; r.ny = normal.y; r.nz = normal.z;
    r.u  = std::midpoint(a.u, b.u);
    r.v  = std::midpoint(a.v, b.v);
    r.u1 = std::midpoint(a.u1, b.u1);
    r.v1 = std::midpoint(a.v1, b.v1);
    r.tx = a.tx + (b.tx - a.tx) * t;
    r.ty = a.ty + (b.ty - a.ty) * t;
    r.tz = a.tz + (b.tz - a.tz) * t;
    r.tw = a.tw; // bitangent sign doesn't interpolate
    return r;
}

} // namespace

// ── Main tessellation function ──────────────────────────────────

bool cpu_tessellate_surface(Surface& surface,
                            const std::string& density_path,
                            const TessellationTopologyParameters& topology,
                            AssetFileSystemRef assets) {
    profile::Scope profile_scope("CPU tessellate surface", "Tessellation");
    if (surface.vertices.empty() || surface.indices.empty()) return false;
    const auto& bt = topology.base_transform;
    const float density_factor = topology.density_factor;
    const float edge_scale = topology.edge_scale;
    const int subdivisions = topology.subdivisions;

    // The `_t` texture controls topology density; `_h` displacement is applied
    // later in the Filament vertex shader so it can follow camera distance.
    std::vector<uint8_t> density_pixels;
    uint32_t density_w = 0, density_h = 0;
    if (density_path.empty()) {
        // Unigine's tessellation base material supplies a white default for
        // s_texture_16. Most ruins materials (including pavement and stairs)
        // author only `_h`, so absence of an explicit `_t` means uniform
        // maximum density rather than "not tessellated".
        density_pixels = {255};
        density_w = density_h = 1;
    } else if (!load_heightmap(density_path, density_pixels,
                               density_w, density_h, assets)) {
        std::fprintf(stderr, "    CPU tess: failed to load density map %s\n",
                     density_path.c_str());
        return false;
    }

    // Each level shares its midpoint for every indexed edge. The green split
    // cases below ensure a triangle whose neighbour requested an edge split
    // follows it, so no T-junction is introduced within the surface.
    std::vector<Vertex> verts = surface.vertices;
    std::vector<uint32_t> indices = surface.indices;

    std::vector<std::pair<glm::vec3, glm::vec3>> boundary_segments;
    if (topology.boundary_fade > 0.0f) {
        std::unordered_map<uint64_t, uint32_t> edge_counts;
        for (std::size_t index = 0; index + 2 < indices.size(); index += 3) {
            for (uint32_t edge = 0; edge < 3; ++edge) {
                const uint32_t a = indices[index + edge];
                const uint32_t b = indices[index + (edge + 1) % 3];
                const uint32_t low = std::min(a, b);
                const uint32_t high = std::max(a, b);
                ++edge_counts[(static_cast<uint64_t>(low) << 32u) | high];
            }
        }
        for (const auto& [key, count] : edge_counts) {
            if (count != 1) continue;
            const Vertex& a = verts[static_cast<uint32_t>(key >> 32u)];
            const Vertex& b = verts[static_cast<uint32_t>(key)];
            boundary_segments.emplace_back(
                glm::vec3{a.px, a.py, a.pz},
                glm::vec3{b.px, b.py, b.pz});
        }
    }

    for (int level = 0; level < subdivisions; level++) {
        std::vector<Vertex> new_verts;
        std::vector<uint32_t> new_indices;
        new_verts = verts;
        new_verts.reserve(verts.size() * 2);
        new_indices.reserve(indices.size() * 4);

        struct IndexEdge {
            uint32_t a, b;
            bool operator==(const IndexEdge&) const = default;
        };
        struct IndexEdgeHash {
            size_t operator()(const IndexEdge& e) const noexcept {
                return (static_cast<size_t>(e.a) << 32u) ^ e.b;
            }
        };
        auto index_edge = [](uint32_t a, uint32_t b) {
            return a < b ? IndexEdge{a, b} : IndexEdge{b, a};
        };
        std::unordered_map<IndexEdge, uint32_t, IndexEdgeHash> midpoint_cache;

        auto wants_split = [&](uint32_t ia, uint32_t ib) {
            const Vertex& a = verts[ia];
            const Vertex& b = verts[ib];
            const float u = (a.u + b.u) * 0.5f * bt[0] + bt[2];
            const float v = (a.v + b.v) * 0.5f * bt[1] + bt[3];
            const float density = sample_height(
                density_pixels, density_w, density_h, u, v);
            const glm::vec3 pa{a.px, a.py, a.pz};
            const glm::vec3 pb{b.px, b.py, b.pz};
            const float level_segments = static_cast<float>(1u << level);
            const float length_factor = std::clamp(
                glm::length(pb - pa) * level_segments *
                std::max(edge_scale, 0.0f), 0.0f, 1.0f);
            // vertex_base.shader stores half the density factor at each
            // endpoint; control_base.shader sums those halves, producing the
            // authored factor for an edge with equal endpoint density.
            // The hardware tessellator chooses the segment count once from
            // each ORIGINAL patch edge. At recursive level N, the current
            // edge is 1/2^N as long, so recover the original-edge length
            // above and compare its factor with 2^N. Reapplying the length
            // attenuation to the child itself makes the desired factor shrink
            // twice and under-tessellates large pavement and stair patches.
            const float target = std::clamp(
                density * std::max(density_factor, 0.0f) * length_factor,
                0.0f, static_cast<float>(1u << subdivisions));
            return target > level_segments;
        };

        auto midpoint = [&](uint32_t ia, uint32_t ib) {
            const IndexEdge edge = index_edge(ia, ib);
            if (auto it = midpoint_cache.find(edge); it != midpoint_cache.end())
                return it->second;
            const uint32_t out = static_cast<uint32_t>(new_verts.size());
            new_verts.push_back(midpoint_vertex(verts[ia], verts[ib]));
            midpoint_cache.emplace(edge, out);
            return out;
        };

        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            new_indices.push_back(a); new_indices.push_back(b); new_indices.push_back(c);
        };

        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            uint32_t i0 = indices[i], i1 = indices[i+1], i2 = indices[i+2];
            const bool s01 = wants_split(i0, i1);
            const bool s12 = wants_split(i1, i2);
            const bool s20 = wants_split(i2, i0);
            const unsigned mask = unsigned(s01) | (unsigned(s12) << 1u) |
                                  (unsigned(s20) << 2u);
            const uint32_t m01 = s01 ? midpoint(i0, i1) : 0;
            const uint32_t m12 = s12 ? midpoint(i1, i2) : 0;
            const uint32_t m20 = s20 ? midpoint(i2, i0) : 0;
            switch (mask) {
                case 0: tri(i0, i1, i2); break;
                case 1: tri(i0, m01, i2); tri(m01, i1, i2); break;
                case 2: tri(i1, m12, i0); tri(m12, i2, i0); break;
                case 4: tri(i2, m20, i1); tri(m20, i0, i1); break;
                case 3: tri(i1, m12, m01); tri(m01, m12, i2); tri(i0, m01, i2); break;
                case 5: tri(i0, m01, m20); tri(m01, i1, i2); tri(m01, i2, m20); break;
                case 6: tri(i2, m20, m12); tri(m12, m20, i0); tri(i1, m12, i0); break;
                case 7: tri(i0, m01, m20); tri(m01, i1, m12);
                        tri(m20, m12, i2); tri(m01, m12, m20); break;
            }
        }
        verts = std::move(new_verts);
        indices = std::move(new_indices);
    }

    if (!boundary_segments.empty()) {
        for (Vertex& vertex : verts) {
            const glm::vec3 point{vertex.px, vertex.py, vertex.pz};
            float minimum_distance = std::numeric_limits<float>::max();
            for (const auto& [a, b] : boundary_segments) {
                const glm::vec3 edge = b - a;
                const float length_squared = glm::dot(edge, edge);
                const float t = length_squared > 1e-12f
                    ? std::clamp(glm::dot(point - a, edge) / length_squared,
                                 0.0f, 1.0f)
                    : 0.0f;
                minimum_distance = std::min(
                    minimum_distance, glm::length(point - (a + edge * t)));
            }
            const float normalized = std::clamp(
                minimum_distance / topology.boundary_fade, 0.0f, 1.0f);
            const float weight = normalized * normalized *
                                 (3.0f - 2.0f * normalized);
            vertex.displacement_normal_scale *= weight;
        }
    }

    // Keep positions neutral. The original signed `_h` displacement is applied
    // per frame in the material vertex stage.
    surface.vertices = std::move(verts);
    surface.indices = std::move(indices);
    return true;
}

// ── Disk cache ──────────────────────────────────────────────────

namespace {

// CACHE_VERSION is bumped whenever the tessellation algorithm changes (e.g.
// new bias formula, sampling change, or normal-welding logic). The cache is
// laid out as <cache_dir>/v<N>/<hash>.bin so different versions never share
// files; a version bump produces a fresh directory and the previous one is
// pruned by prune_stale_tessellation_cache().
constexpr uint32_t CACHE_MAGIC = 0x54455353; // "TESS"
// Resolve the per-version cache directory. Callers pass the *base* cache
// directory (e.g. "<data>/cache/tessellation") and we append "v<N>".
std::string version_dir(const std::string& cache_dir) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "/v%u", kTessellationCacheVersion);
    return cache_dir + buf;
}

uint64_t file_fingerprint(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    uint64_t hash = 1469598103934665603ull;
    char buffer[64 * 1024];
    while (f) {
        f.read(buffer, sizeof(buffer));
        const auto count = f.gcount();
        for (std::streamsize i = 0; i < count; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

std::string cache_filename(const std::string& mesh_path,
                                  const std::string& surface_name,
                                  const std::string& density_path,
                                  float density_factor, float edge_scale,
                                  int subdivisions,
                                  const std::string& cache_dir,
                                  const float base_transform[4]) {
    // Hash the parameters that influence the output. The version is encoded
    // by the parent directory, so we don't need to fold it into the hash.
    std::hash<std::string> hasher;
    size_t h = hasher(mesh_path);
    h ^= hasher(surface_name) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= hasher(density_path) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint64_t>{}(file_fingerprint(mesh_path)) +
         0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint64_t>{}(file_fingerprint(density_path)) +
         0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<float>{}(density_factor) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<float>{}(edge_scale) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<int>{}(subdivisions) + 0x9e3779b9 + (h << 6) + (h >> 2);
    // base_transform is part of the identity since it changes which
    // texels are sampled — bake it into the hash so two surfaces
    // with the same heightmap but different transforms cache separately.
    if (base_transform) {
        for (int k = 0; k < 4; ++k)
            h ^= std::hash<float>{}(base_transform[k]) + 0x9e3779b9 + (h << 6) + (h >> 2);
    }

    char buf[64];
    std::snprintf(buf, sizeof(buf), "/tess_%016zx.bin", h);
    return version_dir(cache_dir) + buf;
}

std::filesystem::path cache_metadata_filename(
        const std::filesystem::path& binary_path) {
    auto metadata_path = binary_path;
    metadata_path.replace_extension(".json");
    return metadata_path;
}

std::string fingerprint_string(uint64_t fingerprint) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(fingerprint));
    return text;
}

} // namespace

bool has_tessellation_cache(const std::string& mesh_path,
                            const std::string& surface_name,
                            const std::string& density_path,
                            float density_factor, float edge_scale,
                            int subdivisions,
                            const std::string& cache_dir,
                            const float base_transform[4]) {
    auto path = cache_filename(mesh_path, surface_name, density_path,
                               density_factor, edge_scale, subdivisions, cache_dir,
                               base_transform);
    return std::filesystem::exists(path) &&
           std::filesystem::exists(cache_metadata_filename(path));
}

void save_tessellation_cache(const Surface& surface,
                             const std::string& mesh_path,
                             const std::string& surface_name,
                             const std::string& density_path,
                             float density_factor, float edge_scale,
                             int subdivisions,
                             const std::string& cache_dir,
                             const float base_transform[4],
                             uint64_t source_vertex_count,
                             uint64_t source_index_count) {
    profile::Scope profile_scope("Save tessellation cache", "Cache");
    if (surface.vertices.empty() || surface.indices.empty() ||
        surface.vertices.size() > std::numeric_limits<uint32_t>::max() ||
        surface.indices.size() > std::numeric_limits<uint32_t>::max() ||
        surface.vertices.size() > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) / sizeof(Vertex) ||
        surface.indices.size() > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()) / sizeof(uint32_t)) {
        return;
    }

    // Ensure the per-version directory exists. cache_filename() returns
    // "<cache_dir>/v<N>/<hash>.bin", so creating the directory of that
    // file gets both the base dir and the version subdir at once.
    std::filesystem::create_directories(version_dir(cache_dir));
    auto path = cache_filename(mesh_path, surface_name, density_path,
                               density_factor, edge_scale, subdivisions, cache_dir,
                               base_transform);

    const std::filesystem::path binary_path(path);
    const std::filesystem::path metadata_path =
        cache_metadata_filename(binary_path);
    const std::filesystem::path binary_temp = binary_path.string() + ".tmp";
    const std::filesystem::path metadata_temp = metadata_path.string() + ".tmp";

    std::ofstream f(binary_temp, std::ios::binary | std::ios::trunc);
    if (!f) return;

    uint32_t vert_count = static_cast<uint32_t>(surface.vertices.size());
    uint32_t idx_count = static_cast<uint32_t>(surface.indices.size());

    f.write(reinterpret_cast<const char*>(&CACHE_MAGIC), 4);
    f.write(reinterpret_cast<const char*>(&kTessellationCacheVersion), 4);
    f.write(reinterpret_cast<const char*>(&vert_count), 4);
    f.write(reinterpret_cast<const char*>(&idx_count), 4);
    f.write(reinterpret_cast<const char*>(surface.vertices.data()),
            static_cast<std::streamsize>(surface.vertices.size() * sizeof(Vertex)));
    f.write(reinterpret_cast<const char*>(surface.indices.data()),
            static_cast<std::streamsize>(surface.indices.size() * sizeof(uint32_t)));
    f.close();
    if (!f) {
        std::error_code ec;
        std::filesystem::remove(binary_temp, ec);
        return;
    }

    const float identity_transform[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    const float* transform = base_transform
        ? base_transform : identity_transform;
    const uint64_t payload_bytes =
        16u + uint64_t{vert_count} * sizeof(Vertex) +
        uint64_t{idx_count} * sizeof(uint32_t);
    nlohmann::json metadata = {
        {"schema_version", 1},
        {"cache_version", kTessellationCacheVersion},
        {"binary", binary_path.filename().string()},
        {"mesh_source", mesh_path},
        {"surface", surface_name},
        {"density_source", density_path},
        {"fingerprints", {
            {"mesh_fnv1a64", fingerprint_string(file_fingerprint(mesh_path))},
            {"density_fnv1a64", fingerprint_string(file_fingerprint(density_path))}
        }},
        {"topology", {
            {"density_factor", density_factor},
            {"edge_scale", edge_scale},
            {"subdivisions", subdivisions},
            {"base_transform", {
                transform[0], transform[1], transform[2], transform[3]
            }}
        }},
        {"source", {
            {"vertices", source_vertex_count},
            {"indices", source_index_count},
            {"triangles", source_index_count / 3u}
        }},
        {"output", {
            {"vertices", vert_count},
            {"indices", idx_count},
            {"triangles", idx_count / 3u}
        }},
        {"payload_bytes", payload_bytes}
    };

    std::ofstream metadata_stream(metadata_temp, std::ios::trunc);
    if (!metadata_stream) {
        std::error_code ec;
        std::filesystem::remove(binary_temp, ec);
        return;
    }
    metadata_stream << metadata.dump(2) << '\n';
    metadata_stream.close();
    if (!metadata_stream) {
        std::error_code ec;
        std::filesystem::remove(binary_temp, ec);
        std::filesystem::remove(metadata_temp, ec);
        return;
    }

    std::error_code ec;
    std::filesystem::rename(binary_temp, binary_path, ec);
    if (ec) {
        std::filesystem::remove(binary_temp, ec);
        std::filesystem::remove(metadata_temp, ec);
        return;
    }
    std::filesystem::rename(metadata_temp, metadata_path, ec);
    if (ec) {
        // A cache entry is the binary and its inspectable metadata together.
        // Do not leave a binary-only hit if metadata publication fails.
        std::filesystem::remove(binary_path, ec);
        std::filesystem::remove(metadata_temp, ec);
    }
}

bool load_tessellation_cache(Surface& surface,
                             const std::string& mesh_path,
                             const std::string& surface_name,
                             const std::string& density_path,
                             float density_factor, float edge_scale,
                             int subdivisions,
                             const std::string& cache_dir,
                             const float base_transform[4]) {
    profile::Scope profile_scope("Load tessellation cache", "Cache");
    auto path = cache_filename(mesh_path, surface_name, density_path,
                               density_factor, edge_scale, subdivisions, cache_dir,
                               base_transform);
    if (!std::filesystem::exists(cache_metadata_filename(path))) return false;

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    // Capture the on-disk size so the element counts can be validated before
    // any resize/read. A stale or corrupt cache whose counts exceed the
    // remaining bytes is rejected (return false), which the caller treats as a
    // cache miss and recomputes the tessellation from the source geometry — no
    // element is read from a bad cache.
    f.seekg(0, std::ios::end);
    const std::streamoff file_size = f.tellg();
    f.seekg(0, std::ios::beg);
    if (file_size < 0) return false;

    uint32_t magic, version, vert_count, idx_count;
    f.read(reinterpret_cast<char*>(&magic), 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    if (!f.good() || magic != CACHE_MAGIC || version != kTessellationCacheVersion) return false;

    f.read(reinterpret_cast<char*>(&vert_count), 4);
    f.read(reinterpret_cast<char*>(&idx_count), 4);
    if (!f.good()) return false;

    // The header is four uint32 fields (16 bytes); the element data follows.
    // Verify both arrays fit in the remaining bytes, computed in 64-bit via
    // loadval::fits so the byte-count products/sums cannot wrap.
    constexpr std::uint64_t kHeaderBytes = 16;
    const std::uint64_t body_size =
        std::cmp_greater_equal(file_size, kHeaderBytes)
            ? static_cast<std::uint64_t>(file_size) - kHeaderBytes
            : 0;
    const std::uint64_t vert_bytes =
        static_cast<std::uint64_t>(vert_count) * sizeof(Vertex);
    const std::uint64_t idx_bytes =
        static_cast<std::uint64_t>(idx_count) * sizeof(uint32_t);
    if (vert_count == 0 || idx_count == 0 || idx_count % 3 != 0 ||
        vert_count > static_cast<uint32_t>(loadval::kMaxElemCount) ||
        idx_count > static_cast<uint32_t>(loadval::kMaxElemCount) ||
        !loadval::fits(0, vert_bytes, body_size) ||
        !loadval::fits(vert_bytes, idx_bytes, body_size) ||
        vert_bytes + idx_bytes != body_size) {
        return false;
    }

    std::vector<Vertex> vertices(vert_count);
    std::vector<uint32_t> indices(idx_count);
    f.read(reinterpret_cast<char*>(vertices.data()),
           static_cast<std::streamsize>(vert_bytes));
    f.read(reinterpret_cast<char*>(indices.data()),
           static_cast<std::streamsize>(idx_bytes));
    if (!f.good() ||
        std::any_of(indices.begin(), indices.end(),
                    [vert_count](uint32_t index) { return index >= vert_count; })) {
        return false;
    }
    surface.vertices = std::move(vertices);
    surface.indices = std::move(indices);
    return true;
}
int prune_stale_tessellation_cache(const std::string& cache_dir) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(cache_dir)) return 0;

    // Build the name of the current version subdirectory so we know what
    // to keep: "v<N>".
    char keep_buf[16];
    std::snprintf(keep_buf, sizeof(keep_buf), "v%u", kTessellationCacheVersion);
    const std::string keep = keep_buf;

    int deleted = 0;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(cache_dir, ec)) {
        const auto name = entry.path().filename().string();

        if (entry.is_directory()) {
            // Drop sibling version directories that aren't the current one.
            // Any "v<digits>" directory other than the current is a stale
            bool looks_versioned =
                name.size() > 1 && name[0] == 'v' &&
                std::all_of(name.begin() + 1, name.end(),
                            [](char c){ return c >= '0' && c <= '9'; });
            if (!looks_versioned) continue;
            // Keep the current version directory!
            if (name == keep) continue;
            std::error_code rmec;
            std::uintmax_t n = fs::remove_all(entry.path(), rmec);
            if (!rmec) deleted += static_cast<int>(n);
        } else if (entry.is_regular_file()) {
            // Loose files left over from older layouts (no version subdir).
            // They never get loaded by the current code, so sweep them.
            if (name.starts_with("tess_") && name.ends_with(".bin")) {
                std::error_code rmec;
                fs::remove(entry.path(), rmec);
                if (!rmec) ++deleted;
            }
        }
    }
    return deleted;
}

// ── Skinned-mesh tessellation ───────────────────────────────────

// Same idea as cpu_tessellate_surface but for SkinnedSurface. We
// take each triangle, subdivide it into n×n sub-triangles where
// n = 2^subdivisions, and write the new vertex set back into the
// surface. Each new vertex carries:
//   - position (will be re-computed every frame by the skinning
//     pipeline; what we set here is the bind-pose result)
//   - normal   (bind-pose interpolated)
//   - UV       (linearly interpolated)
//   - bone_indices / bone_weights, blended barycentrically from
//     the three triangle corners. This is what makes the new
//     vertices follow the right bones at runtime.
// Positions remain neutral; the shared vertex-stage formula displaces both
// static and skinned geometry after skinning.

// No boundary/seam handling: skinned objects in this scene
// (gears, weathercock, mill wings, propeller) are standalone
// items whose edges don't touch other surfaces, so the seam
// preservation logic from the static path isn't needed.

// Bone-weight interpolation between corners can produce
// >4 unique bone indices per new vertex when corners use disjoint
// sets. We collapse the result to top-4 weights (sorted), which
// is what our per-vertex skinning loop expects.
bool cpu_tessellate_skinned_surface(SkinnedSurface& surface,
                                    const std::string& density_path,
                                    const TessellationTopologyParameters& topology,
                                    AssetFileSystemRef assets) {
    profile::Scope profile_scope("CPU tessellate skinned surface", "Tessellation");
    if (surface.vertices.empty() || surface.indices.empty()) return false;
    const float density_factor = topology.density_factor;
    const float edge_scale = topology.edge_scale;
    const int subdivisions = topology.subdivisions;
    if (subdivisions < 1) return true;
    const auto& bt = topology.base_transform;

    HeightChain chain;
    if (density_path.empty()) {
        HeightMip white;
        white.pixels = {255};
        white.w = white.h = 1;
        chain.mips.push_back(std::move(white));
    } else if (!load_heightmap_chain(density_path, chain, assets)) {
        std::fprintf(stderr, "    CPU tess (skinned): failed to load density %s\n",
                     density_path.c_str());
        return false;
    }
    const auto& mip0 = chain.mips[0];

    float maximum_target = 1.0f;
    for (const auto& v : surface.vertices) {
        const float tu = v.u * bt[0] + bt[2];
        const float tv = v.v * bt[1] + bt[3];
        maximum_target = std::max(maximum_target,
            sample_height(mip0.pixels, mip0.w, mip0.h, tu, tv) *
            std::max(density_factor, 0.0f));
    }
    float maximum_edge_factor = 0.0f;
    for (size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
        for (int edge = 0; edge < 3; ++edge) {
            const auto& a = surface.vertices[surface.indices[i + edge]];
            const auto& b = surface.vertices[surface.indices[i + (edge + 1) % 3]];
            maximum_edge_factor = std::max(maximum_edge_factor,
                std::clamp(glm::length(glm::vec3{b.px - a.px, b.py - a.py,
                                                 b.pz - a.pz}) *
                           std::max(edge_scale, 0.0f), 0.0f, 1.0f));
        }
    }
    maximum_target *= maximum_edge_factor;
    int adaptive_levels = 0;
    while (adaptive_levels < subdivisions &&
           float(1u << adaptive_levels) < maximum_target) {
        ++adaptive_levels;
    }
    const int subs = 1 << adaptive_levels;
    const int verts_per_side = subs + 1;       // (n+1)*(n+2)/2 verts per tri

    // Original data captured up-front so we can grow the buffers safely.
    std::vector<SkinnedVertex> orig_verts = surface.vertices;
    std::vector<uint32_t>      orig_idx   = surface.indices;

    surface.vertices.clear();
    surface.indices.clear();
    surface.vertices.reserve(orig_idx.size() / 3 * verts_per_side
                                              * (verts_per_side + 1) / 2);
    surface.indices.reserve(orig_idx.size() / 3 * subs * subs * 3);

    auto blend_weights = [](const SkinnedVertex& a,
                            const SkinnedVertex& b,
                            const SkinnedVertex& c,
                            float wa, float wb, float wc,
                            SkinnedVertex& out) {
        // Accumulate weight per (bone_index) — up to 12 entries
        // (4 from each corner). Collapse to top 4 by weight.
        struct BW { int bone; float weight; };
        BW slots[12];
        int n = 0;
        auto add = [&](const SkinnedVertex& sv, float corner_w) {
            for (int k = 0; k < 4; ++k) {
                float w = sv.bone_weights[k] * corner_w;
                if (w <= 0.0f) continue;
                int bi = sv.bone_indices[k];
                bool merged = false;
                for (int s = 0; s < n; ++s) {
                    if (slots[s].bone == bi) {
                        slots[s].weight += w;
                        merged = true;
                        break;
                    }
                }
                if (!merged && n < 12) {
                    slots[n++] = {bi, w};
                }
            }
        };
        add(a, wa); add(b, wb); add(c, wc);
        // Sort descending by weight.
        std::sort(slots, slots + n,
                  [](const BW& x, const BW& y) { return x.weight > y.weight; });
        // Renormalise the top 4 so they sum to 1.
        int keep = std::min(n, 4);
        float sum = 0.0f;
        for (int k = 0; k < keep; ++k) sum += slots[k].weight;
        for (int k = 0; k < 4; ++k) {
            if (k < keep) {
                out.bone_indices[k] = slots[k].bone;
                out.bone_weights[k] = (sum > 1e-6f) ? slots[k].weight / sum : 0.0f;
            } else {
                out.bone_indices[k] = 0;
                out.bone_weights[k] = 0.0f;
            }
        }
    };

    // Subdivide each original triangle. We use barycentric coords
    // (i, j, k) with i + j + k = subs and (i, j, k) >= 0 to enumerate
    // sub-vertices, then connect them as a triangle strip grid.
    for (size_t t = 0; t + 2 < orig_idx.size(); t += 3) {
        const SkinnedVertex& A = orig_verts[orig_idx[t]];
        const SkinnedVertex& B = orig_verts[orig_idx[t + 1]];
        const SkinnedVertex& C = orig_verts[orig_idx[t + 2]];

        uint32_t base = static_cast<uint32_t>(surface.vertices.size());

        // Build the (subs+1) row vertex grid in row-major order (i, j) with
        // k = subs - i - j; A is at (subs, 0, 0), B at (0, subs, 0),
        // C at (0, 0, subs). For convenience we lay out using (j, k) with
        // i = subs - j - k.
        for (int j = 0; j <= subs; ++j) {
            for (int k = 0; k <= subs - j; ++k) {
                int i = subs - j - k;
                float bA = static_cast<float>(i) / static_cast<float>(subs);
                float bB = static_cast<float>(j) / static_cast<float>(subs);
                float bC = static_cast<float>(k) / static_cast<float>(subs);

                SkinnedVertex v{};

                // Position: barycentric blend (bind pose).
                v.px = bA * A.px + bB * B.px + bC * C.px;
                v.py = bA * A.py + bB * B.py + bC * C.py;
                v.pz = bA * A.pz + bB * B.pz + bC * C.pz;
                // Normal: blend then renormalise.
                float nx = bA * A.nx * A.displacement_normal_scale +
                           bB * B.nx * B.displacement_normal_scale +
                           bC * C.nx * C.displacement_normal_scale;
                float ny = bA * A.ny * A.displacement_normal_scale +
                           bB * B.ny * B.displacement_normal_scale +
                           bC * C.ny * C.displacement_normal_scale;
                float nz = bA * A.nz * A.displacement_normal_scale +
                           bB * B.nz * B.displacement_normal_scale +
                           bC * C.nz * C.displacement_normal_scale;
                float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
                v.displacement_normal_scale = nlen;
                if (nlen > 1e-6f) { nx /= nlen; ny /= nlen; nz /= nlen; }
                v.nx = nx; v.ny = ny; v.nz = nz;
                // UVs: linear blend.
                v.u = bA * A.u + bB * B.u + bC * C.u;
                v.v = bA * A.v + bB * B.v + bC * C.v;
                v.u1 = bA * A.u1 + bB * B.u1 + bC * C.u1;
                v.v1 = bA * A.v1 + bB * B.v1 + bC * C.v1;
                // Skinning weights: barycentric blend, top-4.
                blend_weights(A, B, C, bA, bB, bC, v);

                surface.vertices.push_back(v);
            }
        }

        // Helper: convert (j, k) to grid index within the (subs+1)
        // triangular array enumerated as (j=0..subs, k=0..subs-j).
        auto grid_index = [&](int j, int k) -> uint32_t {
            // Row j has (subs - j + 1) entries; cumulative offset
            // for row 0..j-1 is sum_{r=0..j-1}(subs - r + 1)
            //                 = j*(subs + 1) - j*(j-1)/2.
            uint32_t row_off =
                static_cast<uint32_t>(j) * static_cast<uint32_t>(subs + 1)
                - static_cast<uint32_t>(j) * static_cast<uint32_t>(j - 1) / 2u;
            return base + row_off + static_cast<uint32_t>(k);
        };

        // Stitch the grid into triangles. For each cell (j, k) where
        // j + k < subs, emit one upward triangle; if j + k < subs - 1,
        // also emit the downward companion triangle.
        for (int j = 0; j < subs; ++j) {
            for (int k = 0; k + j < subs; ++k) {
                uint32_t i00 = grid_index(j,     k);
                uint32_t i01 = grid_index(j,     k + 1);
                uint32_t i10 = grid_index(j + 1, k);
                surface.indices.push_back(i00);
                surface.indices.push_back(i10);
                surface.indices.push_back(i01);

                if (k + j + 1 < subs) {
                    uint32_t i11 = grid_index(j + 1, k + 1);
                    surface.indices.push_back(i01);
                    surface.indices.push_back(i10);
                    surface.indices.push_back(i11);
                }
            }
        }
    }
    return true;
}

} // namespace heaven
