// Feature: used-asset-packer,: Target layout preserves the relative path
/*
 * HeavenBench — Property test for the used-asset packer's layout-preserving
 * path mapping (pack_target_path / source_relative).
 *
 * Feature: used-asset-packer
 *   •: Target layout preserves the relative path
 *
 *
 * (target layout preserves the relative path):
 *   For any Pack_Target root and any Relative_Asset_Path `rel`,
 *   pack_target_path(root, rel) is the lexical join `root / rel`, and the
 *   resulting path taken relative to `root` equals `rel` — the file's position
 *   under the Pack_Target mirrors its position under the Source_Tree.
 *
 * Strategy:
 *   A random root path and a random Relative_Asset_Path are each built from
 *   clean path segments — non-empty names drawn from a fixed vocabulary with no
 *   "." / ".." components, no embedded slashes, and no leading slash on `rel`.
 *   Constraining the input space this way avoids filesystem-normalization
 *   ambiguity (e.g. ".."-collapsing or separator coalescing) that would make
 *   the lexical inverse undefined, while still covering arbitrary depth,
 *   arbitrary names, and both relative and absolute roots.
 *
 *   The property joins the segments into a generic-form ('/') relative string
 *   `rel`, computes pack_target_path(root, rel), maps it back with
 *   source_relative(root, …), and asserts the round-trip recovers `rel`
 *   exactly. source_relative returns a generic_string, so comparing against the
 *   '/'-joined `rel` is platform-independent.
 *
 * Both functions are pure (purely lexical, no I/O, no GPU), so this runs on the
 * host `heaven_benchmark_tests` target. The file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register pattern,
 * so the file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the
 * entry point. RapidCheck's default of 100 successful iterations meets the
 * "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "asset_packer/asset_packer.h"

// Mirror the minimal registry surface defined in test_main.cpp. `registry()`
// has external linkage and resolves to the single definition there; the inline
// `Register` constructor is token-identical across test TUs (ODR-safe).
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();
struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

// Fixed vocabulary of clean path segment names: each is non-empty, contains no
// path separator, and is neither "." nor ".." — so a sequence of them joined by
// '/' is a Relative_Asset_Path with an unambiguous lexical inverse.
const std::vector<std::string>& segment_vocab() {
    static const std::vector<std::string> vocab = {
        "a", "bb", "ccc", "heaven", "core", "textures", "meshes",
        "node01", "x_y", "lod_0", "airship", "balloon_d",
    };
    return vocab;
}

// Join clean segments into a generic-form ('/') path string.
std::string join_segments(const std::vector<std::string>& segs) {
    std::string out;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        if (i != 0) out += '/';
        out += segs[i];
    }
    return out;
}

// Generate a non-empty list of clean segments (depth 1..4) drawn from the vocab.
std::vector<std::string> gen_clean_segments() {
    const int n = *rc::gen::inRange(1, 5);
    std::vector<std::string> segs;
    segs.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        segs.push_back(*rc::gen::elementOf(segment_vocab()));
    }
    return segs;
}

bool run_p7_layout_property() {
    return rc::check(
        "Feature: used-asset-packer, Property 7: Target layout preserves the "
        "relative path (Validates: Requirements 3.2)",
        [] {
            // Random Pack_Target root: clean segments, optionally absolute so
            // both relative and absolute roots are exercised.
            const bool absolute_root = *rc::gen::arbitrary<bool>();
            const std::string root_str =
                (absolute_root ? "/" : "") + join_segments(gen_clean_segments());
            const std::filesystem::path root(root_str);

            // Random Relative_Asset_Path: clean segments, never leading-slash.
            const std::string rel = join_segments(gen_clean_segments());

            // pack_target_path is the lexical join root / rel; taking that back
            // relative to root must recover rel exactly (generic '/' form).
            const std::filesystem::path target =
                heaven::pack_target_path(root, rel);
            const std::string recovered = heaven::source_relative(root, target);

            RC_ASSERT(recovered == rel);
        });
}

const heaven::testing::Register g_register_packer_layout{
    &run_p7_layout_property};

} // namespace
