/*
 * HeavenBench — Asset path resolution
 *
 * Feature: asset-path-resolution,: Texture filenames carry exactly
 * one `.dds` extension.
 *
 *
 *
 * For any Data_Root, texture category, and filename, the Resolved_Path returned
 * by PathResolver::texture_path ends with exactly one ".dds" sequence: the
 * extension is appended when (and only when) the path's final four characters
 * are not already ".dds" (matched case-sensitively), and is never appended a
 * second time when the path already ends in ".dds". Equivalently, the internal
 * ensure_dds rule is idempotent — applying it to an already-".dds" path is a
 * no-op.
 *
 * The oracle here is an INDEPENDENT reimplementation of the rule: it builds the
 * raw `data_root + category_fragment + filename` concatenation with literal
 * fragments (no code shared with PathResolver) and applies the ".dds" rule once
 * against the FULL path's final four characters. The property asserts the
 * resolver agrees with that oracle, that the result ends in ".dds", and that a
 * second application of the rule changes nothing (idempotence).
 *
 * The filename generator deliberately covers both branches of the rule: names
 * ending exactly in ".dds" (already correct — no append), names ending in
 * ".DDS" (uppercase — still gets ".dds" appended because the match is
 * case-sensitive), ".dd", "dds" (no dot), and arbitrary names.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "engine/path_resolver.h"

// The registry lives in test_main.cpp (external linkage, no shared header), so
// mirror its minimal interface here to append our test. The struct definition
// is token-identical to the one in test_main.cpp, satisfying the ODR.
namespace heaven::testing {

using TestFn = std::function<bool()>;

std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// ── Independent oracle ──

// Literal category fragments, duplicated here on purpose: the oracle must not
// share code with PathResolver or the property degenerates into comparing a
// function with itself.
std::string_view oracle_fragment(heaven::TextureCategory category) {
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

// Independent reimplementation of the ".dds" rule: append ".dds" iff the FULL
// path's final four characters are not already ".dds" (case-sensitive).
std::string oracle_ensure_dds(std::string path) {
    if (path.size() < 4 || path.substr(path.size() - 4) != ".dds")
        path += ".dds";
    return path;
}

bool run_path_resolver_dds_property() {
    // RapidCheck runs 100 successful cases by default (TestParams::maxSuccess
    // == 100), meeting the ">= 100 iterations" requirement; it can be raised
    // via RC_PARAMS="max_success=...".
    return rc::check(
        "Feature: asset-path-resolution, Property 2: texture filenames carry "
        "exactly one .dds extension (Validates: Requirements 2.2, 2.3)",
        [] {
            // Non-blank Data_Root: prefix a literal segment so create() always
            // succeeds, while the arbitrary tail still exercises trailing
            // separators, embedded dots, and non-ASCII bytes.
            const std::string data_root = "root" + *rc::gen::arbitrary<std::string>();

            // Pick one of the seven texture categories.
            const auto category = *rc::gen::element(
                heaven::TextureCategory::Environment,
                heaven::TextureCategory::Buildings,
                heaven::TextureCategory::Ruins,
                heaven::TextureCategory::Props,
                heaven::TextureCategory::Airship,
                heaven::TextureCategory::Sfx,
                heaven::TextureCategory::Core);

            // Filename generator: an arbitrary base optionally carrying one of
            // the boundary suffixes, covering both rule branches.
            //   ""     -> arbitrary name (and the already-suffixed cases below)
            //   ".dds" -> already correct, exact case: no second extension
            //   ".DDS" -> uppercase: case-sensitive match fails, ".dds" appended
            //   ".dd"  -> near-miss: ".dds" appended
            //   "dds"  -> no dot: ".dds" appended
            const std::string base = *rc::gen::arbitrary<std::string>();
            const std::string suffix = *rc::gen::element<std::string>(
                std::string(""), std::string(".dds"), std::string(".DDS"),
                std::string(".dd"), std::string("dds"));
            const std::string filename = base + suffix;

            auto resolver = heaven::PathResolver::create(data_root);
            RC_ASSERT(resolver.has_value());

            const std::string result = resolver->texture_path(category, filename);

            // Oracle: raw concatenation with the literal fragment, then the
            // ".dds" rule applied exactly once against the full path.
            const std::string raw =
                data_root + std::string(oracle_fragment(category)) + filename;
            const std::string expected = oracle_ensure_dds(raw);

            // The resolver agrees with the independent oracle:
            // exactly one extension, appended only on the case-sensitive miss.
            RC_ASSERT(result == expected);

            // The Resolved_Path always ends in ".dds".
            RC_ASSERT(result.size() >= 4);
            RC_ASSERT(result.substr(result.size() - 4) == ".dds");

            // Idempotence: applying the ".dds" rule a second time to the
            // result is a no-op — no second extension is ever added.
            RC_ASSERT(oracle_ensure_dds(result) == result);

            // When the input filename already ends in ".dds" (exact
            // case), the result is the raw concatenation with no appended
            // extension — i.e. no ".dds.dds" doubling is introduced by the
            // resolver beyond whatever the filename itself carried.
            const bool filename_already_dds =
                filename.size() >= 4 &&
                filename.substr(filename.size() - 4) == ".dds";
            if (filename_already_dds) {
                RC_ASSERT(result == raw);
            }
        });
}

// Append the property to the harness registry at static-init time.
const heaven::testing::Register g_register_path_resolver_dds{
    run_path_resolver_dds_property};

} // namespace
