/*
 * HeavenBench — unit test: AssetUsageRecorder (used-asset packer tap)
 *
 *
 *
 * AssetUsageRecorder is the GPU-free accumulator the packer threads into the
 * renderer's resolution path. Three behaviours are pinned here:
 *
 *   - record_used is idempotent and content-addressed by Relative_Asset_Path:
 *     the same on-disk file recorded twice — including via a different but
 *     equivalent spelling that normalizes to the same canonical path — yields a
 *     single entry in used() (at-most-once).
 *
 *   - record_used normalizes paths and makes them relative to the Source_Tree
 *     root, so a path reached through "heaven/../core/…" collapses to a stable
 *     "core/…" Relative_Asset_Path while a directly-rooted "heaven/…" path
 *     yields "heaven/…". This is the stable, traversal-order-independent
 *     identity used for the deterministic used set.
 *
 *   - record_unresolved deduplicates on the (missing, referrer) pair while
 *     preserving first-seen insertion order, so the Pack_Report lists each
 *     distinct broken edge once in a deterministic order.
 *
 * GATING: the test builds its own temp Source_Tree under
 * std::filesystem::temp_directory_path() (heaven/ + core/ siblings) so that
 * weakly_canonical + lexically_relative are deterministic and it does not
 * depend on the gitignored extracted asset tree. If the temp directory cannot
 * be created (read-only sandbox, etc.) it SKIPS gracefully (prints a notice and
 * returns success) rather than failing spuriously.
 *
 * The test self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, matching
 * the other test translation units; the CMake test-source glob picks it up
 * without a CMake edit. The registered callable returns true on success and
 * false (after printing a diagnostic) on failure.
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "asset_packer/asset_packer.h" // heaven::Unresolved
#include "engine/asset_usage_recorder.h"

// Mirror of the harness registry hooks declared in test_main.cpp. `registry()`
// is a free function with external linkage defined there; the Register helper's
// definition is token-identical so it can be reused from this translation unit.
namespace heaven::testing {

using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

namespace fs = std::filesystem;

bool unit_packer_recorder() {
    bool ok = true;

    // ── Build a real temp Source_Tree root with heaven/ + core/ siblings ──

    // Real directories make weakly_canonical (which resolves the longest
    // existing prefix) and lexically_relative behave deterministically across
    // platforms. If setup fails, SKIP gracefully.
    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[unit_packer_recorder] SKIPPED: no temp dir available (%s)\n",
                    ec.message().c_str());
        return true;
    }

    static std::atomic<unsigned> counter{0};
    const fs::path root =
        temp_base / ("heaven_packer_recorder_" + std::to_string(counter.fetch_add(1)));

    fs::remove_all(root, ec);
    fs::create_directories(root / "heaven" / "textures", ec);
    if (ec) {
        std::printf("[unit_packer_recorder] SKIPPED: cannot create temp tree '%s' (%s)\n",
                    root.string().c_str(), ec.message().c_str());
        return true;
    }
    fs::create_directories(root / "core" / "materials", ec);
    if (ec) {
        std::printf("[unit_packer_recorder] SKIPPED: cannot create temp tree '%s' (%s)\n",
                    root.string().c_str(), ec.message().c_str());
        fs::remove_all(root, ec);
        return true;
    }

    // The recorder is constructed from the canonical Source_Tree root so the
    // Relative_Asset_Paths it derives match the canonical form of the recorded
    // files.
    fs::path canon_root = fs::weakly_canonical(root, ec);
    if (ec) canon_root = root;

    // ── 1. Normalization: "heaven/…" and "heaven/../core/…" → stable rel paths ──
    {
        heaven::AssetUsageRecorder rec(canon_root.generic_string());

        const fs::path heaven_file =
            canon_root / "heaven" / "textures" / "a_d.dds";
        // Reached by walking up out of heaven/ and back down into the sibling
        // core/ tree — must collapse to a clean "core/…" Relative_Asset_Path.
        const fs::path core_via_dotdot =
            canon_root / "heaven" / ".." / "core" / "materials" / "x.mat";

        rec.record_used(heaven_file.string());
        rec.record_used(core_via_dotdot.string());

        const std::set<std::string> expected = {
            "core/materials/x.mat",
            "heaven/textures/a_d.dds",
        };
        if (rec.used() != expected) {
            std::printf("[unit_packer_recorder] FAILED: normalization mismatch. Got:\n");
            for (const std::string& p : rec.used())
                std::printf("    '%s'\n", p.c_str());
            ok = false;
        }
    }

    // ── 2. Idempotency: duplicate + equivalent spelling collapse to one entry ──
    {
        heaven::AssetUsageRecorder rec(canon_root.generic_string());

        const fs::path f = canon_root / "heaven" / "textures" / "a_d.dds";
        rec.record_used(f.string());
        rec.record_used(f.string()); // exact duplicate
        // A different spelling of the SAME file (out of core/ and back into
        // heaven/) normalizes to the same canonical path.
        rec.record_used((canon_root / "core" / ".." / "heaven" / "textures" / "a_d.dds")
                            .string());

        if (rec.used().size() != 1) {
            std::printf("[unit_packer_recorder] FAILED: expected 1 unique used "
                        "entry after idempotent recording, got %zu\n",
                        rec.used().size());
            ok = false;
        } else if (*rec.used().begin() != "heaven/textures/a_d.dds") {
            std::printf("[unit_packer_recorder] FAILED: idempotent entry has "
                        "unexpected path '%s'\n",
                        rec.used().begin()->c_str());
            ok = false;
        }
    }

    // ── 3. Unresolved: dedup on (missing, referrer), preserve insertion order ──
    {
        heaven::AssetUsageRecorder rec(canon_root.generic_string());

        rec.record_unresolved("heaven/missing_a.dds", "heaven/mat/a.mat");
        rec.record_unresolved("heaven/missing_b.dds", "heaven/mat/b.mat");
        // Exact duplicate of the first edge — must be dropped.
        rec.record_unresolved("heaven/missing_a.dds", "heaven/mat/a.mat");
        // Same missing target, DIFFERENT referrer — a distinct edge, kept.
        rec.record_unresolved("heaven/missing_a.dds", "heaven/mat/c.mat");

        const std::vector<heaven::Unresolved>& u = rec.unresolved();
        if (u.size() != 3) {
            std::printf("[unit_packer_recorder] FAILED: expected 3 unresolved "
                        "entries after dedup, got %zu\n",
                        u.size());
            ok = false;
        } else {
            const bool order_ok =
                u[0].missing_rel_path == "heaven/missing_a.dds" &&
                u[0].referring_rel_path == "heaven/mat/a.mat" &&
                u[1].missing_rel_path == "heaven/missing_b.dds" &&
                u[1].referring_rel_path == "heaven/mat/b.mat" &&
                u[2].missing_rel_path == "heaven/missing_a.dds" &&
                u[2].referring_rel_path == "heaven/mat/c.mat";
            if (!order_ok) {
                std::printf("[unit_packer_recorder] FAILED: unresolved dedup/order "
                            "mismatch. Got:\n");
                for (const heaven::Unresolved& e : u)
                    std::printf("    missing='%s' referrer='%s'\n",
                                e.missing_rel_path.c_str(),
                                e.referring_rel_path.c_str());
                ok = false;
            }
        }
    }

    fs::remove_all(root, ec);
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_packer_recorder{unit_packer_recorder};

} // namespace
