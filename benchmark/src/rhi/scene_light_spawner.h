/*
 * HeavenBench — Scene light spawner
 *
 * Owns the scene's point/spot lights: the per-luminaire emission lights
 * synthesised from emissive surfaces (windows, lamps, glass), the
 * world-authored lamp-post / accent lights from set_scene_lights(), their
 * per-frame world-position updates (floating-island bob), and the
 * flattened entity + SceneLightInfo lists handed to features through
 * FrameContext (volumetric fog reads the entities; lens flares read the
 * richer descriptors).
 *
 * Extracted verbatim out of FilamentBackend::spawn_emission_lights /
 * spawn_authored_lights / update_emission_light_positions /
 * update_floating_lights / set_scene_lights / rebuild_scene_light_entities.
 */
#ifndef HEAVENBENCH_SRC_RHI_SCENE_LIGHT_SPAWNER_H_
#define HEAVENBENCH_SRC_RHI_SCENE_LIGHT_SPAWNER_H_

#include "rhi/rhi.h"            // Instance, GpuMesh, MaterialResolver, SceneLight
#include "rhi/render_feature.h" // SceneLightInfo
#include "rhi/backend_core.h"

#include <utils/Entity.h>

#include <glm/glm.hpp>

#include <span>
#include <utility>
#include <vector>

namespace heaven {

class SceneLightSpawner {
public:
    // Synthesise per-luminaire emission point lights. Runs once after
    // the scene is built.
    void spawn_emission(const BackendCore& core,
                        std::span<const Instance> instances,
                        std::span<const GpuMesh>  meshes,
                        const MaterialResolver&   resolve_material);

    // Stage the world-authored lights from the loader (set_scene_lights).
    void set_pending_authored(std::span<const SceneLight> lights);

    // Scene emitters identify the flame above an otherwise opaque torch bowl.
    // This input remains available even when particle rendering is disabled.
    void set_pending_emitters(std::span<const ParticleEmitter> emitters);

    // Spawn the staged authored lights once the scene is built. No-op
    // until then (or when there are none).
    void spawn_authored(const BackendCore& core, bool scene_built);

    // Per-frame: recompute emission-light world positions from their
    // parent instance transform (floating-island bob).
    void update_emission_positions(const BackendCore& core,
                                   std::span<const Instance> instances,
                                   std::span<const int> dirty_instance_indices);

    // Per-frame: translate floating-group authored lights by their Z
    // offset; caches the offsets for features to read.
    void update_floating(const BackendCore& core,
                         std::span<const std::pair<int, float>> group_offsets);

    // Shared lamp power state, updated before shadow selection and features.
    // Visible bulb emission uses the same pure night gain in the material cache.
    void update_operating_state(const BackendCore& core, const LightingParams& lighting);

    // Remove + destroy all light entities. Call before the engine dies.
    void destroy(const BackendCore& core);

    // Read-only spans handed to FrameContext each frame.
    [[nodiscard]] std::span<const utils::Entity>  light_entities() const { return scene_light_entities_; }
    [[nodiscard]] std::span<const SceneLightInfo> light_infos()    const { return scene_light_infos_; }
    [[nodiscard]] std::span<const std::pair<int, float>> floating_offsets() const { return floating_offsets_; }

private:
    void rebuild_scene_light_entities(const BackendCore& core);

    // Per-instance point lights that match the emission textures on
    // windows, doors, and lamps. Spawned once when the scene is built.
    // Their world-space position is `parent.transform * local_pos`,
    // recomputed every frame so they track the floating-island sine
    // wave animation rather than staying frozen at their initial spot.
    struct EmissionLight {
        utils::Entity entity;
        glm::vec3     local_pos;        // mesh-local position
        int           instance_index;    // which scene instance to track
        bool          flare_source = false;
        bool          shadow_eligible = true;
        bool          night_only = false;
        float         nominal_intensity = 0.0f;
        float         operating_gain = 1.0f;
    };
    std::vector<EmissionLight> emission_lights_;

    // World-authored scene lights (Heaven's lamp posts + accent
    // lights). Spawned once when set_scene_lights is called, then
    // optionally translated each frame in update_floating_lights to
    // bob with their parent floating island.
    struct AuthoredLight {
        utils::Entity entity;
        glm::vec3     base_pos;          // authored world position
        int           floating_group;    // -1 if static
        bool          shadow_eligible = true;
        bool          night_only = false;
        float         nominal_intensity = 0.0f;
        float         operating_gain = 1.0f;
    };
    std::vector<AuthoredLight>   authored_lights_;
    std::vector<SceneLight>      pending_scene_lights_;
    bool                         authored_lights_built_ = false;
    struct EmitterAnchor {
        glm::vec3 base_pos;
        int floating_group = -1;
    };
    std::vector<EmitterAnchor> flame_anchors_;
    struct NightLuminaireRegion {
        glm::vec3 bounds_min;
        glm::vec3 bounds_max;
        glm::mat4 transform;
    };
    std::vector<NightLuminaireRegion> night_luminaire_regions_;

    // Flat list of all scene point/spot light entities (emission-
    // derived + world-authored), rebuilt when either list changes.
    std::vector<utils::Entity>                 scene_light_entities_;
    // Richer per-light descriptors handed to features via
    // FrameContext::scene_lights. Rebuilt alongside scene_light_entities_.
    std::vector<SceneLightInfo>                scene_light_infos_;
    // Latest per-group floating-island Z offsets from update_floating().
    std::vector<std::pair<int, float>>         floating_offsets_;
};

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_SCENE_LIGHT_SPAWNER_H_
