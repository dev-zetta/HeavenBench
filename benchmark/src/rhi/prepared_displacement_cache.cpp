#include "benchmark/microprofiler.h"
/* HeavenBench — validated, atomic persistence of bounded CPU refinement. */
#include "rhi/prepared_displacement_cache.h"
#include "engine/sha256.h"
#include "engine/zip_archive.h"
#include "json.hpp"

#include <zip.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <unordered_map>

#ifndef HEAVEN_PREPARED_CACHE_BUILD_ID
#error "The prepared cache requires a build-bound operator/dependency revision"
#endif

namespace heaven {
namespace {
constexpr std::uint32_t kVersion = 1;
constexpr std::uint64_t kAbsentEdge = UINT64_MAX;

// Scalar fields use explicit little-endian bytes; padding, GLM alignment and
// native enum widths never enter either the key or the persisted payload.
template<class T> auto encoded(T value) {
    std::array<std::byte, sizeof(T)> bytes{};
    if constexpr (std::is_floating_point_v<T>) {
        using Bits = std::conditional_t<sizeof(T) == 8, std::uint64_t, std::uint32_t>;
        return encoded(std::bit_cast<Bits>(value));
    } else {
        static_assert(std::is_unsigned_v<T>);
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes[i] = std::byte((value >> (8 * i)) & 255u);
        return bytes;
    }
}
template<class T> T decoded(const std::array<std::byte, sizeof(T)>& bytes) {
    if constexpr (std::is_floating_point_v<T>) {
        using Bits = std::conditional_t<sizeof(T) == 8, std::uint64_t, std::uint32_t>;
        return std::bit_cast<T>(decoded<Bits>(bytes));
    } else {
        static_assert(std::is_unsigned_v<T>);
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value |= T(std::to_integer<unsigned>(bytes[i])) << (8 * i);
        return value;
    }
}
struct KeyDigest {
    Sha256 digest;
    template<class T> void value(T value) { digest.update(encoded(value)); }
    void text(std::string_view text) {
        value(std::uint64_t(text.size())); digest.update(std::as_bytes(std::span(text)));
    }
};
template<class Io, class V> void vector3(Io& io, V& v) {
    io.value(v.x); io.value(v.y); io.value(v.z);
}
template<class Io, class V> void vector2(Io& io, V& v) {
    io.value(v.x); io.value(v.y);
}

Result<std::string> cache_key(const PreparedDisplacementSurface& source,
        const DisplacementPreparationPolicy& policy) {
    const auto n = source.source_vertices.size();
    const auto& p = source.original_packed;
    if (!source.source_ready || !n || n > policy.maximum_source_vertices ||
            source.source_triangles.empty() || source.native_height.size() != n ||
            p.positions.size() != n || p.uv0.size() != n || p.uv1.size() != n ||
            p.tangent_frames.size() != n || p.displacement_custom.size() != n ||
            p.footprints.size() != n || p.unit_displacement_directions.size() != n ||
            !p.displacement_directions_valid || !source.displacement.enabled ||
            !std::isfinite(source.uniform_scale) || source.uniform_scale <= 0 ||
            !std::isfinite(source.minimum_homogeneous_w) || source.minimum_homogeneous_w <= 0 ||
            !std::isfinite(source.maximum_uniform_metric_error) ||
            source.maximum_uniform_metric_error > policy.uniform_metric_tolerance)
        return std::unexpected(Error{"Prepared cache requires a valid frozen native source"});
    KeyDigest io;
    io.value(kVersion); io.text(HEAVEN_PREPARED_CACHE_BUILD_ID);
    io.value(policy.maximum_source_vertices);
    io.value(policy.uniform_metric_tolerance); io.value(policy.sharing_scale_tolerance);
    const auto& g = policy.geometry;
    for (double v : {g.correspondence_bound, g.initial_node_bound, g.position_error,
            g.normal_error_degrees, g.neutral_feature_degrees, g.common_control_blend}) io.value(v);
    for (auto v : {g.initial_levels, g.protected_zero_rings, g.protected_full_rings,
            g.maximum_sample_depth, g.maximum_triangles}) io.value(v);
    io.value(g.maximum_work_bytes); io.value(g.maximum_patch_evaluations);
    io.value(source.uniform_scale); io.value(source.minimum_homogeneous_w);
    io.value(source.maximum_uniform_metric_error);
    io.value(std::uint32_t(source.displacement.enabled));
    for (float v : {source.displacement.amplitude, source.displacement.power,
            source.displacement.fade.threshold, source.displacement.fade.range}) io.value(v);
    io.value(p.deformation_signature); io.value(p.source_signature);
    io.value(std::uint32_t(p.tangent_packing)); io.value(std::uint32_t(p.displacement_directions_valid));
    io.value(std::uint64_t(n)); io.value(std::uint64_t(source.source_triangles.size()));
    for (std::size_t i = 0; i < n; ++i) {
        const auto& v = source.source_vertices[i];
        vector3(io, v.neutral_position); vector3(io, v.displacement_direction);
        vector2(io, v.uv0); vector2(io, v.uv1); vector2(io, v.footprint);
        io.value(v.displacement_magnitude); io.value(v.height); io.value(v.compatible_seam);
        const auto& h = source.native_height[i];
        if (h.id != i || !std::isfinite(h.height) || h.height < 0 || h.height > 1 ||
                !std::isfinite(h.mesh_lod) || h.mesh_lod < 1 || h.selected_lod != h.mesh_lod ||
                h.height != v.height)
            return std::unexpected(Error{"Prepared cache has invalid native height/LOD fields"});
        io.value(h.id); io.value(h.height); io.value(h.mesh_lod); io.value(h.selected_lod);
        vector3(io, p.positions[i]); vector2(io, p.uv0[i]); vector2(io, p.uv1[i]);
        for (auto q : {p.tangent_frames[i].x, p.tangent_frames[i].y,
                p.tangent_frames[i].z, p.tangent_frames[i].w}) io.value(std::uint32_t(std::uint16_t(q)));
        const auto& c = p.displacement_custom[i];
        for (float f : {c.x, c.y, c.z, c.w}) io.value(f);
        for (float f : p.footprints[i]) io.value(f);
        vector3(io, p.unit_displacement_directions[i]);
    }
    for (const auto& triangle : source.source_triangles) for (auto id : triangle) {
        if (id >= n) return std::unexpected(Error{"Prepared cache source index is invalid"});
        io.value(id);
    }
    return io.digest.finish();
}

struct Writer {
    static constexpr bool reading = false;
    std::ofstream stream;
    Sha256 digest;
    std::array<std::byte, 65536> buffer{};
    std::size_t used = 0;
    explicit Writer(const std::filesystem::path& file) : stream(file, std::ios::binary) {}
    void flush() {
        stream.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(used));
        digest.update(std::span(buffer).first(used));
        used = 0;
    }
    void write(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const auto size = std::min(bytes.size(), buffer.size() - used);
            std::memcpy(buffer.data() + used, bytes.data(), size);
            used += size; bytes = bytes.subspan(size);
            if (used == buffer.size()) flush();
        }
    }
    void finish() { flush(); stream.close(); }
    template<class T> void value(T v) {
        write(encoded(v));
    }
    void text(const std::string& v) {
        value(std::uint64_t(v.size()));
        write(std::as_bytes(std::span(v)));
    }
};
struct Reader {
    static constexpr bool reading = true;
    zip_file_t* file;
    std::uint64_t remaining, allocation_left;
    Sha256 digest;
    std::array<std::byte, 65536> buffer{};
    std::size_t begin = 0, end = 0;
    bool good = true;
    Reader(zip_file_t* f, std::uint64_t size, std::uint64_t allocation)
        : file(f), remaining(size), allocation_left(allocation) {}
    bool read(std::span<std::byte> bytes) {
        if (!good || bytes.size() > remaining) { good = false; return false; }
        while (!bytes.empty()) {
            if (begin == end) {
                begin = 0; end = 0;
                const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
                while (end < wanted) {
                    const auto got = zip_fread(file, buffer.data() + end, wanted - end);
                    if (got <= 0) { good = false; return false; }
                    end += static_cast<std::size_t>(got);
                }
                digest.update(std::span(buffer).first(end));
            }
            const auto size = std::min(bytes.size(), end - begin);
            std::memcpy(bytes.data(), buffer.data() + begin, size);
            begin += size; remaining -= size; bytes = bytes.subspan(size);
        }
        return true;
    }
    template<class T> void value(T& v) {
        std::array<std::byte, sizeof(T)> bytes{};
        if (read(bytes)) v = decoded<T>(bytes);
    }
    void text(std::string& v) {
        std::uint64_t size = 0; value(size);
        if (size > 4096 || size > remaining) { good = false; return; }
        v.resize(static_cast<std::size_t>(size));
        read(std::as_writable_bytes(std::span(v)));
    }
};
template<class Io, class E> void enumeration(Io& io, E& e) {
    auto value = std::uint32_t(e); io.value(value);
    if constexpr (Io::reading) e = static_cast<E>(value);
}
template<class Io, class V> void count(Io& io, V& v, std::uint64_t maximum,
        std::uint64_t record_bytes) {
    auto size = std::uint64_t(v.size()); io.value(size);
    if constexpr (Io::reading) {
        if (!io.good || size > maximum || size > io.remaining / record_bytes ||
                size > io.allocation_left / sizeof(typename V::value_type)) {
            io.good = false; return;
        }
        io.allocation_left -= size * sizeof(typename V::value_type);
        v.resize(static_cast<std::size_t>(size));
    }
}
template<class Io, class M> void metrics(Io& io, M& m) {
#define FIELD(name) io.value(m.name)
    FIELD(input_vertices); FIELD(input_triangles); FIELD(output_nodes); FIELD(output_triangles);
    FIELD(pinned_nodes); FIELD(native_regular_patches); FIELD(native_extraordinary_patches);
    FIELD(sampled_quality_failures); FIELD(patch_evaluations); FIELD(target_patch_evaluations);
    FIELD(sampling_patch_evaluations); FIELD(orientation_failures); FIELD(common_control_adjustments);
    FIELD(maximum_common_control_adjustment); FIELD(affine_components); FIELD(eligible_components);
    FIELD(accepted_components); FIELD(minimum_conforming_triangles); FIELD(rejected_components);
    FIELD(rejected_source_triangles); FIELD(rejected_orientation_failures); FIELD(rejected_bound_components);
    FIELD(rejected_numerical_components); FIELD(maximum_rejected_target_bound); FIELD(first_failed_source_parent);
    vector3(io, m.first_failed_source_barycentric);
    FIELD(maximum_target_bound); FIELD(maximum_sampled_movement); FIELD(maximum_sampled_position_error);
    FIELD(maximum_sampled_normal_error_degrees); FIELD(failing_area_fraction); FIELD(minimum_triangle_area);
#undef FIELD
}
template<class Io, class P> void payload(Io& io, P& p, const PreparedDisplacementSurface& source,
        const DisplacementPreparationPolicy& policy) {
    enumeration(io, p.disposition); io.text(p.reason);
    auto& g = p.geometry; enumeration(io, g.status); metrics(io, g.metrics);
    const auto maximum_triangles = policy.geometry.maximum_triangles;
    count(io, g.nodes, source.source_vertices.size() + 3ull * maximum_triangles, 100);
    for (auto& n : g.nodes) {
        vector3(io, n.old_raster_position); vector3(io, n.target_position); vector3(io, n.correction);
        io.value(n.source_parent); vector3(io, n.source_barycentric);
    }
    count(io, g.triangles, maximum_triangles, 88);
    for (auto& t : g.triangles) {
        for (auto& n : t.nodes) io.value(n);
        io.value(t.source_parent);
        for (auto& b : t.source_barycentric) vector3(io, b);
    }
    count(io, g.parent_quality, source.source_triangles.size(), 33);
    for (auto& q : g.parent_quality) {
        io.value(q.output_triangles); io.value(q.sampled_quality_failures);
        io.value(q.maximum_position_error); io.value(q.maximum_normal_error_degrees);
        auto sampled = std::uint32_t(q.sampled_target); io.value(sampled);
        if constexpr (Io::reading) { q.sampled_target = sampled != 0; if (sampled > 1) io.good = false; }
    }
    count(io, g.component_fallbacks, source.source_triangles.size(), 20);
    for (auto& f : g.component_fallbacks) {
        enumeration(io, f.reason); io.text(f.diagnostic);
        count(io, f.source_parents, source.source_triangles.size(), 4);
        for (auto& parent : f.source_parents) io.value(parent);
    }
}
bool finite(glm::dvec3 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
bool barycentric(glm::dvec3 p) {
    return finite(p) && std::min({p.x, p.y, p.z}) >= -1e-10 &&
        std::max({p.x, p.y, p.z}) <= 1 + 1e-10 && std::abs(p.x + p.y + p.z - 1) <= 1e-10;
}
std::uint64_t edge_key(std::uint32_t a, std::uint32_t b) {
    return (std::uint64_t(std::min(a, b)) << 32) | std::max(a, b);
}
bool validate(const PreparedDisplacementSurface& source, const PreparedDisplacementSurface& cached,
        const DisplacementPreparationPolicy& policy) {
    const auto& g = cached.geometry; const auto& m = g.metrics;
    const auto n = source.source_vertices.size(), t = source.source_triangles.size();
    const double bound = policy.geometry.correspondence_bound * std::min(1.0, source.minimum_homogeneous_w);
    const double allowance = 1e-7 * std::min(1.0, source.minimum_homogeneous_w);
    const bool complete = g.status == BoundedDisplacementStatus::Complete;
    if ((!complete && g.status != BoundedDisplacementStatus::BudgetLimited) ||
            cached.disposition != (complete && m.sampled_quality_failures == 0
                ? DisplacementPreparationDisposition::Prepared : DisplacementPreparationDisposition::BudgetLimited) ||
            g.nodes.size() < n || g.triangles.empty() || g.triangles.size() > policy.geometry.maximum_triangles ||
            g.parent_quality.size() != t || m.input_vertices != n || m.input_triangles != t ||
            m.output_nodes != g.nodes.size() || m.output_triangles != g.triangles.size() ||
            m.orientation_failures || m.patch_evaluations > policy.geometry.maximum_patch_evaluations ||
            m.rejected_components != g.component_fallbacks.size() ||
            !std::isfinite(m.maximum_target_bound) || m.maximum_target_bound > bound + allowance ||
            (complete && m.sampled_quality_failures != 0)) return false;
    for (double value : {m.maximum_common_control_adjustment, m.maximum_rejected_target_bound,
            m.maximum_sampled_movement, m.maximum_sampled_position_error,
            m.maximum_sampled_normal_error_degrees, m.failing_area_fraction, m.minimum_triangle_area})
        if (!std::isfinite(value) || value < 0) return false;
    if (m.failing_area_fraction > 1 || !finite(m.first_failed_source_barycentric)) return false;
    std::vector<glm::dvec3> old; old.reserve(n);
    for (const auto& v : source.source_vertices) {
        auto p = source.uniform_scale * v.neutral_position + double(source.displacement.amplitude) *
            v.displacement_direction * double(v.displacement_magnitude) * (double(v.height) - 127.0 / 255.0);
        if (!finite(p)) return false;
        old.push_back(p);
    }
    const auto reference = [&](std::uint32_t parent, glm::dvec3 b) {
        const auto ids = source.source_triangles[parent];
        return b.x * old[ids[0]] + b.y * old[ids[1]] + b.z * old[ids[2]];
    };
    for (std::size_t i = 0; i < g.nodes.size(); ++i) {
        const auto& v = g.nodes[i];
        if (!finite(v.old_raster_position) || !finite(v.target_position) || !finite(v.correction) ||
                glm::length(v.correction) > bound + allowance ||
                glm::length(v.target_position - v.old_raster_position - v.correction) > 1e-7) return false;
        if (i < n) { if (glm::length(old[i] - v.old_raster_position) > 1e-7) return false; }
        else if (v.source_parent >= t || !barycentric(v.source_barycentric) ||
                glm::length(reference(v.source_parent, v.source_barycentric) - v.old_raster_position) > 1e-7) return false;
    }
    struct Edge { std::uint64_t key, boundary; std::uint32_t parent; int direction; };
    if (g.triangles.size() > policy.geometry.maximum_work_bytes / (3 * sizeof(Edge))) return false;
    std::vector<Edge> edges; edges.reserve(g.triangles.size() * 3);
    std::vector<double> coverage(t);
    std::vector<std::uint64_t> counts(t);
    std::vector<bool> rejected(t);
    std::uint64_t rejected_count = 0;
    for (const auto& fallback : g.component_fallbacks) {
        if (fallback.reason < BoundedDisplacementStatus::TargetBoundFailed ||
                fallback.reason > BoundedDisplacementStatus::NumericalLimit || fallback.source_parents.empty()) return false;
        for (auto parent : fallback.source_parents) {
            if (parent >= t || rejected[parent]) return false;
            rejected[parent] = true; ++rejected_count;
        }
    }
    if (rejected_count != m.rejected_source_triangles) return false;
    std::unordered_map<std::uint64_t, unsigned> boundary_incidence;
    for (const auto& source_triangle : source.source_triangles)
        for (int k = 0; k < 3; ++k) ++boundary_incidence[edge_key(source_triangle[k], source_triangle[(k + 1) % 3])];
    for (const auto& triangle : g.triangles) {
        if (triangle.source_parent >= t) return false;
        const auto parent = triangle.source_parent;
        std::array<glm::dvec3, 3> r{}, q{};
        for (int k = 0; k < 3; ++k) {
            if (triangle.nodes[k] >= g.nodes.size() || !barycentric(triangle.source_barycentric[k])) return false;
            r[k] = reference(parent, triangle.source_barycentric[k]);
            q[k] = g.nodes[triangle.nodes[k]].target_position;
            if (glm::length(r[k] - g.nodes[triangle.nodes[k]].old_raster_position) > 1e-7) return false;
        }
        const auto a = triangle.source_barycentric[1] - triangle.source_barycentric[0];
        const auto b = triangle.source_barycentric[2] - triangle.source_barycentric[0];
        const double area = a.y * b.z - a.z * b.y;
        const auto before = glm::cross(r[1] - r[0], r[2] - r[0]);
        const auto after = glm::cross(q[1] - q[0], q[2] - q[0]);
        if (!(area > 0) || !finite(after) || glm::length(before) <= 1e-15 ||
                glm::length(after) <= 1e-15 || glm::dot(before, after) <= 0) return false;
        coverage[parent] += area; ++counts[parent];
        if (rejected[parent]) {
            const std::array<glm::dvec3, 3> corners{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
            if (triangle.nodes != source.source_triangles[parent] ||
                    triangle.source_barycentric != corners) return false;
            // The original prefix is authoritative. A separately reconstructed
            // displacement can differ by FP64 roundoff from the builder's
            // scalar-first expression; source correspondence is checked above.
            for (auto id : triangle.nodes) {
                const auto& node = g.nodes[id];
                if (node.correction != glm::dvec3{}) return false;
                for (int k = 0; k < 3; ++k)
                    if (std::bit_cast<std::uint64_t>(node.target_position[k]) !=
                            std::bit_cast<std::uint64_t>(node.old_raster_position[k])) return false;
            }
        }
        for (int k = 0; k < 3; ++k) {
            const int next = (k + 1) % 3;
            std::uint64_t boundary = kAbsentEdge;
            for (int j = 0; j < 3; ++j)
                if (std::abs(triangle.source_barycentric[k][j]) <= 1e-10 &&
                        std::abs(triangle.source_barycentric[next][j]) <= 1e-10) {
                    const auto ids = source.source_triangles[parent];
                    boundary = edge_key(ids[(j + 1) % 3], ids[(j + 2) % 3]);
                }
            edges.push_back({edge_key(triangle.nodes[k], triangle.nodes[next]), boundary,
                parent, triangle.nodes[k] < triangle.nodes[next] ? 1 : -1});
        }
    }
    std::uint64_t quality_failures = 0;
    for (std::size_t parent = 0; parent < t; ++parent) {
        const auto& quality = g.parent_quality[parent];
        if (std::abs(coverage[parent] - 1) > 1e-9 || counts[parent] != quality.output_triangles ||
                quality.sampled_quality_failures > counts[parent] || (rejected[parent] && counts[parent] != 1) ||
                !std::isfinite(quality.maximum_position_error) || quality.maximum_position_error < 0 ||
                !std::isfinite(quality.maximum_normal_error_degrees) || quality.maximum_normal_error_degrees < 0) return false;
        quality_failures += quality.sampled_quality_failures;
    }
    if (quality_failures != m.sampled_quality_failures) return false;
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.key < b.key; });
    for (std::size_t first = 0; first < edges.size();) {
        std::size_t end = first + 1;
        while (end < edges.size() && edges[end].key == edges[first].key) ++end;
        int direction = 0;
        for (auto i = first; i < end; ++i) {
            direction += edges[i].direction;
            if (edges[i].boundary != edges[first].boundary ||
                    (edges[first].boundary == kAbsentEdge && edges[i].parent != edges[first].parent)) return false;
        }
        const auto boundary = edges[first].boundary;
        if (boundary == kAbsentEdge) {
            if (end - first != 2 || direction != 0) return false;
        } else {
            const auto found = boundary_incidence.find(boundary);
            if (found == boundary_incidence.end() || end - first != found->second ||
                    (end - first == 2 && direction != 0)) return false;
        }
        first = end;
    }
    return true;
}
struct Stage {
    std::filesystem::path path;
    ~Stage() { std::error_code error; if (!path.empty()) std::filesystem::remove_all(path, error); }
};
struct CloseArchive { void operator()(zip_t* p) const { if (p) zip_close(p); } };
struct CloseFile { void operator()(zip_file_t* p) const { if (p) zip_fclose(p); } };
} // namespace

std::filesystem::path default_prepared_displacement_cache_directory(const std::filesystem::path& archive) {
    return archive.empty() ? std::filesystem::path{} : archive.parent_path() / "prepared-displacement-v1";
}
Result<void> store_prepared_displacement_cache(const std::filesystem::path& directory,
        const PreparedDisplacementSurface& refined, const DisplacementPreparationPolicy& policy) {
    profile::Scope profile_scope("Save prepared displacement cache", "Cache");
    if (directory.empty()) return {};
    try {
        auto key = cache_key(refined, policy);
        if (!key) return std::unexpected(key.error());
        if (!refined.ready() || !validate(refined, refined, policy))
            return std::unexpected(Error{"Prepared cache refuses invalid refinement integrity"});
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return std::unexpected(Error{"Cannot create prepared cache directory '" + directory.string() + "': " + error.message()});
        Stage stage;
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            auto path = directory / (".stage-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            if (std::filesystem::create_directory(path, error)) { stage.path = std::move(path); break; }
            if (error) return std::unexpected(Error{"Cannot stage prepared cache: " + error.message()});
        }
        if (stage.path.empty()) return std::unexpected(Error{"Cannot reserve prepared cache staging directory"});
        const auto file = stage.path / "geometry.bin";
        Writer writer(file);
        payload(writer, refined, refined, policy);
        writer.finish();
        if (!writer.stream) return std::unexpected(Error{"Cannot write prepared cache payload: " + file.string()});
        nlohmann::json manifest{{"schema_version", kVersion}, {"key", *key},
            {"operator_revision", HEAVEN_PREPARED_CACHE_BUILD_ID}, {"payload_sha256", writer.digest.finish()},
            {"payload_bytes", std::filesystem::file_size(file)}};
        auto archive = ZipArchiveWriter::create(directory / (*key + ".zip"), true);
        if (!archive) return std::unexpected(archive.error());
        const auto text = manifest.dump() + "\n";
        if (auto r = archive->add_bytes("manifest.json", std::as_bytes(std::span(text)), ZipCompression::Store); !r) return r;
        if (auto r = archive->add_file("geometry.bin", file, ZipCompression::DeflateFast); !r) return r;
        return archive->finish();
    } catch (const std::exception& error) {
        return std::unexpected(Error{"Prepared cache publication failed: " + std::string(error.what())});
    }
}
Result<bool> load_prepared_displacement_cache(const std::filesystem::path& directory,
        PreparedDisplacementSurface& source, const DisplacementPreparationPolicy& policy) {
    profile::Scope profile_scope("Load prepared displacement cache", "Cache");
    if (directory.empty()) return false;
    try {
        auto key = cache_key(source, policy);
        if (!key) return std::unexpected(key.error());
        const auto path = directory / (*key + ".zip");
        std::error_code error;
        if (!std::filesystem::exists(path, error)) {
            if (error) return std::unexpected(Error{"Cannot inspect prepared cache: " + error.message()});
            return false;
        }
        int code = 0;
        std::unique_ptr<zip_t, CloseArchive> archive(zip_open(path.string().c_str(), ZIP_RDONLY, &code));
        if (!archive || zip_get_num_entries(archive.get(), 0) != 2)
            return std::unexpected(Error{"Invalid prepared cache ZIP: " + path.string()});
        zip_stat_t stat{}; zip_stat_init(&stat);
        if (zip_stat(archive.get(), "manifest.json", 0, &stat) != 0 || stat.size > 4096)
            return std::unexpected(Error{"Invalid prepared cache manifest"});
        std::string text(static_cast<std::size_t>(stat.size), '\0');
        std::unique_ptr<zip_file_t, CloseFile> mf(zip_fopen(archive.get(), "manifest.json", 0));
        if (!mf || zip_fread(mf.get(), text.data(), text.size()) != static_cast<zip_int64_t>(text.size()))
            return std::unexpected(Error{"Unreadable prepared cache manifest"});
        const auto manifest = nlohmann::json::parse(text);
        if (manifest.at("schema_version") != kVersion || manifest.at("key") != *key ||
                manifest.at("operator_revision") != HEAVEN_PREPARED_CACHE_BUILD_ID ||
                zip_stat(archive.get(), "geometry.bin", 0, &stat) != 0 ||
                stat.size != manifest.at("payload_bytes").get<std::uint64_t>() ||
                stat.size > policy.geometry.maximum_work_bytes)
            return std::unexpected(Error{"Prepared cache identity/size mismatch"});
        std::unique_ptr<zip_file_t, CloseFile> file(zip_fopen(archive.get(), "geometry.bin", 0));
        if (!file) return std::unexpected(Error{"Unreadable prepared cache geometry"});
        Reader reader(file.get(), stat.size, policy.geometry.maximum_work_bytes);
        PreparedDisplacementSurface cached;
        payload(reader, cached, source, policy);
        if (!reader.good || reader.remaining != 0 || reader.digest.finish() != manifest.at("payload_sha256").get<std::string>() ||
                zip_fclose(file.release()) != 0 || !validate(source, cached, policy))
            return std::unexpected(Error{"Prepared cache payload/integrity validation failed: " + path.string()});
        source.geometry = std::move(cached.geometry);
        source.disposition = cached.disposition; source.reason = std::move(cached.reason);
        const auto scale = std::min(1.0, source.minimum_homogeneous_w);
        source.effective_correspondence_bound = policy.geometry.correspondence_bound * scale;
        source.effective_initial_node_bound = policy.geometry.initial_node_bound * scale;
        source.effective_position_error = policy.geometry.position_error * scale;
        source.geometry_integrity_valid = true;
        source.requested_quality_met = source.disposition == DisplacementPreparationDisposition::Prepared;
        return true;
    } catch (const std::exception& error) {
        return std::unexpected(Error{"Prepared cache read failed: " + std::string(error.what())});
    }
}
} // namespace heaven
