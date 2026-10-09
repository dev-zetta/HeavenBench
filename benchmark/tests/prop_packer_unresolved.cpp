// Feature: used-asset-packer,: Unresolved references are excluded, recorded, and non-fatal
/*
 * HeavenBench — Property test for the used-asset packer's pure traversal
 * core (traverse_reference_graph), unresolved-reference handling.
 *
 * Feature: used-asset-packer
 *   •: Unresolved references are excluded, recorded, and non-fatal
 *
 *
 * (unresolved references are excluded, recorded, and non-fatal):
 *   For any reference graph in which an arbitrary subset of referenced assets
 *   is marked unresolvable, every such asset is ABSENT from `reachable`,
 *   appears in the `unresolved` list paired with a referring asset that pointed
 *   at it, and every other asset reachable through resolvable references still
 *   appears in `reachable` (traversal continues PAST the unresolved edges).
 *
 * Strategy:
 *   A random in-memory reference graph is generated over a small universe of
 *   node ids ("n0".."n{K-1}"). A random subset of those ids is "resolvable"
 *   (present in the edge map, with random out-edges into the universe); every
 *   other id is unresolvable, so the GraphView's references() returns
 *   std::nullopt for it. A random subset of ids forms the roots. Because edge
 *   targets are chosen independently of which ids are resolvable, the generated
 *   graphs reliably contain edges (and roots) pointing at unresolvable nodes —
 *   exactly the "arbitrary subset of referenced assets is unresolvable"
 *   condition the property quantifies over.
 *
 *   Two INDEPENDENT cross-checks are computed in the test (separate
 *   implementations from the production worklist, so the property is a genuine
 *   check and not a tautology):
 *     1. expected_reachable — a depth-first walk that collects only resolvable
 *        nodes and follows out-edges only through resolvable nodes (an
 *        unresolvable node terminates that branch). This is what must remain in
 *        `reachable` despite the unresolved edges.
 *     2. expected_unresolved_pairs — the set of (missing, referrer) edges that
 *        must be recorded: a root that is unresolvable contributes
 *        (root, ""), and every unresolvable out-edge of a reachable resolvable
 *        node `r` contributes (target, r). This pairing is order-independent: a
 *        pair is owed exactly when a reachable referrer (or a root) points at an
 *        unresolvable target.
 *
 *   The property then asserts:
 *     • result.reachable == expected_reachable  (continued traversal +
 *       exclusion of every unresolvable node — none leak into `reachable`);
 *     • the (missing, referrer) pairs in result.unresolved, taken as a set,
 *       equal expected_unresolved_pairs  (every unresolvable referenced asset
 *       is recorded, each paired with a referrer that actually pointed at it,
 *       with no spurious entries);
 *     • each recorded missing path is genuinely unresolvable and absent from
 *       `reachable`, and its referrer either is empty (a root) and the missing
 *       path is a root, or is a reachable node that lists the missing path among
 *       its out-edges  (the referrer really pointed at it).
 *
 * traverse_reference_graph is pure (no I/O, no GPU), so this runs on the host
 * `heaven_benchmark_tests` target. The file self-registers with the RapidCheck
 * harness in test_main.cpp via the heaven::testing::Register pattern, so the
 * file(GLOB) in benchmark/CMakeLists.txt picks it up without editing the entry
 * point. RapidCheck's default of 100 successful iterations meets the
 * "≥ 100 iterations" requirement (raise via RC_PARAMS=max_success=…).
 */
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
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
using heaven::Unresolved;

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

// A node is resolvable iff it is present in the edge map.
bool is_resolvable(const std::map<AssetId, std::vector<AssetId>>& edges,
                   const AssetId& node) {
    return edges.contains(node);
}

// Independent reference reachability: a depth-first walk from the roots that
// follows out-edges ONLY through resolvable nodes and collects only resolvable
// nodes. Unresolvable nodes terminate their branch and are never collected.
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

// The (missing, referrer) edges that traversal must record. A pair is owed
// exactly when an unresolvable node is pointed at by either a root (referrer
// "") or a reachable resolvable node that lists it among its out-edges. This is
// computed directly from the independent reachable set, so it is order- and
// implementation-independent of the production worklist.
std::set<std::pair<AssetId, AssetId>>
expected_unresolved_pairs(const std::vector<AssetId>& roots,
                          const std::map<AssetId, std::vector<AssetId>>& edges,
                          const std::set<AssetId>& reachable) {
    std::set<std::pair<AssetId, AssetId>> pairs;

    // Roots that are themselves unresolvable record against an empty referrer.
    for (const AssetId& r : roots) {
        if (!is_resolvable(edges, r)) {
            pairs.emplace(r, std::string{});
        }
    }

    // Each unresolvable out-edge of a reachable resolvable node records against
    // that node.
    for (const AssetId& n : reachable) {
        auto it = edges.find(n);
        if (it == edges.end()) continue;  // reachable nodes are resolvable
        for (const AssetId& child : it->second) {
            if (!is_resolvable(edges, child)) {
                pairs.emplace(child, n);
            }
        }
    }
    return pairs;
}

bool run_p4_unresolved_property() {
    return rc::check(
        "Feature: used-asset-packer, Property 4: Unresolved references are "
        "excluded, recorded, and non-fatal "
        "(Validates: Requirements 2.11, 6.1)",
        [] {
            // Universe of node ids "n0".."n{K-1}". K may be 0 (empty graph).
            const int k = *rc::gen::inRange(0, 9);

            // Decide which ids are resolvable, and give each resolvable id a
            // random list of out-edges into the universe. Edge targets are
            // ordinary universe ids; whether a target is resolvable is decided
            // independently, so plenty of edges point at unresolvable nodes —
            // the arbitrary unresolvable subset the property quantifies over.
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

            const std::set<AssetId> reachable =
                expected_reachable(roots, graph.edges);
            const std::set<std::pair<AssetId, AssetId>> owed_pairs =
                expected_unresolved_pairs(roots, graph.edges, reachable);

            // (1) Continued traversal + exclusion: every asset reachable
            // through resolvable references is still present, and no
            // unresolvable node leaks into `reachable`.
            RC_ASSERT(result.reachable == reachable);

            // (2) Every unresolvable referenced asset is recorded, each paired
            // with a referrer that actually pointed at it, with no spurious or
            // missing entries. Compared as a set since the unresolved list is
            // deduped by (missing, referrer) and order is not specified here.
            std::set<std::pair<AssetId, AssetId>> got_pairs;
            for (const Unresolved& u : result.unresolved) {
                got_pairs.emplace(u.missing_rel_path, u.referring_rel_path);
            }
            RC_ASSERT(got_pairs == owed_pairs);

            // (3) Each recorded entry is genuinely unresolvable and absent from
            // `reachable`, and its referrer really pointed at it: either an
            // empty referrer for a root, or a reachable node listing the missing
            // path among its out-edges.
            for (const Unresolved& u : result.unresolved) {
                RC_ASSERT(!is_resolvable(graph.edges, u.missing_rel_path));
                RC_ASSERT(!result.reachable.contains(u.missing_rel_path));

                if (u.referring_rel_path.empty()) {
                    const bool is_root =
                        std::find(roots.begin(), roots.end(), u.missing_rel_path) !=
                        roots.end();
                    RC_ASSERT(is_root);
                } else {
                    RC_ASSERT(result.reachable.contains(u.referring_rel_path));
                    auto it = graph.edges.find(u.referring_rel_path);
                    RC_ASSERT(it != graph.edges.end());
                    const bool points_at =
                        std::find(it->second.begin(), it->second.end(),
                                  u.missing_rel_path) != it->second.end();
                    RC_ASSERT(points_at);
                }
            }
        });
}

const heaven::testing::Register g_register_packer_unresolved{
    &run_p4_unresolved_property};

} // namespace
