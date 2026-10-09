// Feature: background-music-playback,: The music asset resolves under Data_Root and mirrors into the Pack_Target preserving its relative path
/*
 * HeavenBench — Property test for the background-music track's path
 * identity: its resolution under the Data_Root and the layout mirror into the
 * Pack_Target.
 *
 * Feature: background-music-playback
 *   •: The music asset resolves under Data_Root and mirrors into the
 *     Pack_Target preserving its relative path
 *
 *
 * (music path identity + Pack_Target mirror):
 *   For any Data_Root and any Pack_Target root, two halves hold against the
 *   single shared Relative_Asset_Path constant k_music_rel_path
 *   ("heaven/heaven.oga"):
 *
 *     (a) Data_Root resolution: PathResolver::music_path() equals the
 *         verbatim Data_Root join with k_music_rel_path — i.e.
 *         data_root + "/" + k_music_rel_path — the exact relative_path()
 *         concatenation rule, with no normalization. PathResolver::create
 *         rejects an empty/whitespace-only Data_Root, so the generator only
 *         produces non-blank roots and create() is asserted to succeed.
 *
 *     (b) Pack_Target mirror: pack_target_path(root, k_music_rel_path)
 *         is the lexical join `root / k_music_rel_path`, and that path taken
 *         relative to `root` (via source_relative) recovers k_music_rel_path
 *         exactly — so the track's position under the Pack_Target mirrors its
 *         position under the Source_Tree.
 *
 * Strategy:
 *   The Pack_Target root is built from clean path segments — non-empty names
 *   from a fixed vocabulary with no "."/".." components, no embedded slashes,
 *   and an optional leading slash so both relative and absolute roots are
 *   exercised. Constraining the root this way avoids filesystem-normalization
 *   ambiguity that would make the lexical inverse undefined. k_music_rel_path
 *   is a fixed generic-form ('/') relative path, so source_relative (which
 *   returns a generic_string) recovers it platform-independently. The
 *   Data_Root for half (a) reuses the same clean-segment builder, which is
 *   always non-blank, so create() always succeeds.
 *
 * music_path / pack_target_path / source_relative are all pure (purely lexical,
 * no I/O, no GPU), so this runs on the host `heaven_benchmark_tests` target.
 * The file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point.
 * RapidCheck's default of 100 successful iterations meets the "≥ 100
 * iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <rapidcheck.h>

#include "asset_packer/asset_packer.h"
#include "engine/path_resolver.h"
#include "engine/types.h"

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
// '/' is a path with an unambiguous lexical inverse and is never blank.
const std::vector<std::string>& segment_vocab() {
    static const std::vector<std::string> vocab = {
        "a", "bb", "ccc", "heaven", "core", "textures", "meshes",
        "node01", "x_y", "lod_0", "airship", "balloon_d", "extracted",
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

bool run_p7_music_path_identity() {
    return rc::check(
        "Feature: background-music-playback, Property 7: The music asset "
        "resolves under Data_Root and mirrors into the Pack_Target preserving "
        "its relative path (Validates: Requirements 4.1, 8.4)",
        [] {
            const std::string music_rel(heaven::k_music_rel_path);

            // ── (a) Data_Root resolution  ──
            // A non-blank Data_Root, optionally absolute. Clean segments are
            // never blank, so create() must succeed and music_path() must equal
            // the verbatim data_root + "/" + k_music_rel_path join.
            const bool absolute_root = *rc::gen::arbitrary<bool>();
            const std::string data_root =
                (absolute_root ? "/" : "") + join_segments(gen_clean_segments());

            auto created = heaven::PathResolver::create(data_root);
            RC_ASSERT(created.has_value());
            const heaven::PathResolver& resolver = *created;
            RC_ASSERT(resolver.music_path() == data_root + "/" + music_rel);

            // ── (b) Pack_Target mirror  ──
            // A random Pack_Target root, optionally absolute. The music track's
            // relative path mapped into the target and taken back relative to
            // the root recovers k_music_rel_path exactly (generic '/' form).
            const bool absolute_target = *rc::gen::arbitrary<bool>();
            const std::string target_root_str =
                (absolute_target ? "/" : "") + join_segments(gen_clean_segments());
            const std::filesystem::path target_root(target_root_str);

            const std::filesystem::path target =
                heaven::pack_target_path(target_root, music_rel);
            const std::string recovered =
                heaven::source_relative(target_root, target);

            RC_ASSERT(recovered == music_rel);
        });
}

const heaven::testing::Register g_register_music_path_identity{
    &run_p7_music_path_identity};

} // namespace
