// Feature: used-asset-packer,: Pack-report counts are consistent
/*
 * HeavenBench — Property test for the used-asset packer's pack-report
 * arithmetic (compute_pack_report).
 *
 * Feature: used-asset-packer
 *   •: Pack-report counts are consistent
 *
 *
 * (pack-report counts are consistent):
 *   For any `used` set and `scanned` set of Relative_Asset_Paths,
 *   compute_pack_report produces
 *       written = |used ∩ scanned| ≥ 0,
 *       omitted = |scanned| − written ≥ 0, and
 *       written + omitted == |scanned|;
 *   and the set of omitted paths is exactly `scanned \ used`, so no path
 *   counted as written is also counted as omitted (the written/omitted
 *   partitions of `scanned` are disjoint).
 *
 * Strategy:
 *   `used` and `scanned` are generated as random subsets of a small shared
 *   universe of path-like strings ("p0".."p{K-1}"). Drawing both from one
 *   universe guarantees frequent, non-trivial overlap (so the intersection and
 *   difference are genuinely exercised) while still allowing `used` to contain
 *   paths absent from `scanned` — the case the implementation guards against by
 *   computing omitted as a subtraction. The written_bytes input is an arbitrary
 *   non-negative value and is asserted to pass through unchanged.
 *
 *   The expected partitions are computed by INDEPENDENT std::set operations in
 *   the test (set_intersection for the written set, set_difference for the
 *   omitted set), then cross-checked against the report's counts. Disjointness
 *   is verified directly: the written and omitted partitions share no member,
 *   and their union is exactly `scanned`.
 *
 * compute_pack_report is pure (no I/O, no GPU), so this runs on the host
 * `heaven_benchmark_tests` target. The file self-registers with the RapidCheck
 * harness in test_main.cpp via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point. RapidCheck's default of 100 successful iterations meets the
 * "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <set>
#include <string>
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

// Stable path-like id for universe index i. Using a shared universe for both
// `used` and `scanned` keeps their intersection and difference non-degenerate
// across iterations.
std::string path_id(int i) { return "p" + std::to_string(i); }

// Draw a random subset of the universe "p0".."p{K-1}" into a std::set. The
// resulting set is automatically deduplicated and sorted — the same shape the
// production code feeds compute_pack_report.
std::set<std::string> random_subset(int k) {
    std::set<std::string> out;
    if (k > 0) {
        const std::vector<int> picks =
            *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, k));
        for (int p : picks) out.insert(path_id(p));
    }
    return out;
}

bool run_p6_report_consistency_property() {
    return rc::check(
        "Feature: used-asset-packer, Property 6: Pack-report counts are "
        "consistent (Validates: Requirements 7.1, 7.2, 7.4, 3.4)",
        [] {
            // Shared universe size; K may be 0 (both sets empty).
            const int k = *rc::gen::inRange(0, 12);

            const std::set<std::string> used = random_subset(k);
            const std::set<std::string> scanned = random_subset(k);
            const auto written_bytes =
                static_cast<uintmax_t>(*rc::gen::arbitrary<uint32_t>());

            const heaven::PackReport report =
                heaven::compute_pack_report(used, scanned, written_bytes);

            // Independent partitions of `scanned`: the written partition is
            // used ∩ scanned, the omitted partition is scanned \ used.
            std::set<std::string> written_set;
            std::set_intersection(used.begin(), used.end(),
                                  scanned.begin(), scanned.end(),
                                  std::inserter(written_set, written_set.end()));
            std::set<std::string> omitted_set;
            std::set_difference(scanned.begin(), scanned.end(),
                                used.begin(), used.end(),
                                std::inserter(omitted_set, omitted_set.end()));

            // Non-negativity (7.1, 7.2) and pass-through of scanned/bytes (7.3).
            RC_ASSERT(report.written >= 0);
            RC_ASSERT(report.omitted >= 0);
            RC_ASSERT(report.scanned == static_cast<int>(scanned.size()));
            RC_ASSERT(report.written_bytes == written_bytes);

            // Counts match the independently computed partitions.
            RC_ASSERT(report.written == static_cast<int>(written_set.size()));
            RC_ASSERT(report.omitted == static_cast<int>(omitted_set.size()));

            // The sum identity (7.4 / complement of 3.4): every scanned file is
            // either written or omitted, never neither and never both.
            RC_ASSERT(report.written + report.omitted == report.scanned);

            // Disjointness: no scanned path is counted as both written and
            // omitted, and the two partitions together cover exactly `scanned`.
            std::vector<std::string> overlap;
            std::set_intersection(written_set.begin(), written_set.end(),
                                  omitted_set.begin(), omitted_set.end(),
                                  std::back_inserter(overlap));
            RC_ASSERT(overlap.empty());

            std::set<std::string> union_set = written_set;
            union_set.insert(omitted_set.begin(), omitted_set.end());
            RC_ASSERT(union_set == scanned);
        });
}

const heaven::testing::Register g_register_packer_report{
    &run_p6_report_consistency_property};

} // namespace
