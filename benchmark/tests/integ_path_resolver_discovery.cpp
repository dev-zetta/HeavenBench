/*
 * HeavenBench — asset-integration test: auto-discovery existence probing
 *
 * Feature: asset-path-resolution
 *
 *
 * Convention-based texture auto-discovery in the material database
 * (engine/material_rhi.cpp) determines whether a candidate texture exists by
 * stat()-ing the filesystem location named by the candidate's Resolved_Path:
 * it iterates texture_search_dirs() × texture_suffixes(), forms each
 * texture_candidate(dir, base, suffix), and collects only those candidates
 * that name a readable regular file. Routine misses (no file on disk) are
 * skipped silently — no missing-asset diagnostic is emitted for them.
 *
 * This test reproduces that discovery loop directly against a real
 * PathResolver and a real (temporary) filesystem layout, pinning two
 * properties:
 *
 *    — Existence is determined by the filesystem location named by the
 *     candidate's Resolved_Path. A texture laid out on disk at one of the
 *     resolved candidate paths is found via that exact resolved path.
 *    — A base name with no matching files on disk yields no match. The
 *     probe loop simply finds nothing (match count == 0); a routine miss is
 *     not a diagnostic, so there is no stderr/stdout noise to assert against —
 *     the absence of any match is the observable behavior.
 *
 * SELF-CONTAINED: unlike the other integ_* tests, this one does NOT need the
 * extracted Heaven asset tree. It builds its own asset layout inside a unique
 * std::filesystem temp directory used as the PathResolver's Data_Root, so it
 * runs (and asserts) in CI without assets. It is still robust: it skips
 * gracefully if a temp directory cannot be created, and it always cleans up
 * the temp tree (fs::remove_all) regardless of pass/fail.
 *
 * The test registers itself with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, following
 * the same self-registration pattern as the other test translation units. The
 * registered callable returns true on success/skip and false (after printing a
 * diagnostic) on a genuine failure. No second main() is defined.
 */
#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "engine/path_resolver.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp. registry()
// is a free function with external linkage defined there; the Register helper's
// definition is identical so it can be reused from this translation unit.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

namespace fs = std::filesystem;

// Existence probe mirroring material_rhi.cpp's auto-discovery: a candidate is
// "found" only when stat() succeeds on its resolved path (a readable regular
// file at that filesystem location). This is the same predicate the renderer
// uses, reproduced here so the test drives the real discovery contract.
bool candidate_exists(const std::string& resolved_path) {
    struct stat st;
    if (stat(resolved_path.c_str(), &st) != 0) return false;
#ifdef _WIN32
    return (st.st_mode & _S_IFMT) == _S_IFREG;
#else
    return S_ISREG(st.st_mode);
#endif
}

// Count how many (search dir × suffix) candidates for `base` name an existing
// file on disk — exactly the loop material_rhi.cpp runs, collecting only the
// candidates whose resolved path stat()s successfully. A routine miss simply
// fails to increment the count; no diagnostic is emitted.
std::size_t count_discovered(const heaven::PathResolver& paths,
                             const std::string& base) {
    std::size_t found = 0;
    for (const auto& dir : paths.texture_search_dirs()) {
        for (const auto& suffix : heaven::PathResolver::texture_suffixes()) {
            const std::string candidate =
                paths.texture_candidate(dir, base, std::string(suffix));
            if (candidate_exists(candidate)) ++found;
        }
    }
    return found;
}

bool integ_path_resolver_discovery() {
    // Create a unique temp directory to serve as the PathResolver's Data_Root.
    // A randomized suffix avoids collisions with concurrent runs. If the temp
    // tree cannot be created (read-only fs, etc.) the test skips gracefully.
    std::error_code ec;
    std::random_device rd;
    const fs::path root = fs::temp_directory_path(ec) /
        ("ugx_path_resolver_discovery_" + std::to_string(rd()));
    if (ec) {
        std::printf("[integ_path_resolver_discovery] SKIPPED: no temp dir (%s)\n",
                    ec.message().c_str());
        return true;
    }

    fs::create_directories(root, ec);
    if (ec) {
        std::printf("[integ_path_resolver_discovery] SKIPPED: cannot create "
                    "temp dir %s (%s)\n",
                    root.string().c_str(), ec.message().c_str());
        return true;
    }

    bool ok = true;

    // RAII-style cleanup: remove the whole temp tree on every exit path.
    struct Cleanup {
        const fs::path& dir;
        ~Cleanup() {
            std::error_code rm_ec;
            fs::remove_all(dir, rm_ec);
        }
    } cleanup{root};

    const auto resolver = heaven::PathResolver::create(root.string());
    if (!resolver.has_value()) {
        std::printf("[integ_path_resolver_discovery] FAILED: PathResolver::create "
                    "rejected a valid temp Data_Root %s\n",
                    root.string().c_str());
        return false;
    }

    // ──: a texture laid out at a resolved candidate path is found ──

    // Pick the first search directory (environment) and the "_d" suffix, build
    // the candidate for base "mytex", then create the parent directories and
    // the file on disk at exactly that resolved path.
    const std::string& env_dir = resolver->texture_search_dirs()[0];
    const std::string present_candidate =
        resolver->texture_candidate(env_dir, "mytex", "_d");

    const fs::path present_path(present_candidate);
    fs::create_directories(present_path.parent_path(), ec);
    if (ec) {
        std::printf("[integ_path_resolver_discovery] FAILED: cannot create "
                    "candidate parent dir %s (%s)\n",
                    present_path.parent_path().string().c_str(),
                    ec.message().c_str());
        return false;
    }
    {
        std::ofstream out(present_path, std::ios::binary);
        out << "DDS placeholder";
    }
    if (!fs::exists(present_path)) {
        std::printf("[integ_path_resolver_discovery] FAILED: could not lay out "
                    "texture at %s\n",
                    present_candidate.c_str());
        return false;
    }

    // Discovery via the candidate's resolved path must locate the laid-out
    // texture: the probe on the exact resolved candidate string finds it.
    if (!candidate_exists(present_candidate)) {
        std::printf("[integ_path_resolver_discovery] FAILED: probe did not find "
                    "the laid-out texture at its resolved candidate path %s\n",
                    present_candidate.c_str());
        ok = false;
    }

    // Running the full discovery loop for base "mytex" must report exactly one
    // match — the single file laid out above (environment dir + "_d").
    if (const std::size_t hits = count_discovered(*resolver, "mytex"); hits != 1) {
        std::printf("[integ_path_resolver_discovery] FAILED: discovery for "
                    "\"mytex\" found %zu candidates, expected 1\n",
                    hits);
        ok = false;
    }

    // ──: a base name with no matching files yields no match ──

    // No file was created for this base, so every candidate (dir × suffix)
    // misses. The probe loop finds nothing and the match count is 0; a routine
    // miss is skipped silently with no diagnostic emitted.
    if (const std::size_t misses = count_discovered(*resolver, "no_such_base");
        misses != 0) {
        std::printf("[integ_path_resolver_discovery] FAILED: discovery for a "
                    "base name with no files found %zu candidates, expected 0\n",
                    misses);
        ok = false;
    }

    if (ok) {
        std::printf("[integ_path_resolver_discovery] OK: found laid-out texture "
                    "via resolved candidate path; absent base yielded no match\n");
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_integ_path_resolver_discovery{
    integ_path_resolver_discovery};

} // namespace
