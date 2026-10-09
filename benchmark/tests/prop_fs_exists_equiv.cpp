/*
 * HeavenBench — Property test: filesystem-existence equivalence
 *
 * Feature: codebase-hardening
 *   •: Filesystem-existence equivalence
 *
 *
 * replaced the POSIX existence/scan primitives the loaders used with
 * std::filesystem:
 *   • scene_rhi.cpp / material_rhi.cpp swapped `struct stat`/`stat(p,&st)==0`
 *     existence probes for std::filesystem::exists.
 *   • xml0_parser.h::scan_dir_recursive swapped the opendir/readdir/stat walk
 *     for std::filesystem::recursive_directory_iterator, filtering by extension
 *     exactly as before.
 *
 * is the behavioural-parity invariant: WHERE the same set of
 * files exists on disk, the std::filesystem-based code SHALL resolve the same
 * files as the prior POSIX code. This property pins that parity directly
 * against the original POSIX oracle on a real temp filesystem:
 *
 *   1. Existence equivalence — for a mix of real files and never-created paths,
 *      std::filesystem::exists(p) agrees bit-for-bit with the prior POSIX
 *      reference `stat(p,&st) == 0`. Including <sys/stat.h> here is purely the
 *      test's reference oracle; the renderer no longer uses it.
 *   2. Scan equivalence — for a directory tree containing files of a target
 *      extension (".mat") mixed with other extensions, scan_dir_recursive(dir,
 *      ".mat", out) returns exactly the set of ".mat" files an independent walk
 *      (the known set we created) reports.
 *
 * xml0_parser.h is GPU-free and host-includable (scan_dir_recursive is a
 * file-local `static` free function). This file self-registers with the
 * RapidCheck harness in test_main.cpp via the heaven::testing::Register
 * pattern, so the file(GLOB) in benchmark/CMakeLists.txt picks it up without
 * editing the entry point.
 */
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <sys/stat.h> // TEST-ONLY reference oracle (renderer no longer uses it)

#include <rapidcheck.h>

#include "xml0_parser.h"

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

// ── Temp-dir scaffolding ───────────────────────────────────────

// Each iteration gets its own directory so concurrent test runs (shared build
// dir) never collide. Mirrors the pattern in prop_tess_bc_fuzz.cpp.
struct TempDir {
    std::filesystem::path path;
    bool ok = false;

    TempDir() {
        static std::atomic<std::uint64_t> counter{0};
        static const std::uint64_t salt = std::random_device{}();
        const std::uint64_t id = counter.fetch_add(1);
        path = std::filesystem::temp_directory_path() /
               std::format("heaven_fs_equiv_{}_{}", salt, id);
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        ok = !ec;
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec); // best-effort cleanup
    }
};

// The prior POSIX existence oracle: stat() succeeding (returns 0) is exactly
// the predicate scene_rhi/material_rhi used before.
bool posix_exists(const std::filesystem::path& p) {
    struct stat st;
    return ::stat(p.string().c_str(), &st) == 0;
}

// Create an empty file, materialising it on disk.
void touch(const std::filesystem::path& p) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
}

// ── Properties ─────────────────────────────────────────────────

// Existence equivalence: for a mix of real
// files and never-created paths, std::filesystem::exists agrees with the prior
// POSIX stat() oracle on every path.
bool run_p19_exists_equiv() {
    return rc::check(
        "Feature: codebase-hardening, Property 19: filesystem-existence "
        "equivalence — exists vs POSIX stat (Validates: Requirements 12.1, "
        "12.2, 12.4)",
        [] {
            TempDir dir;
            RC_ASSERT(dir.ok);

            // A set of real files actually written to disk.
            const int n_real = *rc::gen::inRange(0, 12);
            std::vector<std::filesystem::path> paths;
            for (int i = 0; i < n_real; ++i) {
                const auto ext = *rc::gen::element<std::string>(
                    ".mat", ".mesh", ".dds", ".node", "");
                const std::filesystem::path p =
                    dir.path / std::format("real_{}{}", i, ext);
                touch(p);
                paths.push_back(p);
            }

            // A set of paths that are never created (must not exist).
            const int n_missing = *rc::gen::inRange(0, 12);
            for (int i = 0; i < n_missing; ++i) {
                paths.push_back(dir.path /
                                std::format("missing_{}.mesh", i));
            }

            // A few nested-but-absent paths (parent dir absent too).
            paths.push_back(dir.path / "no_such_dir" / "ghost.mat");

            // For every path, std::filesystem::exists must equal the prior
            // POSIX stat()==0 result.
            for (const auto& p : paths) {
                std::error_code ec;
                const bool fs_result = std::filesystem::exists(p, ec);
                const bool posix_result = posix_exists(p);
                RC_ASSERT(fs_result == posix_result);
            }
        });
}

// Scan equivalence: scan_dir_recursive resolves
// exactly the set of target-extension files present in a tree, regardless of
// other-extension files mixed in — matching an independent known set.
bool run_p19_scan_equiv() {
    return rc::check(
        "Feature: codebase-hardening, Property 19: filesystem-existence "
        "equivalence — recursive scan resolves the same .mat set (Validates: "
        "Requirements 12.3, 12.4)",
        [] {
            TempDir dir;
            RC_ASSERT(dir.ok);

            const std::string target_ext = ".mat";

            // Build a small tree of subdirectories.
            const int n_subdirs = *rc::gen::inRange(0, 4);
            std::vector<std::filesystem::path> dirs;
            dirs.push_back(dir.path);
            for (int i = 0; i < n_subdirs; ++i) {
                const std::filesystem::path sub =
                    dir.path / std::format("sub_{}", i);
                std::error_code ec;
                std::filesystem::create_directories(sub, ec);
                RC_ASSERT(!ec);
                dirs.push_back(sub);
            }

            // The independent oracle: the exact set of target-extension files
            // we create. Stored canonicalised so comparison is order- and
            // representation-independent.
            std::set<std::string> expected;

            const int n_files = *rc::gen::inRange(0, 24);
            for (int i = 0; i < n_files; ++i) {
                const auto& parent =
                    dirs[*rc::gen::inRange<std::size_t>(0, dirs.size())];
                // Mix target-extension files with decoys (other extensions and
                // names that merely contain ".mat" mid-string must NOT match).
                const int kind = *rc::gen::inRange(0, 4);
                std::string name;
                bool is_target = false;
                switch (kind) {
                    case 0:
                        name = std::format("file_{}.mat", i);
                        is_target = true;
                        break;
                    case 1:
                        name = std::format("file_{}.dds", i);
                        break;
                    case 2:
                        name = std::format("file_{}.material", i);
                        break;
                    default:
                        name = std::format("file_{}.mat.bak", i);
                        break;
                }
                const std::filesystem::path p = parent / name;
                touch(p);
                if (is_target) {
                    expected.insert(
                        std::filesystem::weakly_canonical(p).string());
                }
            }

            // Run the production scanner.
            std::vector<std::string> out;
            scan_dir_recursive(dir.path.string(), target_ext, out);

            std::set<std::string> got;
            for (const auto& s : out) {
                got.insert(std::filesystem::weakly_canonical(
                               std::filesystem::path(s))
                               .string());
            }

            RC_ASSERT(got == expected);
        });
}

bool run_fs_exists_equiv_properties() {
    bool ok = true;
    ok = run_p19_exists_equiv() && ok;
    ok = run_p19_scan_equiv() && ok;
    return ok;
}

const heaven::testing::Register g_register_fs_exists_equiv{
    &run_fs_exists_equiv_properties};

} // namespace
