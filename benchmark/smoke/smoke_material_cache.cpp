/*
 * HeavenBench — material-instance-cache key-distinctness +
 *                    exactly-once-teardown smoke case (renderer-linked, GATED)
 *
 * the
 * MaterialInstanceCache keys its MaterialInstances on the composite
 * (mesh_path, surface_name) pair. Surface names are not
 * globally unique — the same name recurs across different meshes — so keying
 * on the surface name alone would alias distinct surfaces onto one shared
 * instance. Keying on the pair keeps each surface's instance distinct, while a
 * repeated pair still returns the cached instance (dedup), and teardown
 * destroys each instance exactly once.
 *
 * This case locks the live-Filament side of that contract:
 *
 *   -  distinctness: two surfaces that share a surface NAME but live on
 *     different meshes resolve to DIFFERENT MaterialInstance pointers; two
 *     fully-distinct pairs likewise differ.
 *   -  dedup: the SAME (mesh_path, surface_name) pair asked twice
 *     returns the IDENTICAL cached pointer (so the destructor visits it once).
 *   -  exactly-once teardown: the cache destructor destroys each instance
 *     exactly once and clears the map. Because the map holds each instance
 *     under exactly one key (dedup), iterating it at teardown destroys each
 *     once; on the real NOOP engine a double-destroy would fault, so a clean
 *     destructor run is the live confirmation. The destroy-COUNT proof over a
 *     fake engine is owned by the host test (prop_material_teardown).
 *
 * WHY THE SMOKE HARNESS (not heaven_benchmark_tests): MaterialInstanceCache
 * builds real filament::Material / MaterialInstance objects from the embedded
 * compiled packages and resolves textures through a real TextureStore. The
 * headless RapidCheck target links NEITHER Filament NOR SDL, so it cannot build
 * a MaterialInstance. This target links the full renderer, so it can — matching
 * the gated-smoke seam testing strategy.
 *
 * HERMETIC ENGINE: the case stands up its OWN no-op (NOOP) Filament engine +
 * TextureStore + MaterialInstanceCache (NOOP needs no GPU/display), so the
 * assertion does not depend on (or perturb) the shared backend. It SKIPs
 * (exit 0 via the harness) if it cannot create the NOOP engine or load the base
 * materials. Mirrors smoke_glass_refraction.cpp exactly for these conventions.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 */

#include "smoke_cases.h"

#include "rhi/material_instance_cache.h"
#include "rhi/texture_store.h"
#include "rhi/rhi.h"

#include <filament/Engine.h>
#include <filament/MaterialInstance.h>

#include <cstdio>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_material_cache]";

// A plain opaque GpuMaterial: two_sided/alpha_ref/glass left at their defaults
// so get_or_create routes to the standard heaven_lit base material and builds a
// fresh MaterialInstance per distinct key.
GpuMaterial make_opaque_material() {
    return GpuMaterial{};
}

} // namespace

CaseResult run_material_cache_case(const CaseContext& /*ctx*/) {
    // Hermetic NOOP engine: no GPU/display needed, isolated from the shared
    // backend. If even NOOP can't be created, SKIP (exit 0 via the harness).
    filament::Engine* engine =
        filament::Engine::create(filament::Engine::Backend::NOOP);
    if (!engine) {
        std::printf("%s SKIP: could not create a NOOP Filament engine\n", kTag);
        return CaseResult::Skip;
    }

    bool ok   = true;
    bool skip = false;

    {
        TextureStore textures(*engine);
        textures.create_placeholders();

        MaterialInstanceCache cache(*engine, textures);
        if (auto r = cache.load_base_materials(); !r) {
            std::printf("%s SKIP: base materials failed to build on NOOP (%s)\n",
                        kTag, r.error().message.c_str());
            skip = true;
        } else {
            const GpuMaterial mat = make_opaque_material();

            // The same surface NAME ("hull") on two different meshes. Keying on
            // the surface name alone would alias these; the composite key keeps
            // them distinct.
            auto* a = cache.get_or_create("airship.mesh", "hull", mat);
            auto* b = cache.get_or_create("dragon.mesh",  "hull", mat);

            // A fully-distinct third pair.
            auto* c = cache.get_or_create("airship.mesh", "glass_pane", mat);

            // Positive control: instances were actually built (else the
            // distinctness comparisons below would be vacuously true on null).
            if (!a || !b || !c) {
                std::printf("%s FAIL: get_or_create returned null on NOOP "
                            "(a=%p b=%p c=%p) — cannot assert distinctness\n",
                            kTag, static_cast<void*>(a), static_cast<void*>(b),
                            static_cast<void*>(c));
                ok = false;
            } else {
                // ──: distinct keys ⇒ distinct instances ──────────
                if (a == b) {
                    std::printf("%s FAIL: same surface name 'hull' on different "
                                "meshes shared one MaterialInstance (%p) — the "
                                "composite (mesh_path, surface_name) key must "
                                "keep them distinct (R8.1)\n", kTag,
                                static_cast<void*>(a));
                    ok = false;
                }
                if (a == c || b == c) {
                    std::printf("%s FAIL: distinct (mesh, surface) pairs shared "
                                "an instance (a=%p b=%p c=%p, R8.1)\n", kTag,
                                static_cast<void*>(a), static_cast<void*>(b),
                                static_cast<void*>(c));
                    ok = false;
                }

                // ──: repeated pair ⇒ cached (same) instance ──
                auto* a_again = cache.get_or_create("airship.mesh", "hull", mat);
                if (a_again != a) {
                    std::printf("%s FAIL: repeated (airship.mesh, hull) returned "
                                "a different instance (%p vs cached %p) — the "
                                "cache must dedup the pair (R8.2/R8.3)\n", kTag,
                                static_cast<void*>(a_again),
                                static_cast<void*>(a));
                    ok = false;
                }
            }
        }
        // ──: exactly-once teardown ──────────────────────────────
        // The cache destructor runs HERE, destroying each instance in the
        // deduped map exactly once on the real NOOP engine. Because each
        // instance lives under exactly one key (dedup asserted above), a
        // double-destroy is impossible; a clean run (no engine fault) is the
        // live confirmation. The destroy-count proof is the test.
    }

    filament::Engine::destroy(&engine);

    if (skip) return CaseResult::Skip;
    if (!ok)  return CaseResult::Fail;

    std::printf("%s composite-key distinctness honored: same surface name on "
                "different meshes ⇒ distinct instances, distinct pairs ⇒ "
                "distinct instances (R8.1); repeated pair ⇒ cached instance "
                "(R8.2/R8.3); cache torn down cleanly, each instance destroyed "
                "once over the deduped map (R8.5)\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
