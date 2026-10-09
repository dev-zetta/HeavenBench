/*
 * HeavenBench — PathResolver regression-anchor example tests
 *
 * These are EXAMPLE (not property) tests: they pin the concrete legacy
 * path strings the PathResolver must reproduce byte-for-byte for the
 * default Data_Root ("extracted/heaven"). They guard against any future
 * drift in the asset subdirectory fragments, the ".dds" extension rule,
 * the mesh/scene-file fragments, or the texture-role suffix family.
 *
 * Self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern (same mechanism the prop_*.cpp TUs
 * use), so the file(GLOB) in benchmark/CMakeLists.txt picks it up with no
 * entry-point edit. No rc::check is needed — each anchor is a plain
 * equality assertion that prints which anchor failed and returns false.
 *
 *
 */
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/path_resolver.h"

namespace heaven::testing {

// Mirror of the registry hooks defined in test_main.cpp. registry() is
// defined there (external linkage); Register is an identical inline
// definition, which ODR permits across translation units. Each test TU
// appends a callable returning true on success.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Check one concrete anchor: report the mismatch (with the anchor label, the
// expected string, and what was actually produced) and return false on failure.
bool check_anchor(const char* label,
                  const std::string& actual,
                  std::string_view expected) {
    if (actual == expected)
        return true;
    std::printf(
        "[unit_path_resolver_anchors] %s mismatch:\n  expected: \"%.*s\"\n  actual:   \"%s\"\n",
        label,
        static_cast<int>(expected.size()), expected.data(),
        actual.c_str());
    return false;
}

// Example test: every regression anchor for Data_Root = "extracted/heaven".
bool unit_path_resolver_anchors() {
    auto r = heaven::PathResolver::create("extracted/heaven");
    if (!r.has_value()) {
        std::printf(
            "[unit_path_resolver_anchors] PathResolver::create(\"extracted/heaven\") failed: %s\n",
            r.error().message.c_str());
        return false;
    }
    const heaven::PathResolver& paths = *r;

    if (paths.sky_asset_path(heaven::SkyAsset::MoonDisk) !=
        "extracted/heaven/sky/moon_disk.png" ||
        paths.sky_asset_path(heaven::SkyAsset::MoonNormal) !=
        "extracted/heaven/sky/moon_normal.png" ||
        paths.sky_asset_path(heaven::SkyAsset::MilkyWay) !=
        "extracted/heaven/sky/milkyway.png") {
        std::printf("[unit_path_resolver_anchors] sky asset paths changed\n");
        return false;
    }

    bool ok = true;

    // ── Scene files  ──
    // Note the doubled "heaven" segment: Data_Root already ends in "heaven"
    // and the scene fragment begins with "/heaven/...".
    ok = check_anchor("world_path()", paths.world_path(),
                      "extracted/heaven/heaven/heaven.world") && ok;

    auto node = paths.node_path("bridges.node");
    if (!node.has_value()) {
        std::printf(
            "[unit_path_resolver_anchors] node_path(\"bridges.node\") returned error: %s\n",
            node.error().message.c_str());
        ok = false;
    } else {
        ok = check_anchor("node_path(\"bridges.node\")", *node,
                          "extracted/heaven/heaven/nodes/bridges.node") && ok;
    }

    // ── Meshes  ──
    ok = check_anchor(
             "mesh_path(Environment, \"fly_island1.mesh\")",
             paths.mesh_path(heaven::MeshCategory::Environment, "fly_island1.mesh"),
             "extracted/heaven/heaven/meshes/environment/fly_island1.mesh") &&
         ok;
    ok = check_anchor(
             "mesh_path(Environment, \"fly_island2.mesh\")",
             paths.mesh_path(heaven::MeshCategory::Environment, "fly_island2.mesh"),
             "extracted/heaven/heaven/meshes/environment/fly_island2.mesh") &&
         ok;
    ok = check_anchor(
             "mesh_path(Environment, \"fly_island3.mesh\")",
             paths.mesh_path(heaven::MeshCategory::Environment, "fly_island3.mesh"),
             "extracted/heaven/heaven/meshes/environment/fly_island3.mesh") &&
         ok;

    // ── Cache / track directories  ──
    // cache_dir uses the "/cache/tessellation" suffix (no second "heaven"),
    // track_dir uses "/heaven/tracks" (doubled "heaven").
    ok = check_anchor("cache_dir()", paths.cache_dir(),
                      "extracted/heaven/cache/tessellation") && ok;
    ok = check_anchor("track_dir()", paths.track_dir(),
                      "extracted/heaven/heaven/tracks") && ok;

    // ── Textures  ──
    // The filename already ends in ".dds", so exactly one extension survives.
    ok = check_anchor(
             "texture_path(Sfx, \"fire_glow_dr.dds\")",
             paths.texture_path(heaven::TextureCategory::Sfx, "fire_glow_dr.dds"),
             "extracted/heaven/heaven/textures/sfx/fire_glow_dr.dds") &&
         ok;
    ok = check_anchor(
             "texture_path(Environment, \"grass_d.dds\")",
             paths.texture_path(heaven::TextureCategory::Environment, "grass_d.dds"),
             "extracted/heaven/heaven/textures/environment/grass_d.dds") &&
         ok;

    // ── Texture suffix family  ──
    // Must equal {"_d","_n","_s","_h","_t","_e","_dd"} in order.
    static constexpr std::string_view k_expected_suffixes[] = {
        "_d", "_n", "_s", "_h", "_t", "_e", "_dd",
    };
    const auto suffixes = heaven::PathResolver::texture_suffixes();
    constexpr std::size_t k_expected_count =
        sizeof(k_expected_suffixes) / sizeof(k_expected_suffixes[0]);
    if (suffixes.size() != k_expected_count) {
        std::printf(
            "[unit_path_resolver_anchors] texture_suffixes() size mismatch: expected %zu, got %zu\n",
            k_expected_count, suffixes.size());
        ok = false;
    } else {
        for (std::size_t i = 0; i < k_expected_count; ++i) {
            if (suffixes[i] != k_expected_suffixes[i]) {
                std::printf(
                    "[unit_path_resolver_anchors] texture_suffixes()[%zu] mismatch: expected \"%.*s\", got \"%.*s\"\n",
                    i,
                    static_cast<int>(k_expected_suffixes[i].size()),
                    k_expected_suffixes[i].data(),
                    static_cast<int>(suffixes[i].size()), suffixes[i].data());
                ok = false;
            }
        }
    }

    return ok;
}

// Register the example test at static-init time.
const heaven::testing::Register reg_unit_path_resolver_anchors{
    &unit_path_resolver_anchors};

} // namespace
