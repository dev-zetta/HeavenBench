/*
 * HeavenBench — asset-path-resolution
 *
 * Feature: asset-path-resolution,: Resolver output is
 * byte-for-byte identical to the legacy inline construction.
 *
 *
 *
 *
 * For any non-empty Data_Root and for any valid set of asset inputs, the
 * value returned by the corresponding PathResolver method must be
 * character-for-character identical to the string produced by a reference
 * implementation that performs the exact pre-refactor inline concatenation
 * for that call site (raw `+`, no normalization). This is the umbrella
 * property covering every resolution arm:
 *
 *   texture_path (all seven categories), texture_candidate, mesh_path,
 *   world_path, node_path (success branch), cache_dir, track_dir,
 *   relative_path, displacement_path.
 *
 * The oracle below is an INDEPENDENT reference implementation: it shares no
 * code with PathResolver and reproduces the legacy fragments and the raw `+`
 * concatenation from the current call-site migration table directly. If the
 * resolver ever drifts from the legacy strings (separator handling, segment
 * order, the `.dds` rule), this property fails with the exact counterexample.
 *
 * The Data_Root generator deliberately includes trailing-`/`, multiple
 * trailing separators, embedded `..`/`.`, whitespace, and non-ASCII bytes so
 * the no-normalization edge cases  are exercised. Data_Root is kept
 * non-blank so construction succeeds (empty/whitespace rejection is Property
 * 7); node filenames are kept non-blank so node_path takes its success branch
 * (the rejection branch is).
 *
 * This file self-registers with the RapidCheck harness in test_main.cpp via
 * heaven::testing::Register, exactly like the other benchmark test TUs.
 */
#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "engine/path_resolver.h"

namespace heaven::testing {

// Mirror of the registry hooks defined in test_main.cpp. registry() is defined
// there (external linkage); Register is an identical inline definition. Each
// test TU appends a callable returning true on success.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// ── Independent reference implementation (the oracle) ──────────────────

// These functions DO NOT call into PathResolver. They reproduce the legacy
// inline concatenation by the implementation migration table using raw `+`, so an
// agreement check is a genuine cross-check rather than a tautology.
namespace ref {

// Empty-or-all-whitespace test (mirrors the resolver's construction guard so
// the generators only produce inputs that take the success branch).
bool is_blank(std::string_view s) {
    return std::ranges::all_of(s, [](unsigned char c) { return std::isspace(c) != 0; });
}

// Legacy ".dds" rule: append ".dds" iff the final four characters are not
// already ".dds" (case-sensitive).
std::string ensure_dds(std::string path) {
    if (path.size() < 4 || path.substr(path.size() - 4) != ".dds")
        path += ".dds";
    return path;
}

// Legacy texture category directory fragments (each begins and ends in "/").
std::string texture_fragment(heaven::TextureCategory category) {
    switch (category) {
        case heaven::TextureCategory::Environment: return "/heaven/textures/environment/";
        case heaven::TextureCategory::Buildings:   return "/heaven/textures/buildings/";
        case heaven::TextureCategory::Ruins:       return "/heaven/textures/ruins/";
        case heaven::TextureCategory::Props:       return "/heaven/textures/props/";
        case heaven::TextureCategory::Airship:     return "/heaven/textures/airship/";
        case heaven::TextureCategory::Sfx:         return "/heaven/textures/sfx/";
        case heaven::TextureCategory::Core:        return "/core/textures/";
    }
    return {};
}

std::string texture_path(const std::string& root,
                         heaven::TextureCategory category,
                         const std::string& filename) {
    return ensure_dds(root + texture_fragment(category) + filename);
}

std::string texture_candidate(const std::string& search_dir,
                              const std::string& base,
                              const std::string& suffix) {
    return search_dir + base + suffix + ".dds";
}

std::string mesh_path(const std::string& root, const std::string& name) {
    return root + "/heaven/meshes/environment/" + name;
}

std::string world_path(const std::string& root) {
    return root + "/heaven/heaven.world";
}

std::string node_path(const std::string& root, const std::string& filename) {
    return root + "/heaven/nodes/" + filename;
}

std::string cache_dir(const std::string& root) {
    return root + "/cache/tessellation";
}

std::string track_dir(const std::string& root) {
    return root + "/heaven/tracks";
}

std::string relative_path(const std::string& root, const std::string& rel) {
    return root + "/" + rel;
}

std::string displacement_path(const std::string& root, const std::string& rel) {
    return ensure_dds(root + "/" + rel);
}

} // namespace ref

// ── Generators ─────────────────────────────────────────────────────────

// A single Data_Root "piece". The set deliberately mixes normal segments with
// pathological pieces — trailing separators, embedded ".."/".", whitespace,
// and non-ASCII (UTF-8) bytes — so that joined Data_Root values exercise the
// resolver's no-normalization guarantee.
rc::Gen<std::string> data_root_piece() {
    return rc::gen::elementOf(std::vector<std::string>{
        "extracted/heaven", "data", "assets", "root", "rel",
        "..", ".", "a/..", "./b",
        "/", "//", "dir/", "trail//", "C:/win",
        " ", "  ", "\t", "a b",
        "h\u00E9aven",       // "héaven" (UTF-8 é)
        "\u00FCnicode",      // "ünicode" (UTF-8 ü)
        "\xE2\x9C\x93",      // "✓" (UTF-8 check mark)
    });
}

// A non-blank Data_Root built by concatenating random pieces. suchThat
// discards the rare all-whitespace/empty combinations so create() succeeds
// (operates on non-empty roots;  owns rejection).
rc::Gen<std::string> data_root_gen() {
    auto joined = rc::gen::map(
        rc::gen::container<std::vector<std::string>>(data_root_piece()),
        [](const std::vector<std::string>& parts) {
            std::string s;
            for (const auto& p : parts)
                s += p;
            return s;
        });
    return rc::gen::suchThat(joined,
                             [](const std::string& s) { return !ref::is_blank(s); });
}

// Texture filename generator covering both ".dds" branches plus concrete
// regression anchors and arbitrary names.
rc::Gen<std::string> filename_gen() {
    return rc::gen::oneOf(
        rc::gen::arbitrary<std::string>(),
        rc::gen::map(rc::gen::arbitrary<std::string>(),
                     [](const std::string& s) { return s + ".dds"; }),
        rc::gen::elementOf(std::vector<std::string>{
            "clouds1_d.dds", "clouds_mask0_d.dds", "grass_d.dds",
            "grass_irregularity_d.dds", "fire_glow_dr.dds", "spark_d.dds",
            "rocks_h.dds", "noext", "x.DDS", "y.dd", "dds", ".dds", "",
        }));
}

// A non-blank arbitrary string, for node filenames (success branch).
rc::Gen<std::string> nonblank_gen() {
    return rc::gen::suchThat(rc::gen::arbitrary<std::string>(),
                             [](const std::string& s) { return !ref::is_blank(s); });
}

// Returns true when every RapidCheck case passes. RapidCheck's
// default maxSuccess is 100, so this runs >= 100 iterations.
bool property_1_byte_for_byte_equivalence() {
    return rc::check(
        "Property 1: resolver output is byte-for-byte identical to the legacy "
        "inline construction",
        [] {
            const std::string data_root = *data_root_gen();

            // Construction must succeed for a non-blank Data_Root,
            // and the resolver must report it back verbatim.
            auto created = heaven::PathResolver::create(data_root);
            RC_ASSERT(created.has_value());
            const heaven::PathResolver& resolver = *created;
            RC_ASSERT(resolver.data_root() == data_root);

            // ── texture_path: every one of the seven categories ──
            const std::string filename = *filename_gen();
            const std::array<heaven::TextureCategory, 7> categories = {
                heaven::TextureCategory::Environment,
                heaven::TextureCategory::Buildings,
                heaven::TextureCategory::Ruins,
                heaven::TextureCategory::Props,
                heaven::TextureCategory::Airship,
                heaven::TextureCategory::Sfx,
                heaven::TextureCategory::Core,
            };
            for (auto category : categories) {
                RC_ASSERT(resolver.texture_path(category, filename) ==
                          ref::texture_path(data_root, category, filename));
            }

            // ── texture_candidate ──
            const std::string search_dir = *rc::gen::arbitrary<std::string>();
            const std::string base = *rc::gen::arbitrary<std::string>();
            const std::string suffix = *rc::gen::arbitrary<std::string>();
            RC_ASSERT(resolver.texture_candidate(search_dir, base, suffix) ==
                      ref::texture_candidate(search_dir, base, suffix));

            // ── mesh_path (the one explicit mesh subdirectory) ──
            const std::string mesh_name = *rc::gen::arbitrary<std::string>();
            RC_ASSERT(resolver.mesh_path(heaven::MeshCategory::Environment, mesh_name) ==
                      ref::mesh_path(data_root, mesh_name));

            // ── world_path ──
            RC_ASSERT(resolver.world_path() == ref::world_path(data_root));

            // ── node_path (success branch: non-blank filename) ──
            const std::string node_file = *nonblank_gen();
            auto node = resolver.node_path(node_file);
            RC_ASSERT(node.has_value());
            RC_ASSERT(*node == ref::node_path(data_root, node_file));

            // ── cache_dir / track_dir ──
            RC_ASSERT(resolver.cache_dir() == ref::cache_dir(data_root));
            RC_ASSERT(resolver.track_dir() == ref::track_dir(data_root));

            // ── relative_path ──
            const std::string rel = *rc::gen::arbitrary<std::string>();
            RC_ASSERT(resolver.relative_path(rel) == ref::relative_path(data_root, rel));

            // ── displacement_path ──
            const std::string disp_rel = *rc::gen::arbitrary<std::string>();
            RC_ASSERT(resolver.displacement_path(disp_rel) ==
                      ref::displacement_path(data_root, disp_rel));
        });
}

// Register the property at static-init time.
const heaven::testing::Register reg_property_1{&property_1_byte_for_byte_equivalence};

} // namespace
