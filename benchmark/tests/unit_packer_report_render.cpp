/*
 * HeavenBench — unit test: packer report / error rendering
 *
 *
 *
 * run_asset_packer's printed output is built from three pure rendering helpers
 * so the rendered text can be pinned without driving the orchestrator's I/O or
 * exit decisions. This test exercises those helpers directly:
 *
 *   - render_unresolved_list: a known unresolved list renders each entry with
 *     BOTH its missing Relative_Asset_Path AND its referring Asset_File, with a
 *     count header. The empty list renders as an empty string.
 *
 *   - render_pack_report: the success report includes a written-bytes line
 *     whose value is a non-negative integer, alongside the
 *     written/omitted/scanned counts.
 *
 *   - render_write_failure: a pre-completion failure renders an error
 *     indication that names the failure (an error, not the success report) AND
 *     reports the written count as zero.
 *
 * These are pure string builders with no filesystem or GPU dependency, so the
 * test needs no temp tree and never skips.
 *
 * The test self-registers with the shared `heaven_benchmark_tests` harness
 * (test_main.cpp) at static-init time via heaven::testing::Register, matching
 * the other test translation units; the CMake test-source glob picks it up
 * without a CMake edit. The registered callable returns true on success and
 * false (after printing a diagnostic) on failure.
 */
#include <cstdio>
#include <functional>
#include <string>
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

// True when `haystack` contains `needle` as a substring.
bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

bool unit_packer_report_render() {
    bool ok = true;

    // ── 1. Unresolved list: each entry shows its path AND referrer  ──
    {
        const std::vector<heaven::Unresolved> unresolved = {
            {"heaven/textures/airship/balloon_d.dds", "heaven/materials/airship.mat"},
            {"heaven/meshes/props/missing.mesh", "heaven/nodes/scene.node"},
        };

        const std::string rendered = heaven::render_unresolved_list(unresolved);

        // Every entry must surface both its missing Relative_Asset_Path and the
        // referring Asset_File that pointed at it.
        for (const heaven::Unresolved& u : unresolved) {
            if (!contains(rendered, u.missing_rel_path)) {
                std::printf("[unit_packer_report_render] FAILED: unresolved render "
                            "missing path '%s'. Got:\n%s\n",
                            u.missing_rel_path.c_str(), rendered.c_str());
                ok = false;
            }
            if (!contains(rendered, u.referring_rel_path)) {
                std::printf("[unit_packer_report_render] FAILED: unresolved render "
                            "missing referrer '%s'. Got:\n%s\n",
                            u.referring_rel_path.c_str(), rendered.c_str());
                ok = false;
            }
        }

        // The header reports how many unresolved references there are.
        if (!contains(rendered, "2")) {
            std::printf("[unit_packer_report_render] FAILED: unresolved render "
                        "missing count header. Got:\n%s\n",
                        rendered.c_str());
            ok = false;
        }

        // An empty list renders as nothing so the caller prints nothing.
        const std::string empty_render =
            heaven::render_unresolved_list(std::vector<heaven::Unresolved>{});
        if (!empty_render.empty()) {
            std::printf("[unit_packer_report_render] FAILED: empty unresolved list "
                        "should render empty, got:\n%s\n",
                        empty_render.c_str());
            ok = false;
        }
    }

    // ── 2. Success report: non-negative written-bytes line  ──
    {
        heaven::PackReport report;
        report.written = 3;
        report.omitted = 5;
        report.scanned = 8;
        report.written_bytes = 4096;

        const std::string rendered = heaven::render_pack_report(report);

        // A written-bytes line must be present with the exact (non-negative)
        // byte total.
        if (!contains(rendered, "written bytes: 4096")) {
            std::printf("[unit_packer_report_render] FAILED: success report missing "
                        "non-negative written-bytes line. Got:\n%s\n",
                        rendered.c_str());
            ok = false;
        }

        // Sanity: the counts are reflected too.
        if (!contains(rendered, "written: 3 files") ||
            !contains(rendered, "omitted: 5 files") ||
            !contains(rendered, "scanned: 8 files")) {
            std::printf("[unit_packer_report_render] FAILED: success report missing "
                        "expected counts. Got:\n%s\n",
                        rendered.c_str());
            ok = false;
        }

        // Zero bytes is a valid non-negative value and must render as "0".
        heaven::PackReport zero;
        zero.written = 0;
        zero.omitted = 0;
        zero.scanned = 0;
        zero.written_bytes = 0;
        const std::string zero_render = heaven::render_pack_report(zero);
        if (!contains(zero_render, "written bytes: 0")) {
            std::printf("[unit_packer_report_render] FAILED: zero-byte report should "
                        "render 'written bytes: 0'. Got:\n%s\n",
                        zero_render.c_str());
            ok = false;
        }
    }

    // ── 3. Pre-completion failure: error indication, written count zero  ──
    {
        const std::string target = "/tmp/pack_out";
        const std::string rendered = heaven::render_write_failure(target);

        // It is an error indication, not a success report.
        if (contains(rendered, "Pack report")) {
            std::printf("[unit_packer_report_render] FAILED: write-failure should not "
                        "render the success report. Got:\n%s\n",
                        rendered.c_str());
            ok = false;
        }
        if (!contains(rendered, "fail")) {
            std::printf("[unit_packer_report_render] FAILED: write-failure should "
                        "indicate failure. Got:\n%s\n",
                        rendered.c_str());
            ok = false;
        }

        // The written count is reported as zero.
        if (!contains(rendered, "0 files written")) {
            std::printf("[unit_packer_report_render] FAILED: write-failure should "
                        "report written count zero. Got:\n%s\n",
                        rendered.c_str());
            ok = false;
        }

        // The conflicting Pack_Target is named so the developer can act on it.
        if (!contains(rendered, target)) {
            std::printf("[unit_packer_report_render] FAILED: write-failure should "
                        "name the Pack_Target '%s'. Got:\n%s\n",
                        target.c_str(), rendered.c_str());
            ok = false;
        }
    }

    return ok;
}

// Register the check with the harness at static-init time.
const heaven::testing::Register reg_unit_packer_report_render{unit_packer_report_render};

} // namespace
