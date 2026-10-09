/*
 * HeavenBench — Material instance cache
 *
 * Owns the three base materials (heaven_lit, heaven_lit_twosided,
 * heaven_unlit) and the per-surface MaterialInstance cache. Building an
 * instance wires up the diffuse / normal / specular / emission / detail /
 * height / tessellation textures and the metal-vs-dielectric class,
 * emission-glow heuristics, UV transforms, displacement scale and alpha
 * threshold from a GpuMaterial. Texture handles are resolved through the
 * TextureStore (which also provides the placeholder fallbacks).
 *
 * Extracted verbatim out of FilamentBackend::get_or_create_material_instance
 * and the base-material loading block in init_pipelines().
 *
 * Screen-space refractive glass: the cache also owns the
 * refractive `heaven_glass` base material. When the refraction feature is
 * enabled (set_refraction_enabled(true)) AND the screen-space scene color it
 * needs is available, surfaces flagged `GpuMaterial::glass` (classified from
 * the xml0-derived material name / parent chain — see glass_material_ref.h)
 * are routed to that refractive material instead of the opaque `heaven_lit`.
 * With the feature disabled — or when the screen-data-availability seam
 * reports unavailable  — glass surfaces keep the opaque `heaven_lit`
 * material, pixel-matching the Shipping_Look.
 */
#ifndef HEAVENBENCH_SRC_RHI_MATERIAL_INSTANCE_CACHE_H_
#define HEAVENBENCH_SRC_RHI_MATERIAL_INSTANCE_CACHE_H_

#include "rhi/rhi.h"            // GpuMaterial, Result
#include "rhi/texture_store.h"
#include "engine/material_model_ref.h"  // EnabledModels, Material_Model selector

#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace filament {
class Engine;
class Material;
class MaterialInstance;
class TextureSampler;
}

namespace heaven {

// Composite material-instance cache key. Surface names are not
// globally unique — the same name recurs across different meshes — so keying
// the cache on the surface name alone aliases distinct surfaces onto one shared
// MaterialInstance. Keying on (mesh_path, surface_name) keeps each surface's
// instance distinct. A type-safe struct is used over a `mesh_path\0surface`
// concatenation to avoid any chance of a separator collision.
struct MaterialKey {
    std::string mesh_path;
    std::string surface_name;
    bool operator==(const MaterialKey&) const = default;
};

} // namespace heaven

namespace std {

template <>
struct hash<heaven::MaterialKey> {
    std::size_t operator()(const heaven::MaterialKey& k) const noexcept {
        // Hash-combine of the two member string hashes (boost-style mix).
        std::size_t h = std::hash<std::string>{}(k.mesh_path);
        h ^= std::hash<std::string>{}(k.surface_name) +
             0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return h;
    }
};

} // namespace std

namespace heaven {

// Not `final`: the "screen-space scene color unavailable" decision is
// a protected virtual seam (`refraction_scene_color_available`) so a gated
// smoke test can subclass and force the unavailable branch deterministically
// without a GPU dependency, exactly like
// WetReflectionsFeature::probe_capability. The default seam is the production
// feature-level check, so behaviour is unchanged for the shipping renderer.
class MaterialInstanceCache {
public:
    MaterialInstanceCache(filament::Engine& engine, TextureStore& textures)
        : engine_(engine), textures_(textures) {}
    virtual ~MaterialInstanceCache();

    // Load the compiled base materials (lit / lit_twosided / unlit + the
    // refractive glass material). Returns an error if any fails to build.
    [[nodiscard]] Result<void> load_base_materials();

    // Enable/disable the screen-space-refraction routing. When
    // enabled, glass-flagged surfaces resolve to the refractive glass
    // material (subject to the screen-data-availability seam); when disabled
    // they keep the opaque heaven_lit material (Shipping_Look parity).
    // Set from AppConfig::features.refraction at init, before the scene is
    // built. No effect on already-cached instances.
    void set_refraction_enabled(bool enabled) noexcept { refraction_enabled_ = enabled; }
    [[nodiscard]] bool refraction_enabled() const noexcept { return refraction_enabled_; }

    void set_tessellation_config(float scale, float distance_multiplier,
                                bool enabled = true) noexcept {
        tessellation_scale_ = scale;
        tessellation_distance_multiplier_ = distance_multiplier;
        tessellation_enabled_ = enabled;
    }

    // Enable/disable the per-surface advanced material-model selection
    // (Group A —). Carries the per-model enable gate
    // (clear_coat / anisotropic / subsurface / cloth) into the data-driven
    // Material_Model_Selector run in get_or_create. Set from
    // AppConfig::features at init, BEFORE the scene is built (mirrors
    // set_refraction_enabled). No effect on already-cached instances. With
    // every field false the selector returns each surface's Shipping_Look model
    // so the existing base material + parameter writes are used unchanged
    // (≤1/255 parity).
    void set_advanced_models(material_model_ref::EnabledModels models) noexcept {
        advanced_models_ = models;
    }
    [[nodiscard]] material_model_ref::EnabledModels advanced_models() const noexcept {
        return advanced_models_;
    }

    // Enable/disable the transparent (coverage-modulated) foliage-shadow
    // routing (Group B —). When enabled, surfaces the
    // foliage-caster predicate matches
    // (material_model_ref::is_foliage_shadow_caster over the xml0 surface /
    // material name + parent chain —) resolve to the
    // heaven_foliage_shadow base material, which casts a coverage-modulated
    // (screen-door) transparent shadow whose density follows the caster's
    // per-texel alpha  and whose tint derives from the foliage
    // colour. When disabled, foliage keeps the Shipping_Look
    // opaque-silhouette path (heaven_lit_twosided), so a flagless run is
    // pixel-identical (≤1/255 parity). Set from
    // AppConfig::features.transparent_shadows at init, BEFORE the scene is
    // built (mirrors set_refraction_enabled / set_advanced_models). No effect
    // on already-cached instances.
    void set_transparent_shadows(bool enabled) noexcept { transparent_shadows_ = enabled; }
    [[nodiscard]] bool transparent_shadows() const noexcept { return transparent_shadows_; }

    // Enable/disable colored-penumbra routing for plain lit surfaces
    // Group B —). When enabled, plain Standard_Lit single-sided
    // surfaces (the heaven_lit path — not glass, not foliage, not an advanced
    // shading model) are built from the heaven_lit_colored_penumbra base
    // material instead of heaven_lit. That variant is heaven_lit authored with
    // `coloredPenumbra: true`, so the committed HAS_COLORED_PENUMBRA define
    // fix emits the colored-penumbra tint block for THIS variant only — every
    // shipped lit material leaves it off. It compiles against the
    // epsilon-clamped lib/filament colored-penumbra shader so near-white/white
    // surfaces stay firefly-free. When disabled, the surface keeps
    // heaven_lit, so a flagless run is pixel-identical (≤1/255 parity,
    // Set from AppConfig::features.colored_penumbra at init,
    // BEFORE the scene is built (mirrors set_specular_aa /
    // set_transparent_shadows). No effect on already-cached instances.
    void set_colored_penumbra(bool enabled) noexcept { colored_penumbra_ = enabled; }
    [[nodiscard]] bool colored_penumbra() const noexcept { return colored_penumbra_; }

    // Enable/disable geometric specular anti-aliasing on the lit base materials
    // (Group B —). The materials are compiled with
    // `specularAntiAliasing: true`; the per-instance STRENGTH is applied here
    // through Filament's MaterialInstance setters
    // (setSpecularAntiAliasingVariance / setSpecularAntiAliasingThreshold) —
    // there are no declared.mat parameters for it. When enabled the variance
    // and threshold are resolved ONCE from heaven::spec_aa_strength(knobs
    // .specular_aa_level) and applied to every glossy/metallic lit-model
    // instance (the lit / lit_twosided / advanced bases — NOT unlit). When
    // disabled the setters are never called, so each instance keeps Filament's
    // defaults and a flagless run is pixel-identical (≤1/255 parity).
    // Set from AppConfig::features.specular_aa + AppConfig::quality at init,
    // BEFORE the scene is built (mirrors set_refraction_enabled /
    // set_transparent_shadows). No effect on already-cached instances.
    void set_specular_aa(bool enabled, const QualityKnobs& knobs) noexcept {
        specular_aa_enabled_ = enabled;
        const SpecAaStrength s = spec_aa_strength(knobs.specular_aa_level);
        specular_aa_variance_  = s.variance;
        specular_aa_threshold_ = s.threshold;
    }
    [[nodiscard]] bool specular_aa_enabled() const noexcept { return specular_aa_enabled_; }

    void set_material_workflow(MaterialWorkflow workflow) noexcept {
        material_workflow_ = workflow;
    }
    [[nodiscard]] MaterialWorkflow material_workflow() const noexcept {
        return material_workflow_;
    }
    [[nodiscard]] uint64_t compatibility_surface_count() const noexcept {
        return compatibility_surfaces_;
    }
    [[nodiscard]] uint64_t experimental_surface_count() const noexcept {
        return experimental_surfaces_;
    }
    [[nodiscard]] uint64_t compatibility_instance_count() const noexcept {
        return compatibility_instances_;
    }
    [[nodiscard]] uint64_t experimental_instance_count() const noexcept {
        return experimental_instances_;
    }
    [[nodiscard]] uint64_t material_binding_count() const noexcept {
        return mat_instance_cache_.size();
    }

    // Destroy only per-surface instances. Compiled base materials remain
    // alive, allowing a reload to rebuild bindings against a different texture
    // set without recompiling or reloading shader packages.
    void clear_instances();

    // Keep enclosed bulb emission on the same day/night schedule as its light.
    // Newly created instances inherit the current state; cached bindings update
    // only when the gain changes.
    void update_lamp_emission(float time_of_day);

    // Build (or fetch from cache) the MaterialInstance for a surface. Keyed on
    // the composite (mesh_path, surface_name) so the same surface name on
    // different meshes resolves to distinct instances.
    [[nodiscard]] filament::MaterialInstance* get_or_create(
        const std::string& mesh_path, const std::string& surface_name,
        const GpuMaterial& gpu_mat, uint64_t usage_weight = 1);

    // Build the invisible transparent-shadow caster instance for alpha-tested
    // foliage. Visible foliage must stay on the masked material path; this
    // separate role lets SceneBuilder add a shadow-only duplicate renderable
    // without putting the trees/log branches into Filament's transparent
    // colour-sorting path.
    [[nodiscard]] filament::MaterialInstance* get_or_create_foliage_shadow(
        const std::string& mesh_path, const std::string& surface_name,
        const GpuMaterial& gpu_mat);

protected:
    // screen-space-scene-color availability seam, modeled on
    // WetReflectionsFeature::probe_capability. Returns whether the
    // screen-space scene color the refractive path samples is available.
    // The default implementation is the production check — Filament's
    // screen-space refraction needs the standard FEATURE_LEVEL_1
    // (GLES-3.0-class) feature set the color/refraction buffers depend on;
    // FEATURE_LEVEL_0 backends lack it. Virtual so a test can force the
    // unavailable branch; on every real desktop backend it returns true.
    [[nodiscard]] virtual bool refraction_scene_color_available() const;

private:
    // Build the refractive glass MaterialInstance for a glass surface
    // (base color / normal + the transmission / absorption / IOR / thickness
    // refraction set). Returns nullptr if the glass base material is missing.
    [[nodiscard]] filament::MaterialInstance* build_glass_instance(
        const std::string& surface_name, const GpuMaterial& gpu_mat);

    // Resolve and bind one emission policy for opaque and refractive routes.
    // Only actual authored sources or lamp/lantern bulbs acquire new
    // refractive emission; the existing opaque fallback remains unchanged.
    void bind_surface_emission(filament::MaterialInstance& instance,
        const std::string& surface_name, const GpuMaterial& gpu_mat,
        const filament::TextureSampler& sampler, bool refractive);

    [[nodiscard]] filament::MaterialInstance* get_or_create_impl(
        const std::string& mesh_path, const std::string& surface_name,
        const GpuMaterial& gpu_mat, bool force_foliage_shadow,
        uint64_t usage_weight);

    filament::Engine& engine_;
    TextureStore&     textures_;
    filament::Material* lit_material_          = nullptr;
    filament::Material* lit_twosided_material_ = nullptr;
    // Opaque double-sided lit base. For surfaces authored two-sided but NOT
    // alpha-tested (cloth flags/banners): rendered solid (alpha ignored) and
    // double-sided, instead of the masked heaven_lit_twosided which would cut
    // their partial diffuse alpha into see-through holes.
    filament::Material* lit_doublesided_material_ = nullptr;
    filament::Material* unlit_material_        = nullptr;
    filament::Material* glass_material_        = nullptr;
    // advanced base materials (Group A). Each is the standard lit
    // material extended/re-authored for one Filament shading model:
    //   clearcoat / aniso → standard-lit + clearCoat / anisotropy parameters
    //   subsurface        → shadingModel: subsurface
    //   cloth             → shadingModel: cloth
    // All four carry the full common lit parameter set so the existing
    // per-instance parameter writes apply uniformly; the selector picks one
    // per surface in get_or_create when its model is enabled.
    filament::Material* clearcoat_material_     = nullptr;
    filament::Material* aniso_material_         = nullptr;
    filament::Material* subsurface_material_    = nullptr;
    filament::Material* cloth_material_         = nullptr;
    filament::Material* foliage_shadow_material_ = nullptr;
    // Colored-penumbra lit variant. heaven_lit authored with
    // `coloredPenumbra: true`; the only material that compiles in the
    // HAS_COLORED_PENUMBRA tint block. Plain Standard_Lit single-sided surfaces
    // route here when set_colored_penumbra(true). Loaded unconditionally so
    // base-material setup stays deterministic; the routing in get_or_create()
    // gates its use, so a disabled feature never binds it.
    filament::Material* colored_penumbra_material_ = nullptr;
    // Window variant. heaven_lit re-authored for Heaven's opaque, baked
    // `*window*` wall surfaces: the specular map drives a glass-aware
    // microsurface (reflective panes vs matte frame) and the night emission is
    // tamed to a warm interior glow instead of a clipped yellow card. Surfaces
    // are routed here from get_or_create when window_ref::is_window_material
    // matches (name-based, no text mapping). Loaded best-effort: if it fails,
    // windows fall back to the plain heaven_lit path unchanged.
    filament::Material* window_material_       = nullptr;
    bool                refraction_enabled_    = false;
    material_model_ref::EnabledModels advanced_models_{};
    bool                transparent_shadows_   = false;
    // Colored-penumbra routing gate. Set from
    // AppConfig::features.colored_penumbra before the scene is built.
    bool                colored_penumbra_      = false;
    MaterialWorkflow    material_workflow_ =
        MaterialWorkflow::MetallicRoughnessCompat;
    // Specular-AA enable + resolved strength. Resolved once in
    // set_specular_aa from the quality knob; applied per glossy/metallic lit
    // instance via Filament's instance setters in get_or_create when enabled.
    bool                specular_aa_enabled_   = false;
    float               specular_aa_variance_  = 0.0f;
    float               specular_aa_threshold_ = 0.0f;
    float               tessellation_scale_               = 1.5f;
    float               tessellation_distance_multiplier_ = 1.5f;
    bool                tessellation_enabled_ = true;
    // Latched so the "scene color unavailable" diagnostic is emitted
    // at most once rather than once per glass surface.
    bool                refraction_unavailable_warned_ = false;
    // Latched one-time diagnostics for the selector:
    // a rejected (negative/NaN) clear-coat strength, and the subsurface
    // screen-data-unavailable fallback to the opaque Shipping_Look material.
    bool                clear_coat_rejected_warned_   = false;
    bool                subsurface_unavailable_warned_ = false;
    std::unordered_map<MaterialKey, filament::MaterialInstance*> mat_instance_cache_;
    struct LampEmissionBinding {
        filament::MaterialInstance* instance;
        float nominal_scale;
    };
    std::vector<LampEmissionBinding> lamp_emission_bindings_;
    float lamp_emission_gain_ = 1.0f;
    std::unordered_map<MaterialKey, bool> workflow_eligibility_;
    uint64_t compatibility_surfaces_ = 0;
    uint64_t experimental_surfaces_ = 0;
    uint64_t compatibility_instances_ = 0;
    uint64_t experimental_instances_ = 0;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_MATERIAL_INSTANCE_CACHE_H_
