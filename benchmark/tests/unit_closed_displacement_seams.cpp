#include "engine/closed_displacement_seams.h"
#include "engine/cpu_tessellation.h"
#include "engine/zip_archive.h"

#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
} // namespace heaven::testing

namespace {

heaven::Vertex vertex(float x, float y) {
    heaven::Vertex v{};
    v.px = x; v.py = y; v.nz = 1;
    v.u = x * 2; v.v = y * 3; v.u1 = x + 7; v.v1 = y - 4;
    v.tx = 1; v.tw = -1; v.displacement_normal_scale = 1.25f;
    return v;
}
heaven::Mesh adjacent() {
    heaven::Mesh mesh;
    heaven::Surface raised, flat;
    raised.name = "raised"; flat.name = "flat";
    raised.vertices = {vertex(0, 0), vertex(1, 0), vertex(1, 1), vertex(0, 1)};
    flat.vertices = {vertex(-1, 0), vertex(0, 0), vertex(0, 1), vertex(-1, 1)};
    raised.indices = flat.indices = {0, 1, 2, 0, 2, 3};
    mesh.surfaces = {raised, flat};
    return mesh;
}

auto vertex_bits(const heaven::Vertex& v) {
    const std::array values{v.px, v.py, v.pz, v.nx, v.ny, v.nz,
        v.u, v.v, v.u1, v.v1, v.tx, v.ty, v.tz, v.tw,
        v.displacement_normal_scale};
    std::array<std::uint32_t, values.size()> result{};
    for (std::size_t i = 0; i < values.size(); ++i)
        result[i] = std::bit_cast<std::uint32_t>(values[i]);
    return result;
}

bool unit_closed_displacement_seams() {
    using heaven::SeamSurfaceKind;
    bool ok = true;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { std::printf("[unit_closed_displacement_seams] FAILED: %s\n", message); ok = false; }
    };
    auto mesh = adjacent();
    // Real source export rounding need not produce byte-identical positions.
    mesh.surfaces[1].vertices[1].px = 1e-6f;
    mesh.surfaces[1].vertices[2].px = 1e-6f;
    const std::array kinds{SeamSurfaceKind::Displaced, SeamSurfaceKind::Flat};
    const auto seams = heaven::collect_closed_displacement_seams(mesh, kinds);
    check(seams.matched_segments == 1 && seams.surfaces[0].size() == 1 && seams.surfaces[1].empty(),
          "near-coincident reversed material boundary did not constrain only its displaced side");
    auto cold = mesh.surfaces[0];
    check(heaven::cpu_tessellate_surface(cold, "", heaven::TessellationTopologyParameters{
              .density_factor = 4, .edge_scale = 1, .subdivisions = 2}), "neutral tessellation failed");
    // A neutral cache hit returns this same untouched topology. Applying the
    // constraint after either path must give identical uploaded attributes.
    auto warm = cold;
    auto before = cold;
    const auto changed = heaven::apply_closed_displacement_seams(cold, seams.surfaces[0],
        seams.fade_distance, seams.matching_tolerance);
    check(changed > 0, "closed seam did not change any displacement magnitude");
    static_cast<void>(heaven::apply_closed_displacement_seams(warm, seams.surfaces[0],
        seams.fade_distance, seams.matching_tolerance));
    check(cold.indices == before.indices && cold.vertices.size() == before.vertices.size(), "topology changed");
    std::size_t shared_points = 0, open_points = 0;
    for (std::size_t i = 0; i < cold.vertices.size(); ++i) {
        auto copy = cold.vertices[i]; copy.displacement_normal_scale = before.vertices[i].displacement_normal_scale;
        check(vertex_bits(copy) == vertex_bits(before.vertices[i]), "a position, UV, normal or tangent changed");
        check(vertex_bits(cold.vertices[i]) == vertex_bits(warm.vertices[i]), "cold/warm constraint differs");
        const auto& v = cold.vertices[i];
        if (v.px == 0) {
            ++shared_points;
            // Independent displacement closure oracle with a nonneutral height.
            const double old_gap = 0.4 * before.vertices[i].displacement_normal_scale * (0.7 - 127.0 / 255.0);
            const double new_gap = 0.4 * v.displacement_normal_scale * (0.7 - 127.0 / 255.0);
            check(old_gap > 0.05 && new_gap == 0, "shared boundary retains a displaced-to-flat opening");
        }
        if (v.px == 1) { ++open_points; check(v.displacement_normal_scale == before.vertices[i].displacement_normal_scale,
                                              "an unpaired open boundary lost displacement"); }
    }
    check(shared_points >= 3 && open_points >= 3, "subdivided shared/open edge controls were absent");
    auto probe = mesh.surfaces[0];
    probe.vertices.push_back(vertex(static_cast<float>(seams.matching_tolerance + 0.5 * seams.fade_distance), 0.5f));
    static_cast<void>(heaven::apply_closed_displacement_seams(probe, seams.surfaces[0], seams.fade_distance, seams.matching_tolerance));
    check(std::abs(probe.vertices.back().displacement_normal_scale - 0.625f) < 1e-6f,
          "narrow smooth transition does not retain partial relief");
    for (const auto second : {SeamSurfaceKind::Inactive}) {
        const std::array excluded{SeamSurfaceKind::Displaced, second};
        check(heaven::collect_closed_displacement_seams(mesh, excluded).matched_segments == 0,
              "hidden neighbor changed the field");
    }
    auto separated = mesh; separated.surfaces[1].vertices[1].px = 0.001f; separated.surfaces[1].vertices[2].px = 0.001f;
    check(heaven::collect_closed_displacement_seams(separated, kinds).matched_segments == 0,
          "a real source opening was incorrectly treated as a closed material joint");
    auto overlapping = mesh;
    for (std::size_t i = 0; i < overlapping.surfaces[1].indices.size(); i += 3)
        std::swap(overlapping.surfaces[1].indices[i + 1], overlapping.surfaces[1].indices[i + 2]);
    check(heaven::collect_closed_displacement_seams(overlapping, kinds).matched_segments == 0,
          "same-winding overlapping faces established an adjacent material joint");
    auto ambiguous = mesh; ambiguous.surfaces.push_back(mesh.surfaces[1]);
    const std::array ambiguous_kinds{SeamSurfaceKind::Displaced, SeamSurfaceKind::Flat, SeamSurfaceKind::Flat};
    check(heaven::collect_closed_displacement_seams(ambiguous, ambiguous_kinds).matched_segments == 0,
          "ambiguous overlapping neighbors established a closed seam");
    auto invalid = mesh; invalid.surfaces[1].indices[0] = 500;
    check(heaven::collect_closed_displacement_seams(invalid, kinds).matched_segments == 0,
          "invalid neighbor indices established a closed seam");

    const std::array displaced{SeamSurfaceKind::Displaced, SeamSurfaceKind::Displaced};
    auto charts = adjacent();
    check(heaven::collect_closed_displacement_seams(charts, displaced).matched_segments == 2,
          "distinct-surface height footprints were assumed continuous");
    for (auto& v : charts.surfaces[1].vertices) v.u += 2;
    charts.surfaces[1].vertices.push_back(vertex(0, 0.5f));
    charts.surfaces[1].vertices.back().u += 2;
    charts.surfaces[1].indices = {0, 1, 4, 0, 4, 2, 0, 2, 3};
    // Model a source export offset, not a position weld. A long boundary must
    // pair uniquely with both shorter reversed chart boundaries.
    for (auto& v : charts.surfaces[1].vertices) v.pz += 0.00015f;
    const auto chain_seams = heaven::collect_closed_displacement_seams(charts, displaced);
    check(chain_seams.matched_segments == 3 && chain_seams.surfaces[0].size() == 1 &&
          chain_seams.surfaces[1].size() == 2, "unequal chart chains were not pinned reciprocally");
    for (std::size_t si = 0; si < charts.surfaces.size(); ++si) {
        const auto original = charts.surfaces[si];
        static_cast<void>(heaven::apply_closed_displacement_seams(charts.surfaces[si], chain_seams.surfaces[si],
            chain_seams.fade_distance, chain_seams.matching_tolerance));
        for (std::size_t vi = 0; vi < original.vertices.size(); ++vi) {
            const auto& v = charts.surfaces[si].vertices[vi];
            if (v.px == 0) check(v.displacement_normal_scale == 0, "a chart side retains a displaced opening");
            auto copy = v; copy.displacement_normal_scale = original.vertices[vi].displacement_normal_scale;
            check(vertex_bits(copy) == vertex_bits(original.vertices[vi]), "chart matching welded source attributes");
        }
    }
    auto incomplete = adjacent();
    incomplete.surfaces[1].vertices.push_back(vertex(0, 0.5f));
    incomplete.surfaces[1].indices = {0, 1, 4, 0, 2, 3};
    for (auto& v : incomplete.surfaces[1].vertices) v.u += 2;
    check(heaven::collect_closed_displacement_seams(incomplete, displaced).matched_segments == 0,
          "a partly open chart chain pinned only one side");
    auto same_surface = adjacent();
    auto& merged = same_surface.surfaces[0];
    for (auto v : same_surface.surfaces[1].vertices) merged.vertices.push_back(v);
    for (auto index : same_surface.surfaces[1].indices) merged.indices.push_back(index + 4);
    same_surface.surfaces.resize(1);
    const std::array one_kind{SeamSurfaceKind::Displaced};
    check(heaven::collect_closed_displacement_seams(same_surface, one_kind).matched_segments == 0,
          "exact compatible within-surface copies were faded");
    auto near_copy = same_surface;
    for (std::size_t i = 4; i < near_copy.surfaces[0].vertices.size(); ++i)
        near_copy.surfaces[0].vertices[i].pz += 0.00015f;
    check(heaven::collect_closed_displacement_seams(near_copy, one_kind).matched_segments == 2,
          "near-position copies were assumed to share exact height footprints");
    for (std::size_t i = 4; i < merged.vertices.size(); ++i) merged.vertices[i].nz = -1;
    check(heaven::collect_closed_displacement_seams(same_surface, one_kind).matched_segments == 2,
          "incompatible raw-normal charts within one surface were discarded");
    auto ambiguous_charts = adjacent(); ambiguous_charts.surfaces.push_back(ambiguous_charts.surfaces[1]);
    const std::array three_displaced{SeamSurfaceKind::Displaced, SeamSurfaceKind::Displaced, SeamSurfaceKind::Displaced};
    check(heaven::collect_closed_displacement_seams(ambiguous_charts, three_displaced).matched_segments == 0,
          "ambiguous displaced chart overlaps were pinned");
    auto distant = adjacent();
    for (auto& v : distant.surfaces[1].vertices) { v.u += 2; v.pz += 0.001f; }
    check(heaven::collect_closed_displacement_seams(distant, displaced).matched_segments == 0,
          "a real chart gap outside the half-millimetre bound was paired");

    // A short triangle beside a closed join is translated safely by the
    // original constant height. A mesh-only 5 cm fade reverses its projected
    // winding when displacement has a tangential component. The independent
    // oracle uses actual displaced positions, not the helper's gradient math.
    heaven::Surface thin;
    thin.vertices = {vertex(0, 0), vertex(0.03f, 0), vertex(0, 1), vertex(2, 0)};
    thin.indices = {0, 1, 2};
    for (auto& v : thin.vertices) {
        v.nx = 0.8f; v.nz = 0.6f; v.displacement_normal_scale = 1;
    }
    const std::array join{heaven::ClosedSeamSegment{{0, 0, 0}, {0, 1, 0}}};
    auto abrupt = thin, bounded = thin;
    static_cast<void>(heaven::apply_closed_displacement_seams(abrupt, join, 0.05, 0));
    heaven::ClosedSeamFadeReport fade_report;
    static_cast<void>(heaven::apply_closed_displacement_seams(bounded, join, 0.05, 0, 0.35, &fade_report));
    const auto projected_winding = [](const heaven::Surface& s) {
        std::array<glm::dvec3, 3> p{};
        for (std::size_t i = 0; i < p.size(); ++i) {
            const auto& v = s.vertices[i];
            p[i] = glm::dvec3{v.px, v.py, v.pz} -
                0.35 * v.displacement_normal_scale * glm::dvec3{v.nx, v.ny, v.nz};
        }
        return glm::cross(p[1] - p[0], p[2] - p[0]).z;
    };
    check(projected_winding(thin) > 0 && projected_winding(abrupt) < 0 && projected_winding(bounded) > 0,
          "amplitude-aware transition did not prevent the short-triangle reversal");
    check(fade_report.maximum_added_slope <= 1.0 && fade_report.fade_distance >= 0.525 - 1e-12,
          "reported transition exceeds its additional-slope bound");
    check(bounded.vertices[0].displacement_normal_scale == 0 && bounded.vertices[2].displacement_normal_scale == 0 &&
          bounded.vertices[3].displacement_normal_scale == 1,
          "safe transition lost closure or full relief outside its band");
    for (std::size_t i = 0; i < thin.vertices.size(); ++i) {
        auto restored = bounded.vertices[i];
        restored.displacement_normal_scale = thin.vertices[i].displacement_normal_scale;
        check(vertex_bits(restored) == vertex_bits(thin.vertices[i]), "slope guard changed source vertex attributes");
    }
    const glm::mat4 identity(1);
    check(std::abs(heaven::closed_seam_minimum_spatial_scale(identity) - 1) < 1e-12,
          "identity instance stretch is incorrect");
    auto small = identity; small[0][0] = 0.25f; small[1][1] = 2;
    check(heaven::closed_seam_minimum_spatial_scale(small) <= 0.25 &&
          heaven::closed_seam_minimum_spatial_scale(small) > 0.249999,
          "nonuniform instance minimum stretch is not conservative");
    auto shear = identity; shear[1][0] = 1;
    const double shear_stretch = heaven::closed_seam_minimum_spatial_scale(shear);
    check(shear_stretch <= (std::sqrt(5.0) - 1) / 2 && shear_stretch > 0.6180339,
          "sheared instance minimum stretch is incorrect");

    // One sliver requires local relaxation. A separate triangle is outside
    // the initial band and must not inherit a global width increase. The
    // duplicated point participates in another triangle: its lowered weight
    // must follow the canonical group, not merely the initial smoothstep.
    heaven::Surface local;
    local.vertices = {vertex(0.015f, 0), vertex(0.03f, 0), vertex(0.0225f, 0.00001f),
        vertex(0.03f, 0), vertex(0.04f, 0), vertex(0.03f, 0.02f),
        vertex(0.08f, 0), vertex(0.09f, 0), vertex(0.08f, 0.01f)};
    local.indices = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    auto narrow_local = local;
    static_cast<void>(heaven::apply_closed_displacement_seams(narrow_local, join, 0.05, 0));
    heaven::ClosedSeamFadeReport local_report;
    static_cast<void>(heaven::apply_closed_displacement_seams(local, join, 0.05, 0, 0.01, &local_report));
    check(local_report.slope_limit_met && local_report.maximum_added_slope <= 1.0 &&
          local_report.evaluated_triangles <= 64 * (local.indices.size() / 3),
          "local relaxation lacks a bounded-work final slope check");
    check(local.vertices[1].displacement_normal_scale < narrow_local.vertices[1].displacement_normal_scale &&
          vertex_bits(local.vertices[1]) == vertex_bits(local.vertices[3]),
          "compatible duplicate groups diverged after sliver relaxation");
    for (std::size_t i = 6; i < 9; ++i)
        check(local.vertices[i].displacement_normal_scale == 1.25f,
              "a local sliver reduced distant unaffected relief");
    // Nonzero float triangles below the former squared-area threshold still
    // have a real gradient. Use an independent equal-Z-offset derivative;
    // treating this as degenerate and returning zero misses a huge slope.
    heaven::Surface tiny;
    tiny.vertices = {vertex(0, 0), vertex(0.1f, 0), vertex(0.05f, 1e-13f)};
    tiny.indices = {0, 1, 2};
    for (auto& v : tiny.vertices) v.displacement_normal_scale = 1;
    heaven::ClosedSeamFadeReport tiny_report;
    static_cast<void>(heaven::apply_closed_displacement_seams(tiny, join, 0.05, 0, 0.1, &tiny_report));
    const double dx = tiny.vertices[1].px, x = tiny.vertices[2].px, dy = tiny.vertices[2].py;
    const double dzdx = 0.1 * (tiny.vertices[1].displacement_normal_scale - tiny.vertices[0].displacement_normal_scale) / dx;
    const double dzdy = (0.1 * (tiny.vertices[2].displacement_normal_scale - tiny.vertices[0].displacement_normal_scale) - x * dzdx) / dy;
    check(tiny_report.slope_limit_met && std::hypot(dzdx, dzdy) <= 1.0 &&
          tiny_report.evaluated_triangles <= 64,
          "a tiny nonzero triangle falsely certified zero added derivative");
    auto degenerate = tiny;
    degenerate.vertices[2].py = 0;
    for (auto& v : degenerate.vertices) v.displacement_normal_scale = 1;
    heaven::ClosedSeamFadeReport degenerate_report;
    static_cast<void>(heaven::apply_closed_displacement_seams(degenerate, join, 0.05, 0, 0.1, &degenerate_report));
    check(degenerate_report.slope_limit_met && degenerate.vertices[0].displacement_normal_scale ==
          degenerate.vertices[1].displacement_normal_scale && degenerate.vertices[1].displacement_normal_scale ==
          degenerate.vertices[2].displacement_normal_scale,
          "an exactly degenerate triangle retained unequal displacement weights");
    // Unequal original magnitudes cannot generally share a rounded ratio.
    // These values reproduce the actual house3 collinear roof failure. A
    // verified dyadic ratio retains partial relief instead of rejecting the
    // whole surface; a distant independent triangle keeps its full field.
    heaven::Surface rounded;
    rounded.vertices = {vertex(0.059f, 0), vertex(0.059f, 0.01f), vertex(0.059f, 0.005f),
        vertex(1, 0), vertex(1.1f, 0), vertex(1, 0.1f)};
    rounded.indices = {0, 1, 2, 3, 4, 5};
    rounded.vertices[0].displacement_normal_scale = 1;
    rounded.vertices[1].displacement_normal_scale = 1.000009537f;
    rounded.vertices[2].displacement_normal_scale = 0.994131088f;
    const auto rounded_before = rounded;
    const auto check_common = [&](const heaven::Surface& result, const heaven::ClosedSeamFadeReport& r) {
        const double ratio = static_cast<double>(result.vertices[0].displacement_normal_scale) /
            rounded_before.vertices[0].displacement_normal_scale;
        check(r.slope_limit_met && r.unresolved_triangles == 0 && !r.work_limit_exhausted &&
              r.exact_ratio_relaxations > 0 && r.evaluated_triangles <= 64 * (result.indices.size() / 3) &&
              ratio > 0 && ratio < 0.4,
              "unequal float magnitudes caused a whole-surface seam fallback");
        for (std::size_t i = 0; i < 3; ++i)
            check(static_cast<double>(result.vertices[i].displacement_normal_scale) /
                  rounded_before.vertices[i].displacement_normal_scale == ratio,
                  "local committed float products lack an exact common ratio");
        for (std::size_t i = 3; i < result.vertices.size(); ++i)
            check(vertex_bits(result.vertices[i]) == vertex_bits(rounded_before.vertices[i]),
                  "a local rounding repair suppressed distant relief");
    };
    heaven::ClosedSeamFadeReport rounded_report;
    static_cast<void>(heaven::apply_closed_displacement_seams(rounded, join, 0.05, 0, 0.1, &rounded_report));
    check_common(rounded, rounded_report);
    auto finite_rounding = rounded_before;
    finite_rounding.vertices[2].pz = 1e-13f;
    heaven::ClosedSeamFadeReport finite_report;
    static_cast<void>(heaven::apply_closed_displacement_seams(finite_rounding, join, 0.05, 0, 0.1, &finite_report));
    check_common(finite_rounding, finite_report);
    check(finite_report.rounding_stalls > 0, "finite float rounding stall control did not engage");
    // A near-neutral mip chain must not inherit the universal half-height
    // envelope. A stronger high-resolution variant remains in the union;
    // High2K uses exactly the manifest-selected start mip as TextureStore does.
    const auto pack_path = std::filesystem::temp_directory_path() /
        ("heaven-seam-height-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".zip");
    struct RemovePack {
        std::filesystem::path path;
        ~RemovePack() { std::error_code ec; std::filesystem::remove(path, ec); }
    } remove_pack{pack_path};
    const auto dds_bytes = [](const std::array<std::uint8_t, 3>& red) {
        std::vector<std::uint8_t> bytes(128);
        const auto put = [&](std::size_t offset, std::uint32_t value) {
            for (unsigned j = 0; j < 4; ++j) bytes[offset + j] = static_cast<std::uint8_t>(value >> (8u * j));
        };
        put(0, 0x20534444); put(4, 124); put(12, 4); put(16, 4); put(28, 3);
        put(76, 32); put(80, 0x41); put(88, 32); put(92, 0xff); put(96, 0xff00);
        put(100, 0xff0000); put(104, 0xff000000);
        for (std::size_t mip = 0; mip < red.size(); ++mip) {
            const std::size_t edge = std::size_t{4} >> mip;
            for (std::size_t j = 0; j < edge * edge; ++j)
                bytes.insert(bytes.end(), {red[mip], 0, 0, 255});
        }
        return bytes;
    };
    const auto original = dds_bytes({127, 127, 127});
    const auto variant = dds_bytes({255, 191, 127});
    // BC4, a partial 8->4->2 chain, alternating source texels and a neutral
    // final mip. The negative-UV point crosses a texel-center phase boundary;
    // the indexed triangle's large UV edges force meshLOD>1 into the last mip.
    std::vector<std::uint8_t> bc4(128);
    const auto bc_put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned j = 0; j < 4; ++j) bc4[offset + j] = static_cast<std::uint8_t>(value >> (8u * j));
    };
    bc_put(0, 0x20534444); bc_put(4, 124); bc_put(12, 8); bc_put(16, 8); bc_put(28, 3);
    bc_put(76, 32); bc_put(80, 4); bc_put(84, 0x31495441); // ATI1 unsigned BC4
    std::uint64_t selectors = 0;
    for (unsigned j = 0; j < 16; ++j) selectors |= std::uint64_t(j % 2) << (3u * j);
    for (unsigned block = 0; block < 5; ++block) {
        bc4.insert(bc4.end(), {130, 120});
        for (unsigned j = 0; j < 6; ++j) bc4.push_back(static_cast<std::uint8_t>(selectors >> (8u * j)));
    }
    bc4.insert(bc4.end(), {127, 127, 0, 0, 0, 0, 0, 0});
    std::array<std::vector<std::uint8_t>, 16> constant_bc4;
    for (std::size_t field = 0; field < constant_bc4.size(); ++field) {
        auto& bytes = constant_bc4[field]; bytes.assign(bc4.begin(), bc4.begin() + 128);
        const std::uint8_t first = field < 8 ? 255 : 64, second = field < 8 ? 0 : 192;
        std::uint64_t repeated = 0;
        for (unsigned j = 0; j < 16; ++j) repeated |= std::uint64_t(field % 8) << (3u * j);
        for (unsigned block = 0; block < 6; ++block) {
            bytes.insert(bytes.end(), {first, second});
            for (unsigned j = 0; j < 6; ++j) bytes.push_back(static_cast<std::uint8_t>(repeated >> (8u * j)));
        }
    }
    auto unsupported_bc1 = constant_bc4[4];
    unsupported_bc1[84] = 'D'; unsupported_bc1[85] = 'X'; unsupported_bc1[86] = 'T'; unsupported_bc1[87] = '1';
    auto bc5 = std::vector<std::uint8_t>(constant_bc4[4].begin(), constant_bc4[4].begin() + 128);
    bc5[84] = 'A'; bc5[85] = 'T'; bc5[86] = 'I'; bc5[87] = '2';
    for (std::size_t block = 128; block < constant_bc4[4].size(); block += 8) {
        bc5.insert(bc5.end(), constant_bc4[4].begin() + static_cast<std::ptrdiff_t>(block),
            constant_bc4[4].begin() + static_cast<std::ptrdiff_t>(block + 8));
        bc5.insert(bc5.end(), {127, 127, 0, 0, 0, 0, 0, 0});
    }
    const std::string hash(64, '0');
    const std::string manifest =
        R"({"schema_version":2,"data_root_prefix":"Data_Root","texture_sets":{"estimated_gpu_bytes":{"original":84,"2k":20,"4k":84},"textures":{"Data_Root/heaven/h.dds":{"source_sha256":")" +
        hash + R"(","variant":{"path":"Data_Root/texture_sets/4k/h.dds","sha256":")" + hash +
        R"(","format":"RGBA8","width":4096,"height":4096,"gpu_bytes_2k":20,"gpu_bytes_4k":84}}}}})";
    const std::string world = "world";
    auto writer = heaven::ZipArchiveWriter::create(pack_path, false);
    bool written = writer &&
        writer->add_bytes(std::string(heaven::kAssetArchiveManifest), std::as_bytes(std::span(manifest)), heaven::ZipCompression::Store) &&
        writer->add_bytes("Data_Root/heaven/heaven.world", std::as_bytes(std::span(world)), heaven::ZipCompression::Store) &&
        writer->add_bytes("Data_Root/heaven/h.dds", std::as_bytes(std::span(original)), heaven::ZipCompression::Store) &&
        writer->add_bytes("Data_Root/texture_sets/4k/h.dds", std::as_bytes(std::span(variant)), heaven::ZipCompression::Store) &&
        writer->add_bytes("Data_Root/heaven/bc4.dds", std::as_bytes(std::span(bc4)), heaven::ZipCompression::Store) &&
        writer->add_bytes("Data_Root/heaven/bc5.dds", std::as_bytes(std::span(bc5)), heaven::ZipCompression::Store) &&
        writer->add_bytes("Data_Root/heaven/bc1.dds", std::as_bytes(std::span(unsupported_bc1)), heaven::ZipCompression::Store);
    for (std::size_t field = 0; field < constant_bc4.size() && written; ++field)
        written = writer->add_bytes("Data_Root/heaven/constant" + std::to_string(field) + ".dds",
            std::as_bytes(std::span(constant_bc4[field])), heaven::ZipCompression::Store).has_value();
    written = written && writer->finish();
    check(written, "height DDS fixture could not be written");
    auto assets = heaven::AssetFileSystem::open(pack_path.string());
    check(assets.has_value(), "height DDS archive could not be opened");
    if (assets) {
        heaven::ClosedSeamHeightSampler sampler(*assets);
        heaven::Surface height_surface;
        height_surface.vertices = {vertex(0.1f, 0), vertex(0.1f, 0.1f), vertex(0.2f, 0)};
        height_surface.indices = {0, 1, 2};
        for (auto& v : height_surface.vertices) { v.u = 0; v.v = 0; }
        const std::array<float, 4> transform{1, 1, 0, 0};
        const auto neutral = sampler.bounds(height_surface, "Data_Root/heaven/h.dds", transform, heaven::TextureQuality::Original);
        const auto high = sampler.bounds(height_surface, "Data_Root/heaven/h.dds", transform, heaven::TextureQuality::High2K);
        const auto ultra = sampler.bounds(height_surface, "Data_Root/heaven/h.dds", transform, heaven::TextureQuality::Ultra4K);
        check(neutral.universal_vertices == 0 && high.universal_vertices == 0 && ultra.universal_vertices == 0 &&
              neutral.maximum_height_delta[0] < 0.002 && high.maximum_height_delta[0] > 0.24 &&
              high.maximum_height_delta[0] < 0.26 && ultra.maximum_height_delta[0] > 0.49,
              "filtered envelope lost neutral relief or selected variant/start-mip identity");
        auto bc_surface = height_surface;
        bc_surface.vertices[1].u = 1; bc_surface.vertices[2].v = 1;
        auto periodic = vertex(0, 0); periodic.u = periodic.v = -0.125f;
        bc_surface.vertices.push_back(periodic);
        const auto bc_bounds = sampler.bounds(bc_surface, "Data_Root/heaven/bc4.dds", transform, heaven::TextureQuality::Original);
        check(bc_bounds.universal_vertices == 0 && bc_bounds.maximum_height_delta[0] < 0.005 &&
              bc_bounds.maximum_height_delta[3] >= 7.0 / 255.0 && bc_bounds.maximum_height_delta[3] < 0.04,
              "BC4 quantization, negative periodic phase or clamped partial mip envelope is wrong");
        for (std::size_t field = 0; field < constant_bc4.size(); ++field) {
            const auto constant = sampler.bounds(height_surface, "Data_Root/heaven/constant" + std::to_string(field) + ".dds",
                transform, heaven::TextureQuality::Original);
            const auto selector = field % 8;
            const double first = field < 8 ? 1 : 64.0 / 255.0, second = field < 8 ? 0 : 192.0 / 255.0;
            double low = std::min(first, second), high_value = std::max(first, second);
            if (selector < 2) low = high_value = selector == 0 ? first : second;
            else if (field >= 8 && selector >= 6) low = high_value = selector == 6 ? 0 : 1;
            const double expected = std::max(std::abs(low - 127.0 / 255.0), std::abs(high_value - 127.0 / 255.0));
            check(constant.universal_vertices == 0 && constant.maximum_height_delta[0] >= expected &&
                  constant.maximum_height_delta[0] <= std::min(128.0 / 255.0, expected + 1.0 / 255.0 + 1.0 / 1024.0) + 1e-12,
                  "constant RGTC selector endpoint/special hull is incorrect");
            if (field == 4) check(constant.maximum_height_delta[0] >= std::abs(37.0 / 64.0 - 127.0 / 255.0),
                "full-range BC4 interpolation rounding escaped the envelope");
        }
        const auto red_bc5 = sampler.bounds(height_surface, "Data_Root/heaven/bc5.dds", transform, heaven::TextureQuality::Original);
        check(red_bc5.universal_vertices == 0 && red_bc5.maximum_height_delta[0] == 128.0 / 255.0,
              "BC5 hull read its green block instead of red");
        const auto unknown = sampler.bounds(height_surface, "Data_Root/heaven/bc1.dds", transform, heaven::TextureQuality::Original);
        check(unknown.universal_vertices == height_surface.vertices.size() && unknown.maximum_height_delta[0] == 128.0 / 255.0,
              "unsupported compressed red precision was silently certified");
        auto huge_uv = height_surface; huge_uv.vertices[0].u = 1e20f;
        check(sampler.bounds(huge_uv, "Data_Root/heaven/h.dds", transform, heaven::TextureQuality::Original).universal_vertices > 0,
              "unprovable floating texture coordinates did not use explicit universal fallback");
        std::vector<double> near_neutral_offsets(neutral.maximum_height_delta.size());
        for (std::size_t i = 0; i < near_neutral_offsets.size(); ++i)
            near_neutral_offsets[i] = 0.7 * neutral.maximum_height_delta[i] * height_surface.vertices[i].displacement_normal_scale;
        heaven::ClosedSeamFadeReport height_report;
        static_cast<void>(heaven::apply_closed_displacement_seams(height_surface, join, 0.05, 0, 0, &height_report, near_neutral_offsets));
        for (const auto& v : height_surface.vertices)
            check(v.displacement_normal_scale == 1.25f, "near-neutral unaffected interior lost full relief");
        check(height_report.slope_limit_met, "filtered envelope lacks final local gradient validation");
    }
    return ok;
}

heaven::testing::Register reg_closed_displacement_seams(unit_closed_displacement_seams);

} // namespace
