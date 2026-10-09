/*
 * HeavenBench — Group-B shadows-&-lighting presence /
 *                    disabled-parity smoke case (renderer-linked, GATED)
 *
 *
 * the three Group-B "shadows & lighting
 * polish" capabilities — transparent foliage shadows, the safe
 * colored-penumbra revisit, and geometric specular anti-aliasing
 * — are all routed/applied per surface in
 * MaterialInstanceCache::get_or_create, gated by set_transparent_shadows(),
 * set_colored_penumbra(), and set_specular_aa(). This case locks down the
 * routing/enable contract the visible effects depend on, plus the
 * Shipping_Look fall-through (≤1/255 parity) of each.
 *
 * WHY THE "PRESENCE" ASSERTIONS ARE MATERIAL-ROUTING / INSTANCE-STATE
 * ASSERTIONS: the hermetic NOOP Filament engine produces no pixels, so —
 * exactly like smoke_glass_refraction.cpp and smoke_advanced_materials.cpp (the
 * Group-A precedent) — the genuine pixel-level effects (a lower-density/tinted
 * foliage shadow, a firefly-free colored penumbra on near-white surfaces, the
 * reduced frame-to-frame highlight shimmer of specular AA) require separate
 * full-scene and moving-camera captures. Here "presence" is expressed as the
 * material routing / per-instance state those effects are built on, read back
 * through MaterialInstance::getMaterial()->getName() and the specular-AA
 * instance getters:
 *
 *   - Transparent foliage shadows: visible foliage keeps the masked
 *     "heaven_lit_twosided" material. With the feature enabled, SceneBuilder
 *     adds a separate "heaven_foliage_shadow" caster whose color writes are
 *     disabled. With the feature disabled, the visible renderable casts its
 *     own opaque silhouette and no extra caster is added. This case verifies
 *     both material roles, their separate cache entries, and the actual scene
 *     renderables produced for the enabled and disabled configurations.
 *   - Colored penumbra: with the feature enabled a plain
 *     Standard_Lit single-sided surface routes to "heaven_lit_colored_penumbra"
 *     — heaven_lit authored with `coloredPenumbra: true`, the ONLY material the
 *     committed HAS_COLORED_PENUMBRA define fix compiles the tint block into,
 *     built against the epsilon-clamped lib/filament shader
 *     (Fd / (2.0 * max(1.0 - diffuseColor, vec3(1e-3)))) so the divisor is
 *     strictly positive and near-white/white surfaces stay firefly-free (the
 *     pixel-level no-firefly behavior still needs separate full-scene review
 *     of that clamped shader). Disabled it keeps "heaven_lit" (≤1/255 parity,
 *
 *   - Specular AA: with the feature enabled the lit instance's
 *     specular-AA variance/threshold are set from
 *     heaven::spec_aa_strength(knobs.specular_aa_level); the host-checkable part
 *     is the strength ladder (strictly increasing in level, bounded by the
 *     configured maximum — higher quality ⇒ stronger highlight-variance
 *     suppression) and that those exact host values reach the MaterialInstance
 *     (read back via getSpecularAntiAliasingVariance/Threshold). Disabled, the
 *     setters are never called so the instance keeps Filament's compiled default
 *     (untouched ⇒ ≤1/255 parity).
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
#include "rhi/scene_builder.h"
#include "rhi/rhi.h"
#include "engine/types.h"               // QualityKnobs, spec_aa_strength
#include "engine/material_model_ref.h"  // is_foliage_shadow_caster (documentation)

#include <filament/Engine.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace heaven::smoke {

namespace {

constexpr const char* kTag = "[smoke_shadows_lighting]";

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

// An opaque, single-sided GpuMaterial (routes the Shipping_Look fall-through to
// heaven_lit). A two-sided/alpha-tested surface (foliage) routes it to
// heaven_lit_twosided.
GpuMaterial make_opaque_material() { return GpuMaterial{}; }
GpuMaterial make_foliage_material() {
    GpuMaterial m;
    m.two_sided = true;   // foliage is two-sided/alpha-tested (heaven_lit_twosided)
    m.alpha_ref = 0.5f;
    return m;
}

bool approx(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

// Exercise the production caller of the separate shadow role: enabling the
// feature adds an invisible caster while the visible leaf remains masked.
// Disabling it restores one visible renderable that casts its own shadow.
bool check_foliage_scene(filament::Engine& engine, MaterialInstanceCache& cache,
                         const GpuMaterial& foliage, bool enabled,
                         bool gpu_instancing) {
    auto* scene = engine.createScene();
    if (!scene) return false;
    const BackendCore core{.engine = &engine, .scene = scene};
    MeshStore meshes(engine);
    Mesh mesh;
    mesh.path = "smoke_foliage.mesh";
    Surface surface;
    surface.name = "fir_branch";
    surface.vertices.resize(3);
    surface.vertices[1].px = 1.0f;
    surface.vertices[1].u = 1.0f;
    surface.vertices[2].py = 1.0f;
    surface.vertices[2].v = 1.0f;
    for (auto& vertex : surface.vertices) vertex.nz = 1.0f;
    surface.indices = {0, 1, 2};
    mesh.surfaces.push_back(std::move(surface));
    const std::array gpu_meshes{meshes.upload(mesh)};
    // Two matching instances exercise both SceneBuilder construction paths.
    const std::array instances{
        Instance{0, glm::mat4(1.0f)}, Instance{0, glm::mat4(1.0f)}};
    SceneBuilder builder;
    builder.build(core, meshes, cache, instances, gpu_meshes,
        [&](const std::string& /*mesh_path*/, const std::string& /*surface_name*/) {
            return foliage;
        }, /*gpu_skinning=*/false, gpu_instancing);

    auto& renderables = engine.getRenderableManager();
    std::size_t visible_count = 0;
    std::size_t shadow_count = 0;
    bool ok = true;
    scene->forEach([&](utils::Entity entity) {
        const auto instance = renderables.getInstance(entity);
        const auto* material = renderables.getMaterialInstanceAt(instance, 0);
        if (material_is(material, "heaven_lit_twosided")) {
            ++visible_count;
            ok = ok && material->isColorWriteEnabled() &&
                 renderables.isShadowReceiver(instance) &&
                 renderables.isScreenSpaceContactShadowsEnabled(instance) &&
                 renderables.isShadowCaster(instance) == !enabled;
        } else if (material_is(material, "heaven_foliage_shadow")) {
            ++shadow_count;
            ok = ok && !material->isColorWriteEnabled() &&
                 !renderables.isShadowReceiver(instance) &&
                 !renderables.isScreenSpaceContactShadowsEnabled(instance) &&
                 renderables.isShadowCaster(instance);
        } else {
            ok = false;
        }
    });
    const std::size_t expected_visible =
        gpu_instancing && engine.getMaxAutomaticInstances() >= 2 ? 1u : 2u;
    ok = ok && visible_count == expected_visible &&
         shadow_count == (enabled ? expected_visible : 0u);
    if (!ok) {
        std::printf("%s FAIL: foliage scene with transparent shadows %s has "
                    "%zu visible / %zu shadow-only renderables or invalid "
                    "color-write/shadow/contact-receiver state (instancing %s)\n", kTag,
                    enabled ? "enabled" : "disabled", visible_count, shadow_count,
                    gpu_instancing ? "enabled" : "disabled");
    }
    builder.clear(core);
    engine.destroy(scene);
    return ok;
}

} // namespace

CaseResult run_shadows_lighting_case(const CaseContext& /*ctx*/) {
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

        const GpuMaterial foliage = make_foliage_material();
        const GpuMaterial opaque  = make_opaque_material();

        // ──: transparent foliage shadows ──────────────────
        // Visible "fir_branch" foliage stays masked in both configurations.
        // Enabling transparent shadows adds an invisible foliage-shadow caster;
        // disabling it makes the visible renderable cast the opaque silhouette.
        if (ok && !skip) {
            MaterialInstanceCache cache(*engine, textures);
            if (auto r = cache.load_base_materials(); !r) {
                std::printf("%s SKIP: base materials failed to build on NOOP "
                            "(%s)\n", kTag, r.error().message.c_str());
                skip = true;
            } else {
                cache.set_transparent_shadows(true);
                auto* visible = cache.get_or_create("fir.mesh", "fir_branch", foliage);
                auto* fol = cache.get_or_create_foliage_shadow(
                    "fir.mesh", "fir_branch", foliage);
                if (!material_is(visible, "heaven_lit_twosided") ||
                    !visible->isColorWriteEnabled() ||
                    visible->getMaterial()->getBlendingMode() != filament::BlendingMode::MASKED) {
                    std::printf("%s FAIL: enabled transparent shadows changed the "
                                "visible foliage's masked/color-writing material\n", kTag);
                    ok = false;
                }
                if (!material_is(fol, "heaven_foliage_shadow") ||
                    fol->isColorWriteEnabled() ||
                    fol->getMaterial()->getBlendingMode() != filament::BlendingMode::TRANSPARENT) {
                    std::printf("%s FAIL: enabled transparent-shadow foliage "
                                "caster routed to '%s' (expected "
                                "heaven_foliage_shadow — lower-density/tinted "
                                "shadow present, Req 6.1/6.2/6.3)\n", kTag,
                                material_name_of(fol));
                    ok = false;
                }
                if (fol == visible ||
                    cache.get_or_create("fir.mesh", "fir_branch", foliage) != visible ||
                    cache.get_or_create_foliage_shadow("fir.mesh", "fir_branch", foliage) != fol) {
                    std::printf("%s FAIL: visible and shadow-only foliage cache "
                                "roles alias or fail to reuse their own instances\n", kTag);
                    ok = false;
                }
                for (const bool instancing : {false, true}) {
                    ok = check_foliage_scene(*engine, cache, foliage, true, instancing) && ok;
                }
                // Non-foliage visible surfaces keep their standard material.
                auto* wall = cache.get_or_create("fort.mesh", "stone_wall", opaque);
                if (!material_is(wall, "heaven_lit")) {
                    std::printf("%s FAIL: enabled transparent-shadow non-foliage "
                                "surface routed to '%s' (expected heaven_lit — "
                                "the foliage-caster predicate must gate the "
                                "routing, Req 6.4)\n", kTag,
                                material_name_of(wall));
                    ok = false;
                }
            }
        }
        if (ok && !skip) {
            MaterialInstanceCache cache(*engine, textures);
            if (auto r = cache.load_base_materials(); !r) {
                std::printf("%s SKIP: base materials failed to build on NOOP "
                            "(%s)\n", kTag, r.error().message.c_str());
                skip = true;
            } else {
                // transparent_shadows disabled (default) ⇒ Shipping_Look.
                for (const bool instancing : {false, true}) {
                    ok = check_foliage_scene(*engine, cache, foliage, false, instancing) && ok;
                }
                auto* fol = cache.get_or_create("fir.mesh", "fir_branch", foliage);
                if (!material_is(fol, "heaven_lit_twosided")) {
                    std::printf("%s FAIL: disabled transparent-shadow foliage "
                                "caster routed to '%s' (expected "
                                "heaven_lit_twosided opaque silhouette ⇒ "
                                "Shipping_Look, Req 6.5)\n", kTag,
                                material_name_of(fol));
                    ok = false;
                }
            }
        }

        // ──: colored penumbra ─────────────────────────────
        // A plain Standard_Lit single-sided surface ("stone_wall" — no advanced
        // foliage / glass markers). Enabled ⇒ routes to the
        // heaven_lit_colored_penumbra variant (the only material that compiles
        // the HAS_COLORED_PENUMBRA tint block, against the epsilon-clamped
        // shader whose divisor max(1.0 - diffuseColor, 1e-3) is strictly
        // positive ⇒ firefly-free on near-white surfaces).
        // Disabled ⇒ keeps heaven_lit (≤1/255 parity).
        if (ok && !skip) {
            MaterialInstanceCache cache(*engine, textures);
            if (auto r = cache.load_base_materials(); !r) {
                std::printf("%s SKIP: base materials failed to build on NOOP "
                            "(%s)\n", kTag, r.error().message.c_str());
                skip = true;
            } else {
                cache.set_colored_penumbra(true);
                auto* lit = cache.get_or_create("fort.mesh", "stone_wall", opaque);
                if (!material_is(lit, "heaven_lit_colored_penumbra")) {
                    std::printf("%s FAIL: enabled colored-penumbra lit surface "
                                "routed to '%s' (expected "
                                "heaven_lit_colored_penumbra, Req 7.1)\n", kTag,
                                material_name_of(lit));
                    ok = false;
                }
            }
        }
        if (ok && !skip) {
            MaterialInstanceCache cache(*engine, textures);
            if (auto r = cache.load_base_materials(); !r) {
                std::printf("%s SKIP: base materials failed to build on NOOP "
                            "(%s)\n", kTag, r.error().message.c_str());
                skip = true;
            } else {
                // colored_penumbra disabled (default) ⇒ Shipping_Look heaven_lit.
                auto* lit = cache.get_or_create("fort.mesh", "stone_wall", opaque);
                if (!material_is(lit, "heaven_lit")) {
                    std::printf("%s FAIL: disabled colored-penumbra lit surface "
                                "routed to '%s' (expected heaven_lit ⇒ "
                                "Shipping_Look, Req 7.3)\n", kTag,
                                material_name_of(lit));
                    ok = false;
                }
            }
        }

        // ──: specular anti-aliasing ───────────────────────
        // (a) Host strength ladder is strictly increasing in level and bounded
        // by the configured maximum  — higher quality ⇒ stronger
        // highlight-variance suppression. This is the host-checkable part of
        // "reduces frame-to-frame highlight shimmer"; the genuine
        // distant-glossy-surface shimmer measurement needs separate moving captures.
        if (ok && !skip) {
            const SpecAaStrength lo  = spec_aa_strength(QualityLevel::Low);
            const SpecAaStrength mid = spec_aa_strength(QualityLevel::Medium);
            const SpecAaStrength hi  = spec_aa_strength(QualityLevel::High);

            const bool variance_increasing =
                lo.variance < mid.variance && mid.variance < hi.variance;
            const bool threshold_increasing =
                lo.threshold < mid.threshold && mid.threshold < hi.threshold;
            const bool bounded =
                hi.variance  <= QualityKnobs::k_spec_aa_variance_max &&
                hi.threshold <= QualityKnobs::k_spec_aa_threshold_max;

            if (!variance_increasing || !threshold_increasing) {
                std::printf("%s FAIL: specular-AA strength ladder not strictly "
                            "increasing (variance %g<%g<%g, threshold %g<%g<%g, "
                            "Req 8.3)\n", kTag, lo.variance, mid.variance,
                            hi.variance, lo.threshold, mid.threshold,
                            hi.threshold);
                ok = false;
            }
            if (!bounded) {
                std::printf("%s FAIL: specular-AA High strength exceeds the "
                            "configured maximum (variance %g>%g or threshold "
                            "%g>%g, Req 8.3)\n", kTag, hi.variance,
                            QualityKnobs::k_spec_aa_variance_max, hi.threshold,
                            QualityKnobs::k_spec_aa_threshold_max);
                ok = false;
            }
        }

        // (b) Enabled ⇒ the exact host strength reaches the lit MaterialInstance
        // (read back through the instance getters), at each level. (c) Disabled
        // ⇒ the setters are never called so the instance keeps Filament's
        // compiled default — untouched ⇒ ≤1/255 parity. The disabled
        // default differs from the enabled-High variance, so the parity claim is
        // not vacuous (enabling genuinely changes the instance state).
        if (ok && !skip) {
            // Baseline: the untouched, disabled instance variance.
            float baseline_var = 0.0f;
            {
                MaterialInstanceCache cache(*engine, textures);
                if (auto r = cache.load_base_materials(); !r) {
                    std::printf("%s SKIP: base materials failed to build on NOOP "
                                "(%s)\n", kTag, r.error().message.c_str());
                    skip = true;
                } else {
                    // specular_aa disabled (default).
                    auto* mi = cache.get_or_create("fort.mesh", "stone_wall", opaque);
                    if (!mi) {
                        std::printf("%s FAIL: could not build lit instance for "
                                    "the specular-AA disabled baseline\n", kTag);
                        ok = false;
                    } else {
                        baseline_var = mi->getSpecularAntiAliasingVariance();
                    }
                }
            }

            // Enabled at each ordered level: the instance reflects the exact
            // spec_aa_strength host values.
            const QualityLevel levels[] = {
                QualityLevel::Low, QualityLevel::Medium, QualityLevel::High };
            for (QualityLevel lvl : levels) {
                if (!ok || skip) break;
                MaterialInstanceCache cache(*engine, textures);
                if (auto r = cache.load_base_materials(); !r) {
                    std::printf("%s SKIP: base materials failed to build on NOOP "
                                "(%s)\n", kTag, r.error().message.c_str());
                    skip = true;
                    break;
                }
                QualityKnobs knobs;
                knobs.specular_aa_level = lvl;
                cache.set_specular_aa(true, knobs);
                auto* mi = cache.get_or_create("fort.mesh", "stone_wall", opaque);
                if (!mi) {
                    std::printf("%s FAIL: could not build lit instance for "
                                "specular-AA enabled level %d\n", kTag,
                                static_cast<int>(lvl));
                    ok = false;
                    break;
                }
                const SpecAaStrength want = spec_aa_strength(lvl);
                const float got_var = mi->getSpecularAntiAliasingVariance();
                const float got_thr = mi->getSpecularAntiAliasingThreshold();
                if (!approx(got_var, want.variance) || !approx(got_thr, want.threshold)) {
                    std::printf("%s FAIL: enabled specular-AA level %d instance "
                                "state (variance %g, threshold %g) does not "
                                "match the host strength ladder (variance %g, "
                                "threshold %g, Req 8.1)\n", kTag,
                                static_cast<int>(lvl), got_var, got_thr,
                                want.variance, want.threshold);
                    ok = false;
                }
            }

            // Parity is non-vacuous: the untouched disabled baseline differs
            // from what enabling High would write (the feature genuinely changes
            // instance state, so "disabled ⇒ untouched default" is meaningful).
            if (ok && !skip) {
                const SpecAaStrength hi = spec_aa_strength(QualityLevel::High);
                if (approx(baseline_var, hi.variance)) {
                    std::printf("%s FAIL: disabled specular-AA baseline variance "
                                "(%g) equals the enabled-High value (%g) — the "
                                "disabled-parity assertion would be vacuous "
                                "(Req 8.4)\n", kTag, baseline_var, hi.variance);
                    ok = false;
                }
            }
        }
        // Caches' destructors run at their block-scope exits; TextureStore is
        // destroyed here (before the engine).
    }

    filament::Engine::destroy(&engine);

    if (skip) return CaseResult::Skip;
    if (!ok)  return CaseResult::Fail;

    std::printf("%s transparent foliage shadows: visible foliage stays masked; "
                "enabled adds a distinct colorWrite:false foliage-shadow caster, "
                "disabled uses the visible opaque shadow (Req 6.1/6.2/6.3/6.5); "
                "colored penumbra: lit⇒heaven_lit_colored_penumbra enabled / "
                "heaven_lit disabled, firefly-clamped divisor (Req 7.1/7.2/7.3); "
                "specular AA: strength ladder strictly increasing+bounded and "
                "applied to the lit instance, untouched when disabled "
                "(Req 8.1/8.2/8.4)\n", kTag);
    return CaseResult::Pass;
}

} // namespace heaven::smoke
