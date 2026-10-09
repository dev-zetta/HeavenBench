// Feature: background-music-playback,
/*
 * HeavenBench — Property test for the case-insensitive `.oga` classifier
 * used by the used-asset packer's Source_Tree scan.
 *
 * Feature: background-music-playback
 *   •: The `.oga` extension is classified case-insensitively and
 *     counted once
 *
 *
 * The background-music feature teaches the packer to treat the Ogg Vorbis music
 * track (`heaven/heaven.oga`) as an Asset_File. Because the original
 * scan_source_tree did an exact, case-sensitive suffix match, the implementation adds a
 * pure classifier:
 *
 *     bool has_extension_ci(std::string_view name, std::string_view ext);
 *
 * which is true exactly when `name` ends in `ext` compared case-insensitively.
 * The `.oga` pass of scan_source_tree filters its directory hits through this
 * classifier, so a `.oga` / `.OGA` / `.Oga` file is recognised regardless of
 * its on-disk case spelling and is counted exactly once.
 *
 * has_extension_ci is pure (no I/O, no GPU, no Filament/SDL) and is compiled
 * into the host `heaven_benchmark_tests` target via asset_packer.cpp, so this
 * property runs on the host with RapidCheck and no real filesystem.
 *
 * — two halves:
 *
 *   (a) Classification matches an independent oracle. For arbitrary filenames —
 *       including names deliberately ending in a random-case permutation of
 *       ".oga" — has_extension_ci(name, ".oga") equals an independently
 *       computed case-insensitive "ends with.oga" predicate. This holds for
 *       BOTH the matching and non-matching cases, so the classifier neither
 *       misses a true ".oga" suffix in any case spelling nor accepts a name
 *       that does not end in ".oga".
 *
 *   (b) Counted once. The real scan collects matching Relative_Asset_Paths into
 *       a std::set, filtering each candidate through has_extension_ci. Modelling
 *       that here, a fixture set passed through the classifier-keyed set yields
 *       exactly one entry per matching file (no double-count across case
 *       variants, and a single physical file is never counted more than once).
 *       The on-disk filesystem walk and PathResolver join in scan_source_tree
 *       are I/O shell over this same pure classifier (covered structurally /
 *       by the packer integration tests), so this property targets the
 *       classifier contract that determines the scan's count.
 *
 * The file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/
 * CMakeLists.txt picks it up without editing the entry point. RapidCheck's
 * default of 100 successful iterations meets the "≥ 100 iterations" bar.
 */
#include <cctype>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <string_view>
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

// Independent oracle: true when `name` ends in `ext` compared
// case-insensitively. Deliberately written differently from the production
// has_extension_ci (lowercases a copy of the relevant tail and compares to a
// lowercased ext) so the property checks behaviour, not an identical algorithm.
bool ends_with_ci_oracle(std::string_view name, std::string_view ext) {
    if (name.size() < ext.size()) {
        return false;
    }
    auto lower = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    const std::string tail =
        lower(std::string(name.substr(name.size() - ext.size())));
    const std::string want = lower(std::string(ext));
    return tail == want;
}

// Generate a random-case permutation of ".oga": the leading '.' is invariant,
// each of 'o','g','a' is independently upper- or lower-cased. Exercises the
// full space of case spellings of the extension.
std::string gen_random_case_oga() {
    std::string out = ".";
    for (char base : std::string("oga")) {
        const bool upper = *rc::gen::arbitrary<bool>();
        out.push_back(upper
                          ? static_cast<char>(std::toupper(static_cast<unsigned char>(base)))
                          : base);
    }
    return out;
}

// Generate a filename that does NOT end in ".oga" case-insensitively. Built by
// appending a non-.oga suffix to an arbitrary base; rejected via the oracle so
// the (rare) base that already ends in ".oga" cannot leak into the negative
// pool.
std::string gen_non_oga_name() {
    static const std::vector<std::string> non_oga_suffixes = {
        "", ".dds", ".mat", ".node", ".world", ".mesh", ".track", ".ogg",
        ".og", ".oga.bak", "oga", ".oga2", ".txt", "a", "music",
    };
    const std::string base = *rc::gen::arbitrary<std::string>();
    const std::string suffix = *rc::gen::elementOf(non_oga_suffixes);
    std::string name = base + suffix;
    // Discard the unlucky case where the arbitrary base+suffix still ends in
    // ".oga" (e.g. base ended with ".og" and suffix "a"); the negative arm of
    // the property requires a genuine non-match.
    RC_PRE(!ends_with_ci_oracle(name, ".oga"));
    return name;
}

// ── (a): classification matches the independent oracle ──
bool run_p6_classifier_matches_oracle() {
    return rc::check(
        "Feature: background-music-playback, Property 6: The `.oga` extension "
        "is classified case-insensitively and counted once — classifier matches "
        "oracle (Validates: Requirements 8.3)",
        [] {
            // Three arms, all asserting has_extension_ci == oracle:
            //   0: arbitrary base + random-case ".oga"  → must classify true
            //   1: a deliberately non-.oga name          → must classify false
            //   2: a wholly arbitrary string             → either, but agree
            const int arm = *rc::gen::inRange(0, 3);

            std::string name;
            if (arm == 0) {
                name = *rc::gen::arbitrary<std::string>() + gen_random_case_oga();
            } else if (arm == 1) {
                name = gen_non_oga_name();
            } else {
                name = *rc::gen::arbitrary<std::string>();
            }

            const bool got = heaven::has_extension_ci(name, ".oga");
            const bool want = ends_with_ci_oracle(name, ".oga");
            RC_ASSERT(got == want);

            // Arm 0 deterministically ends in a case spelling of ".oga", so the
            // classifier MUST accept it; arm 1 deterministically does not, so it
            // MUST reject it. This guarantees both branches are actually hit.
            if (arm == 0) RC_ASSERT(got);
            if (arm == 1) RC_ASSERT(!got);
        });
}

// ── (b): the classifier counts a matching file exactly once ──

// Models scan_source_tree's `.oga` pass: it walks the tree and, for each file,
// admits it to the scan set when has_extension_ci(name, ".oga") is true,
// collecting Relative_Asset_Paths into a std::set. Over a fixture of candidate
// filenames the number of admitted entries equals the number of DISTINCT names
// that the oracle says end in ".oga", and a given matching path is present
// exactly once (set-dedup), so it can never be double-counted across case
// variants.
bool run_p6_counted_once() {
    return rc::check(
        "Feature: background-music-playback, Property 6: The `.oga` extension "
        "is classified case-insensitively and counted once — matching file "
        "counted exactly once (Validates: Requirements 8.3)",
        [] {
            // A designated music file in a random-case ".oga" spelling, plus a
            // handful of arbitrary other candidate names (which may or may not
            // themselves end in ".oga").
            const std::string music_name =
                *rc::gen::arbitrary<std::string>() + gen_random_case_oga();

            std::vector<std::string> candidates =
                *rc::gen::container<std::vector<std::string>>(
                    rc::gen::arbitrary<std::string>());
            candidates.push_back(music_name);

            // Run the scan model: admit a candidate iff the classifier accepts
            // it, collecting into a std::set exactly as scan_source_tree does.
            std::set<std::string> scanned;
            for (const std::string& c : candidates) {
                if (heaven::has_extension_ci(c, ".oga")) {
                    scanned.insert(c);
                }
            }

            // Independent oracle: the set of DISTINCT candidate names that end
            // in ".oga" case-insensitively.
            std::set<std::string> expected;
            for (const std::string& c : candidates) {
                if (ends_with_ci_oracle(c, ".oga")) {
                    expected.insert(c);
                }
            }

            // The classifier-driven scan set equals the oracle's set: every
            // matching file is admitted, nothing else is, and each appears once.
            RC_ASSERT(scanned == expected);

            // The designated music file is present, and counted exactly once
            // (a std::set cannot hold a duplicate, so its membership count is 1).
            RC_ASSERT(scanned.count(music_name) == 1u);
        });
}

bool run_music_oga_ext_properties() {
    bool ok = true;
    ok = run_p6_classifier_matches_oracle() && ok;
    ok = run_p6_counted_once() && ok;
    return ok;
}

const heaven::testing::Register g_register_music_oga_ext{
    &run_music_oga_ext_properties};

} // namespace
