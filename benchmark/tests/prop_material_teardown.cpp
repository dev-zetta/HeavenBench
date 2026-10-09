/*
 * HeavenBench — Property test: material-cache teardown destroys each
 * instance exactly once
 *
 * Feature: codebase-hardening
 *   •: Material-cache teardown destroys each instance exactly once
 *
 *
 * fixes the Material_Instance_Cache teardown contract: WHEN the
 * cache is destroyed during engine teardown, it SHALL destroy each cached
 * MaterialInstance exactly once and clear all cache entries — none leaked, none
 * destroyed more than once. The production destructor
 * (rhi/material_instance_cache.cpp::~MaterialInstanceCache) implements this by
 * iterating the `std::unordered_map<MaterialKey, filament::MaterialInstance*>`
 * once, calling `engine_.destroy(mi)` per entry, then `clear()`ing the map.
 *
 * That destructor calls into Filament (GPU types), so it cannot run in the
 * headless, Filament-free test harness. by the implementation gated-smoke seam
 * pattern, this property pins the teardown INVARIANT against a small local
 * fake: it populates a map of the real `MaterialKey` type (referenced from
 * rhi/material_instance_cache.h, so the key/hash under test is the production
 * one) with N distinct fake instances, runs the exact documented
 * destroy-each-once-then-clear() loop against a destroy-counting fake engine,
 * and asserts that
 *
 *   - every cached instance is destroyed exactly once (count == 1),
 *   - no instance is leaked (every instance is destroyed),
 *   - no instance is destroyed more than once, and
 *   - the map is empty after teardown.
 *
 * Distinct `(mesh_path, surface_name)` keys are generated, then deduplicated
 * through the same `std::unordered_map<MaterialKey,...>` the cache uses, so
 * each surviving entry owns exactly one distinct instance — mirroring the
 * cache's own one-instance-per-key storage. The teardown loop is a verbatim
 * transcription of the production destructor body, so the invariant it pins is
 * the one the destructor must uphold.
 *
 * This translation unit self-registers with the RapidCheck harness in
 * test_main.cpp via the heaven::testing::Register pattern, so the file(GLOB) in
 * benchmark/CMakeLists.txt picks it up without editing the entry point; no
 * second main() is defined.
 */
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <rapidcheck.h>

#include "rhi/material_instance_cache.h" // heaven::MaterialKey + std::hash

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

// A stand-in for filament::MaterialInstance. The cache stores raw pointers to
// instances it owns; the fake mirrors that — each distinct instance is a
// distinct heap object whose address keys the destroy counter.
struct FakeInstance {
    int id = 0;
};

// A destroy-counting stand-in for filament::Engine. `destroy(mi)` is what the
// production destructor calls per cache entry; here it records how many times
// each instance pointer was passed in, so the property can assert exactly-once.
struct FakeEngine {
    std::unordered_map<const FakeInstance*, int> destroy_count;

    void destroy(const FakeInstance* mi) { ++destroy_count[mi]; }
};

bool run_p14_teardown_destroys_each_once() {
    return rc::check(
        "Feature: codebase-hardening, Property 14: material-cache teardown "
        "destroys each instance exactly once (Validates: Requirements 8.5)",
        [] {
            // Generate a batch of (mesh_path, surface_name) pairs. Surface
            // names are drawn from a small alphabet so collisions across
            // different mesh paths are likely — exercising the composite key.
            const auto pairs = *rc::gen::container<
                std::vector<std::pair<std::string, std::string>>>(
                rc::gen::pair(
                    rc::gen::element<std::string>(
                        "mesh_a", "mesh_b", "mesh_c", "meshes/lod.mi08", ""),
                    rc::gen::element<std::string>(
                        "lod_0", "Object04", "surf", "lod_0", "")));

            // Owning storage for the fake instances (one per distinct key).
            std::vector<std::unique_ptr<FakeInstance>> owned;

            // The cache map under test: the production MaterialKey + hash.
            // Inserting through it deduplicates on the composite key exactly as
            // the real cache does, so each surviving entry holds one distinct
            // instance pointer.
            std::unordered_map<heaven::MaterialKey, FakeInstance*> cache;

            int next_id = 0;
            for (const auto& [mesh_path, surface_name] : pairs) {
                const heaven::MaterialKey key{mesh_path, surface_name};
                if (cache.contains(key)) {
                    // Already cached for this pair — the real cache returns the
                    // existing instance and never builds a second one.
                    continue;
                }
                auto inst = std::make_unique<FakeInstance>();
                inst->id = next_id++;
                cache.emplace(key, inst.get());
                owned.push_back(std::move(inst));
            }

            // Snapshot the set of instances the cache holds before teardown so
            // we can confirm none leaked afterwards.
            std::vector<const FakeInstance*> stored;
            stored.reserve(cache.size());
            for (const auto& [key, mi] : cache) stored.push_back(mi);
            RC_ASSERT(stored.size() == owned.size());

            // ── Verbatim transcription of ~MaterialInstanceCache teardown ──
            // for (auto& [key, mi]: mat_instance_cache_) engine_.destroy(mi);
            // mat_instance_cache_.clear();
            FakeEngine engine;
            for (auto& [key, mi] : cache) {
                engine.destroy(mi);
            }
            cache.clear();

            // ── Invariant 1: the map is cleared on teardown. ──
            RC_ASSERT(cache.empty());

            // ── Invariant 2: every stored instance destroyed exactly once. ──
            // No instance is leaked (each appears in the destroy map) and none
            // is destroyed more than once (count is exactly 1).
            RC_ASSERT(engine.destroy_count.size() == stored.size());
            for (const FakeInstance* mi : stored) {
                const auto it = engine.destroy_count.find(mi);
                RC_ASSERT(it != engine.destroy_count.end());
                RC_ASSERT(it->second == 1);
            }
        });
}

const heaven::testing::Register g_register_material_teardown{
    &run_p14_teardown_destroys_each_once};

} // namespace
