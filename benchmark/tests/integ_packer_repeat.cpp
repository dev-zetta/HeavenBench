/*
 * HeavenBench — integration test: overwrite + determinism (used-asset packer)
 *
 *
 *
 * This test exercises the packer's write/report half end-to-end against a real
 * temp Source_Tree, plus the orchestrator's Data_Root failure contract. It uses
 * the packer's own helper functions directly (write_used_assets +
 * compute_pack_report) so it does not need a full binary xml0 asset fixture:
 * the discovery half (which parses real.world/.node/.mat files) is covered by
 * the parity tests; here we pin the deterministic, byte-faithful write and the
 * overwrite/Data_Root behaviour that sits on top of std::filesystem.
 *
 *   - Overwrite replaces a same-path file while leaving unrelated target files
 *     untouched (5.3): a stale file pre-placed at a used Relative_Asset_Path is
 *     replaced byte-for-byte by the source, while an unrelated file living at a
 *     path NOT in the used set is left exactly as it was.
 *
 *   - Two consecutive overwrite runs into the same target are deterministic:
 *     they produce an identical set of Relative_Asset_Paths (8.2),
 *     byte-identical file content at every path (8.3), and identical
 *     PackReport written/omitted/scanned counts and written-byte totals (8.4).
 *
 *   - Data_Root failure is fatal before any write (6.4): an empty,
 *     whitespace-only, or non-existent Data_Root makes run_asset_packer return
 *     EXIT_DATA_ROOT and write nothing to the Pack_Target.
 *
 *   - The completed-with-unresolved exit code is distinct from the fatal
 *     Data_Root exit code (6.3): EXIT_UNRESOLVED != EXIT_DATA_ROOT, and the
 *     unresolved path is exercised through the pure traversal core + the
 *     unresolved-list renderer that feeds run_asset_packer's EXIT_UNRESOLVED
 *     decision. (A full end-to-end unresolved run requires a binary xml0 world
 *     fixture, which is out of scope for this host test; the resolved-set write
 *     and the distinct exit code are pinned here, the renderer-parity discovery
 *     is covered separately.)
 *
 * GATING: the test builds its scratch tree under
 * std::filesystem::temp_directory_path(). If the temp directory cannot be
 * created (read-only sandbox, etc.) it SKIPS gracefully (prints a notice and
 * returns success) rather than failing spuriously.
 *
 * The test self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, matching
 * the other test translation units; the tests directory file(GLOB) picks it
 * up without a CMake edit. The registered callable returns true on success and
 * false (after printing a diagnostic) on failure.
 */
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
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

// Write `content` to `path`, creating parent directories. Returns false on any
// filesystem error so the caller can SKIP rather than fail spuriously.
bool write_text(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    const size_t n = content.empty()
                         ? 0
                         : std::fwrite(content.data(), 1, content.size(), f);
    const bool closed = std::fclose(f) == 0;
    return n == content.size() && closed;
}

// Read an entire file into a string. Returns std::nullopt if it cannot be read.
std::optional<std::string> read_text(const fs::path& path) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return std::nullopt;
    std::string out;
    char buf[4096];
    for (;;) {
        const size_t n = std::fread(buf, 1, sizeof(buf), f);
        out.append(buf, n);
        if (n != sizeof(buf)) break;
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) return std::nullopt;
    return out;
}

// Enumerate every regular file under `root` as a (Relative_Asset_Path → content)
// map, keyed by the '/'-normalized path relative to `root`. This is the snapshot
// used to compare two consecutive runs for an identical path set + byte content.
std::map<std::string, std::string> snapshot_tree(const fs::path& root) {
    std::map<std::string, std::string> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec) || ec) continue;
        const std::string rel = it->path().lexically_relative(root).generic_string();
        std::optional<std::string> content = read_text(it->path());
        out[rel] = content.value_or(std::string{});
    }
    return out;
}

bool integ_packer_repeat() {
    bool ok = true;

    std::error_code ec;
    const fs::path temp_base = fs::temp_directory_path(ec);
    if (ec) {
        std::printf("[integ_packer_repeat] SKIPPED: no temp dir available (%s)\n",
                    ec.message().c_str());
        return true;
    }

    static std::atomic<unsigned> counter{0};
    const fs::path base =
        temp_base / ("heaven_packer_repeat_" + std::to_string(counter.fetch_add(1)));
    fs::remove_all(base, ec);

    // Source_Tree: Data_Root is base/heaven, so the canonical parent (the
    // Source_Tree root the writer reconstructs from) is `base`, and a file at
    // base/heaven/x has the stable Relative_Asset_Path "heaven/x".
    const fs::path data_root = base / "heaven";

    // Used Asset_Files (each with distinct content/size for a meaningful byte
    // total) plus one unused source file so the report's omitted count is > 0.
    const std::set<std::string> used = {
        "heaven/materials/m.mat",
        "heaven/textures/a_d.dds",
        "heaven/textures/b_n.dds",
    };
    const std::map<std::string, std::string> source_content = {
        {"heaven/materials/m.mat", "MATERIAL-DEFINITION-BYTES"},
        {"heaven/textures/a_d.dds", "AAAA"},
        {"heaven/textures/b_n.dds", "BBBBBB"},
        {"heaven/extra_unused.dds", "UNUSED-NOT-REFERENCED"}, // omitted from used set
    };

    bool setup_ok = true;
    for (const auto& [rel, content] : source_content) {
        if (!write_text(base / rel, content)) { setup_ok = false; break; }
    }
    if (!setup_ok) {
        std::printf("[integ_packer_repeat] SKIPPED: cannot create temp Source_Tree "
                    "under '%s'\n",
                    base.string().c_str());
        fs::remove_all(base, ec);
        return true;
    }

    heaven::Result<heaven::PathResolver> paths =
        heaven::PathResolver::create(data_root.string());
    if (!paths) {
        std::printf("[integ_packer_repeat] SKIPPED: PathResolver::create failed: %s\n",
                    paths.error().message.c_str());
        fs::remove_all(base, ec);
        return true;
    }

    // ── 1. Overwrite replaces a same-path file; unrelated target files survive (5.3) ──
    const fs::path target = base / "target";
    // Pre-place a STALE file at a USED Relative_Asset_Path — overwrite must
    // replace its bytes with the source content.
    if (!write_text(target / "heaven/textures/a_d.dds", "STALE-OLD-CONTENT")) {
        std::printf("[integ_packer_repeat] SKIPPED: cannot seed target stale file\n");
        fs::remove_all(base, ec);
        return true;
    }
    // Pre-place an UNRELATED file at a path that is NOT in the used set — it must
    // be left untouched by the overwrite run.
    const std::string kUnrelatedRel = "unrelated/keep.txt";
    const std::string kUnrelatedContent = "DO-NOT-TOUCH-ME";
    if (!write_text(target / kUnrelatedRel, kUnrelatedContent)) {
        std::printf("[integ_packer_repeat] SKIPPED: cannot seed unrelated target file\n");
        fs::remove_all(base, ec);
        return true;
    }

    // The scanned denominator for the report consistency identity. Two unchanged
    // runs see the same Source_Tree, so this is identical across runs.
    const std::set<std::string> scanned = heaven::scan_source_tree(*paths);

    // ── Run 1 (Copy mode = overwrite semantics; copy_file overwrites) ──
    const heaven::WriteOutcome run1 =
        heaven::write_used_assets(used, *paths, target, heaven::TransferMode::Copy);
    if (run1.fatal || !run1.failures.empty()) {
        std::printf("[integ_packer_repeat] FAILED: run 1 reported a write failure "
                    "(fatal=%d, failures=%zu)\n",
                    run1.fatal ? 1 : 0, run1.failures.size());
        ok = false;
    }
    const heaven::PackReport report1 =
        heaven::compute_pack_report(used, scanned, run1.bytes);
    const std::map<std::string, std::string> snap1 = snapshot_tree(target);

    // The stale used-path file was replaced byte-for-byte by the source (5.3).
    {
        const std::optional<std::string> replaced =
            read_text(target / "heaven/textures/a_d.dds");
        if (!replaced || *replaced != "AAAA") {
            std::printf("[integ_packer_repeat] FAILED: overwrite did not replace the "
                        "stale used-path file with source bytes (got '%s')\n",
                        replaced ? replaced->c_str() : "<unreadable>");
            ok = false;
        }
    }
    // The unrelated file at a non-used path is untouched (5.3).
    {
        const std::optional<std::string> kept = read_text(target / kUnrelatedRel);
        if (!kept || *kept != kUnrelatedContent) {
            std::printf("[integ_packer_repeat] FAILED: unrelated target file was not "
                        "left untouched (got '%s')\n",
                        kept ? kept->c_str() : "<unreadable>");
            ok = false;
        }
    }

    // ── Run 2 (same used set, same target, overwrite again) ──
    const heaven::WriteOutcome run2 =
        heaven::write_used_assets(used, *paths, target, heaven::TransferMode::Copy);
    if (run2.fatal || !run2.failures.empty()) {
        std::printf("[integ_packer_repeat] FAILED: run 2 reported a write failure "
                    "(fatal=%d, failures=%zu)\n",
                    run2.fatal ? 1 : 0, run2.failures.size());
        ok = false;
    }
    const heaven::PackReport report2 =
        heaven::compute_pack_report(used, scanned, run2.bytes);
    const std::map<std::string, std::string> snap2 = snapshot_tree(target);

    // ── 8.2: identical set of Relative_Asset_Paths across the two runs ──
    {
        std::set<std::string> keys1, keys2;
        for (const auto& [k, v] : snap1) keys1.insert(k);
        for (const auto& [k, v] : snap2) keys2.insert(k);
        if (keys1 != keys2) {
            std::printf("[integ_packer_repeat] FAILED: 8.2 — target path set differs "
                        "between runs (%zu vs %zu paths)\n",
                        keys1.size(), keys2.size());
            ok = false;
        }
        // Sanity: the path set is the used set plus the surviving unrelated file.
        std::set<std::string> expected = used;
        expected.insert(kUnrelatedRel);
        if (keys1 != expected) {
            std::printf("[integ_packer_repeat] FAILED: 8.2 — target path set is not "
                        "the used set plus the preserved unrelated file\n");
            for (const std::string& k : keys1)
                std::printf("    target has: '%s'\n", k.c_str());
            ok = false;
        }
    }

    // ── 8.3: byte-identical file content at every path across the two runs ──
    if (snap1 != snap2) {
        std::printf("[integ_packer_repeat] FAILED: 8.3 — target file content differs "
                    "between the two runs\n");
        ok = false;
    }
    // And every written used file matches its Source_Tree counterpart byte-for-byte.
    for (const std::string& rel : used) {
        const auto src_it = source_content.find(rel);
        const auto dst_it = snap2.find(rel);
        if (src_it == source_content.end() || dst_it == snap2.end() ||
            src_it->second != dst_it->second) {
            std::printf("[integ_packer_repeat] FAILED: 8.3 — written '%s' is not "
                        "byte-identical to its source\n",
                        rel.c_str());
            ok = false;
        }
    }

    // ── 8.4: identical report counts + written byte total across the two runs ──
    {
        const bool same = report1.written == report2.written &&
                          report1.omitted == report2.omitted &&
                          report1.scanned == report2.scanned &&
                          report1.written_bytes == report2.written_bytes;
        if (!same) {
            std::printf("[integ_packer_repeat] FAILED: 8.4 — report differs between "
                        "runs: r1{w=%d o=%d s=%d b=%llu} r2{w=%d o=%d s=%d b=%llu}\n",
                        report1.written, report1.omitted, report1.scanned,
                        static_cast<unsigned long long>(report1.written_bytes),
                        report2.written, report2.omitted, report2.scanned,
                        static_cast<unsigned long long>(report2.written_bytes));
            ok = false;
        }
        // The report arithmetic should reflect the fixture: 3 used files written,
        // the unused source file omitted, and a positive written-byte total.
        if (std::cmp_not_equal(report1.written, used.size())) {
            std::printf("[integ_packer_repeat] FAILED: 8.4 — expected %zu written, "
                        "got %d\n",
                        used.size(), report1.written);
            ok = false;
        }
        if (report1.omitted < 1) {
            std::printf("[integ_packer_repeat] FAILED: 8.4 — expected the unused "
                        "source file to be omitted (omitted=%d)\n",
                        report1.omitted);
            ok = false;
        }
        if (report1.written_bytes == 0) {
            std::printf("[integ_packer_repeat] FAILED: 8.4 — expected a positive "
                        "written-byte total\n");
            ok = false;
        }
    }

    // ── 6.4: empty / whitespace-only / non-existent Data_Root → EXIT_DATA_ROOT,
    //          nothing written to the Pack_Target ──
    {
        const std::vector<std::pair<const char*, std::string>> bad_roots = {
            {"empty", std::string{}},
            {"whitespace", std::string("   ")},
            {"non-existent", (base / "no_such_data_root").string()},
        };
        unsigned i = 0;
        for (const auto& [label, dir] : bad_roots) {
            heaven::AppConfig config;
            config.data_dir = dir;

            heaven::PackOptions opts;
            // A fresh, non-existent Pack_Target per case: the Data_Root failure
            // is fatal BEFORE target preparation, so the target must never be
            // created/written.
            const fs::path packed = base / ("dr_target_" + std::to_string(i++));
            opts.target = packed.string();
            opts.overwrite = false;
            opts.move = false;

            const int code = heaven::run_asset_packer(config, opts);
            if (code != heaven::EXIT_DATA_ROOT) {
                std::printf("[integ_packer_repeat] FAILED: 6.4 — %s Data_Root returned "
                            "%d, expected EXIT_DATA_ROOT (%d)\n",
                            label, code, heaven::EXIT_DATA_ROOT);
                ok = false;
            }
            // No Asset_File written: the target was never even created.
            if (fs::exists(packed, ec)) {
                std::printf("[integ_packer_repeat] FAILED: 6.4 — %s Data_Root wrote to "
                            "the Pack_Target '%s'\n",
                            label, packed.string().c_str());
                ok = false;
            }
        }
    }

    // ── 6.3: EXIT_UNRESOLVED is distinct from EXIT_DATA_ROOT, and the unresolved
    //          path is reachable/recorded/rendered (the contract run_asset_packer
    //          uses to choose EXIT_UNRESOLVED after writing the resolved set) ──
    {
        if (heaven::EXIT_UNRESOLVED == heaven::EXIT_DATA_ROOT) {
            std::printf("[integ_packer_repeat] FAILED: 6.3 — EXIT_UNRESOLVED (%d) must "
                        "be distinct from EXIT_DATA_ROOT (%d)\n",
                        heaven::EXIT_UNRESOLVED, heaven::EXIT_DATA_ROOT);
            ok = false;
        }

        // A reference graph with one resolvable root pointing at a missing target
        // models the "completed with unresolved references" case: the resolved
        // node is reachable (would be written), and the missing edge is recorded
        // with its referrer — exactly the list run_asset_packer renders before
        // exiting EXIT_UNRESOLVED.
        struct MissingRefGraph : heaven::GraphView {
            std::optional<std::vector<heaven::AssetId>>
            references(const heaven::AssetId& node) const override {
                if (node == "heaven/world.node")
                    return std::vector<heaven::AssetId>{"heaven/missing.dds"};
                if (node == "heaven/missing.dds")
                    return std::nullopt; // unresolvable target
                return std::vector<heaven::AssetId>{};
            }
        } graph;

        const heaven::TraversalResult tr =
            heaven::traverse_reference_graph({"heaven/world.node"}, graph);

        // The resolvable root is in the written set; the missing target is not.
        if (!tr.reachable.contains("heaven/world.node") ||
            tr.reachable.contains("heaven/missing.dds")) {
            std::printf("[integ_packer_repeat] FAILED: 6.3 — unresolved target leaked "
                        "into (or root missing from) the resolved set\n");
            ok = false;
        }
        // The unresolved edge is recorded with its referrer.
        const bool recorded =
            tr.unresolved.size() == 1 &&
            tr.unresolved[0].missing_rel_path == "heaven/missing.dds" &&
            tr.unresolved[0].referring_rel_path == "heaven/world.node";
        if (!recorded) {
            std::printf("[integ_packer_repeat] FAILED: 6.3 — unresolved edge not "
                        "recorded with its referrer\n");
            ok = false;
        }
        // The renderer that feeds the EXIT_UNRESOLVED branch produces a non-empty
        // listing naming the missing path and its referrer.
        const std::string rendered =
            heaven::render_unresolved_list(tr.unresolved);
        if (rendered.find("heaven/missing.dds") == std::string::npos ||
            rendered.find("heaven/world.node") == std::string::npos) {
            std::printf("[integ_packer_repeat] FAILED: 6.3 — rendered unresolved list "
                        "does not name the missing path + referrer; got:\n%s",
                        rendered.c_str());
            ok = false;
        }
    }

    fs::remove_all(base, ec);
    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_integ_packer_repeat{integ_packer_repeat};

} // namespace
