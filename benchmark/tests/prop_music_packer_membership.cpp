// Feature: background-music-playback,
/*
 * HeavenBench — Property test for the used-asset packer's music-membership
 * decision (the inclusion/exclusion rule in discover_used_assets step 5).
 *
 * Feature: background-music-playback
 *   •: Packer includes the music asset exactly once iff music is
 *     enabled
 *
 *
 * The packer's music decision (asset_packer.cpp) is: when config.music.enabled
 * AND the Music_Track is resolvable/readable, record_used(paths.music_path())
 * adds the Music_Asset_Path to the Used_Asset_Set; when music is disabled,
 * nothing referencing the Music_Asset_Path is recorded. The Used_Asset_Set is
 * the AssetUsageRecorder's deduping std::set keyed by Relative_Asset_Path, so
 * the inclusion is "exactly one" by construction and repeated recording of the
 * same on-disk path adds no duplicate.
 *
 * This property exercises that decision rule over random recorder states
 * directly against the AssetUsageRecorder — the GPU/SDL/Filament-free
 * accumulator that the renderer's resolution path feeds — so it runs on the
 * host `heaven_benchmark_tests` target. The recorder normalizes a recorded
 * on-disk path via weakly_canonical (which does NOT require the file to exist)
 * and makes it relative to the Source_Tree root. We therefore model the
 * "enabled + resolvable" branch by calling record_used on the absolute music
 * path (= Source_Tree root + k_music_rel_path) a random number of times, and
 * the "disabled" branch by NOT recording it.
 *
 * To make the recorded path normalize to exactly k_music_rel_path
 * ("heaven/heaven.oga"), the recorder's Source_Tree root is a REAL temp
 * directory that we canonicalize the same way weakly_canonical does, then the
 * absolute music path is `<root>/heaven/heaven.oga`. weakly_canonical resolves
 * the existing `<root>` prefix and appends the (non-existent) tail, so the
 * lexically-relative result is exactly k_music_rel_path with no dependence on
 * the music file existing on disk.
 *
 * GATING: the Source_Tree root is created under std::filesystem::
 * temp_directory_path(). If that temp directory is unavailable (read-only
 * sandbox, etc.) the test SKIPS gracefully (prints a notice, returns true),
 * mirroring integ_packer_parity.cpp.
 *
 * The file self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/
 * CMakeLists.txt picks it up without editing the entry point. RapidCheck's
 * default of 100 successful iterations meets the "≥ 100 iterations" bar.
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include <rapidcheck.h>

#include "engine/asset_usage_recorder.h"
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

namespace fs = std::filesystem;

// Join a Relative_Asset_Path onto the Source_Tree root, yielding the absolute
// on-disk path the resolution code would hand to record_used().
std::string abs_under(const fs::path& root, std::string_view rel) {
    return (root / fs::path(rel)).string();
}

bool run_p5_packer_membership_property() {
    // ── Establish a real, canonical Source_Tree root ──

    // weakly_canonical canonicalizes the existing leading portion of a path, so
    // the recorder's root must be canonicalized identically for the recorded
    // music path to reduce to exactly k_music_rel_path. We create a real temp
    // directory (it need not contain the music file) and canonicalize it.
    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[prop_music_packer_membership] SKIPPED: no temp dir "
                    "available (%s)\n", ec.message().c_str());
        return true;
    }

    static std::atomic<unsigned> counter{0};
    const fs::path raw_root =
        temp_base / ("heaven_music_membership_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(raw_root, ec);
    if (!fs::create_directories(raw_root, ec) && ec) {
        std::printf("[prop_music_packer_membership] SKIPPED: cannot create temp "
                    "root '%s' (%s)\n", raw_root.string().c_str(), ec.message().c_str());
        return true;
    }

    const fs::path source_root = fs::weakly_canonical(raw_root, ec);
    if (ec) {
        std::printf("[prop_music_packer_membership] SKIPPED: cannot canonicalize "
                    "temp root '%s' (%s)\n", raw_root.string().c_str(), ec.message().c_str());
        fs::remove_all(raw_root, ec);
        return true;
    }

    const std::string music_rel(heaven::k_music_rel_path);     // "heaven/heaven.oga"
    const std::string music_abs = abs_under(source_root, music_rel);

    const bool result = rc::check(
        "Feature: background-music-playback, Property 5: Packer includes the "
        "music asset exactly once iff music is enabled (Validates: Requirements "
        "8.1, 8.2)",
        [&] {
            // Random recorder state: whether music is enabled, how many times
            // the enabled+resolvable branch records the same music path (≥ 1),
            // and a set of unrelated used assets recorded alongside it.
            const bool enabled = *rc::gen::arbitrary<bool>();
            const int  repeat  = enabled ? *rc::gen::inRange(1, 6) : 0;
            const int  others  = *rc::gen::inRange(0, 5);

            heaven::AssetUsageRecorder recorder(source_root.string());

            // Unrelated assets, each a distinct Relative_Asset_Path that is
            // never the Music_Asset_Path — these stand in for the scene/material
            // track members the packer records before the music decision.
            for (int i = 0; i < others; ++i) {
                const std::string rel = "other/file_" + std::to_string(i) + ".dat";
                recorder.record_used(abs_under(source_root, rel));
            }

            // The music decision. Enabled + resolvable ⇒ record_used on the
            // absolute music path, possibly repeatedly. Disabled ⇒ record
            // nothing referencing the Music_Asset_Path.
            if (enabled) {
                for (int i = 0; i < repeat; ++i) recorder.record_used(music_abs);
            }

            const std::set<std::string>& used = recorder.used();

            // The Used_Asset_Set contains the Music_Asset_Path exactly when
            // music is enabled — exactly one entry when enabled (the set dedups
            // the repeated recordings), zero when disabled.
            const std::size_t music_members = used.count(music_rel);
            RC_ASSERT(music_members == (enabled ? 1u : 0u));

            // Repeated recording adds no duplicate: the total membership is the
            // distinct unrelated assets plus the single music entry (if any).
            const std::size_t expected_size =
                static_cast<std::size_t>(others) + (enabled ? 1u : 0u);
            RC_ASSERT(used.size() == expected_size);
        });

    fs::remove_all(source_root, ec);
    return result;
}

const heaven::testing::Register g_register_music_packer_membership{
    &run_p5_packer_membership_property};

} // namespace
