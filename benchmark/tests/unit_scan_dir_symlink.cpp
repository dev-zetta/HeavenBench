/*
 * HeavenBench — unit test: scan_dir_recursive symlink-cycle termination
 *
 * Feature: codebase-hardening
 * — Validates:
 *
 * scan_dir_recursive (xml0_parser.h) was rewritten  on top of
 * std::filesystem::recursive_directory_iterator with the default
 * (non-symlink-following) directory_options plus skip_permission_denied. The
 * prior opendir/readdir/stat walk dereferenced symlinks and could be trapped
 * forever by a directory symlink that points back at an ancestor.
 *
 * This deterministic unit test constructs such a cycle on a real temporary
 * filesystem (`dir/loop -> dir`), drops a real extension-matching file into the
 * tree, and asserts that:
 *   1. scan_dir_recursive RETURNS at all — the test completing IS the
 *      termination proof; the old symlink-following walk would loop forever, and
 *   2. the scan still finds the real file(s) in the tree.
 *
 * If the platform cannot create directory symlinks (permissions / filesystem
 * limitations), the test SKIPS gracefully and is treated as a pass, since the
 * symlink-cycle scenario cannot be constructed there.
 *
 * scan_dir_recursive is GPU-free (host filesystem only). This translation unit
 * self-registers with the RapidCheck harness in test_main.cpp via the
 * heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point. This is
 * a deterministic unit test (no random input), so it does not use rc::check.
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "xml0_parser.h"

namespace heaven::testing {

// Mirror of the harness registry hooks declared in test_main.cpp.
using TestFn = std::function<bool()>;
std::vector<TestFn>& registry();

struct Register {
    explicit Register(TestFn fn) { registry().push_back(std::move(fn)); }
};

} // namespace heaven::testing

namespace {

namespace fs = std::filesystem;

// ── Temp-tree scaffolding ──────────────────────────────────────

// Build a uniquely-named temp directory; remove_all it on destruction so a
// failing assertion never leaks the symlink cycle into the temp dir. A
// monotonic counter keeps names distinct within a run.
struct TempTree {
    fs::path root;
    bool ok = false;

    TempTree() {
        static std::atomic<unsigned> counter{0};
        const unsigned id = counter.fetch_add(1);
        std::error_code ec;
        root = fs::temp_directory_path(ec) /
               ("heaven_scan_symlink_" + std::to_string(id));
        if (ec) return;
        fs::create_directories(root, ec);
        ok = !ec;
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

// Write a small file at `p` so the scan has a real, extension-matching hit.
bool write_file(const fs::path& p, const std::string& content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) return false;
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    os << content;
    os.flush();
    return static_cast<bool>(os);
}

bool unit_scan_dir_symlink() {
    bool ok = true;
    auto fail = [&](const std::string& msg) {
        std::printf("[unit_scan_dir_symlink] FAILED: %s\n", msg.c_str());
        ok = false;
    };

    const std::string ext = ".mat"; // a real extension the scanner filters on

    TempTree tree;
    if (!tree.ok) {
        // Could not even create the temp tree — treat as a skip rather than a
        // false failure (no filesystem access in this environment).
        std::printf("[unit_scan_dir_symlink] SKIPPED: could not create temp "
                    "directory\n");
        return true;
    }

    // ── Build the tree: a real file plus a nested subdirectory ──
    //   root/asset.mat                 (real, extension-matching)
    //   root/sub/nested.mat            (real, extension-matching, deeper)
    //   root/sub/loop -> root          (directory symlink forming a cycle)
    const fs::path real_top = tree.root / ("asset" + ext);
    const fs::path sub_dir = tree.root / "sub";
    const fs::path real_nested = sub_dir / ("nested" + ext);

    if (!write_file(real_top, "top")) {
        fail("could not write the top-level .mat file");
        return ok;
    }
    if (!write_file(real_nested, "nested")) {
        fail("could not write the nested .mat file");
        return ok;
    }

    // Create a directory symlink that points back at an ancestor, forming a
    // cycle: root/sub/loop -> root. A symlink-following walk would descend
    // root/sub/loop/sub/loop/sub/loop/... without end.
    std::error_code ec;
    const fs::path loop_link = sub_dir / "loop";
    fs::create_directory_symlink(tree.root, loop_link, ec);
    if (ec) {
        // Platform refused the symlink (permissions / unsupported FS). The
        // cycle cannot be constructed here, so the termination scenario is not
        // exercisable — skip gracefully (treated as pass),.
        std::printf("[unit_scan_dir_symlink] SKIPPED: directory symlinks "
                    "unsupported here (%s)\n",
                    ec.message().c_str());
        return true;
    }

    // ── The actual check ──

    // Reaching the line after this call at all is the termination proof: with
    // the old symlink-following walk this would never return. We also assert
    // the scan still discovered the real extension-matching files in the tree.
    std::vector<std::string> hits;
    scan_dir_recursive(tree.root.string(), ext, hits);

    // The real files must both be present. The scan may legitimately also visit
    // the symlinked-but-not-followed entry; we only require that the genuine
    // files were found and that the call returned.
    bool found_top = false;
    bool found_nested = false;
    for (const std::string& h : hits) {
        std::error_code eec;
        if (fs::equivalent(fs::path(h), real_top, eec) && !eec)
            found_top = true;
        if (fs::equivalent(fs::path(h), real_nested, eec) && !eec)
            found_nested = true;
    }

    if (!found_top)
        fail("scan did not find the top-level real .mat file");
    if (!found_nested)
        fail("scan did not find the nested real .mat file");

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_scan_dir_symlink{&unit_scan_dir_symlink};

} // namespace
