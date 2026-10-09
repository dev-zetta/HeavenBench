// Feature: used-asset-packer,: Reference-graph reachability
/*
 * HeavenBench — Property test for the used-asset packer's pure traversal
 * core (traverse_reference_graph).
 *
 * Feature: used-asset-packer
 *   •: Reference-graph reachability
 *
 *
 * (reference-graph reachability):
 *   For any reference graph and any set of root assets,
 *   traverse_reference_graph returns a `reachable` set that contains EXACTLY
 *   the assets reachable from the roots by following references through
 *   resolvable nodes — every reachable resolvable asset is included, and no
 *   asset that is unreachable from any root is included.
 *
 * Strategy:
 *   A random in-memory reference graph is generated over a small universe of
 *   node ids ("n0".."n{K-1}"). A random subset of those ids is "resolvable"
 *   (present in the edge map, with random out-edges into the universe); the
 *   remaining ids — and any edge target outside the universe — are
 *   unresolvable, so the GraphView's references() returns std::nullopt for
 *   them. A random subset of ids forms the roots (roots may themselves be
 *   resolvable or not).
 *
 *   The expected reachable set is computed by an INDEPENDENT depth-first walk
 *   in the test: starting from the roots, a node contributes to the expected
 *   set only when it is resolvable, and only a resolvable node's out-edges are
 *   followed (an unresolvable node terminates that branch). The property then
 *   asserts traverse_reference_graph's `reachable` equals that independent set
 *   exactly — proving inclusion of every reachable resolvable asset and
 *   exclusion of everything unreachable.
 *
 * traverse_reference_graph is pure (no I/O, no GPU), so this runs on the host
 * `heaven_benchmark_tests` target. The file self-registers with the RapidCheck
 * harness in test_main.cpp via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point. RapidCheck's default of 100 successful iterations meets the
 * "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <functional>
#include <map>
#include <optional>
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

// Independent reference reachability: a depth-first walk from the roots that
// follows out-edges ONLY through resolvable nodes and collects only resolvable
// nodes. This is intentionally a separate implementation from the production
// worklist so the property is a genuine cross-check, not a tautology.
std::set<AssetId>
expected_reachable(const std::vector<AssetId>& roots,
                   const std::map<AssetId, std::vector<AssetId>>& edges) {
    std::set<AssetId> reachable;
    std::set<AssetId> seen;
    std::vector<AssetId> stack(roots.begin(), roots.end());

    while (!stack.empty()) {
        AssetId node = stack.back();
        stack.pop_back();
        if (!seen.insert(node).second) continue;  // already processed

        auto it = edges.find(node);
        if (it == edges.end()) continue;  // unresolvable: never reachable, no edges

        reachable.insert(node);
        for (const AssetId& child : it->second) {
            if (!seen.contains(child)) stack.push_back(child);
        }
    }
    return reachable;
}

bool run_p1_reachability_property() {
    return rc::check(
        "Feature: used-asset-packer, Property 1: Reference-graph reachability "
        "(Validates: Requirements 2.1, 2.2, 2.3, 2.4, 2.5, 2.7)",
        [] {
            // Universe of node ids "n0".."n{K-1}". K may be 0 (empty graph).
            const int k = *rc::gen::inRange(0, 9);

            // Decide which ids are resolvable, and give each resolvable id a
            // random list of out-edges into the universe. Edge targets are
            // ordinary universe ids; whether a target is resolvable is decided
            // independently, so plenty of edges point at unresolvable nodes.
            MapGraph graph;
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
                graph.edges.emplace(node_id(i), std::move(out));
            }

            // Random root subset over the universe (roots may be resolvable or
            // not, and may repeat).
            std::vector<AssetId> roots;
            if (k > 0) {
                const std::vector<int> root_ids =
                    *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, k));
                roots.reserve(root_ids.size());
                for (int r : root_ids) roots.push_back(node_id(r));
            }

            const TraversalResult result =
                heaven::traverse_reference_graph(roots, graph);

            const std::set<AssetId> expected =
                expected_reachable(roots, graph.edges);

            // `reachable` is EXACTLY the resolvable-through-resolvable set: set
            // equality proves both directions at once — every reachable
            // resolvable asset is present, and no unreachable asset leaks in.
            RC_ASSERT(result.reachable == expected);
        });
}

const heaven::testing::Register g_register_packer_reachability{
    &run_p1_reachability_property};

} // namespace
