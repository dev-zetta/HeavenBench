/*
 * HeavenBench — Scene light spawner
 *
 * Extracted verbatim out of FilamentBackend::spawn_emission_lights /
 * spawn_authored_lights / update_emission_light_positions /
 * update_floating_lights / set_scene_lights / rebuild_scene_light_entities.
 */
#include "rhi/scene_light_spawner.h"
#include "rhi/filament_util.h"   // EmissionLightTuning, classify_emission_light
#include "render_tuning.h"

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/LightManager.h>

#include <utils/Entity.h>
#include <utils/EntityManager.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <utility>

namespace heaven {

void SceneLightSpawner::set_pending_authored(std::span<const SceneLight> lights) {
    pending_scene_lights_.assign(lights.begin(), lights.end());
    if (!pending_scene_lights_.empty()) {
        size_t spots = 0, omnis = 0;
        for (const auto& l : pending_scene_lights_) {
            if (l.kind == SceneLightKind::Spot) ++spots;
            else                                 ++omnis;
        }
        std::printf("  Scene lights: received %zu (%zu spot, %zu omni)\n",
                    pending_scene_lights_.size(), spots, omnis);
    }
}

void SceneLightSpawner::set_pending_emitters(std::span<const ParticleEmitter> emitters) {
    flame_anchors_.clear();
    for (const auto& emitter : emitters) {
        if (emitter.emitter_enabled && emitter.material_name == "fire_flame") {
            flame_anchors_.push_back({glm::vec3(emitter.transform[3]),
                                      emitter.floating_group});
        }
    }
}

void SceneLightSpawner::spawn_emission(const BackendCore& core,
                                       std::span<const Instance> instances,
                                       std::span<const GpuMesh> meshes,
                                       const MaterialResolver& resolve_material)
{
    auto* engine_ = core.engine;
    auto* scene_  = core.scene;
    std::vector<glm::vec3> flame_positions;
    flame_positions.reserve(flame_anchors_.size());
    for (const auto& anchor : flame_anchors_) {
        flame_positions.push_back(floating_light_position(
            anchor.base_pos, anchor.floating_group, floating_offsets_));
    }

    // Each emissive surface (window, lamp, door…) gets its own point
    // light placed at the surface's local-space AABB centre, pushed
    // slightly outward along the surface's outward normal so the light
    // sits in front of the wall rather than embedded in the stone.
    // This is essential: a light at the building's bbox centre is
    // INSIDE the wall, and Filament's NdotL term zeroes out the
    // contribution to the wall's outward face.
    emission_lights_.reserve(instances.size() * 2);
    for (int idx = 0; std::cmp_less(idx, instances.size()); idx++) {
        const auto& inst = instances[idx];
        if (inst.mesh_index < 0 ||
            std::cmp_greater_equal(inst.mesh_index, meshes.size())) continue;
        const auto& gpu_mesh = meshes[inst.mesh_index];

        // Mesh-level centre — used to compute the outward direction we
        // push the light along.
        glm::vec3 mesh_center = (gpu_mesh.bbox_min + gpu_mesh.bbox_max) * 0.5f;

        for (const auto& surf : gpu_mesh.surfaces) {
            if (surf.index_count == 0) continue;
            auto gpu_mat = resolve_material(gpu_mesh.path, surf.name);
            if (gpu_mat.hidden) continue;

            // A surface is a "luminaire" if its emission state/map makes it
            // an eligible source, OR its name identifies an intentional light
            // source. The original benchmark uses a `building_glass_on`
            // material variant for lit lamp glass, but our material
            // database resolves these sources independently of the current
            // hour. Enclosed lamp glass receives the shared operating gain
            // below; windows and open flames keep their separate behavior.

            // NOTE: do NOT match the bare substring "lamp". A lamp-post
            // mesh has several surfaces — the metal pole/bracket bodies
            // (e.g. `lamp_post_lamp_post_1_lod0`, `lamp_post_lamp_post_2`)
            // AND the lit glass (`lamp_post_lamp_post_glass`). The actual
            // emitter is always the paired `*_glass` surface; the metal
            // bodies are opaque fixtures. Matching "lamp" spawned a light
            // (and a lens flare) at every metal body's AABB centroid too,
            // so each lamp showed two stray flares — one on the bare pole,
            // one on the arm — in addition to the correct one on the glass.
            // Every lamp in the data has a `*_glass` companion, so keying
            // on "glass" (plus open-flame torch/lantern keywords) captures
            // the real light source and drops the spurious bodies.
            const auto& sn = surf.name;
            auto contains = [&](std::string_view k){
                return sn.find(k) != std::string::npos;
            };
            const bool has_emission = gpu_mat.emission != 0 &&
                                      gpu_mat.emission_light_source;
            bool name_lit = contains("lantern") || contains("torch") ||
                            contains("glass") || contains("window");
            if (!has_emission && !name_lit) continue;
            // Some emissive surfaces are large surface tints, not
            // localised light sources — the airship balloon is an
            // emission-tinted fabric, the mill's small accent emission
            // isn't a "lamp". Don't spawn a point light for those; the
            // emission texture alone is enough.
            if (contains("balloon")) continue;

            // Skip LOD imposters — those sit on top of the lod0 surface
            // and adding a duplicate light per LOD over-illuminates the
            // building.
            if (is_lod_imposter(sn)) continue;

            // Surface centre in local space.
            glm::vec3 surf_center = (surf.bbox_min + surf.bbox_max) * 0.5f;

            // Decide where the light sits *relative* to the surface
            // based on what kind of surface it is.

            // For a hanging lantern or torch (3D luminaire that sits in
            // mid-air on a bracket), the bulb position IS the surface
            // centre — there's no wall behind it to push past, and
            // offsetting along an arbitrary direction lands the light in
            // the wall above the bracket and stamps a hard cone there.

            // For a window or door (a flat panel recessed in a wall),
            // the surface is nearly coplanar with the wall and we DO
            // want a small outward push so Filament's NdotL doesn't zero
            // out the contribution to the wall's outward face. We use
            // the vector from the building's centre to the surface's
            // centre, which approximates the wall normal for surfaces
            // near a building's perimeter — but that vector can point
            // upward when the surface sits in a gable wall, so we
            // project out the Z component to keep the offset lateral.
            glm::vec3 local_pos;
            bool anchored_flame = false;
            bool is_luminaire = contains("lantern") || contains("torch") ||
                                contains("glass");
            if (is_luminaire) {
                local_pos = surf_center;
                if (contains("torch")) {
                    if (const auto anchor = luminaire_emitter_anchor(
                            surf.bbox_min, surf.bbox_max, inst.transform,
                            flame_positions)) {
                        local_pos = *anchor;
                        anchored_flame = true;
                    }
                }
            } else {
                glm::vec3 outward_dir = surf_center - mesh_center;
                outward_dir.z = 0.0f;   // lateral only
                float outward_len = glm::length(outward_dir);
                if (outward_len > 1e-3f) outward_dir /= outward_len;
                else outward_dir = glm::vec3(1.0f, 0.0f, 0.0f);
                local_pos = surf_center + outward_dir * 0.4f;
            }

            glm::vec4 world_pos = inst.transform * glm::vec4(local_pos, 1.0f);

            // Colour / intensity / falloff from the surface name.
            EmissionLightTuning t = classify_emission_light(sn);
            const bool night_only = is_night_luminaire(sn);
            if (night_only) {
                const auto color = render_tuning::lighting.enclosed_lamp_color;
                t.color = {color.x, color.y, color.z};
                t.intensity *= render_tuning::lighting.enclosed_lamp_power_scale;
            }

            auto light = utils::EntityManager::get().create();
            filament::LightManager::Builder(
                filament::LightManager::Type::POINT)
                .position({world_pos.x, world_pos.y, world_pos.z})
                .color({t.color.x, t.color.y, t.color.z})
                .intensity(t.intensity)
                .falloff(t.falloff)
                .castShadows(false)
                .build(*engine_, light);
            scene_->addEntity(light);
            // Glass encloses the source. Treating its metal housing as a
            // shadow caster from this interior point blocks the lamp's own
            // downward illumination; the original lamp spot authors shadow=0.
            const bool enclosed = contains("glass") &&
                (contains("lamp") || contains("lantern"));
            const bool visible_bulb = is_luminaire &&
                (contains("lamp") || contains("lantern") || anchored_flame);
            const auto light_instance = engine_->getLightManager().getInstance(light);
            const float nominal = engine_->getLightManager().getIntensity(light_instance);
            emission_lights_.push_back({light, local_pos, idx,
                                       visible_bulb, !enclosed, night_only, nominal});
            if (night_only) {
                night_luminaire_regions_.push_back(
                    {surf.bbox_min, surf.bbox_max, inst.transform});
            }
        }
    }
    std::printf("  Emission lights: %zu created (all point lights)\n",
                emission_lights_.size());
    // Refresh the flattened light list consumed by volumetric fog.
    rebuild_scene_light_entities(core);
}

void SceneLightSpawner::spawn_authored(const BackendCore& core, bool scene_built) {
    auto* engine_ = core.engine;
    auto* scene_  = core.scene;

    // Spawned once after the scene is built, so we know the engine is
    // alive and the floating-island instance list is stable. Lights
    // tagged with a floating_group are repositioned each frame in
    // update_floating_lights().
    if (authored_lights_built_ || !scene_built ||
        pending_scene_lights_.empty()) return;

    authored_lights_built_ = true;
    authored_lights_.reserve(pending_scene_lights_.size());

    for (const auto& sl : pending_scene_lights_) {
        // Calibrated lumen baseline, not an original photometric unit: the
        // actual street source authors multiplier 2, giving 8000 lumens before
        // the enclosed-lamp gain. Unrelated accent/fire lights keep this base.
        float intensity = std::max(1000.0f, sl.multiplier * 4000.0f);
        const glm::vec3 position = floating_light_position(
            sl.world_pos, sl.floating_group, floating_offsets_);

        // Only a unique actual lamp enclosure gives an authored source the
        // shared power calibration and operating schedule.
        const std::array origins{position};
        std::size_t matches = 0;
        for (const auto& region : night_luminaire_regions_) {
            if (luminaire_emitter_anchor(region.bounds_min, region.bounds_max,
                                         region.transform, origins)) ++matches;
        }
        const bool night_only = matches == 1;
        if (night_only) intensity *= render_tuning::lighting.enclosed_lamp_power_scale;

        auto entity = utils::EntityManager::get().create();

        if (sl.kind == SceneLightKind::Spot) {
            // Filament's outer cone is a half-angle; Heaven's
            // outer_angle is also a half-angle, so the values pass
            // through directly. Inner cone lives below outer cone
            // always (clamped in case of bad data).
            float outer = std::clamp(sl.outer_cone_rad, 0.01f, 1.55f);
            float inner = std::clamp(sl.inner_cone_rad, 0.005f, outer);

            filament::LightManager::Builder(
                filament::LightManager::Type::FOCUSED_SPOT)
                .position({position.x, position.y, position.z})
                .direction({sl.direction.x, sl.direction.y, sl.direction.z})
                .color({sl.color.x, sl.color.y, sl.color.z})
                .intensity(intensity)
                .falloff(sl.radius)
                .spotLightCone(inner, outer)
                .castShadows(false)
                .build(*engine_, entity);
        } else {
            // LightOmni → Filament POINT. We keep a non-zero floor on
            // radius so a missing <radius> tag in the source data
            // doesn't produce a zero-falloff light (which Filament
            // treats as covering the whole scene and tanks frame rate).
            float falloff = std::max(0.5f, sl.radius);
            filament::LightManager::Builder(
                filament::LightManager::Type::POINT)
                .position({position.x, position.y, position.z})
                .color({sl.color.x, sl.color.y, sl.color.z})
                .intensity(intensity)
                .falloff(falloff)
                .castShadows(false)
                .build(*engine_, entity);
        }
        scene_->addEntity(entity);
        const auto light_instance = engine_->getLightManager().getInstance(entity);
        const float nominal = engine_->getLightManager().getIntensity(light_instance);
        authored_lights_.push_back({entity, sl.world_pos, sl.floating_group,
                                    sl.shadow_eligible, night_only, nominal});
    }
    std::printf("  Authored lights: %zu created (%zu attached to floating groups)\n",
                authored_lights_.size(),
                [&]{ size_t n=0; for (auto& al:authored_lights_) if (al.floating_group>=0) ++n; return n; }());
    // Free the staging copy now that all entities exist.
    pending_scene_lights_.clear();
    // Authored lights joined the scene — refresh the flattened list
    // consumed by volumetric fog + lens flares.
    rebuild_scene_light_entities(core);
}

void SceneLightSpawner::update_emission_positions(const BackendCore& core,
                                                  std::span<const Instance> instances,
                                                  std::span<const int> dirty_instance_indices)
{
    if (dirty_instance_indices.empty()) return;

    // Each light was spawned in mesh-local space; its world-space
    // position is `parent.transform * local_pos`. Floating islands bob
    // up and down on a sine wave, so without this update the lights stay
    // frozen at their initial spawn position and the surfaces they were
    // attached to drift away from them. Filament's LightManager has no
    // transform-parenting (unlike RenderableManager), so we recompute
    // world position per frame ourselves.
    auto& lm = core.engine->getLightManager();
    for (std::size_t index = 0; index < emission_lights_.size(); ++index) {
        auto& el = emission_lights_[index];
        if (el.instance_index < 0 ||
            std::cmp_greater_equal(el.instance_index, instances.size()) ||
            !std::binary_search(dirty_instance_indices.begin(),
                                dirty_instance_indices.end(),
                                el.instance_index)) continue;
        const auto& inst = instances[el.instance_index];
        glm::vec4 world_pos = inst.transform * glm::vec4(el.local_pos, 1.0f);
        auto li = lm.getInstance(el.entity);
        if (li) {
            lm.setPosition(li, {world_pos.x, world_pos.y, world_pos.z});
            scene_light_infos_[authored_lights_.size() + index].world_pos =
                glm::vec3(world_pos);
        }
    }
    // Lens flares attached to emission lights track their parent
    // instance themselves now (LensFlareFeature::update via
    // FrameContext::instances + the per-flare instance_index).
}

void SceneLightSpawner::update_floating(const BackendCore& core,
                                        std::span<const std::pair<int, float>> group_offsets)
{
    // Cache the offsets so render_frame can forward them to features
    // (the particle feature moves its floating-anchored emitters via
    // FrameContext::floating_offsets).
    if (std::equal(group_offsets.begin(), group_offsets.end(),
                   floating_offsets_.begin(), floating_offsets_.end())) {
        return;
    }
    floating_offsets_.assign(group_offsets.begin(), group_offsets.end());

    if (authored_lights_.empty()) return;

    // Build a small dense lookup. The number of floating groups in
    // Heaven's world is well under 32, so a linear scan per light is
    // cheap (no map allocation, no hashing). We early-out on lights
    // with floating_group < 0 since those don't move.
    auto& lm = core.engine->getLightManager();
    for (std::size_t index = 0; index < authored_lights_.size(); ++index) {
        const auto& al = authored_lights_[index];
        if (al.floating_group < 0) continue;
        const glm::vec3 p = floating_light_position(
            al.base_pos, al.floating_group, group_offsets);
        auto li = lm.getInstance(al.entity);
        if (li) {
            lm.setPosition(li, {p.x, p.y, p.z});
            scene_light_infos_[index].world_pos = p;
        }
    }

    // Lens flares and particle emitters anchored to a floating-island
    // group track it themselves now (their features read
    // FrameContext::floating_offsets, cached in floating_offsets_ above).
}

void SceneLightSpawner::rebuild_scene_light_entities(const BackendCore& core) {
    // Flatten the emission-derived and world-authored point/spot
    // lights into one entity list for features (volumetric fog) to
    // query through the LightManager. Rebuilt whenever either source
    // list changes (scene build, authored-light spawn).
    scene_light_entities_.clear();
    scene_light_entities_.reserve(emission_lights_.size() +
                                  authored_lights_.size());
    for (const auto& el : emission_lights_) scene_light_entities_.push_back(el.entity);
    for (const auto& al : authored_lights_) scene_light_entities_.push_back(al.entity);

    // Richer per-light descriptors for the lens-flare feature. Colour
    // and current world position come from the live LightManager (so a
    // light that already bobbed this far is captured correctly); the
    // local_pos / instance_index / floating_group come from our own
    // bookkeeping so the feature can keep tracking the parent.
    auto& lm = core.engine->getLightManager();
    scene_light_infos_.clear();
    scene_light_infos_.reserve(emission_lights_.size() +
                               authored_lights_.size());
    for (const auto& al : authored_lights_) {
        SceneLightInfo info;
        info.entity         = al.entity;
        info.world_pos      = al.base_pos;
        info.local_pos      = al.base_pos;
        info.floating_group = al.floating_group;
        info.instance_index = -1;
        info.authored       = true;
        info.shadow_eligible = al.shadow_eligible;
        info.nominal_intensity = al.nominal_intensity;
        info.color          = glm::vec3(1.0f, 0.7f, 0.3f);
        if (auto li = lm.getInstance(al.entity)) {
            auto c = lm.getColor(li);
            info.color = glm::vec3(c.r, c.g, c.b);
            const auto position = lm.getPosition(li);
            info.world_pos = glm::vec3(position.x, position.y, position.z);
            info.intensity = lm.getIntensity(li);
        }
        scene_light_infos_.push_back(info);
    }
    for (const auto& el : emission_lights_) {
        SceneLightInfo info;
        info.local_pos      = el.local_pos;
        info.floating_group = -1;
        info.instance_index = el.instance_index;
        info.authored       = false;
        info.flare_source   = el.flare_source;
        info.local_volume_source = el.night_only && el.flare_source;
        info.shadow_eligible = el.shadow_eligible;
        info.nominal_intensity = el.nominal_intensity;
        info.entity         = el.entity;
        info.world_pos      = el.local_pos;
        info.color          = glm::vec3(1.0f);
        if (auto li = lm.getInstance(el.entity)) {
            auto c = lm.getColor(li);
            info.color = glm::vec3(c.r, c.g, c.b);
            auto p = lm.getPosition(li);
            info.world_pos = glm::vec3(p.x, p.y, p.z);
            info.intensity = lm.getIntensity(li);
        }
        scene_light_infos_.push_back(info);
    }
}

void SceneLightSpawner::update_operating_state(
        const BackendCore& core, const LightingParams& lighting) {
    auto& manager = core.engine->getLightManager();
    const float night_gain = night_luminaire_gain(lighting.time_of_day);
    const auto update = [&](auto& light, SceneLightInfo& info) {
        const auto instance = manager.getInstance(light.entity);
        if (!instance) {
            info.intensity = 0.0f;
            return;
        }
        const float gain = light.night_only ? night_gain : 1.0f;
        if (gain != light.operating_gain) {
            manager.setIntensityCandela(instance, light.nominal_intensity * gain);
            light.operating_gain = gain;
        }
        info.intensity = manager.getIntensity(instance);
    };
    for (std::size_t i = 0; i < authored_lights_.size(); ++i) {
        update(authored_lights_[i], scene_light_infos_[i]);
    }
    for (std::size_t i = 0; i < emission_lights_.size(); ++i) {
        update(emission_lights_[i], scene_light_infos_[authored_lights_.size() + i]);
    }
}

void SceneLightSpawner::destroy(const BackendCore& core) {
    auto* engine_ = core.engine;
    auto* scene_  = core.scene;

    for (auto& light : emission_lights_) {
        scene_->remove(light.entity);
        engine_->destroy(light.entity);
        utils::EntityManager::get().destroy(light.entity);
    }
    emission_lights_.clear();

    // Authored scene lights (lamp posts + accent lights). Same lifecycle
    // as emission_lights_ — entities live on the global EntityManager
    // and we own them.
    for (auto& al : authored_lights_) {
        scene_->remove(al.entity);
        engine_->destroy(al.entity);
        utils::EntityManager::get().destroy(al.entity);
    }
    authored_lights_.clear();
    pending_scene_lights_.clear();
    scene_light_entities_.clear();
    scene_light_infos_.clear();
    floating_offsets_.clear();
    flame_anchors_.clear();
    night_luminaire_regions_.clear();
    authored_lights_built_ = false;
}

} // namespace heaven
