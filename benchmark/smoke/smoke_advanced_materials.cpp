/*
 * HeavenBench — Group-A advanced-material visual-presence /
 *                    disabled-parity / cloth-sim-independence smoke case
 *                    (renderer-linked, GATED)
 *
 *
 * the four Group-A advanced shading models
 * (clear coat, anisotropic, subsurface translucency, cloth sheen) are routed
 * per surface by the data-driven Material_Model_Selector
 * (engine/material_model_ref.h) inside MaterialInstanceCache::get_or_create,
 * gated by MaterialInstanceCache::set_advanced_models(EnabledModels{...}). This
 * case locks the routing contract that the visible lobes depend on, plus the
 * Shipping_Look fall-through and the cloth-sim independence guarantee.
 *
 * WHY THE "VISUAL PRESENCE" ASSERTIONS ARE MATERIAL-ROUTING ASSERTIONS:
 * the hermetic NOOP Filament engine produces no pixels, so — exactly like
 * smoke_glass_refraction.cpp (the Group-A precedent) and the smoke_ssgi /
 * contact-shadow cases — the genuine pixel-level lobe/streak/transmission/sheen
 * presence requires separate full-scene capture comparison. Here "presence"
 * is expressed as: with a model enabled, a surface whose xml0 name selects that
 * model routes to the corresponding advanced base material; with the model
 * disabled it routes back to the Shipping_Look heaven_lit / heaven_lit_twosided
 * (≤1/255 parity). The base material name is read back through
 * MaterialInstance::getMaterial()->getName():
 *
 *   - clear coat  → "heaven_lit_clearcoat"  (present / disabled)
 *   - anisotropic → "heaven_lit_aniso"       (present / disabled)
 *   - subsurface  → "heaven_subsurface"      (present / disabled)
 *   - cloth       → "heaven_cloth"           (present / disabled)
 *   - Shipping_Look → "heaven_lit" / "heaven_lit_twosided" (parity)
 *
 * Per-model independence: enabling exactly
 * one model routes ONLY that model's surfaces to its advanced base; the other
 * marked surfaces stay Shipping_Look.
 *
 * Subsurface routing additionally consults the screen-space-scene-data
 * availability seam (refraction_scene_color_available, shared with the glass
 * path) — when unavailable, subsurface surfaces fall back to opaque heaven_lit.
 * To exercise the subsurface PRESENCE branch deterministically on a NOOP engine
 * (whose feature level may not advertise FEATURE_LEVEL_1) this case forces that
 * seam available via a test subclass, exactly like
 * ForcedSceneColorAvailable in smoke_glass_refraction.cpp.
 *
 * CLOTH-SIM INDEPENDENCE: cloth SHADING must never mutate the PBD
 * cloth simulation. The shading path (MaterialInstanceCache) and the sim path
 * (engine/cloth_sim.cpp) are entirely separate — step_cloth(ClothBody&, dt,
 * time) takes NO material/shading input. This case proves that observably: it
 * steps an identical synthetic PBD cloth body twice with the same dt/time
 * sequence — once with cloth shading inert and once after the cloth model has
 * been enabled and a "flag" surface routed to heaven_cloth (shading active) —
 * and asserts the particle positions are bit-identical, with the cloth having
 * actually moved (so the equality is not vacuous).
 *
 * HERMETIC ENGINE: the case stands up its OWN NOOP Filament engine +
 * TextureStore + MaterialInstanceCache, so it neither depends on nor perturbs
 * the shared backend. NOOP needs no GPU/display. It SKIPs (exit 0 via the
 * harness) if it cannot create the NOOP engine or load the base materials.
 *
 * The shared gate (HEAVEN_SMOKE=1 + display) and the window/backend bring-up
 * live in smoke_main.cpp; this file provides only the CASE FUNCTION (no main()).
 */

#include "smoke_cases.h"

#include "rhi/material_instance_cache.h"
#include "rhi/texture_store.h"
#include "rhi/rhi.h"
#include "engine/material_model_ref.h"
#include "engine/cloth_sim.h"

#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>

#include <glm/glm.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_advanced_materials]";

// Positive-control subclass: forces the screen-space-scene-data availability
// seam AVAILABLE so the subsurface routing branch (which consults it, mirroring
// the glass path) can be reached on a NOOP engine that may not advertise
// FEATURE_LEVEL_1. Mirrors ForcedSceneColorAvailable in
// smoke_glass_refraction.cpp.
class ForcedSceneColorAvailable final : public MaterialInstanceCache {
public:
    using MaterialInstanceCache::MaterialInstanceCache;
protected:
    bool refraction_scene_color_available() const override { return true; }
};

// The base material name behind an instance, or a sentinel for null.
const char* material_name_of(const filament::MaterialInstance* mi) {
    if (!mi) return "<null>";
    const char* n = mi->getMaterial()->getName();
    return n ? n : "<unnamed>";
}

bool material_is(const filament::MaterialInstance* mi, const char* name) {
    if (!mi) return false;
    const char* mat_name = mi->getMaterial()->getName();
    return mat_name != nullptr && std::strcmp(mat_name, name) == 0;
}

// A GpuMaterial whose only meaningful state is the surface marker carried in
// the surface name (passed separately to get_or_create). Opaque defaults route
// the Shipping_Look fall-through to heaven_lit; a two_sided surface routes it to
// the opaque heaven_lit_doublesided. Alpha-tested foliage uses
// heaven_lit_twosided instead.
GpuMaterial make_opaque_material() { return GpuMaterial{}; }
GpuMaterial make_twosided_material() {
    GpuMaterial m;
    m.two_sided = true;
    return m;
}
GpuMaterial make_foliage_material() {
    auto m = make_twosided_material();
    m.alpha_ref = 0.5f;
    return m;
}

// One routing expectation: the (mesh_path, surface_name, gpu_mat) tuple must
// resolve to the base material `expected`.
struct RouteExpect {
    const char* mesh;
    const char* surface;
    GpuMaterial mat;
    const char* expected;
    const char* note;
};

// Build a fresh forced-available cache with `models`, load the base materials,
// and assert each expectation routes to its expected base material. Returns
// false on any routing mismatch; sets `skip` true (and returns true) if the
// base materials cannot build on this NOOP engine.
bool check_routing(filament::Engine& engine, TextureStore& textures,
                   const char* scenario,
                   const material_model_ref::EnabledModels& models,
                   const std::vector<RouteExpect>& expects,
                   bool& skip) {
    ForcedSceneColorAvailable cache(engine, textures);
    if (auto r = cache.load_base_materials(); !r) {
        std::printf("%s SKIP: base materials failed to build on NOOP (%s)\n",
                    kTag, r.error().message.c_str());
        skip = true;
        return true;
    }
    cache.set_advanced_models(models);

    bool ok = true;
    for (const auto& e : expects) {
        auto* mi = cache.get_or_create(e.mesh, e.surface, e.mat);
        if (!material_is(mi, e.expected)) {
            std::printf("%s FAIL [%s]: surface '%s' routed to '%s' "
                        "(expected '%s' — %s)\n", kTag, scenario, e.surface,
                        material_name_of(mi), e.expected, e.note);
            ok = false;
        }
    }
    return ok;
}

// ── Synthetic PBD cloth body for the independence assertion ─────

// A minimal hanging strip: 3 anchored top particles + 3 free bottom particles,
// woven with triangle-edge distance constraints and wind-drag triangles. It is
// authored entirely in body-local space (clear of the pole-collision cylinder
// at the local origin) so step_cloth moves the free row under gravity + wind.
// step_cloth reads only this struct — there is no material/shading input — so
// stepping it is deterministic and independent of any shading state.
ClothBody make_synthetic_cloth() {
    ClothBody c;

    const glm::vec3 rest[6] = {
        {0.3f, 0.0f, 1.0f}, {0.6f, 0.0f, 1.0f}, {0.9f, 0.0f, 1.0f}, // anchored row
        {0.3f, 0.0f, 0.5f}, {0.6f, 0.0f, 0.5f}, {0.9f, 0.0f, 0.5f}, // free row
    };
    c.rest_pos.assign(rest, rest + 6);
    c.position      = c.rest_pos;
    c.prev_position = c.rest_pos;

    c.inv_mass = {0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    c.anchor_idx = {0, 1, 2};

    auto add_edge = [&](int a, int b) {
        ClothLinearConstraint lc;
        lc.a = a;
        lc.b = b;
        lc.rest_length = glm::length(c.rest_pos[a] - c.rest_pos[b]);
        c.linear.push_back(lc);
    };
    // Grid edges: horizontal (top + bottom), vertical, and one diagonal per
    // quad so the strip is a properly woven mesh.
    add_edge(0, 1); add_edge(1, 2);          // top row
    add_edge(3, 4); add_edge(4, 5);          // bottom row
    add_edge(0, 3); add_edge(1, 4); add_edge(2, 5); // verticals
    add_edge(0, 4); add_edge(1, 5);          // diagonals

    c.triangles = {{0, 1, 3}, {1, 4, 3}, {1, 2, 4}, {2, 5, 4}};

    c.gravity_local  = glm::vec3(0.0f, 0.0f, -9.81f);
    c.wind_velocity  = glm::vec3(3.0f, 0.5f, 0.0f);
    c.num_iterations = 8;
    c.sim_hz         = 60.0f;
    c.sim_time_accum = 0.0f;
    return c;
}

// Advance a cloth body `n` whole 60 Hz steps with the same dt/time sequence
// every call, so two identical bodies produce identical trajectories.
void step_cloth_n(ClothBody& c, int n) {
    constexpr float dt = 1.0f / 60.0f;
    for (int i = 0; i < n; ++i) {
        const float time = static_cast<float>(i) * dt;
        step_cloth(c, dt, time);
    }
}

bool positions_identical(const ClothBody& a, const ClothBody& b) {
    if (a.position.size() != b.position.size()) return false;
    for (std::size_t i = 0; i < a.position.size(); ++i) {
        if (a.position[i] != b.position[i]) return false; // exact: same code/input ⇒ same bits
    }
    return true;
}

bool cloth_moved(const ClothBody& c) {
    for (std::size_t i = 0; i < c.position.size(); ++i) {
        if (c.inv_mass[i] != 0.0f && c.position[i] != c.rest_pos[i]) return true;
    }
    return false;
}

} // namespace

CaseResult run_advanced_materials_case(const CaseContext& /*ctx*/) {
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

        using material_model_ref::EnabledModels;

        // The four marker surfaces — one per advanced model. Names chosen so
        // each matches exactly one marker table (cloth → subsurface →
        // clear-coat → anisotropic precedence): "dragon" (clear coat), "gear"
        // (anisotropic), "leaf" (subsurface), "flag" (cloth).
        const GpuMaterial opaque = make_opaque_material();

        // ──: presence — all four models enabled ──
        // Each marked surface routes to its advanced base material.
        if (ok && !skip) {
            const EnabledModels all{true, true, true, true};
            ok &= check_routing(*engine, textures, "all-enabled (presence)", all,
                {
                    {"dragon.mesh", "dragon", opaque, "heaven_lit_clearcoat",
                     "clear-coat lobe present (Req 1.2)"},
                    {"crane.mesh",  "gear",   opaque, "heaven_lit_aniso",
                     "anisotropic streak present (Req 2.2)"},
                    {"fir.mesh",    "leaf",   opaque, "heaven_subsurface",
                     "subsurface back-lit transmission present (Req 3.2)"},
                    {"flag.mesh",   "flag",   opaque, "heaven_cloth",
                     "cloth grazing sheen present (Req 4.2)"},
                }, skip);
        }

        // ──: disabled parity — all models off ──
        // The same surfaces fall through to the Shipping_Look base materials
        // (≤1/255 parity). Opaque single-sided surfaces resolve to heaven_lit;
        // opaque two-sided surfaces use heaven_lit_doublesided, while
        // alpha-tested foliage keeps the masked heaven_lit_twosided.
        if (ok && !skip) {
            const EnabledModels none{}; // all false (every --no-<feature> passed)
            ok &= check_routing(*engine, textures, "all-disabled (parity)", none,
                {
                    {"dragon.mesh", "dragon", opaque, "heaven_lit",
                     "clear-coat off ⇒ Shipping_Look (Req 1.4)"},
                    {"crane.mesh",  "gear",   opaque, "heaven_lit",
                     "anisotropic off ⇒ Shipping_Look (Req 2.4)"},
                    {"fir.mesh",    "leaf",   opaque, "heaven_lit",
                     "subsurface off ⇒ Shipping_Look (Req 3.4)"},
                    {"flag.mesh",   "flag",   opaque, "heaven_lit",
                     "cloth off ⇒ Shipping_Look (Req 4.5)"},
                    {"fir.mesh",    "leaf_twosided", make_foliage_material(),
                     "heaven_lit_twosided",
                     "alpha-tested foliage keeps the masked material (Req 5.4)"},
                    {"flag.mesh",   "flag_twosided", make_twosided_material(),
                     "heaven_lit_doublesided",
                     "opaque two-sided cloth must not acquire alpha cutouts"},
                }, skip);
        }

        // ──  (per-model independence): enable exactly one model ──
        // Only that model's surface routes to its advanced base; the other
        // marked surfaces stay Shipping_Look heaven_lit.
        struct IndependenceScenario {
            const char*   scenario;
            EnabledModels models;
            const char*   dragon;
            const char*   gear;
            const char*   leaf;
            const char*   flag;
        };
        const IndependenceScenario indep[] = {
            {"only clear-coat", EnabledModels{true, false, false, false},
             "heaven_lit_clearcoat", "heaven_lit", "heaven_lit", "heaven_lit"},
            {"only anisotropic", EnabledModels{false, true, false, false},
             "heaven_lit", "heaven_lit_aniso", "heaven_lit", "heaven_lit"},
            {"only subsurface", EnabledModels{false, false, true, false},
             "heaven_lit", "heaven_lit", "heaven_subsurface", "heaven_lit"},
            {"only cloth", EnabledModels{false, false, false, true},
             "heaven_lit", "heaven_lit", "heaven_lit", "heaven_cloth"},
        };
        for (const auto& s : indep) {
            if (!ok || skip) break;
            ok &= check_routing(*engine, textures, s.scenario, s.models,
                {
                    {"dragon.mesh", "dragon", opaque, s.dragon, "independence"},
                    {"crane.mesh",  "gear",   opaque, s.gear,   "independence"},
                    {"fir.mesh",    "leaf",   opaque, s.leaf,   "independence"},
                    {"flag.mesh",   "flag",   opaque, s.flag,   "independence"},
                }, skip);
        }

        // ──: cloth shading never mutates the PBD cloth sim ──
        // Step an identical synthetic cloth body twice with the same dt/time
        // sequence. The first run happens with cloth shading inert; the second
        // after the cloth model is enabled and a "flag" surface is routed to
        // heaven_cloth (shading active). step_cloth takes no material/shading
        // input, so the trajectories must be bit-identical — and the cloth must
        // have actually moved, so the equality is not vacuous.
        if (ok && !skip) {
            // Run 1: shading inert.
            ClothBody shading_off = make_synthetic_cloth();
            step_cloth_n(shading_off, 60);

            // Activate cloth shading: enable the cloth model and route a flag
            // surface to heaven_cloth, proving the shading path is live while
            // the sim runs.
            ForcedSceneColorAvailable cache(*engine, textures);
            if (auto r = cache.load_base_materials(); !r) {
                std::printf("%s SKIP: base materials failed to build on NOOP "
                            "(%s)\n", kTag, r.error().message.c_str());
                skip = true;
            } else {
                cache.set_advanced_models(EnabledModels{false, false, false, true});
                auto* flag_mi = cache.get_or_create("flag.mesh", "flag", opaque);
                if (!material_is(flag_mi, "heaven_cloth")) {
                    std::printf("%s FAIL: cloth shading not active — 'flag' "
                                "routed to '%s' (expected heaven_cloth) so the "
                                "independence check would be vacuous\n", kTag,
                                material_name_of(flag_mi));
                    ok = false;
                } else {
                    // Run 2: cloth shading active.
                    ClothBody shading_on = make_synthetic_cloth();
                    step_cloth_n(shading_on, 60);

                    if (!cloth_moved(shading_off) || !cloth_moved(shading_on)) {
                        std::printf("%s FAIL: synthetic cloth did not move — the "
                                    "position-identity check would be vacuous\n",
                                    kTag);
                        ok = false;
                    } else if (!positions_identical(shading_off, shading_on)) {
                        std::printf("%s FAIL: PBD cloth positions DIFFER with "
                                    "cloth shading toggled — shading must never "
                                    "mutate cloth_sim (Req 4.4)\n", kTag);
                        ok = false;
                    }
                }
            }
        }
        // Caches' destructors run at their block-scope exits; TextureStore is
        // destroyed here (before the engine).
    }

    filament::Engine::destroy(&engine);

    if (skip) return CaseResult::Skip;
    if (!ok)  return CaseResult::Fail;

    std::printf("%s presence: dragon⇒clearcoat, gear⇒aniso, leaf⇒subsurface, "
                "flag⇒cloth (Req 1.2/2.2/3.2/4.2); all-disabled ⇒ Shipping_Look "
                "heaven_lit / heaven_lit_twosided (Req 1.4/2.4/3.4/4.5/5.4); "
                "per-model independence holds; PBD cloth positions identical "
                "with cloth shading toggled (Req 4.4)\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
