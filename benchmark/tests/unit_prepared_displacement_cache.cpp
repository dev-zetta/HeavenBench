#include "rhi/prepared_displacement_cache.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <numbers>
#include <vector>

namespace heaven::testing {
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register { explicit Register(TestFn fn) { registry().push_back(std::move(fn)); } };
namespace {
bool check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "[prepared_displacement_cache] %s\n", message);
    return condition;
}
PreparedDisplacementSurface frozen_source() {
    PreparedDisplacementSurface source;
    source.source_ready = true;
    source.disposition = DisplacementPreparationDisposition::SourceSampled;
    source.displacement = {true, .7f, 1.f, {0.f, 100.f}};
    auto& packed = source.original_packed;
    packed.displacement_directions_valid = true;
    packed.tangent_packing = StaticTangentPacking::NormalOnlyFallback;
    // Non-axis decoded FLOAT directions expose the FP64 operation-order
    // differences in algebraically equivalent displacement reconstruction.
    const auto direction = glm::normalize(glm::vec3(.3f, -.4f, .8660254f));
    for (std::uint32_t part = 0; part < 2; ++part) {
        const auto first = static_cast<std::uint32_t>(source.source_vertices.size());
        for (std::uint32_t y = 0; y < 9; ++y) for (std::uint32_t x = 0; x < 9; ++x) {
            const double u = x / 8.0, v = y / 8.0;
            BoundedDisplacementVertex vertex;
            // Match the actual FLOAT source position -> double metric contract.
            const float px = float(part + .5 * (u - .5)), py = float(.5 * (v - .5));
            vertex.neutral_position = {px, py, 0}; vertex.displacement_direction = direction;
            vertex.uv0 = vertex.uv1 = {float(u), float(v)}; vertex.footprint = {.13f, .13f};
            const double s = std::sin(std::numbers::pi * u) * std::sin(std::numbers::pi * v);
            vertex.height = float(127.0 / 255.0 + (part ? .2 : .0005) * s * s);
            const auto id = std::uint64_t(source.source_vertices.size());
            source.source_vertices.push_back(vertex);
            source.native_height.push_back({id, vertex.height, 1.5f, 1.5f});
            packed.positions.emplace_back(px, py, 0); packed.uv0.emplace_back(float(u), float(v));
            packed.uv1.emplace_back(float(u), float(v)); packed.tangent_frames.emplace_back(0, 0, 0, 32767);
            packed.displacement_custom.emplace_back(1, .13f, .13f, 0);
            packed.footprints.push_back({.13f, .13f});
            packed.unit_displacement_directions.emplace_back(direction.x, direction.y, direction.z);
        }
        for (std::uint32_t y = 0; y < 8; ++y) for (std::uint32_t x = 0; x < 8; ++x) {
            const auto a = first + y * 9 + x, b = a + 1, c = a + 9, d = c + 1;
            source.source_triangles.push_back({a, b, c}); source.source_triangles.push_back({b, d, c});
        }
    }
    return source;
}
std::vector<char> bytes(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
bool prepared_cache_contract() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("heaven-prepared-cache-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
    auto source = frozen_source(); auto refined = source;
    DisplacementPreparationPolicy policy;
    policy.geometry.correspondence_bound = .001;
    policy.geometry.initial_node_bound = .000999;
    policy.geometry.normal_error_degrees = 2;
    policy.geometry.maximum_sample_depth = 3;
    policy.geometry.maximum_triangles = 32768;
    policy.geometry.maximum_patch_evaluations = 2'000'000;
    policy.geometry.maximum_work_bytes = 64ull * 1024 * 1024;
    const auto directory = root / "prepared";
    auto miss = load_prepared_displacement_cache(directory, source, policy);
    if (!check(miss && !*miss, "cold source must miss")) return false;
    auto built = build_bounded_displacement_surface({source.source_vertices, source.source_triangles,
        source.uniform_scale, source.displacement.amplitude}, policy.geometry);
    if (!check(bool(built), "real bounded builder failed")) return false;
    refined.geometry = std::move(*built);
    refined.geometry_integrity_valid = true;
    refined.requested_quality_met = refined.geometry.status == BoundedDisplacementStatus::Complete &&
        refined.geometry.metrics.sampled_quality_failures == 0;
    refined.disposition = refined.requested_quality_met ? DisplacementPreparationDisposition::Prepared :
        DisplacementPreparationDisposition::BudgetLimited;
    refined.reason = "Explicit quality/fallback result";
    if (!check(refined.geometry.component_fallbacks.size() == 1 &&
            refined.geometry.metrics.accepted_components == 1, "fixture must exercise retained and rejected components")) return false;
    if (auto stored = store_prepared_displacement_cache(directory, refined, policy); !stored) {
        std::fprintf(stderr, "[prepared_displacement_cache] store: %s\n", stored.error().message.c_str()); return false;
    }
    auto loaded = source;
    auto hit = load_prepared_displacement_cache(directory, loaded, policy);
    if (!hit) std::fprintf(stderr, "[prepared_displacement_cache] load: %s\n", hit.error().message.c_str());
    if (!check(hit && *hit && loaded.ready() && loaded.reason == refined.reason &&
            loaded.requested_quality_met == refined.requested_quality_met &&
            loaded.geometry.component_fallbacks[0].source_parents == refined.geometry.component_fallbacks[0].source_parents &&
            loaded.geometry.component_fallbacks[0].diagnostic == refined.geometry.component_fallbacks[0].diagnostic,
            "warm load did not preserve explicit quality/fallback result")) return false;
    const auto file = fs::directory_iterator(directory)->path();
    const auto original_bytes = bytes(file);
    if (!check(store_prepared_displacement_cache(root / "copy", loaded, policy).has_value() &&
            bytes(fs::directory_iterator(root / "copy")->path()) == original_bytes,
            "all serialized fields must round trip byte exactly")) return false;
    // Every changed semantic input must miss, rather than silently reusing geometry.
    using Change = std::function<void(PreparedDisplacementSurface&, DisplacementPreparationPolicy&)>;
    const std::vector<Change> changes{
        [](auto& s, auto&) { s.native_height[40].height += 1e-6f; s.source_vertices[40].height = s.native_height[40].height; },
        [](auto& s, auto&) { s.original_packed.tangent_frames[0].x = 1; },
        [](auto& s, auto&) { s.original_packed.footprints[0][0] += 1e-6f; },
        [](auto& s, auto&) { s.source_vertices[0].uv0.x += 1e-6f; },
        [](auto& s, auto&) { s.native_height[0].mesh_lod += .1f; s.native_height[0].selected_lod = s.native_height[0].mesh_lod; },
        [](auto& s, auto&) { s.uniform_scale += 1e-6; },
        [](auto& s, auto&) { s.minimum_homogeneous_w = .9; },
        [](auto& s, auto&) { s.displacement.amplitude += 1e-6f; },
        [](auto& s, auto&) { s.displacement.fade.range += 1; },
        [](auto&, auto& p) { --p.geometry.maximum_triangles; },
        [](auto&, auto& p) { --p.geometry.maximum_patch_evaluations; },
        [](auto&, auto& p) { p.geometry.common_control_blend += .001; },
    };
    for (const auto& change : changes) {
        auto changed = source; auto changed_policy = policy; change(changed, changed_policy);
        const auto result = load_prepared_displacement_cache(directory, changed, changed_policy);
        if (!check(result && !*result && changed.geometry.nodes.empty(), "changed frozen input/policy reused a cached mesh")) return false;
    }
    auto invalid = refined;
    invalid.geometry.nodes.back().correction.z += .1;
    if (!check(!store_prepared_displacement_cache(directory, invalid, policy) && bytes(file) == original_bytes,
            "invalid geometry must not replace a valid cache")) return false;
    invalid = refined; invalid.geometry.triangles.pop_back();
    if (!check(!store_prepared_displacement_cache(directory, invalid, policy), "missing source coverage was accepted")) return false;
    invalid = refined; std::swap(invalid.geometry.triangles[0].nodes[0], invalid.geometry.triangles[0].nodes[1]);
    if (!check(!store_prepared_displacement_cache(directory, invalid, policy), "reversed lineage/winding was accepted")) return false;
    // Truncated ZIP is a diagnosed nonfatal error and must not modify the caller.
    { std::ofstream stream(file, std::ios::binary | std::ios::trunc);
      stream.write(original_bytes.data(), static_cast<std::streamsize>(original_bytes.size() / 2)); }
    auto untouched = source;
    const auto corrupt = load_prepared_displacement_cache(directory, untouched, policy);
    if (!check(!corrupt && untouched.geometry.nodes.empty() && !untouched.geometry_integrity_valid,
            "corrupt cache partially committed geometry")) return false;
    const auto blocker = root / "file-parent"; std::ofstream(blocker) << "file";
    if (!check(!store_prepared_displacement_cache(blocker / "child", refined, policy),
            "unwritable parent was not diagnosed")) return false;
    return check(default_prepared_displacement_cache_directory({}).empty() &&
        default_prepared_displacement_cache_directory("cache.zip") == "prepared-displacement-v1" &&
        store_prepared_displacement_cache({}, refined, policy).has_value() &&
        load_prepared_displacement_cache({}, untouched, policy).value_or(true) == false,
        "empty persistence must remain disabled; bare archive must use a sibling directory");
}
} // namespace
const Register prepared_cache_test{prepared_cache_contract};
} // namespace heaven::testing
