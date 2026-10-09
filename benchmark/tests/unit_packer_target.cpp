/*
 * HeavenBench — unit test: prepare_pack_target decision table
 *
 *
 *
 * prepare_pack_target classifies the Pack_Target pre-state per the implementation
 * decision table. This test pins one example per row using
 * real temp directories:
 *
 *   - absent path                         → ReadyCreated (and the dir is created)
 *   - existing empty directory            → ReadyCreated
 *   - existing non-empty dir, no overwrite → ConflictNonEmpty
 *   - existing non-empty dir, overwrite    → ReadyOverwrite
 *   - existing path that is a regular file → NotADirectory
 *
 * GATING: the test builds its scratch directories under
 * std::filesystem::temp_directory_path(). If the temp directory cannot be
 * created (read-only sandbox, etc.) it SKIPS gracefully (prints a notice and
 * returns success) rather than failing spuriously.
 *
 * The test self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, matching
 * the other test translation units; the tests glob picks it up without a
 * CMake edit. The registered callable returns true on success and
 * false (after printing a diagnostic) on failure.
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "asset_packer/asset_packer.h"

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

std::FILE* open_binary_write(const fs::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

// Map a TargetStatus to a short label for diagnostics.
const char* status_name(heaven::TargetStatus s) {
    switch (s) {
        case heaven::TargetStatus::ReadyCreated:     return "ReadyCreated";
        case heaven::TargetStatus::ReadyOverwrite:   return "ReadyOverwrite";
        case heaven::TargetStatus::ConflictNonEmpty: return "ConflictNonEmpty";
        case heaven::TargetStatus::NotADirectory:    return "NotADirectory";
        case heaven::TargetStatus::CreateFailed:     return "CreateFailed";
    }
    return "?";
}

bool unit_packer_target() {
    bool ok = true;

    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[unit_packer_target] SKIPPED: no temp dir available (%s)\n",
                    ec.message().c_str());
        return true;
    }

    static std::atomic<unsigned> counter{0};
    const fs::path root =
        temp_base / ("heaven_packer_target_" + std::to_string(counter.fetch_add(1)));

    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    if (ec) {
        std::printf("[unit_packer_target] SKIPPED: cannot create temp tree '%s' (%s)\n",
                    root.string().c_str(), ec.message().c_str());
        return true;
    }

    // Small helper to check one row and report on mismatch.
    auto expect = [&](const char* row, const heaven::TargetDecision& d,
                      heaven::TargetStatus want) {
        if (d.status != want) {
            std::printf("[unit_packer_target] FAILED: %s — expected %s, got %s "
                        "(message: '%s')\n",
                        row, status_name(want), status_name(d.status),
                        d.message.c_str());
            ok = false;
        }
    };

    // ── Row 1: absent path → ReadyCreated, and the directory is created ──
    {
        const fs::path target = root / "absent" / "nested";
        heaven::TargetDecision d = heaven::prepare_pack_target(target, /*overwrite=*/false);
        expect("absent path", d, heaven::TargetStatus::ReadyCreated);

        const bool created = fs::is_directory(target, ec) && !ec;
        if (!created) {
            std::printf("[unit_packer_target] FAILED: absent path — directory "
                        "'%s' was not created\n",
                        target.string().c_str());
            ok = false;
        }
    }

    // ── Row 2: existing empty directory → ReadyCreated ──
    {
        const fs::path target = root / "empty_dir";
        fs::create_directories(target, ec);
        heaven::TargetDecision d = heaven::prepare_pack_target(target, /*overwrite=*/false);
        expect("existing empty dir", d, heaven::TargetStatus::ReadyCreated);
    }

    // ── Row 3: existing non-empty directory, no overwrite → ConflictNonEmpty ──
    {
        const fs::path target = root / "nonempty_no_overwrite";
        fs::create_directories(target, ec);
        { std::FILE* f = open_binary_write(target / "stale.txt");
          if (f) { std::fputs("x", f); std::fclose(f); } }
        heaven::TargetDecision d = heaven::prepare_pack_target(target, /*overwrite=*/false);
        expect("non-empty dir, no overwrite", d, heaven::TargetStatus::ConflictNonEmpty);
    }

    // ── Row 4: existing non-empty directory, overwrite → ReadyOverwrite ──
    {
        const fs::path target = root / "nonempty_overwrite";
        fs::create_directories(target, ec);
        { std::FILE* f = open_binary_write(target / "stale.txt");
          if (f) { std::fputs("x", f); std::fclose(f); } }
        heaven::TargetDecision d = heaven::prepare_pack_target(target, /*overwrite=*/true);
        expect("non-empty dir, overwrite", d, heaven::TargetStatus::ReadyOverwrite);
    }

    // ── Row 5: existing path that is a regular file → NotADirectory ──
    {
        const fs::path target = root / "a_regular_file";
        { std::FILE* f = open_binary_write(target);
          if (f) { std::fputs("x", f); std::fclose(f); } }
        // overwrite flag is irrelevant for this row; a non-directory is always fatal.
        heaven::TargetDecision d = heaven::prepare_pack_target(target, /*overwrite=*/true);
        expect("path is a regular file", d, heaven::TargetStatus::NotADirectory);
    }

    fs::remove_all(root, ec);
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_packer_target{unit_packer_target};

} // namespace
