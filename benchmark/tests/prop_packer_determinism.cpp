// Feature: used-asset-packer,: Traversal result is order-independent (deterministic)
/*
 * HeavenBench — Property test for the used-asset packer's pure traversal
 * core (traverse_reference_graph).
 *
 * Feature: used-asset-packer
 *   •: Traversal result is order-independent (deterministic)
 *
 *
 * (order-independence / determinism):
 *   For any reference graph and root set, permuting the order of the roots and
 *   the order of each asset's out-edges yields an IDENTICAL `reachable` set
 *   (compared by Relative_Asset_Path). The traversal result does not depend on
 *   enumeration or traversal order.
 *
 * Strategy:
 *   A random in-memory reference graph is generated over a small universe of
 *   node ids ("n0".."n{K-1}"), exactly as in the reachability property: a
 *   random subset of ids is "resolvable" (present in the edge map with random
 *   out-edges into the universe), the rest are unresolvable (references()
 *   returns std::nullopt). A random root list is drawn over the universe.
 *
 *   The graph and roots are then PERMUTED two ways that must not change the
 *   result: (1) the root list is shuffled, and (2) every resolvable node's
 *   out-edge list is shuffled. The permutation is driven by a RapidCheck-
 *   generated seed fed to std::shuffle (std::shuffle reorders the SAME elements,
 *   so membership is preserved while order varies); the seed ranges over many
 *   distinct orderings across iterations. The property asserts that the
 *   `reachable` set from the original ordering equals the `reachable` set from
 *   the permuted ordering — proving the Used_Asset_Set membership is independent
 *   of the order roots are supplied and the order each node enumerates its
 *   references.
 *
 *   `reachable` is a std::set<AssetId>, so equality is by Relative_Asset_Path
 *   exactly as the requirement specifies.
 *
 * traverse_reference_graph is pure (no I/O, no GPU), so this runs on the host
 * `heaven_benchmark_tests` target. The file self-registers with the RapidCheck
 * harness in test_main.cpp via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point. RapidCheck's default of 100 successful iterations meets the
 * "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <random>
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

using heaven::AssetId;
using heaven::GraphView;
using heaven::TraversalResult;

// In-memory GraphView over an explicit edge map. A node present in the map is
// resolvable and yields its (possibly empty) out-edge list; a node absent from
// the map is unresolvable, so references() returns std::nullopt — exactly the
// contract the production discovery adapter honours for missing/unreadable
// files.
struct MapGraph final : GraphView {
    std::map<AssetId, std::vector<AssetId>> edges;

    [[nodiscard]] std::optional<std::vector<AssetId>>
    references(const AssetId& node) const override {
        auto it = edges.find(node);
        if (it == edges.end()) return std::nullopt;
        return it->second;
    }
};

// Stable node id for universe index i.
AssetId node_id(int i) { return "n" + std::to_string(i); }

bool run_p5_determinism_property() {
    return rc::check(
        "Feature: used-asset-packer, Property 5: Traversal result is "
        "order-independent (deterministic) "
        "(Validates: Requirements 8.1, 8.4)",
        [] {
            // Universe of node ids "n0".."n{K-1}". K may be 0 (empty graph).
            const int k = *rc::gen::inRange(0, 9);

            // Decide which ids are resolvable and give each a random list of
            // out-edges into the universe. This is the canonical ("original")
            // ordering of both the edge lists and (below) the roots.
            MapGraph original;
            for (int i = 0; i < k; ++i) {
                const bool resolvable = *rc::gen::arbitrary<bool>();
                if (!resolvable) continue;

                std::vector<AssetId> out;
                if (k > 0) {
                    const std::vector<int> targets =
                        *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, k));
                    out.reserve(targets.size());
                    for (int t : targets) out.push_back(node_id(t));
                }
                original.edges.emplace(node_id(i), std::move(out));
            }

            // Random root list over the universe (roots may be resolvable or
            // not, and may repeat).
            std::vector<AssetId> roots;
            if (k > 0) {
                const std::vector<int> root_ids =
                    *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, k));
                roots.reserve(root_ids.size());
                for (int r : root_ids) roots.push_back(node_id(r));
            }

            // Build the PERMUTED ordering: the same graph and roots, but with
            // the root list shuffled and every node's out-edge list shuffled.
            // std::shuffle reorders the SAME elements, so membership is
            // preserved while order varies; a RapidCheck-generated seed drives
            // the permutation across many distinct orderings.
            const std::uint64_t seed = *rc::gen::arbitrary<std::uint64_t>();
            std::mt19937_64 rng(seed);

            std::vector<AssetId> roots_permuted = roots;
            std::shuffle(roots_permuted.begin(), roots_permuted.end(), rng);

            MapGraph permuted;
            for (const auto& [node, out] : original.edges) {
                std::vector<AssetId> out_permuted = out;
                std::shuffle(out_permuted.begin(), out_permuted.end(), rng);
                permuted.edges.emplace(node, std::move(out_permuted));
            }

            const TraversalResult original_result =
                heaven::traverse_reference_graph(roots, original);
            const TraversalResult permuted_result =
                heaven::traverse_reference_graph(roots_permuted, permuted);

            // Order independence: the reachable set (compared by
            // Relative_Asset_Path via std::set equality) is identical
            // regardless of root order or per-node edge order.
            RC_ASSERT(original_result.reachable == permuted_result.reachable);
        });
}

const heaven::testing::Register g_register_packer_determinism{
    &run_p5_determinism_property};

} // namespace
