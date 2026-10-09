/*
 * HeavenBench — Lighting system
 *
 * Owns the scene's global lighting: the directional sun, the night
 * moonlight fill, the procedural Z-up IBL cubemap + its Skybox, the
 * IndirectLight probe, and the time-of-day bloom
 * retune. The IBL probe is lighting infrastructure (it drives
 * reflections + ambient on every lit surface) so it lives here, NOT in
 * the visible-sky feature — it must exist even with `--no-sky`. The
 * backend tells this system whether the sky-sphere feature is enabled so
 * it knows whether to attach the cubemap Skybox as the visible
 * background.
 *
 * Extracted verbatim out of FilamentBackend::setup_lighting and the
 * init-time skybox/IBL block in init_pipelines().
 */
#ifndef HEAVENBENCH_SRC_RHI_LIGHTING_SYSTEM_H_
#define HEAVENBENCH_SRC_RHI_LIGHTING_SYSTEM_H_

#include "rhi/rhi.h"            // LightingParams
#include "rhi/backend_core.h"
#include "rhi/render_feature.h" // SceneLightInfo

#include <utils/Entity.h>

#include <span>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <utility>

namespace filament {
class Skybox;
class IndirectLight;
class Texture;
class LightManager;
}

namespace heaven {

struct EnvironmentProbes;

class LightingSystem {
public:
    explicit LightingSystem(const BackendCore& core);
    ~LightingSystem();

    // Build the initial procedural cubemap + Skybox + IBL probe. Called
    // once from init_pipelines() before the first frame.
    void init_environment(const LightingParams& initial_light);
    [[nodiscard]] bool prefiltered_environment() const noexcept {
        return environment_probes_ != nullptr;
    }

    // (Re)configure the sun, time-of-day cubemap band, night moon fill, and
    // bloom. `sky_sphere_enabled` decides whether the
    // cubemap Skybox is attached as the visible background. `quality` /
    // `features` carry the advanced-rendering knobs/toggles (e.g. soft
    // directional shadows) so the View/light shadow configuration is applied
    // through this existing per-frame configuration path rather than being
    // inlined into render_frame and without adding per-effect backend members
    // Re-applied every call so a runtime feature flip
    // lands by the next frame.
    void setup(const LightingParams& lighting, bool sky_sphere_enabled,
               const QualityKnobs& quality, const FeatureToggles& features,
               float requested_bloom_strength = render_tuning::lighting.bloom_reference_strength);

    // Update time-dependent light uniforms, the cached probe band, background
    // visibility and bloom without rebuilding post-processing or shadow options.
    // An unchanged lighting / sky state does no work.
    void update(const LightingParams& lighting, bool sky_sphere_enabled,
                const FeatureToggles& features,
                float requested_bloom_strength = render_tuning::lighting.bloom_reference_strength);

    // Apply shadow casting to the scene's point/spot lights.
    // Selects the highest-luminous-intensity `min(budget, n)` lights (ties
    // broken by the stable per-light entity id) via the pure
    // select_shadow_casting_lights helper and flips setShadowCaster(true) on
    // exactly those, setShadowCaster(false) on every other candidate
    // When `enabled` is false, shadow casting is disabled on
    // ALL candidates (budget treated as 0) so local lighting is bit-identical
    // to the Shipping_Look. Creates/destroys no lights — it only
    // toggles shadow casting on the backend-owned lights handed in via
    // FrameContext::scene_lights. Called per-frame from render_frame
    // so a runtime toggle / budget change lands by the next frame.
    void configure_point_light_shadows(std::span<const SceneLightInfo> scene_lights,
                                        int budget, int map_size, bool enabled);

private:
    // Apply the directional sun shadow configuration through the View /
    // LightManager option path. When `enabled`, switch the
    // View to a contact-hardening soft-shadow mode (PCSS) and scale the
    // penumbra by the soft-shadow Quality_Knob sample count
    // (soft_shadow_sample_count); when disabled, restore the Shipping_Look
    // basic PCF mode with the default options so the disabled-state output is
    // bit-identical to the baseline path.

    // `colored_penumbra` carries the colored-penumbra toggle
    // onto this same soft-shadow path. Colored penumbra is only meaningful
    // while the DPCF/PCSS soft-shadow path is active, so it is threaded here
    // rather than added as a separate View-option group. It defaults off and,
    // when off, this path is bit-identical to the Shipping_Look.
    // See the.cpp for why there is no runtime View field to set and how the
    // committed HAS_COLORED_PENUMBRA define fix governs the actual tint.
    void configure_soft_shadows(filament::LightManager& lm, utils::Entity sun,
                                const QualityKnobs& quality, bool enabled,
                                bool colored_penumbra);

    // Apply the directional sun's screen-space contact-shadow configuration
    // through the LightManager option path. When `enabled`,
    // set the sun's ShadowOptions::screenSpaceContactShadows = true with a
    // ray-march stepCount from the contact-shadow Quality_Knob
    // (contact_shadow_steps, bounded by [min, max]); when disabled, leave
    // screenSpaceContactShadows = false so the directional lighting is
    // bit-identical to the Shipping_Look baseline default.
    void configure_contact_shadows(filament::LightManager& lm, utils::Entity sun,
                                   const QualityKnobs& quality, bool enabled);

    // The dominant directional light owns Filament's shadow atlas. At night
    // that is the Moon; inherit the same quality/contact choices as the Sun,
    // with the lunar angular size instead of a default one-radian bulb.
    void configure_moon_shadow_options();

    BackendCore core_;
    utils::Entity            sun_entity_;
    utils::Entity            moon_entity_;
    bool                     moon_in_scene_ = false;
    std::optional<LightingParams> previous_lighting_;
    bool previous_sky_sphere_enabled_ = false;
    bool previous_bloom_enabled_ = false;
    float previous_requested_bloom_strength_ = render_tuning::lighting.bloom_reference_strength;
    bool previous_clouds_enabled_ = false;
    filament::Skybox*        skybox_         = nullptr;
    filament::IndirectLight* indirect_light_ = nullptr;
    filament::Texture*       sky_cubemap_      = nullptr;
    int                      sky_cubemap_band_ = -1;
    std::unique_ptr<EnvironmentProbes> environment_probes_;

    // Cache for the point/spot shadow-caster selection. configure_point_light_
    // shadows() is called every frame, but mutating each light's shadow-caster
    // state every frame forces Filament to rebuild its shadow atlas per frame
    // (visible flicker) and stalls the main thread (camera-movement lag despite
    // a high FPS). The selection is deterministic and only changes when the
    // light set, budget, or enabled flag changes, so we apply it once and skip
    // the per-frame churn while the inputs are unchanged. `ptlight_applied_`
    // guards the very first apply; the signature fields detect input changes.
    std::vector<utils::Entity> ptlight_shadow_casters_;   // entities currently casting
    int                        ptlight_budget_sig_    = -1;
    int                        ptlight_map_size_sig_  = -1;
    bool                       ptlight_enabled_sig_   = false;
    uint64_t                   ptlight_set_sig_       = 0;
    bool                       ptlight_applied_       = false;

    // Original (unclamped) falloff radius of each light whose falloff we shrank
    // for shadow cost (B1′). A point light's cube shadow map renders every
    // caster inside its falloff sphere, so the brightest lamps — which have the
    // largest radii — dominate the per-frame shadow cost. We cap a caster's
    // falloff while it casts and restore the original when it stops, so the
    // lighting reach is only reduced for the handful of lights actually paying
    // the shadow cost. Keyed by entity, parallel bookkeeping to
    // ptlight_shadow_casters_.
    std::vector<std::pair<utils::Entity, float>> ptlight_orig_falloff_;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_LIGHTING_SYSTEM_H_
