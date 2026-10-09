/*
 * HeavenBench — integration test: write_used_assets (copy/move + layout)
 *
 *
 *
 * This test drives the packer's filesystem writer, write_used_assets, directly
 * against real temp directories — it needs no full asset fixture, only a couple
 * of known-byte files arranged like a Source_Tree. write_used_assets
 * reconstructs each source on-disk path as (canonical parent of Data_Root) /
 * Relative_Asset_Path, so each scenario builds a temp tree whose Data_Root is
 * "<scratch>/heaven" and whose used set holds "heaven/..." rels; the sources
 * then resolve back to the files this test created.
 *
 * Scenarios:
 *
 *   - Copy mode (3.1, 3.2, 3.3, 4.1, 4.3, 3.5): a clean copy of two files
 *     produces byte-identical targets at the mirrored Relative_Asset_Paths
 *     (creating the nested target directories), leaves the source files
 *     unchanged, and reports written==2 / fatal==false / no failures with the
 *     written-byte total matching the source sizes (the writer-level success
 *     that maps to the orchestrator's exit-zero, no unresolved refs).
 *
 *   - Move mode (4.2): the two files are relocated into the target (byte
 *     identical) and removed from the Source_Tree.
 *
 *   - Forced per-file failure (4.4, 4.5): a used rel whose source file does not
 *     exist forces copy_file to fail — that produces a recorded per-file
 *     diagnostic, leaves fatal==false, still writes the other (present) files,
 *     and leaves the other source files intact.
 *
 *   - Read-only target parent (3.6, 5.5): making the target's parent directory
 *     read-only so create_directories for a nested rel fails yields the fatal
 *     create-failure path (outcome.fatal==true). Permissions are restored before
 *     cleanup. Skipped gracefully where the platform does not enforce the
 *     read-only parent (e.g. running as root).
 *
 * GATING: the test builds its scratch tree under
 * std::filesystem::temp_directory_path(). If the temp directory cannot be
 * created (read-only sandbox, etc.) it SKIPS gracefully (prints a notice and
 * returns success) rather than failing spuriously.
 *
 * The test self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, matching
 * the other test translation units; the tests glob picks it up
 * without a CMake edit. The registered callable returns true on success and
 * false (after printing a diagnostic) on failure.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "asset_packer/asset_packer.h"
#include "engine/path_resolver.h"

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

// ── Small byte-level file helpers (binary, no text translation) ──

bool write_all(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
#ifdef _WIN32
    std::FILE* f = _wfopen(p.c_str(), L"wb");
#else
    std::FILE* f = std::fopen(p.c_str(), "wb");
#endif
    if (!f) return false;
    const bool written = data.empty() ||
        std::fwrite(data.data(), 1, data.size(), f) == data.size();
    const bool closed = std::fclose(f) == 0;
    return written && closed;
}

std::optional<std::string> read_all(const fs::path& p) {
#ifdef _WIN32
    std::FILE* f = _wfopen(p.c_str(), L"rb");
#else
    std::FILE* f = std::fopen(p.c_str(), "rb");
#endif
    if (!f) return std::nullopt;
    std::string out;
    char buf[4096];
    for (;;) {
        const size_t n = std::fread(buf, 1, sizeof buf, f);
        out.append(buf, n);
        if (n != sizeof buf) break;
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) return std::nullopt;
    return out;
}

// Known payloads spanning the full byte range so the byte-identity checks would
// catch any text translation or truncation.
std::string make_a_bytes() {
    std::string s;
    for (int i = 0; i < 256; ++i) s.push_back(static_cast<char>(i));
    s += "a.dds known payload";
    return s;
}

std::string make_b_bytes() {
    std::string s = "heaven material def\n";
    for (int i = 255; i >= 0; --i) s.push_back(static_cast<char>(i));
    return s;
}

bool integ_packer_write() {
    bool ok = true;

    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[integ_packer_write] SKIPPED: no temp dir available (%s)\n",
                    ec.message().c_str());
        return true;
    }

    static std::atomic<unsigned> counter{0};
    const fs::path base =
        temp_base / ("heaven_packer_write_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    if (ec) {
        std::printf("[integ_packer_write] SKIPPED: cannot create temp tree '%s' (%s)\n",
                    base.string().c_str(), ec.message().c_str());
        return true;
    }

    const std::string a_bytes = make_a_bytes();
    const std::string b_bytes = make_b_bytes();

    // The two known Relative_Asset_Paths used across scenarios. Data_Root is
    // "<scenario>/heaven", so the rels live under "heaven/" and write_used_assets
    // reconstructs sources as "<scenario>/heaven/...".
    const std::string rel_a = "heaven/a.dds";
    const std::string rel_b = "heaven/sub/b.mat";

    // ── Scenario 1: Copy mode — byte-identical, mirrored layout, source intact ──

    {
        const fs::path scen = base / "copy";
        const fs::path data_root = scen / "heaven";
        const fs::path src_a = data_root / "a.dds";
        const fs::path src_b = data_root / "sub" / "b.mat";
        write_all(src_a, a_bytes);
        write_all(src_b, b_bytes);

        heaven::Result<heaven::PathResolver> pr =
            heaven::PathResolver::create(data_root.string());
        if (!pr) {
            std::printf("[integ_packer_write] FAILED: PathResolver::create rejected "
                        "'%s'\n", data_root.string().c_str());
            ok = false;
        } else {
            const fs::path target = scen / "target";  // does not pre-exist
            const std::set<std::string> used{rel_a, rel_b};

            const heaven::WriteOutcome out = heaven::write_used_assets(
                used, *pr, target, heaven::TransferMode::Copy);

            // Writer-level success: both files placed, nothing fatal, no per-file
            // failures (this is the no-unresolved success that maps to exit zero,
            // Default Copy_Mode leaves the sources unchanged.
            if (out.written != 2 || out.fatal || !out.failures.empty()) {
                std::printf("[integ_packer_write] FAILED: copy outcome unexpected "
                            "(written=%d fatal=%d failures=%zu)\n",
                            out.written, out.fatal ? 1 : 0, out.failures.size());
                ok = false;
            }

            const uintmax_t expect_bytes =
                static_cast<uintmax_t>(a_bytes.size() + b_bytes.size());
            if (out.bytes != expect_bytes) {
                std::printf("[integ_packer_write] FAILED: copy byte total %ju, "
                            "expected %ju\n",
                            static_cast<uintmax_t>(out.bytes), expect_bytes);
                ok = false;
            }

            // Targets at the mirrored Relative_Asset_Paths  with
            // byte-identical content.
            const std::optional<std::string> got_a = read_all(target / "heaven" / "a.dds");
            const std::optional<std::string> got_b =
                read_all(target / "heaven" / "sub" / "b.mat");
            if (got_a != std::optional<std::string>(a_bytes)) {
                std::printf("[integ_packer_write] FAILED: copied a.dds not "
                            "byte-identical at mirrored path\n");
                ok = false;
            }
            if (got_b != std::optional<std::string>(b_bytes)) {
                std::printf("[integ_packer_write] FAILED: copied b.mat not "
                            "byte-identical at mirrored path\n");
                ok = false;
            }

            // Source unchanged.
            if (read_all(src_a) != std::optional<std::string>(a_bytes) ||
                read_all(src_b) != std::optional<std::string>(b_bytes)) {
                std::printf("[integ_packer_write] FAILED: copy mode mutated the "
                            "source files\n");
                ok = false;
            }
        }
    }

    // ── Scenario 2: Move mode — relocated to target, removed from source ──

    {
        const fs::path scen = base / "move";
        const fs::path data_root = scen / "heaven";
        const fs::path src_a = data_root / "a.dds";
        const fs::path src_b = data_root / "sub" / "b.mat";
        write_all(src_a, a_bytes);
        write_all(src_b, b_bytes);

        heaven::Result<heaven::PathResolver> pr =
            heaven::PathResolver::create(data_root.string());
        if (!pr) {
            std::printf("[integ_packer_write] FAILED: PathResolver::create rejected "
                        "'%s'\n", data_root.string().c_str());
            ok = false;
        } else {
            const fs::path target = scen / "target";
            const std::set<std::string> used{rel_a, rel_b};

            const heaven::WriteOutcome out = heaven::write_used_assets(
                used, *pr, target, heaven::TransferMode::Move);

            if (out.written != 2 || out.fatal || !out.failures.empty()) {
                std::printf("[integ_packer_write] FAILED: move outcome unexpected "
                            "(written=%d fatal=%d failures=%zu)\n",
                            out.written, out.fatal ? 1 : 0, out.failures.size());
                ok = false;
            }

            // Relocated, byte-identical, into the target.
            if (read_all(target / "heaven" / "a.dds") != std::optional<std::string>(a_bytes) ||
                read_all(target / "heaven" / "sub" / "b.mat") != std::optional<std::string>(b_bytes)) {
                std::printf("[integ_packer_write] FAILED: move did not place "
                            "byte-identical files in the target\n");
                ok = false;
            }

            // Removed from the Source_Tree.
            if (fs::exists(src_a, ec) || fs::exists(src_b, ec)) {
                std::printf("[integ_packer_write] FAILED: move left source files "
                            "in place\n");
                ok = false;
            }
        }
    }

    // ── Scenario 3: forced per-file failure — recorded, non-fatal, continues ──

    {
        const fs::path scen = base / "fail";
        const fs::path data_root = scen / "heaven";
        const fs::path src_a = data_root / "a.dds";
        const fs::path src_b = data_root / "sub" / "b.mat";
        write_all(src_a, a_bytes);
        write_all(src_b, b_bytes);
        // Note: "heaven/missing.dds" is deliberately NOT created on disk.

        heaven::Result<heaven::PathResolver> pr =
            heaven::PathResolver::create(data_root.string());
        if (!pr) {
            std::printf("[integ_packer_write] FAILED: PathResolver::create rejected "
                        "'%s'\n", data_root.string().c_str());
            ok = false;
        } else {
            const fs::path target = scen / "target";
            const std::string rel_missing = "heaven/missing.dds";
            const std::set<std::string> used{rel_a, rel_missing, rel_b};

            const heaven::WriteOutcome out = heaven::write_used_assets(
                used, *pr, target, heaven::TransferMode::Copy);

            // The missing source produces exactly one recorded per-file
            // diagnostic and is NOT fatal; the two present files are still
            // written (continue past the failure).
            if (out.fatal) {
                std::printf("[integ_packer_write] FAILED: per-file failure was "
                            "treated as fatal\n");
                ok = false;
            }
            if (out.failures.size() != 1) {
                std::printf("[integ_packer_write] FAILED: expected 1 per-file "
                            "failure diagnostic, got %zu\n", out.failures.size());
                ok = false;
            } else if (out.failures[0].find("missing.dds") == std::string::npos) {
                std::printf("[integ_packer_write] FAILED: failure diagnostic does "
                            "not name the failing path: '%s'\n",
                            out.failures[0].c_str());
                ok = false;
            }
            if (out.written != 2) {
                std::printf("[integ_packer_write] FAILED: expected 2 files written "
                            "past the failure, got %d\n", out.written);
                ok = false;
            }

            // The present files were written byte-identically.
            if (read_all(target / "heaven" / "a.dds") != std::optional<std::string>(a_bytes) ||
                read_all(target / "heaven" / "sub" / "b.mat") != std::optional<std::string>(b_bytes)) {
                std::printf("[integ_packer_write] FAILED: files around the failing "
                            "one were not written correctly\n");
                ok = false;
            }

            // Other source files remain intact and unchanged.
            if (read_all(src_a) != std::optional<std::string>(a_bytes) ||
                read_all(src_b) != std::optional<std::string>(b_bytes)) {
                std::printf("[integ_packer_write] FAILED: a per-file failure "
                            "disturbed the surviving source files\n");
                ok = false;
            }
        }
    }

    // ── Scenario 4: read-only target parent → fatal create-failure path ──

    {
        const fs::path scen = base / "ro";
        const fs::path data_root = scen / "heaven";
        write_all(data_root / "a.dds", a_bytes);

        heaven::Result<heaven::PathResolver> pr =
            heaven::PathResolver::create(data_root.string());
        if (!pr) {
            std::printf("[integ_packer_write] FAILED: PathResolver::create rejected "
                        "'%s'\n", data_root.string().c_str());
            ok = false;
        } else {
            const fs::path ro_parent = scen / "ro_parent";
            fs::create_directories(ro_parent, ec);

            // Drop write permission on the parent so create_directories of the
            // nested target ("<ro_parent>/target/heaven") fails.
            fs::permissions(ro_parent,
                            fs::perms::owner_write | fs::perms::group_write |
                                fs::perms::others_write,
                            fs::perm_options::remove, ec);

            // Probe whether the platform actually enforces the read-only parent
            // (it does not when running as root). If a probe directory can be
            // created despite the chmod, skip this scenario rather than fail.
            std::error_code probe_ec;
            const fs::path probe = ro_parent / "probe_writable";
            fs::create_directory(probe, probe_ec);
            const bool enforced = static_cast<bool>(probe_ec);
            if (!probe_ec) {
                fs::remove(probe, probe_ec);
            }

            if (!enforced) {
                std::printf("[integ_packer_write] NOTE: read-only parent not "
                            "enforced (likely root); skipping fatal create-failure "
                            "scenario\n");
            } else {
                const fs::path target = ro_parent / "target";  // nested under RO parent
                const std::set<std::string> used{rel_a};

                const heaven::WriteOutcome out = heaven::write_used_assets(
                    used, *pr, target, heaven::TransferMode::Copy);

                // A directory-creation failure under the Pack_Target is the fatal
                // case.
                if (!out.fatal) {
                    std::printf("[integ_packer_write] FAILED: read-only target "
                                "parent did not yield a fatal create-failure "
                                "(fatal=%d written=%d failures=%zu)\n",
                                out.fatal ? 1 : 0, out.written, out.failures.size());
                    ok = false;
                }
            }

            // Restore permissions so cleanup (remove_all) can succeed.
            fs::permissions(ro_parent, fs::perms::owner_all, fs::perm_options::add, ec);
        }
    }

    fs::remove_all(base, ec);
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_integ_packer_write{integ_packer_write};

} // namespace
