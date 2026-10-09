// Feature: codebase-hardening,
/*
 * HeavenBench — Property test for the material-instance cache key
 * (design B2).
 *
 * Feature: codebase-hardening
 *   •: Material-cache key distinctness and dedup
 *
 *
 * The real MaterialInstanceCache builds Filament MaterialInstances, which needs
 * a live GPU/engine. Per the implementation, the *key logic* is host-testable in
 * isolation: the cache is an `std::unordered_map<MaterialKey, MaterialInstance*>`
 * whose find-or-build behaviour is governed entirely by `MaterialKey`'s defaulted
 * `operator==` and the `std::hash<MaterialKey>` specialisation (both defined in
 * rhi/material_instance_cache.h). This test drives sets of
 * (mesh_path, surface_name) pairs through a stub instance map keyed by the REAL
 * `MaterialKey`, using a monotonically increasing `int` as a stand-in instance
 * id (a unique non-null MaterialInstance pointer), and asserts:
 *
 *   • Distinctness/dedup iff equality (8.1, 8.2): two pairs share the same
 *     instance id IFF their (mesh_path, surface_name) are equal — so two
 *     surfaces with the SAME surface_name but DIFFERENT mesh_path always get
 *     distinct instances and never alias one pointer.
 *   • Repeat returns the cached instance (8.3): re-driving an already-present
 *     pair returns the previously assigned instance and builds no second one
 *     (the map does not grow).
 *
 * material_instance_cache.h declares Filament types only via forward
 * declarations, so this TU includes it without pulling in any Filament header —
 * it links into the headless RapidCheck harness (heaven_benchmark_tests) like
 * the other prop_*.cpp files. The file self-registers via the
 * heaven::testing::Register pattern, so the file(GLOB) in benchmark/CMakeLists.txt
 * picks it up without editing test_main.cpp.
 */
#include <cstdio>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rapidcheck.h>

#include "rhi/material_instance_cache.h"

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

using heaven::MaterialKey;

// Stub instance map that mirrors MaterialInstanceCache::get_or_create's
// find-or-build contract on the REAL MaterialKey: a request for a key already
// present returns the previously assigned instance id; a new key is assigned a
// fresh unique id (standing in for a freshly built, distinct MaterialInstance
// pointer) and stored. Keyed on the real (mesh_path, surface_name) struct +
// std::hash<MaterialKey>, this exercises both the defaulted operator== and the
// hash specialisation exactly as the production cache does.
struct StubInstanceCache {
    std::unordered_map<MaterialKey, int> map;
    int next_id = 0;

    int get_or_create(const std::string& mesh_path,
                      const std::string& surface_name) {
        const MaterialKey key{mesh_path, surface_name};
        if (auto it = map.find(key); it != map.end()) {
            return it->second; // dedup: return the previously cached instance
        }
        const int id = next_id++;
        map.emplace(key, id);
        return id;
    }
};

// Small pools so the random pairs frequently collide on surface_name across
// different mesh_paths (the alias case) and repeat outright (the
// dedup case), instead of almost always being unique.
const std::vector<std::string>& mesh_paths() {
    static const std::vector<std::string> v = {
        "heaven/meshes/buildings/weathercock.mesh",
        "heaven/meshes/airship/airship_cannons.mesh",
        "heaven/meshes/props/barrel.mesh",
        "heaven/meshes/props/bench.mesh",
    };
    return v;
}

const std::vector<std::string>& surface_names() {
    static const std::vector<std::string> v = {
        "lod_0", "lod_1", "Object04", "Object04", // duplicate entry → more repeats
    };
    return v;
}

// ──: cache-key distinctness and dedup ───────────────
bool run_p13_material_key_property() {
    return rc::check(
        "Feature: codebase-hardening, Property 13: Material-cache key "
        "distinctness and dedup (Validates: Requirements 8.1, 8.2, 8.3)",
        [] {
            const int n = *rc::gen::inRange(1, 16);

            std::vector<std::pair<std::string, std::string>> pairs;
            pairs.reserve(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i) {
                const std::string mesh = *rc::gen::elementOf(mesh_paths());
                const std::string surf = *rc::gen::elementOf(surface_names());
                pairs.emplace_back(mesh, surf);
            }

            // First pass: drive every pair through the stub once, recording the
            // instance id each resolved to.
            StubInstanceCache cache;
            std::vector<int> ids;
            ids.reserve(pairs.size());
            for (const auto& [mesh, surf] : pairs) {
                ids.push_back(cache.get_or_create(mesh, surf));
            }

            // Distinctness/dedup IFF equality: two pairs share an
            // instance id exactly when their (mesh_path, surface_name) are equal.
            // This covers the alias case directly — same surface_name with a
            // different mesh_path is NOT equal, so it must get a distinct id.
            for (std::size_t i = 0; i < pairs.size(); ++i) {
                for (std::size_t j = 0; j < pairs.size(); ++j) {
                    const bool keys_equal = (pairs[i] == pairs[j]);
                    RC_ASSERT((ids[i] == ids[j]) == keys_equal);
                }
            }

            // Repeat returns the cached instance: re-driving each pair
            // returns its original id and builds no second instance, so the map
            // does not grow.
            const std::size_t size_after_first_pass = cache.map.size();
            for (std::size_t i = 0; i < pairs.size(); ++i) {
                const int again =
                    cache.get_or_create(pairs[i].first, pairs[i].second);
                RC_ASSERT(again == ids[i]);
            }
            RC_ASSERT(cache.map.size() == size_after_first_pass);

            // The number of distinct instances equals the number of distinct
            // keys — no key was split across two instances, none merged.
            RC_ASSERT(static_cast<std::size_t>(cache.next_id) ==
                      cache.map.size());
        });
}

// ── Explicit example assertions for the three named requirement clauses ──
// Property coverage above is exhaustive, but these pin the exact scenarios
// from the acceptance criteria so a regression names the failing clause.
bool run_p13_examples() {
    bool ok = true;

    // same surface_name, different mesh_path → distinct instances.
    {
        StubInstanceCache cache;
        const int a = cache.get_or_create("meshA.mesh", "Object04");
        const int b = cache.get_or_create("meshB.mesh", "Object04");
        if (a == b) {
            std::printf("[prop_material_key] Req 8.2 FAILED: same surface_name on "
                        "different mesh_path aliased to one instance\n");
            ok = false;
        }
    }

    // a repeated identical pair returns the previously cached instance
    // and does not build a second one.
    {
        StubInstanceCache cache;
        const int first  = cache.get_or_create("meshA.mesh", "lod_0");
        const int second = cache.get_or_create("meshA.mesh", "lod_0");
        if (first != second || cache.map.size() != 1u) {
            std::printf("[prop_material_key] Req 8.3 FAILED: repeated pair did not "
                        "return the cached instance (first=%d second=%d size=%zu)\n",
                        first, second, cache.map.size());
            ok = false;
        }
    }

    // the entry is keyed on the (mesh_path, surface_name) pair — same
    // mesh_path with different surface_name resolves to distinct instances too.
    {
        StubInstanceCache cache;
        const int a = cache.get_or_create("meshA.mesh", "lod_0");
        const int b = cache.get_or_create("meshA.mesh", "lod_1");
        if (a == b) {
            std::printf("[prop_material_key] Req 8.1 FAILED: distinct surface_name "
                        "on one mesh_path aliased to one instance\n");
            ok = false;
        }
    }

    return ok;
}

bool run_material_key_properties() {
    bool ok = true;
    ok = run_p13_examples() && ok;
    ok = run_p13_material_key_property() && ok;
    return ok;
}

const heaven::testing::Register g_register_material_key{
    &run_material_key_properties};

} // namespace
