// Feature: used-asset-packer,: Each asset included at most once
/*
 * HeavenBench — Property test for the used-asset packer's pure traversal
 * core (traverse_reference_graph).
 *
 * Feature: used-asset-packer
 *   •: Each asset included at most once
 *
 *
 * (each asset included at most once):
 *   For any reference graph — including one with arbitrarily many distinct
 *   edges pointing at the same asset — the `reachable` result contains each
 *   asset identifier at most once, and the traversal expands any single
 *   asset's references at most once (repeated edges to an already-visited
 *   asset add no duplicate membership and trigger no re-expansion).
 *
 * Strategy:
 *   A random in-memory reference graph is generated over a small universe of
 *   node ids ("n0".."n{K-1}"). Every node in the universe is resolvable
 *   (present in the edge map) so that every edge target is a resolvable node;
 *   this keeps the focus squarely on the dedup/at-most-once invariant rather
 *   than the unresolved-reference path (covered by). Each node's
 *   out-edge list is then DELIBERATELY DUPLICATED — every generated edge is
 *   emitted twice — so the graph carries many distinct edges pointing at the
 *   same asset. The root list is likewise duplicated so the same root appears
 *   multiple times.
 *
 *   The GraphView used here wraps the edge map with a per-node CALL COUNTER:
 *   every invocation of references(node) increments a counter for `node`. The
 *   traversal expands a node by calling references() on it exactly once before
 *   marking it visited, so the counter is a faithful witness of "how many
 *   times was this asset expanded".
 *
 *   The property then asserts three things despite the duplicated edges/roots:
 *     1. No re-expansion — references() is called AT MOST ONCE per node, and
 *        EXACTLY ONCE for every node that ends up reachable.
 *     2. No wasted expansion — the total number of references() calls equals
 *        the number of distinct reachable nodes (so duplicated edges never
 *        cause extra expansions).
 *     3. At-most-once membership — `reachable` contains exactly the set
 *        reachable from the roots, with each id present once (a std::set holds
 *        no duplicates by construction, cross-checked against an independent
 *        reachability walk).
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

// In-memory GraphView over an explicit edge map that additionally COUNTS how
// many times references() is queried for each node. Because traverse_reference_
// graph calls references() exactly once when it expands a node and never again
// (the node is then in the visited set), the counter is a direct witness for
// "number of times this asset was expanded". Every node in this test is present
// in the map and therefore resolvable. The counter map is mutable so the const
// references() override can record into it.
struct CountingGraph final : GraphView {
    std::map<AssetId, std::vector<AssetId>> edges;
    mutable std::map<AssetId, int>          calls;

    [[nodiscard]] std::optional<std::vector<AssetId>>
    references(const AssetId& node) const override {
        ++calls[node];
        auto it = edges.find(node);
        if (it == edges.end()) return std::nullopt;  // unreachable here: all resolvable
        return it->second;
    }
};

// Stable node id for universe index i.
AssetId node_id(int i) { return "n" + std::to_string(i); }

// Independent reference reachability: a depth-first walk from the roots that
// follows out-edges ONLY through resolvable nodes and collects only resolvable
// nodes. A separate implementation from the production worklist so the property
// is a genuine cross-check, not a tautology.
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

bool run_p2_dedup_property() {
    return rc::check(
        "Feature: used-asset-packer, Property 2: Each asset included at most "
        "once (Validates: Requirements 2.9)",
        [] {
            // Universe of node ids "n0".."n{K-1}". K may be 0 (empty graph).
            const int k = *rc::gen::inRange(0, 9);

            // Every node is resolvable. Each gets a random list of out-edges
            // into the universe, then that list is DUPLICATED so every edge
            // appears (at least) twice — guaranteeing many distinct edges that
            // point at the same asset.
            CountingGraph graph;
            for (int i = 0; i < k; ++i) {
                std::vector<AssetId> out;
                if (k > 0) {
                    const std::vector<int> targets =
                        *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, k));
                    out.reserve(targets.size() * 2);
                    for (int t : targets) out.push_back(node_id(t));
                    // Duplicate every edge: append a second identical copy.
                    out.insert(out.end(), out.begin(), out.end());
                }
                graph.edges.emplace(node_id(i), std::move(out));
            }

            // Random root subset, then duplicated so the same root recurs.
            std::vector<AssetId> roots;
            if (k > 0) {
                const std::vector<int> root_ids =
                    *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, k));
                roots.reserve(root_ids.size() * 2);
                for (int r : root_ids) roots.push_back(node_id(r));
                roots.insert(roots.end(), roots.begin(), roots.end());
            }

            const TraversalResult result =
                heaven::traverse_reference_graph(roots, graph);

            const std::set<AssetId> expected =
                expected_reachable(roots, graph.edges);

            // (3) At-most-once membership: `reachable` is exactly the set
            // reachable from the roots. A std::set holds each id once by
            // construction; cross-checking against the independent walk proves
            // the duplicated edges/roots neither added nor dropped any member.
            RC_ASSERT(result.reachable == expected);

            // (1) No re-expansion: references() is called AT MOST ONCE per
            // node, and (2) total calls equal the number of distinct reachable
            // nodes — duplicated edges never trigger an extra expansion.
            int total_calls = 0;
            for (const auto& [id, count] : graph.calls) {
                RC_ASSERT(count <= 1);
                total_calls += count;
            }
            RC_ASSERT(static_cast<std::size_t>(total_calls) == result.reachable.size());

            // (1, restated) Every reachable node was expanded EXACTLY once.
            for (const AssetId& id : result.reachable) {
                RC_ASSERT(graph.calls.at(id) == 1);
            }
        });
}

const heaven::testing::Register g_register_packer_dedup{
    &run_p2_dedup_property};

} // namespace
