/*
 * HeavenBench — Rendering Hardware Interface (RHI)
 *
 * Abstract interface for the complete rendering pipeline. The
 * Filament backend is the sole implementation today (the legacy
 * hand-rolled OpenGL 4.5 backend was removed once Filament reached
 * feature parity). This is a "fat interface" by design — the
 * pipeline is fixed (shadow → scene → post → tonemap) and backends
 * implement the entire thing internally, so application code never
 * sees GPU-API specifics.
 */
#ifndef HEAVENBENCH_SRC_RHI_RHI_H_
#define HEAVENBENCH_SRC_RHI_RHI_H_

#include "engine/types.h"
#include "engine/camera.h"
#include "engine/asset_file_system.h"
#include "engine/path_resolver.h"
#include "engine/gpu_driver_info.h"
#include "engine/loading_progress.h"

#include <memory>
#include <functional>
#include <filesystem>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace heaven {

enum class TextureSemantic : uint8_t {
    ColorSRGB = 0,
    ScalarLinear,
    NormalLinear,
    HDRLinear,
    // Encoded color composed by the legacy material before gamma-2 decoding.
    // Upload raw values; retain a distinct cache identity from linear data.
    ColorLegacyGamma2,
};

// Forward declaration: upload_skinned takes a SkinnedMesh by const-ref. The
// full definition (engine/skinned_mesh.h) is GPU-free but glm-heavy; the
// backends that implement upload_skinned include it directly.
struct SkinnedMesh;

// ── Backend API enum ────────────────────────────────────────────

// `GraphicsApi` and its `to_string` were relocated to engine/types.h
// (a GPU-free, GLM-only header) so the host-testable CLI parser can
// produce a BenchConfig without depending on Filament/SDL. This header
// includes types.h above, so every backend call site here keeps working
// unchanged.

// ── Lighting / tonemap parameters ───────────────────────────────

struct LightingParams {
    // Sun (directional light)
    glm::vec3 sun_dir       = glm::normalize(glm::vec3(0.4f, 0.3f, 0.8f));
    glm::vec3 sun_color     = {1.5f, 1.4f, 1.2f};
    float     sun_intensity = 110000.0f; // lux at the surface (Filament units)

    // Ambient / hemisphere — drives the skybox tint and IBL
    // strength on the Filament backend.
    glm::vec3 ambient       = {0.15f, 0.18f, 0.25f};

    // Sky / fog colour visible at the horizon — drives the skybox.
    glm::vec3 sky_color     = {0.55f, 0.70f, 0.85f};

    // Time of day in [0, 24); 12 = noon, 18 = sunset.
    float     time_of_day   = 18.0f;
};

// Compute LightingParams for a given hour in [0, 24). Models the sun's
// elevation as a sine over the full day, blends sun colour from a cool
// noon to a warm sunset to a near-zero blue night, and scales intensity
// down at dawn/dusk so the per-instance window/lamp lights start to win.

// Convention: `sun_dir` is the direction FROM the world TO the sun (i.e.
// the unit vector that points up at the sun). The grass shader and god-
// rays code already assume this convention; the Filament backend negates
// it when handing it to LightManager (which wants direction-of-travel).
[[nodiscard]] inline LightingParams lighting_for_time_of_day(float hour) {
    LightingParams L;
    L.time_of_day = hour;

    // Map hour to a "solar angle" in radians: 06:00 = sunrise (0),
    // 12:00 = high noon (π/2), 18:00 = sunset (π), 00:00 = nadir (-π/2).
    // The Heaven world is Z-up, so the sun's elevation lives in Z.
    float t   = (hour - 6.0f) * (std::numbers::pi_v<float> / 12.0f);
    float sin_t = std::sin(t);                  // -1..1, peak at noon (Z component)
    float cos_t = std::cos(t);                  // +1 at sunrise, -1 at sunset (XY plane)

    // Sun position direction (FROM origin TO sun). At noon the sun is
    // straight up (+Z). At sunrise it sits east, at sunset west, with a
    // fixed azimuth offset for visual interest. Continue below the horizon
    // at night so twilight, stars and the opposite lunar path agree.
    float az = 0.6f;
    glm::vec3 sun_pos(cos_t * std::cos(az),
                      cos_t * std::sin(az),
                      sin_t);
    L.sun_dir = glm::normalize(sun_pos);

    // Day fraction: 1 = midday, 0 = horizon, negative when below.
    float day = std::max(0.0f, sin_t);

    // Sun colour: white at noon, deep amber at sunset, faint blue at night.
    glm::vec3 noon  (1.05f, 1.00f, 0.95f);
    glm::vec3 sunset(1.20f, 0.55f, 0.20f);
    glm::vec3 night (0.10f, 0.18f, 0.32f);

    // Sunset window blends from ~16:30 to ~19:30, dawn 04:30 to 07:30.
    auto smooth = [](float x){ x = std::clamp(x, 0.0f, 1.0f); return x*x*(3.0f - 2.0f*x); };
    float dusk_t = smooth(1.0f - std::abs(hour - 18.0f) / 1.5f);
    float dawn_t = smooth(1.0f - std::abs(hour -  6.0f) / 1.5f);
    float warm_t = std::max(dusk_t, dawn_t);
    float day_t  = smooth((day - 0.05f) / 0.95f); // 1 when sun high, 0 at horizon

    glm::vec3 base = night * (1.0f - day_t) + noon * day_t;
    L.sun_color    = base * (1.0f - warm_t) + sunset * warm_t;

    // Intensity (lux): noon ~110000, sunset ~6000, deep night ~0.
    // We deliberately push night to ZERO so the per-instance point lights
    // (windows, lamps, torches) become the dominant light source.
    float full_day_lux = 110000.0f;
    float sunset_lux   = 6000.0f;
    float night_lux    = 0.0f;
    float lux = night_lux * (1.0f - day_t) + full_day_lux * day_t;
    lux = lux * (1.0f - warm_t) + sunset_lux * warm_t;
    L.sun_intensity = std::max(0.0f, lux);

    // Sky colour: drives the skybox.
    glm::vec3 sky_noon (0.50f, 0.70f, 0.95f);
    glm::vec3 sky_dusk (0.85f, 0.45f, 0.30f);
    glm::vec3 sky_night(0.02f, 0.04f, 0.10f);
    glm::vec3 sky_base = sky_night * (1.0f - day_t) + sky_noon * day_t;
    L.sky_color = sky_base * (1.0f - warm_t) + sky_dusk * warm_t;

    // Ambient: soft fill that follows the sky. Keeps things visible at
    // night without nuking the contrast that the point lights provide.
    L.ambient = L.sky_color * 0.18f;

    return L;
}

struct MoonLightingParams {
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
    float weight = 0.0f;
};

// There is no lunar calendar in the benchmark: use a full moon opposite the
// Sun on the same daily path. Its disk, illumination and cloud scattering must
// share this direction. Solar lux is weather-dependent and cannot determine
// whether the Moon is above the horizon (a dark noon storm is still daytime).
[[nodiscard]] inline MoonLightingParams moon_lighting(const LightingParams& lighting) {
    const float length_squared = glm::dot(lighting.sun_dir, lighting.sun_dir);
    if (!std::isfinite(length_squared) || length_squared <= 0.0f) return {};
    const glm::vec3 direction = -lighting.sun_dir / std::sqrt(length_squared);
    const float elevation = std::clamp(direction.z / 0.10f, 0.0f, 1.0f);
    return {direction, elevation * elevation * (3.0f - 2.0f * elevation)};
}

struct TonemapParams {
    float exposure       = 1.0f;
    float bloom_strength = render_tuning::lighting.bloom_reference_strength;
};

// Neutral per-frame telemetry returned by every backend. CPU fields measure
// main-thread submission stages; gpu_ms is Filament's completed whole-frame
// timestamp and can lag the submitted frame by a few frames. A negative GPU
// value means the driver has not completed a timestamped frame yet.
struct TraceCapabilities {
    bool hardware_timestamps = false;
    bool cpu_clock_calibrated = false;
    std::string backend;
    std::string reason = "GPU timestamp capabilities have not been reported yet";
    unsigned timestamp_valid_bits = 0;
    double timestamp_period_ns = 0;
};

namespace profile { class Recorder; struct Context; }

struct FrameTimings {
    double camera_ms       = 0.0;
    double scene_sync_ms   = 0.0;
    double feature_ms      = 0.0;
    double render_submit_ms = 0.0;
    double gpu_ms          = -1.0;
};

struct FrameResult {
    bool presented = false;
    FrameTimings timings;
};

// ── GPU-side mesh handle (opaque to the application) ────────────

struct GpuSurface {
    uint64_t    handle      = 0;  // Backend-specific opaque handle
    uint32_t    index_count = 0;
    std::string name;
    glm::vec3   bbox_min{0};      // local-space AABB of this surface only
    glm::vec3   bbox_max{0};
};

struct GpuMesh {
    std::string name;
    std::string path;          // Relative asset path (e.g. heaven/meshes/...)
                               // Used by the material resolver to disambiguate
                               // surface names that recur across meshes.
    std::vector<GpuSurface> surfaces;
    glm::vec3 bbox_min{0};
    glm::vec3 bbox_max{0};

    // Whether this mesh casts directional-sun shadows. Default true. Set false
    // for fast-spinning rotors (the airship's port_propeller): a real propeller
    // at speed motion-blurs into a faint translucent disc, but a per-frame
    // skinned shadow map instead sweeps a hard blade shadow across nearby walls
    // — a distracting "rotating blades on the wall" artifact, worst at a low sun
    // (time-of-day 16) when the blade's shadow is long. Dropping the cast is
    // closer to the real look than a hard sweeping shadow. See scene_rhi.cpp.
    bool cast_shadows = true;
};

// ── Material with opaque texture handles ────────────────────────

struct GpuMaterial {
    uint64_t diffuse        = 0;
    uint64_t normal         = 0;
    uint64_t specular       = 0;
    uint64_t detail_diffuse = 0;
    uint64_t detail_normal  = 0;
    uint64_t detail_specular = 0;
    uint64_t emission       = 0;
    uint64_t cubemap        = 0;
    uint64_t height         = 0;
    uint64_t tessellation   = 0;
    TextureSemantic diffuse_semantic = TextureSemantic::ScalarLinear;
    TextureSemantic detail_diffuse_semantic = TextureSemantic::ScalarLinear;
    float    alpha_ref      = 0.0f;
    bool     has_detail     = false;
    int      detail_mode    = 0;
    bool     two_sided      = false;
    bool     hidden         = false;
    // True when the surface is glazing (a window, airship glass, or
    // lab equipment), classified purely from the xml0-derived
    // material name / parent chain by `heaven::glass_ref::
    // is_glass_material` — the same name-based discipline as the
    // foliage auto-detection above. Drives the screen-space-
    // refraction feature's per-surface material-domain choice
    // nothing else reads it today.
    bool     glass          = false;
    // True when the specular slot points to a DEDICATED `_s`
    // texture (different from the diffuse). Heaven materials often
    // alias the diffuse map as the spec slot; in that case the
    // R channel is the diffuse luminance, which would falsely
    // classify pale stone as metallic. The lit shader checks this
    // flag and only enables the metal/roughness classification
    // when it is true.
    bool     dedicated_spec = false;
    // True when the material's parent chain inherits from
    // `mesh_paint_reflection_cube_base` (or its tessellation
    // sibling) — Heaven's marker for "uses cubemap reflections".
    // The lit shader uses this as the authoritative metallic flag
    // instead of guessing from the spec map. False means matte
    // dielectric: plaster, stone, wood, fabric.
    bool     is_metallic = false;
    // Whether surfaces with this material cast the directional sun shadow.
    // Default true. Set false for thin rope/cable/rigging materials (airship
    // rigging, bridge/pier/island ropes): a sub-texel-thin caster in the sun
    // shadow map at a low, grazing sun (time-of-day 16) produces an aliased,
    // light-leaking streak that swims across nearby walls rather than a clean
    // thin shadow — confirmed by the artifact vanishing at night when the sun
    // shadow is off. The geometry still renders and still RECEIVES shadows; it
    // just stops casting. Honored per-surface by SceneBuilder, which splits the
    // non-casting surfaces of a mesh into their own renderable.
    bool     casts_shadow = true;
    float    detail_transform[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    float    detail_strength[3] = {0.5f, 0.5f, 0.5f};
    bool     geometry_displacement = false;
    // True only for policies whose visible shader should derive its base
    // normal from the displaced position. Depth materials deliberately ignore
    // this shading-only control and keep identical displacement geometry.
    bool     displaced_surface_normal = false;
    // True only for the exact village-gravel surfaces selected by the shared
    // displacement policy. It enables ground-specific normal, dry-response,
    // and indirect cavity tuning without changing unrelated lit materials.
    bool     ground_relief_shading = false;
    float    tess_length = 0.0f;
    float    tess_power = 2.0f;
    float    tess_distance = 10.0f;
    float    tess_threshold = 0.0f;
    float    tess_attenuation = 2.0f;
    float    tess_factor = 1.0f;
    float    tess_scale = 0.5f;

    // Per-material UV transform (applied in the shader to UV0
    // before sampling base/normal/spec/height). Mirrors Heaven's
    // `base_transform` parameter — see MaterialDef::base_transform
    // for the rationale. Identity is `{1,1,0,0}`.
    float    base_transform[4]   = {1.0f, 1.0f, 0.0f, 0.0f};

    // Emission tint and overall scale from the.mat file. These
    // multiply the sampled emission map and let each surface have
    // its own authored glow colour (cream balloon, warm window).
    float    emission_color[3]   = {1.0f, 1.0f, 1.0f};
    float    emission_scale      = 1.0f;
    // Synthetic-light eligibility, independent of the retained texture handle
    // and shader emission parameters. Core defaults are commonly bound even
    // when the material explicitly disables emission.
    bool     emission_light_source = false;

    // xml0-derived material identity used by the data-driven
    // Material_Model_Selector. `material_name`
    // is the resolved material name; `parent_chain` is its ordered
    // `parent="..."` ancestry walked from the material database. Both are
    // populated at resolve time (alongside the glass classification, which
    // walks the same chain) so MaterialInstanceCache::get_or_create can run
    // the pure selector (engine/material_model_ref.h) without re-reading the
    // database. Empty when the material was resolved purely by texture
    // auto-discovery (no parent chain); the selector then keys off the
    // surface/material name alone.
    std::string              material_name;
    std::vector<std::string> parent_chain;
};

// ── Material resolver callback ──────────────────────────────────

// The resolver receives BOTH the mesh path (e.g. "heaven/meshes/buildings/
// weathercock.smesh") and the surface name. The mesh path is needed
// because surface names like "Object04" appear in multiple meshes and
// resolve to different materials per mesh — passing only the surface
// name caused weathercock to inherit airship_cannons' rigging texture.
using MaterialResolver = std::function<GpuMaterial(const std::string& mesh_path,
                                                    const std::string& surface_name)>;

// ── RHI Backend Interface ───────────────────────────────────────

struct RhiScene;

class IRhiBackend {
public:
    virtual void set_trace_sink(profile::Recorder* /*recorder*/) {}
    virtual void set_trace_frame_context(const profile::Context& /*context*/) {}
    [[nodiscard]] virtual TraceCapabilities trace_capabilities() const { return {}; }
    virtual ~IRhiBackend() = default;

    // No copy, no move (polymorphic, heap-allocated)
    IRhiBackend(const IRhiBackend&) = delete;
    IRhiBackend& operator=(const IRhiBackend&) = delete;

    // ── Identification ──────────────────────────────────────────

    [[nodiscard]] virtual GraphicsApi api() const noexcept = 0;
    [[nodiscard]] virtual std::string device_name() const = 0;
    [[nodiscard]] virtual std::string api_version() const = 0;
    [[nodiscard]] virtual RendererFeatureStats renderer_feature_stats() const = 0;

    // ── Resource management ─────────────────────────────────────

    //Upload a CPU-side mesh to GPU memory.
    [[nodiscard]] virtual GpuMesh upload_mesh(const Mesh& mesh) = 0;

    //Upload a CPU-side skinned (ms08) mesh in its REST pose for GPU
    //skinning. The vertex buffer additionally carries BONE_INDICES /
    //BONE_WEIGHTS attributes and each surface is marked skinned with the
    //mesh's bone count so the scene builder can enable native skinning on
    //its renderable and the per-frame bone update can target it. Used by the
    //scene loader for Animated_Objects when GPU skinning is enabled (it
    //keeps the rest-pose positions, so a per-object CPU fallback can still
    //re-upload through update_mesh_vertices). When GPU skinning is disabled
    //the loader uses upload_mesh on the bind-pose vertices instead.
    [[nodiscard]] virtual GpuMesh upload_skinned(const SkinnedMesh& mesh) = 0;

    // Retire a complete uploaded mesh after all renderables/depth proxies have
    // stopped borrowing its buffers. Startup tessellation has no borrowers yet.
    // Released surface handles become invalid; repeated/unknown releases are safe.
    // CPU-only backends own no GPU resources and keep the default no-op.
    virtual void release_mesh(const GpuMesh& /*mesh*/) {}

    //Load a DDS texture from disk. Returns an opaque handle (0 = failed).
    //Results are cached by path.
    [[nodiscard]] virtual uint64_t load_texture(
        const std::string& path,
        TextureSemantic semantic = TextureSemantic::ScalarLinear) = 0;

    //Initialize all shader pipelines and framebuffers.
    [[nodiscard]] virtual Result<void> init_pipelines(const AppConfig& config) = 0;

    // Rebuild every GPU object that can retain a texture or material-instance
    // reference after a Reload setting changes.  The default is sufficient for
    // headless backends; the Filament backend clears persistent renderables,
    // feature resources, material instances, and its texture cache in lifetime
    // order before applying the new configuration.
    [[nodiscard]] virtual Result<void>
    reconfigure_for_reload(const AppConfig& /*config*/) { return {}; }

    // Startup/reload only, before renderables borrow newly uploaded meshes.
    [[nodiscard]] virtual Result<void> prepare_static_displacement(
        RhiScene& /*scene*/, const MaterialResolver& /*resolve*/,
        const LoadingProgressCallback& /*progress*/ = {}) { return {}; }

    // Borrow the application-owned logical asset context. It is established
    // before pipeline initialization and outlives the backend.
    virtual void set_asset_context(PathResolverRef /*paths*/,
                                   AssetFileSystemRef /*assets*/) {}

    // Explicit procedural foliage roots, consumed by the normal grass modules.
    [[nodiscard]] virtual Result<void> set_render_test_ground(
        std::size_t /*instance_index*/, glm::vec2 /*local_min*/,
        glm::vec2 /*local_max*/, float /*local_z*/) { return {}; }

    // Owner-thread only: draw startup/reload progress while a CPU worker prepares
    // the scene. A progress fraction in [0,1] is determinate; negative/non-finite
    // values mean unknown progress. The first ordinary scene frame clears it.
    virtual void show_loading_screen(const std::string& /*data_dir*/,
                                     std::string_view /*status*/,
                                     int /*viewport_w*/,
                                     int /*viewport_h*/,
                                     float /*progress*/ = -1.0f) {}

    // ── Frame rendering ─────────────────────────────────────────

    //Render a complete frame: shadow → scene → bloom → tonemap → present.
    //`weather` rides next to `lighting` (read-only, by reference): the
    //backend threads it into FrameContext::weather and derives the
    //fog-density override from it, exactly as it consumes `lighting`.
    virtual FrameResult render_frame(
        std::span<const Instance> instances,
        std::span<const int> dirty_instance_indices,
        std::span<const GpuMesh> meshes,
        const MaterialResolver& resolve_material,
        const Camera& camera,
        const LightingParams& lighting,
        const WeatherState& weather,
        const TonemapParams& tonemap,
        int viewport_w, int viewport_h) = 0;

    //Swap buffers / present the swapchain image.
    virtual void present() = 0;

    //Update vertex data for an existing mesh surface (for CPU skinning).
    //The vertices must have the same count as the original upload; a
    //mismatched count is rejected with an Error rather than re-uploaded.
    //@param surface_handle  The opaque handle from GpuSurface
    //@param vertices        New vertex data
    //@param vertex_count    Number of vertices (must match original)
    virtual Result<void> update_mesh_vertices(uint64_t surface_handle,
                                      const Vertex* vertices,
                                      uint32_t vertex_count) = 0;

    //Push the per-frame bone matrices for an Animated_Object (GPU skinning).
    //`mesh_index` selects the GpuMesh (an Animated_Object uploaded via
    //upload_skinned and built with skinning enabled in the scene builder);
    //`bone_matrices` is one transform per bone for the interpolated frame
    //(`bone_world(frame) * inverse(bone_world(bind))`), sized to the mesh's
    //bone count. The backend routes the matrices to every renderable of that
    //object (the caster + non-caster split means up to two renderables) and
    //updates ONLY their bone transforms — it does not rebuild the renderable,
    //vertex buffer, or index buffer. The default is a no-op so
    //backends without GPU skinning (the null backend) need no override; an
    //unknown mesh index is a safe no-op.
    virtual Result<void> update_skinned_bones(
        int /*mesh_index*/, std::span<const glm::mat4> /*bone_matrices*/) { return {}; }

    //Whether the Animated_Object at `mesh_index` is actually being GPU-skinned
    //this run. Lets the animation loop pick the per-object path:
    //- `std::nullopt` while the persistent scene has NOT been built yet (the
    //decision is not yet knowable — the caller should defer skinning that
    //frame; the renderable, if any, shows its rest pose);
    //- `true`  when the scene builder built a skinning-enabled renderable for
    //the object (its skinned upload took effect);
    //- `false` when GPU skinning was requested but did NOT take effect for
    //this object (e.g. bone count over Filament's limit, or a skinned
    //upload/enable failure), so the caller MUST drive it through the
    //CPU_Skinning_Path instead.
    //The default is `std::nullopt` so backends without GPU skinning (the null
    //backend) never resolve and are unaffected; an unknown mesh index on a
    //backend with a built scene reports `false`.
    [[nodiscard]] virtual std::optional<bool> gpu_skinning_active(
        int /*mesh_index*/) const { return std::nullopt; }

    //Provide particle emitters discovered during scene loading.
    //Backends that support particles will spawn and render them on
    //every subsequent render_frame call. Default is a no-op so
    //backends without particle support don't break.
    virtual void set_particle_emitters(std::span<const ParticleEmitter> /*emitters*/) {}

    //Provide world-authored scene lights (lamp posts, accent lights)
    //discovered during scene loading. The backend creates persistent
    //light entities once. Lights tagged with a `floating_group >= 0`
    //move with their parent island; the backend should expect a
    //follow-up `update_floating_lights()` call each frame to refresh
    //their world position.
    virtual void set_scene_lights(std::span<const SceneLight> /*lights*/) {}

    //Update the per-frame Z offset for each floating-island group.
    //`group_offsets` is a list of (group, z_offset) pairs — every
    //scene light whose `floating_group` matches will be translated
    //by `(0, 0, z_offset)` from its base world position before this
    //frame's render. Empty span ⇒ all lights stay at their original
    //authored positions. Default is a no-op.
    virtual void update_floating_lights(
        std::span<const std::pair<int, float>> /*group_offsets*/) {}

    //Per-frame wind debug visualisation (cloth simulation).

    //Each entry encodes one arrow to render in WORLD space at
    //`origin`, pointing along `direction` (already a 3-vector
    //in world coordinates) with length proportional to its
    //magnitude. Backends that have no debug-line capability
    //can ignore this entirely; the default is a no-op.
    struct WindArrow {
        glm::vec3 origin;
        glm::vec3 direction;   // not normalised; length = visual scale
    };
    virtual void set_wind_debug_arrows(
        std::span<const WindArrow> /*arrows*/) {}

    // ── Runtime feature toggle ──────────────────────────────────
    // Enable or disable a registered render feature by name at runtime.
    // The backend finds the feature whose name() matches `name` and drives
    // its IRenderFeature::set_enabled(), firing the feature's
    // on_enabled_changed() add/remove path so the toggle takes effect on the
    // next frame without a scene/pipeline rebuild. A name that matches no
    // registered feature (or a null name) is a safe no-op, so callers can map
    // configuration toggles to feature names without first checking whether a
    // feature exists. Backends without render features default to a no-op.
    virtual void set_feature_enabled(const char* /*name*/, bool /*on*/) {}

    // Update the live physical-camera controls without rebuilding pipelines.
    // The application owns the active UI configuration; render backends that
    // cache startup configuration must override this so aperture, shutter, ISO,
    // and the enable toggle take effect on the next frame.
    virtual void set_camera_config(
        bool /*enabled*/, const PhysicalCameraConfig& /*config*/) {}

    // Copy only the shadow/reflection controls advertised as Live. Backends
    // retain their existing reload-only material and post-process settings.
    virtual void set_live_render_config(const FeatureToggles& /*features*/,
                                        const QualityKnobs& /*quality*/) {}

    // A scored benchmark must render a stable amount of GPU work. Backends use
    // this signal to disable adaptive resolution for the measurement interval,
    // then restore the configured runtime behavior when the interval ends.
    // Default no-op for backends without adaptive rendering.
    virtual void set_benchmark_measurement(bool /*enabled*/) {}

    // Finish all GPU work submitted during the active benchmark interval and
    // return each completed whole-frame GPU duration exactly once. The
    // Filament backend uses its asynchronous frame-history stream and filters
    // by submission timestamp; non-timestamped backends return no samples.
    [[nodiscard]] virtual std::vector<double>
    finalize_benchmark_gpu_times() { return {}; }

    // Capture the next accepted, fully composed frame. SDR writes a top-down
    // P6 RGB image. HDR writes a diagnostic SDR preview and a sibling .pq.pfm
    // containing float PQ Rec.2020 before swap-chain quantization. Skipped
    // frames leave the request pending; completion reports readback / file errors.
    [[nodiscard]] virtual Result<void> request_screenshot(
        const std::filesystem::path& /*path*/) {
        return std::unexpected(Error("Screenshot capture is unsupported by this backend"));
    }
    [[nodiscard]] virtual Result<bool> screenshot_complete() const { return false; }

    // Audit-only simulation / shader time in seconds. nullopt restores the
    // normal wall-clock progression; supplied times must be finite, >= 0.
    virtual void set_capture_time(std::optional<float> /*seconds*/) {}

    // Most recently applied internal render scale, for capture diagnostics.
    [[nodiscard]] virtual glm::vec2 render_resolution_scale() const { return {1.0f, 1.0f}; }

    // ── Window / swap-chain resize ──────────────────────────────
    // Recreate the swap chain for a new window drawable size after the
    // Main_Loop resizes the OS window (e.g. an in-UI resolution change). The
    // per-frame viewport is supplied to render_frame separately; this exists so
    // the backend can rebuild any size-dependent swap-chain state. Default
    // no-op for backends that need no explicit resize.
    virtual void resize(int /*width*/, int /*height*/) {}

    // ── Overlay (Dear ImGui via filagui) ────────────────────────
    // Initialise the ImGui context + filagui ImGuiHelper. Called once after
    // init_pipelines(). No-op default for backends without overlay support.
    virtual void overlay_init(const AppConfig& /*config*/) {}

    // Begin an ImGui frame: set display size, delta time, and pump queued
    // input state. Called once per frame before widgets are built.
    virtual void overlay_begin_frame(float /*dt*/, int /*w*/, int /*h*/) {}

    // Forward one SDL event to ImGui. Returns true if ImGui consumed it (mouse
    // over a widget / keyboard while a text field is focused), so the caller
    // can suppress camera/app handling.
    [[nodiscard]] virtual bool overlay_handle_event(const void* /*sdl_event*/) { return false; }

    // The backend draws the ImGui view on top of the scene during the next
    // render_frame. The widget-building callback runs between ImGui NewFrame
    // and Render.
    using OverlayDraw = std::function<void()>;
    // Derived backends take ownership of the callback for later frames.
    // NOLINTNEXTLINE(performance-unnecessary-value-param)
    virtual void overlay_set_draw([[maybe_unused]] OverlayDraw build_widgets) {}

    // ── Frame pacing  ───────────────────────────
    // True when vsync / frame-rate cap is disabled (IMMEDIATE present, no
    // pacing). The Filament backend reports the state it configured in its
    // constructor.
    [[nodiscard]] virtual bool frame_pacing_disabled() const { return false; }

    // ── GPU / render-API info for the Result_Report  ─
    // Raw GPU descriptors from the backend's ACTIVE rendering backend (its
    // graphics device). Defaulted to all-undetermined so non-Filament/headless
    // backends report the Unknown_Value_Placeholder via the pure RunInfo
    // formatter. Application code stays GPU-API-agnostic: it never inspects
    // Filament/Vulkan types, only this neutral struct (AGENTS.md).
    struct GpuInfoRaw {
        std::string model_name;        // graphics device model
        long        vram_mb      = -1; // video memory MB, <0 ⇒ undetermined
        std::string driver_name;       // active driver name or GL vendor
        std::string driver_info;       // unmodified vendor detail/GL_VERSION
        std::string api_version;       // active device's graphics API version
        DriverFamily driver_family = DriverFamily::Unknown;
        std::string driver_version;    // known implementation format; empty if unknown
        uint32_t    vendor_id     = 0; // PCI/Khronos vendor identifier
        uint32_t    device_id     = 0; // vendor device identifier
        int         pci_domain    = -1;
        int         pci_bus       = -1;
        int         pci_device    = -1;
        int         pci_function  = -1;
    };
    [[nodiscard]] virtual GpuInfoRaw gpu_info() const { return {}; }

    // Live GPU telemetry. All values use neutral units and negative sentinels
    // so the UI can distinguish unsupported driver data from a real zero.
    // Vulkan's memory-budget extension supplies the current process estimate;
    // Linux DRM/HWMON may additionally supply device-wide load and sensors.
    struct GpuTelemetry {
        double utilization_percent       = -1.0;
        double memory_utilization_percent = -1.0;
        double process_vram_used_mb       = -1.0;
        double process_vram_budget_mb     = -1.0;
        double device_vram_used_mb        = -1.0;
        double device_vram_total_mb       = -1.0;
        double temperature_c              = -1.0;
        double power_w                    = -1.0;
        double core_clock_mhz             = -1.0;
        double memory_clock_mhz           = -1.0;
        double fan_rpm                    = -1.0;
    };
    [[nodiscard]] virtual GpuTelemetry gpu_telemetry() const { return {}; }

    // Name of the graphics API the active backend is CURRENTLY using — e.g.
    // "Vulkan" or "OpenGL" — matching the backend in use for the run.
    // Distinct from the existing api()/api_version() identification strings,
    // which report the fixed RHI family ("Filament"); this reflects the live
    // driver path (Filament Vulkan, or the OpenGL fallback). Empty ⇒ the
    // formatter substitutes the Unknown_Value_Placeholder.
    [[nodiscard]] virtual std::string render_api_name() const { return {}; }

    // ── Per-frame discipline counters  ────────
    // Debug-only instrumentation that lets a gated integration check confirm
    // the per-frame path mutates ONLY transforms/animation uniforms and never
    // re-runs the one-time builders. Each counter records how many times the
    // backend ran a given build/sync site:
    //   - scene_builds      → scene_builder_->build()      (one-time)
    //   - emission_spawns   → light_spawner_->spawn_emission() (one-time)
    //   - grass_generations → grass blade generation        (one-time/deferred)
    //   - transform_updates → scene_builder_->update_transforms() (per-frame)
    //   - rain_streak_builds→ RainFeature::build_rain()      (one-time/deferred)
    //   - splash_builds     → RainFeature::build_splashes()  (one-time/deferred)
    // The default is all-zero so non-Filament backends (and the headless test
    // target) are unaffected.

    // rain_streak_builds / splash_builds mirror grass_generations: the rain
    // streak and splash geometry is built once on the first active frame and
    // animated through uniforms thereafter, so a gated smoke case can confirm
    // each builder ran exactly once (weather spec). Like
    // grass_generations they are owned by the feature (RainFeature) and sourced
    // on demand in debug_build_counters().
    struct BackendBuildCounters {
        uint64_t scene_builds       = 0;  // times scene_builder_->build() ran
        uint64_t emission_spawns    = 0;  // times spawn_emission() ran
        uint64_t grass_generations  = 0;  // times grass blades were generated
        uint64_t foliage_generations = 0; // times massive-foliage geometry was generated
        uint64_t transform_updates  = 0;  // times update_transforms() ran (per-frame)
        uint64_t rain_streak_builds = 0;  // times RainFeature::build_rain() ran
        uint64_t splash_builds      = 0;  // times RainFeature::build_splashes() ran
        // Times the per-frame weather change path re-applied the
        // weather-dependent Filament state (lighting/bloom + distance fog +
        // post-process) AFTER the first-frame baseline seed. Bumped only when
        // the current WeatherState differs from the last applied one and the
        // re-apply succeeds, so an unchanged WeatherState leaves it flat — the
        // observable a gated smoke case asserts for the re-apply discipline
        // The first-frame seed is NOT counted (it is
        // not a re-application); mirrors rain_streak_builds / splash_builds.
        uint64_t weather_reapplies  = 0;
    };
    [[nodiscard]] virtual BackendBuildCounters debug_build_counters() const { return {}; }

protected:
    IRhiBackend() = default;
};

// ── Window creation ─────────────────────────────────────────────

// Query physical pixels using the API selected when the window was created.
void get_drawable_size(SDL_Window* window, int& width, int& height);

struct WindowHandle {
    SDL_Window* window      = nullptr;
    void*       api_context = nullptr; // unused since the GL backend
                                       // was removed; kept so existing
                                       // move/copy semantics don't
                                       // change. Will go away once
                                       // we audit all callers.
    int         width       = 0;
    int         height      = 0;

    ~WindowHandle();
    WindowHandle() = default;
    WindowHandle(WindowHandle&& o) noexcept;
    WindowHandle& operator=(WindowHandle&& o) noexcept;
    WindowHandle(const WindowHandle&) = delete;
    WindowHandle& operator=(const WindowHandle&) = delete;

    void set_title(std::string_view title);
    void set_drawable_size(int requested_width, int requested_height);
};

//Create an SDL window configured for Filament. The `api`
//parameter is currently ignored (only Filament is supported);
//it remains in the signature so adding a new backend later
//doesn't ripple through call sites.
[[nodiscard]] Result<WindowHandle> create_window(
    const AppConfig& config, GraphicsApi api,
    std::string_view title = "HeavenBench");

// ── Backend factory ─────────────────────────────────────────────

//Create a rendering backend for the specified API.
[[nodiscard]] Result<std::unique_ptr<IRhiBackend>> create_backend(
    GraphicsApi api, WindowHandle& window, const AppConfig& config);

} // namespace heaven

#endif // HEAVENBENCH_SRC_RHI_RHI_H_
