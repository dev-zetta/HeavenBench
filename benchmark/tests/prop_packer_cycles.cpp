// Feature: used-asset-packer,: Cyclic references terminate
/*
 * HeavenBench — Property test for the used-asset packer's pure traversal
 * core (traverse_reference_graph).
 *
 * Feature: used-asset-packer
 *   •: Cyclic references terminate
 *
 *
 * (cyclic references terminate):
 *   For any reference graph that contains cycles (including self-loops),
 *   traverse_reference_graph TERMINATES and returns the SAME reachable set it
 *   would for the cycle-free graph with the same reachability, expanding each
 *   asset's references at most once.
 *
 * Strategy:
 *   We build TWO graphs over a small universe of node ids ("n0".."n{K-1}") that
 *   are guaranteed to have IDENTICAL reachability, one acyclic and one cyclic:
 *
 *     • The acyclic graph is a random DAG: every node is resolvable (present in
 *       the edge map) and each node i only emits FORWARD edges to targets j > i.
 *       Forward-only edges cannot form a cycle, so this graph is cycle-free by
 *       construction. Its reachable-from-roots set R is computed by an
 *       INDEPENDENT depth-first walk in the test (a genuine cross-check, not a
 *       tautology against the production worklist).
 *
 *     • The cyclic graph is the acyclic graph PLUS cycle-inducing edges that
 *       provably do not change reachability: for every node already in R we add
 *       (a) a SELF-LOOP (id → id) and (b) a random set of extra edges whose
 *       targets are also drawn from R. Because both endpoints of every added
 *       edge already lie in the reachable set R — and R is closed under the
 *       original edges — these additions cannot make any new node reachable, so
 *       the cyclic graph's reachability equals the acyclic graph's exactly.
 *       The self-loops (and any back-edge among R's members) introduce real
 *       cycles whenever R is non-empty.
 *
 *   The property then asserts, despite the injected cycles/self-loops:
 *     1. Termination — the call returns at all (a non-terminating traversal
 *        would hang the test); the visited-set guard is what makes a node's
 *        references expand at most once and thus bounds the work.
 *     2. Same reachable set — the cyclic graph's `reachable` equals the
 *        cycle-free graph's `reachable`, and both equal the independently
 *        computed R (equality by Relative_Asset_Path via std::set).
 *     3. At-most-once expansion — a per-node CALL COUNTER on the cyclic
 *        GraphView proves references() is invoked at most once per node and
 *        EXACTLY once for every reachable node, so the total expansion work
 *        equals the number of distinct reachable nodes even with the cycles.
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
// the map is unresolvable, so references() returns std::nullopt. Used here for
// the cycle-free reference graph.
struct MapGraph final : GraphView {
    std::map<AssetId, std::vector<AssetId>> edges;

    [[nodiscard]] std::optional<std::vector<AssetId>>
    references(const AssetId& node) const override {
        auto it = edges.find(node);
        if (it == edges.end()) return std::nullopt;
        return it->second;
    }
};

// In-memory GraphView that additionally COUNTS how many times references() is
// queried for each node. traverse_reference_graph calls references() exactly
// once when it expands a node (after the visited-set guard) and never again, so
// the counter is a direct witness for "how many times this asset was expanded".
// Used here for the cyclic reference graph to prove the cycles never cause a
// re-expansion. The counter map is mutable so the const override can record.
struct CountingGraph final : GraphView {
    std::map<AssetId, std::vector<AssetId>> edges;
    mutable std::map<AssetId, int>          calls;

    [[nodiscard]] std::optional<std::vector<AssetId>>
    references(const AssetId& node) const override {
        ++calls[node];
        auto it = edges.find(node);
        if (it == edges.end()) return std::nullopt;
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

bool run_p3_cycles_property() {
    return rc::check(
        "Feature: used-asset-packer, Property 3: Cyclic references terminate "
        "(Validates: Requirements 2.10)",
        [] {
            // Universe of node ids "n0".."n{K-1}". K may be 0 (empty graph).
            const int k = *rc::gen::inRange(0, 9);

            // Build a random DAG: every node is resolvable and emits only
            // FORWARD edges (i → j with j > i), which cannot form a cycle.
            MapGraph acyclic;
            for (int i = 0; i < k; ++i) {
                std::vector<AssetId> out;
                if (i + 1 < k) {
                    const std::vector<int> targets =
                        *rc::gen::container<std::vector<int>>(
                            rc::gen::inRange(i + 1, k));
                    out.reserve(targets.size());
                    for (int t : targets) out.push_back(node_id(t));
                }
                acyclic.edges.emplace(node_id(i), std::move(out));
            }

            // Random root list over the universe (roots may repeat).
            std::vector<AssetId> roots;
            if (k > 0) {
                const std::vector<int> root_ids =
                    *rc::gen::container<std::vector<int>>(rc::gen::inRange(0, k));
                roots.reserve(root_ids.size());
                for (int r : root_ids) roots.push_back(node_id(r));
            }

            // Independently computed reachable set of the cycle-free graph.
            const std::set<AssetId> expected =
                expected_reachable(roots, acyclic.edges);
            const std::vector<AssetId> r_vec(expected.begin(), expected.end());

            // Build the cyclic graph: the same DAG plus cycle-inducing edges
            // that cannot change reachability. For every reachable node add a
            // self-loop and a random set of extra edges into the reachable set
            // R. Both endpoints already lie in R (which is closed under the
            // original edges), so reachability is unchanged while real cycles
            // (self-loops + back-edges among R) are introduced whenever R is
            // non-empty.
            CountingGraph cyclic;
            cyclic.edges = acyclic.edges;
            for (const AssetId& id : r_vec) {
                std::vector<AssetId>& out = cyclic.edges.at(id);
                out.push_back(id);  // self-loop → guaranteed cycle
                if (!r_vec.empty()) {
                    const std::vector<int> extra =
                        *rc::gen::container<std::vector<int>>(
                            rc::gen::inRange(0, static_cast<int>(r_vec.size())));
                    for (int idx : extra) out.push_back(r_vec[idx]);
                }
            }

            // (1) Termination: both calls must RETURN. A traversal that failed
            // to terminate on the cycles would hang here rather than fail.
            const TraversalResult cyclic_result =
                heaven::traverse_reference_graph(roots, cyclic);
            const TraversalResult acyclic_result =
                heaven::traverse_reference_graph(roots, acyclic);

            // (2) Same reachable set: the cyclic graph yields exactly the
            // cycle-free graph's reachable set, and both equal the independent
            // walk's R (equality by Relative_Asset_Path via std::set).
            RC_ASSERT(cyclic_result.reachable == acyclic_result.reachable);
            RC_ASSERT(cyclic_result.reachable == expected);

            // (3) At-most-once expansion: despite the cycles, references() is
            // queried AT MOST ONCE per node, and the total expansion work
            // equals the number of distinct reachable nodes.
            int total_calls = 0;
            for (const auto& [id, count] : cyclic.calls) {
                RC_ASSERT(count <= 1);
                total_calls += count;
            }
            RC_ASSERT(static_cast<std::size_t>(total_calls) ==
                      cyclic_result.reachable.size());

            // (3, restated) Every reachable node was expanded EXACTLY once.
            for (const AssetId& id : cyclic_result.reachable) {
                RC_ASSERT(cyclic.calls.at(id) == 1);
            }
        });
}

const heaven::testing::Register g_register_packer_cycles{
    &run_p3_cycles_property};

} // namespace
