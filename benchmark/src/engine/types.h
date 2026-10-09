/*
 * HeavenBench — Core type definitions
 *
 * Shared value types used across the engine. No OpenGL dependencies here —
 * these are pure data structures for the CPU side.
 */
#ifndef HEAVENBENCH_SRC_ENGINE_TYPES_H_
#define HEAVENBENCH_SRC_ENGINE_TYPES_H_

#include "engine/number_parse.h"
#include "render_tuning.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cctype>
#include <algorithm>
#include <utility>
#include <string>
#include <vector>
#include <map>
#include <expected>
#include <optional>
#include <string_view>
#include <span>
#include <format>

namespace heaven {

// ── Error handling ──────────────────────────────────────────────

struct Error {
    std::string message;

    Error(std::string msg) : message(std::move(msg)) {}
};

template <typename T>
using Result = std::expected<T, Error>;

// ── Mesh data (CPU side) ────────────────────────────────────────

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float u1, v1;
    // Length of the original barycentrically-interpolated normal before it is
    // normalized for shading. Unigine displaces along that unnormalized TES
    // normal; the Filament vertex shader restores the magnitude from CUSTOM0.
    float displacement_normal_scale = 1.0f;
    float tx, ty, tz, tw; // tangent (xyz) + bitangent sign (w)
};

struct Surface {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

struct Mesh {
    std::string name;
    std::string path;
    std::vector<Surface> surfaces;
    glm::vec3 bbox_min{0};  // local-space bounding box from mi08 header
    glm::vec3 bbox_max{0};
};

// ── Material definitions (from xml0 parsing) ────────────────────

struct MaterialDef {
    std::string name;
    std::string parent;
    std::map<std::string, std::string> textures; // suffix (_d, _dd, _n, _s,...) → path
    std::map<std::string, std::string> texture_formats; // authored format, e.g. "srgb"
    // Authored address mode by slot. Missing means Unigine's material default
    // (repeat); keeping the unset state distinct lets inventory generation
    // record provenance while still applying that default deterministically.
    std::map<std::string, std::string> texture_wraps;
    int  alpha_test    = -1;  // -1=unset, 0=off, 1=on
    int  detail_mode   = -1;  // -1=unset, 0=disabled, 1+=overlay/multiply modes
    int  two_sided     = -1;  // -1=unset, 0=off, 1=on
    int  hidden        = -1;  // -1=unset, 0=visible, 1=hidden (depth_mask="0")
    float detail_transform[4] = {0, 0, 0, 0}; // {scale_u, scale_v, offset_u, offset_v}
    float detail_diffuse  = -1.0f;
    float detail_normal   = -1.0f;
    float detail_specular = -1.0f;

    // Per-material UV0 transform. Heaven's mesh shader applies
    //   uv = uv * base_transform.xy + base_transform.zw
    // before sampling diffuse / normal / specular / height. Many
    // materials author non-default values here (e.g. the ruins
    // merlon material uses vec4(2,1,-1,0) so the merlon texture
    // wraps twice along U with a -1 offset; airship rigging uses
    // vec4(2,1,0,0); fort_stucko uses vec4(0.6,0.6,0,0)). Without
    // applying it, those surfaces render the wrong region of the
    // diffuse atlas. Default is identity.
    float base_transform[4] = {1.0f, 1.0f, 0.0f, 0.0f};

    // Tessellation parameters from the original engine. The shader formula is:
    //   offset = pow(saturate(1 - (dist - threshold)/distance), power)
    //         * (height_lod - 127/255) * scale;
    //   edge_factor *= saturate(edge_length / tessellation_length);
    //   vertex.xyz += normal * offset;
    // CPU topology generation consumes density/edge parameters; the vertex
    // shader consumes the displacement and distance-fade parameters.
    float tess_length      = -1.0f; // target edge length; -1 = unset
    float tess_power        = -1.0f;
    float tess_scale_param  = -1.0f; // height displacement amplitude
    float tess_factor       = -1.0f;
    float tess_distance_far = -1.0f;
    float tess_threshold    = -1.0f;
    float tess_attenuation  = -1.0f;
    bool tess_length_set      = false;
    bool tess_power_set       = false;
    bool tess_scale_set       = false;
    bool tess_factor_set      = false;
    bool tess_distance_set    = false;
    bool tess_threshold_set   = false;
    bool tess_attenuation_set = false;

    // Emission parameters from the.mat file.
    //   emission_color: per-channel multiplier on the emission map
    //   emission_scale: overall brightness multiplier
    // We feed both to the lit material so each emissive surface gets
    // its authored tone (cream balloon vs warm yellow window).
    float emission_color[3] = {1.0f, 1.0f, 1.0f};
    float emission_scale    = -1.0f; // -1 = unset
    // Authored emission state: -1 inherits, 0 disables, 1 enables. A bound
    // engine default texture alone does not make a surface a light source.
    int emission_enabled = -1;

    // Material class as inferred from the parent chain. Heaven's
    // base shaders are organised so that any material descending
    // from `mesh_paint_reflection_cube_base` or its tessellation
    // sibling renders with cubemap reflections (metal pipeline);
    // everything descending from plain `mesh_base` /
    // `mesh_tessellation_base` is matte dielectric. Without this
    // distinction the lit shader's spec-map heuristic flags any
    // bright stucco highlight as metallic, leaving plaster walls
    // looking like polished plastic.
    bool is_metallic = false;

    // True when the material descends from a `*_tessellation*` base in
    // the engine's shader library. ONLY these materials carry the
    // TCS/TES (control/evaluate) stages and therefore geometrically
    // displace their surface; plain `*_base` materials that ship a
    // `parallax`/`height` texture use it for parallax-occlusion mapping
    // in the fragment shader only and must NOT be tessellated. CPU
    // tessellation gates on this flag (plus tess_length > 0) to mirror
    // the engine, instead of displacing every surface that happens to
    // have a height texture.
    bool is_tessellation = false;
};

// ── Scene node entry (from xml0 node/world parsing) ─────────────

struct NodeEntry {
    std::string mesh_path;
    std::string parent_name; // name of the parent node (for bridge chain identification)
    float transform[16];
    int floating_group = -1; // -1 = not floating, 0+ = group index
    // When this entry was constructed from an `ObjectMeshDynamic`
    // node we keep the path of the source.node so cloth_sim can
    // re-parse it for the BodyCloth blob (positions, constraints,
    // anchor list, sibling PhysicalWind). Empty for normal meshes.
    std::string cloth_source_node;
    // True when this entry is a descendant of the heaven.world
    // `stone_noise` WorldExpression. The original benchmark drives
    // each stone's Z with a per-position seeded sine wave (see the
    // expression body in the script: `position.z = sin(time*0.5 +
    // seed) * 6 * sin(seed)` where `seed = pos.x*13 + pos.y`). The
    // stones float independently of each other and of the four
    // labelled islands, which is why they need their own animation
    // path rather than sharing a `floating_group`.
    bool is_stone_anim = false;
    // Authored *world* position of the stone's parent ObjectDummy
    // (the node the original script iterates over). The seed and
    // the per-frame Z replacement are computed against this
    // position, NOT the rendered mesh's position — the rendered
    // mesh sits at a per-instance Z offset above/below the dummy
    // (e.g. stone_24 is 31m below its dummy), so re-running the
    // script on the mesh position would collapse all rocks to
    // ±5m around z=0.
    float stone_dummy_x = 0.0f;
    float stone_dummy_y = 0.0f;
    float stone_dummy_z = 0.0f;
};

// ── Scene light (from LightOmni / LightSpot in.node /.world) ──

// Mirrors the parameters Heaven authors on each light node. Distance
// units, colour space and the radius semantics follow the original
// engine: `radius` is a hard cutoff distance and the visual falloff
// inside it is `saturate(1 - d²/r²)²`. We map this to Filament's
// `LightManager::falloff` (which is also a cutoff distance) plus a
// hand-tuned intensity in lumens.

// `multiplier` is the original brightness scalar; combined with our
// own absolute lumens scale it produces the final `LightManager`
// intensity. Spot cone angles are stored in radians.

// When `floating_group >= 0`, the light's position must be re-derived
// every frame from the floating-island Z animation — `local_pos` is
// the position relative to the island's base transform and the
// backend uses the tracked island translation to recompute world
// position before each frame.
enum class SceneLightKind : uint8_t {
    Omni = 0,
    Spot = 1,
};

struct SceneLight {
    SceneLightKind kind            = SceneLightKind::Omni;
    glm::vec3       world_pos      {0.0f};   // world-space position (initial)
    glm::vec3       local_pos      {0.0f};   // mesh-local pos relative to floating-group base
    glm::vec3       direction      {0.0f, 0.0f, -1.0f}; // world-space, normalised
    glm::vec3       color          {1.0f};   // linear-ish RGB
    float           multiplier     = 1.0f;   // authored brightness scalar
    float           radius         = 8.0f;   // hard cutoff distance (= Filament falloff)
    float           inner_cone_rad = 0.6f;   // SpotLight inner cone half-angle (radians)
    float           outer_cone_rad = 0.9f;   // SpotLight outer cone half-angle (radians)
    int             floating_group = -1;     // -1 = static, 0+ = move with this island group
    std::string     name;                    // diagnostic only
    // Authored shadow=0 is significant for lights inside opaque lamp housings.
    // A global local-shadow budget must not promote those lights to casters.
    bool            shadow_eligible = true;
};

// ── Particle emitter (from ObjectParticles in.node files) ──────

// Mirrors the Unigine ObjectParticles parameters discovered in
// extracted/heaven/heaven/nodes/{smoke,torch_light,flies}.node.
// Distributions are uniform over [mean - spread/2, mean + spread/2]
// in the original engine; we replicate that.
struct ParticleEmitter {
    std::string name;          // "lab_smoke", "white_smoke", "torch_fire",...
    std::string material_name; // "fire_smoke", "fire_smoke_white", "fire_flame",...
    glm::mat4   transform{1.0f}; // world-space transform of the emitter

    int   emitter_type    = 0;   // 0 = box (default), 1 = sphere/point, 2 = mesh-following
    glm::vec3 emitter_size {0.0f}; // box half-extents (x,y) plus z dimension
    bool  emitter_enabled = true;

    float spawn_rate    = 5.0f;  // particles per second

    // Per-particle distributions (mean, spread) — actual = mean + (rand-0.5)*spread
    float life_mean     = 4.0f;  // seconds
    float life_spread   = 1.0f;
    float velocity_mean = 1.0f;  // initial speed along emitter +Z
    float velocity_spread = 0.5f;
    float angle_mean    = 0.0f;  // initial cone angle (radians)
    float angle_spread  = 0.0f;
    float rotation_mean = 0.0f;  // billboard roll (degrees in source — store as deg)
    float rotation_spread = 0.0f;
    float radius_mean   = 0.4f;  // initial billboard radius
    float radius_spread = 0.1f;
    float growth_mean   = 0.3f;  // radius growth per second
    float growth_spread = 0.1f;

    glm::vec3 gravity{0, 0, 0};  // constant acceleration on each particle
    float linear_damping = 0.0f; // velocity damping per second

    // Visibility
    float max_visible_distance = 1e8f;
    int   max_particles        = 64; // pool size cap

    // Floating-island parenting. When >= 0 the emitter (and any
    // associated lens-flare / firefly etc.) tracks the matching
    // group's per-frame Z animation offset. -1 = static.
    int   floating_group = -1;
};

// ── Scene instance (GPU-ready) ──────────────────────────────────

struct Instance {
    int mesh_index;
    glm::mat4 transform;
};

// ── Application configuration ───────────────────────────────────

// Tessellation parameters mirroring the original Heaven benchmark's
// Tessellation panel (Scale / Factor / Distance / Enable).
//   tess_enabled  — master on/off
//   tess_scale    — displacement strength multiplier
//   tess_factor   — scales each material's authored edge factor
//                   (1.0 = authored density)
//   tess_distance — scales each material's authored fade range
// Defaults reproduce Normal (1x). Named Moderate / Normal / Extreme levels
// set all three controls to 0.75 / 1 / 1.5; rendering presets supply base values.
struct TessellationConfig {
    bool  enabled  = true;
    float scale    = 1.0f;
    float factor   = 1.0f;
    float distance = 1.0f;
    // Optional writable archive override. Empty selects
    // <Data_Root>/cache/tessellation-v<N>.zip for a directory mount or
    // <asset-zip-parent>/cache/tessellation-v<N>.zip for a ZIP mount.
    std::string cache_file;
};

// Per-feature on/off switches for the renderer's optional visual
// modules and Filament's built-in post-processing passes.
// The original Heaven benchmark exposed equivalent toggles through
// its Quality / Environment UI panels; we drive them from the command
// line today (`--no-<feature>`) and will surface them in a settings
// UI in a later phase. Defaults match the shipping look.

// Note: `clouds` defaults OFF — the cloud-quad approach has a known
// sky seam (see docs/KNOWN_BUGS.md) and is dormant until the sky
// sphere supersedes it.
struct FeatureToggles {
    bool grass       = true;
    bool particles   = true;   // smoke / fire / glow / fireflies
    bool god_rays    = true;
    bool volumetric_fog = true;
    bool clouds      = false;
    bool sky         = true;   // procedural sky sphere
    bool lens_flares = true;
    bool heat_haze   = true;   // chimney refraction grab-pass
    bool rain        = true;   // falling precipitation + splashes (renders only while rain intensity > 0)
    bool wet_reflections = true; // wet-surface SSR + wetness shading
    bool bloom       = true;
    bool ambient_occlusion = true;
    bool taa         = true;
    bool depth_of_field = true; // authored playback or physical-camera DOF

    // ── Advanced-rendering feature set (advanced-rendering-features spec) ──
    // Tier 1 quick wins  and Tier 2 new techniques
    // By product decision ALL of these default ON so a
    // flagless run spends the GPU headroom on the full modern-realism look;
    // each is individually disablable with `--no-<feature>`. The
    // explicitly-all-disabled configuration still matches the Shipping_Look
    // within the ≤1/255 parity bar, which is defined over
    // the disabled state, not the default run.
    bool soft_shadows        = true;   // PCSS/DPCF soft directional shadows (--no-soft-shadows)
    bool contact_shadows     = true;   // screen-space contact shadows      (--no-contact-shadows)
    bool point_light_shadows = true;   // shadow-casting point/spot lights   (--no-point-light-shadows)
    bool msaa                = true;   // MSAA on top of TAA                 (--no-msaa)
    bool refraction          = true;   // screen-space glass refraction      (--no-refraction)
    bool reflections         = true;   // screen-space reflections (SSR)           (--no-reflections)
    bool volumetric_clouds   = true;   // raymarched volumetric clouds       (--no-volumetric-clouds)
    bool ssgi                = true;   // screen-space global illumination    (--no-ssgi)
    bool massive_foliage     = true;   // massive foliage / GPU-driven geom   (--no-massive-foliage)
    bool ground_relief       = true;   // village gravel relief                (--no-ground-relief)

    // ──  — advanced materials, shadows & color grading ──
    // (advanced-materials-and-grading spec). By product
    // decision the recommended subset of these capabilities now defaults ON so
    // a flagless run spends the GPU headroom on the best visual look; each of
    // the seven is individually disablable with `--no-<feature>` (and keeps a
    // bare `--<feature>` compat enable alias). The remaining three stay OFF
    // (opt-in via the bare `--<feature>` enable flag). The ≤1/255 Shipping_Look
    // parity contract now holds in the ALL-DISABLED configuration (every
    // `--no-<feature>` passed), not the default run. Tone-mapper selection
    // is an enum (ToneMapperSelection), not a bool, and stays ACES by
    // default; color grading stays default-off (neutral).

    // Default ON (disable via --no-<feature>):
    bool clear_coat          = true;   // clear-coat lobe                     (--no-clear-coat)
    bool anisotropic         = true;   // anisotropic specular                (--no-anisotropic)
    bool subsurface          = true;   // subsurface / translucency           (--no-subsurface)
    bool cloth               = true;   // cloth / sheen shading               (--no-cloth)
    bool transparent_shadows = true;   // transparent foliage shadows         (--no-transparent-shadows)
    bool colored_penumbra    = true;   // colored penumbra                    (--no-colored-penumbra)
    bool specular_aa         = true;   // specular anti-aliasing              (--no-specular-aa)
    // Default OFF (opt-in via --<feature>):
    bool color_grading       = false;  // full color grading                  (--color-grading)
    bool physical_camera     = false;  // physically-based camera             (--physical-camera)
    bool dynamic_resolution  = false;  // dynamic resolution + FSR            (--dynamic-resolution)

    // ── GPU instancing & skinning (gpu-instancing-and-skinning spec) ──
    // Both default ON and are read BEFORE the persistent scene is built,
    // mirroring the existing pre-build toggle discipline. Disabling either
    // (`--no-gpu-instancing` / `--no-gpu-skinning`) restores the exact current
    // code path so the all-disabled configuration is byte-parity with the
    // Shipping_Look. `gpu_instancing` groups repeated static meshes into
    // instanced renderables; `gpu_skinning` skins the four Animated_Objects on
    // the GPU instead of re-uploading CPU-skinned vertices each frame.
    bool gpu_instancing      = true;   // instanced repeated meshes         (--no-gpu-instancing)
    bool gpu_skinning        = true;   // GPU-skinned animated meshes        (--no-gpu-skinning)
};

// ── Advanced-rendering quality knobs (advanced-rendering-features spec) ──

// Ordered discrete quality level shared by the sample-/step-count knobs
// (soft-shadow penumbra, contact-shadow steps, cloud raymarch steps, SSGI
// samples). Each higher level maps to a strictly greater, bounded count via
// the pure mapping functions below
// The integer values are deliberately contiguous and ordered so the
// CLI can clamp an out-of-range request to the nearest valid level.
enum class QualityLevel : int { Low = 0, Medium = 1, High = 2 };

// Reproducible rendering/stress profiles. Custom is a detected state rather
// than a profile with its own values: changing any preset-owned field makes a
// configuration Custom while display, scene, weather and audio remain outside
// preset ownership.
enum class RenderPreset : int {
    Custom = 0,
    BalancedSystem,
    VisualUltra,
    GpuTorture,
    CpuTorture,
};

enum class MaterialWorkflow : int {
    MetallicRoughnessCompat = 0,
    SpecularGlossinessExperimental,
};

// Offline texture-set selection. The serialized identifiers are deliberately
// stable because they are shared by config files, the CLI, archive manifests,
// benchmark results, and generated texture-set reports.
enum class TextureQuality : int {
    Original = 0,
    High2K,
    Ultra4K,
};

[[nodiscard]] constexpr std::string_view
texture_quality_id(TextureQuality quality) noexcept {
    switch (quality) {
        case TextureQuality::Original: return "original";
        case TextureQuality::High2K:   return "2k";
        case TextureQuality::Ultra4K:  return "4k";
    }
    return "original";
}

[[nodiscard]] inline std::optional<TextureQuality>
parse_texture_quality(std::string_view value) noexcept {
    if (value == "original") return TextureQuality::Original;
    if (value == "2k") return TextureQuality::High2K;
    if (value == "4k") return TextureQuality::Ultra4K;
    return std::nullopt;
}

struct CpuStressConfig {
    bool  enabled          = false;
    int   workers          = 0;     // 0 = hardware_concurrency(), capped at 256
    float duration_seconds = 10.0f;
};

struct RendererFeatureStats {
    bool bloom_enabled = false;
    float bloom_strength = 0.0f;
    bool ibl_prefiltered = false;
    bool msaa_requested = false;
    bool msaa_active = false;
    bool custom_msaa_resolve = false;
    bool alpha_to_coverage = false;
    bool bent_normals = false;
    bool multi_bounce_ao = false;
    bool gpu_instancing = false;
    bool gpu_skinning = false;
    bool refraction_requested = false;
    bool refraction_active = false;
    int msaa_samples = 1;
    int sun_shadow_map_size = 0;
    int point_shadow_map_size = 0;
    uint64_t srgb_textures = 0;
    uint64_t scalar_textures = 0;
    uint64_t normal_textures = 0;
    uint64_t hdr_textures = 0;
    uint64_t texture_fallbacks = 0;
    uint64_t hires_textures = 0;
    uint64_t original_textures = 0;
    uint64_t texture_missing_variant_fallbacks = 0;
    uint64_t texture_unsupported_format_fallbacks = 0;
    uint64_t texture_invalid_variant_fallbacks = 0;
    uint64_t selected_texture_bytes = 0;
    uint32_t effective_texture_max_dimension = 0;
    uint64_t enabled_render_features = 0;
    uint64_t total_render_features = 0;
    uint64_t material_bindings = 0;
    uint64_t compatibility_workflow_surfaces = 0;
    uint64_t experimental_workflow_surfaces = 0;
    uint64_t compatibility_workflow_instances = 0;
    uint64_t experimental_workflow_instances = 0;
    MaterialWorkflow material_workflow_requested =
        MaterialWorkflow::MetallicRoughnessCompat;
    MaterialWorkflow material_workflow_active =
        MaterialWorkflow::MetallicRoughnessCompat;
    TextureQuality texture_quality_requested = TextureQuality::Original;
    TextureQuality texture_quality_active = TextureQuality::Original;
    QualityLevel ao_quality = QualityLevel::Medium;
};

// All per-feature quality/scale bounds for the advanced-rendering feature set,
// in one place as the single source of truth shared by the CLI parser
// (main.cpp / apply_quality_args) and the property tests — exactly the
// WeatherConfig precedent. Header-only, GLM-only, no GPU dependency, so the
// whole struct is unit/property testable on the host. Defaults sit at the
// Shipping_Look baseline so a flagless run (with look-changing features off)
// returns GPU cost toward baseline.
struct QualityKnobs {
    // soft-shadow penumbra sample count (≥3 ordered levels). The
    // level→count mapping is fixed (no configured bounds field) — see
    // soft_shadow_sample_count().
    QualityLevel soft_shadow_level    = QualityLevel::Medium;

    // contact-shadow ray-march step count (≥3 ordered levels), bounded
    // by [min, max] so the per-pixel cost is bounded at the highest level.
    QualityLevel contact_shadow_level = QualityLevel::Medium;
    int   contact_shadow_step_min     = 8;
    int   contact_shadow_step_max     = 32;

    // shadow-casting point/spot light budget in [0, max], max ≥ 16.
    // Each candidate is a POINT light, which renders a 6-face cube shadow map
    // every frame; the emission lights also bob with the floating islands, so
    // those faces re-render continuously over the heavily-tessellated scene.
    // This is the dominant per-frame GPU cost: measured from frames Filament
    // accepts, not main-loop spins. Three casters drop the scene to ~14 FPS
    // versus ~40 FPS with the feature off. Each caster costs roughly a
    // third of the frame. 2 keeps the feature on  — shadows on the two
    // brightest lights — at a noticeably more sustainable present rate; the
    // budget is still tunable up to the max via --point-light-quality for users
    // who want the original 3+ at the framerate cost.
    int   shadow_light_budget         = 2;
    int   shadow_light_budget_max     = 16;

    // MSAA sample count — accepted set EXACTLY {1,2,4} (reject, not
    // clamp, out-of-set values; handled by the MSAA helper in).
    int   msaa_samples                = 2;

    // Stock Filament post-processing quality ladders. These are kept separate
    // from the technique-specific controls above so presets can raise the
    // fullscreen cost without changing authored scene behaviour.
    QualityLevel ao_quality           = QualityLevel::Medium;
    QualityLevel bloom_quality        = QualityLevel::Low;
    QualityLevel dof_quality          = QualityLevel::Low;
    QualityLevel taa_quality          = QualityLevel::Low;

    // Independent directional and point/spot shadow resolutions. Point-light
    // resolution is deliberately not coupled to shadow_light_budget.
    int   sun_shadow_map_size         = 2048;
    int   point_shadow_map_size       = 256;

    // volumetric-cloud raymarch step count (≥3 ordered levels), bounded
    // by [min, max] with min ≥ 1.
    QualityLevel cloud_quality        = QualityLevel::Medium;
    int   cloud_step_min              = 16;   // ≥ 1
    int   cloud_step_max              = 128;

    // SSGI sample/ray count (≥3 ordered levels), bounded by a maximum.
    QualityLevel ssgi_quality         = QualityLevel::Medium;
    int   ssgi_sample_max             = 32;

    // Density scales the source-template blade budget, capped by
    // foliage_blade_max. Repeated island instances reuse each template, so
    // the rendered scene count can exceed this source budget.
    float foliage_density             = 1.0f;
    float foliage_density_max         = 1000000.0f / 466000.0f; // source-template budget
    int   foliage_blade_baseline      = 466000;
    int   foliage_blade_max           = 1000000;

    // ──  — advanced-materials-and-grading knobs ──
    // specular-AA strength expressed as an ordered quality level (≥3
    // levels, reusing clamp_quality_level / QualityLevel) whose variance and
    // threshold endpoints are bounded. spec_aa_strength(level) interpolates the
    // variance/threshold from the *_min endpoints (Low) to the *_max endpoints
    // (High); the *_max values are the bounded maximum strength the ladder can
    // reach. These endpoints are the single source of truth shared by
    // MaterialInstanceCache and the property tests.
    QualityLevel specular_aa_level    = QualityLevel::Medium;
    static constexpr float k_spec_aa_variance_min  = 0.0f;
    static constexpr float k_spec_aa_variance_max  = 0.5f; // bounded maximum strength
    static constexpr float k_spec_aa_threshold_min = 0.0f;
    static constexpr float k_spec_aa_threshold_max = 0.2f;

    // dynamic-resolution minimum render scale in (0, 1]. Filament's
    // frame-time controller scales the internal render target between
    // dynres_min_scale and 1.0; clamp_dynres_min_scale keeps the configured
    // value inside [k_dynres_min_scale_lo, k_dynres_min_scale_hi] with the lower
    // bound strictly > 0 and the upper bound ≤ 1. Bounds are
    // constexpr — the single source of truth shared with the CLI and tests.
    float dynres_min_scale            = 0.5f;
    static constexpr float k_dynres_min_scale_lo = 0.25f; // valid range lower bound (> 0)
    static constexpr float k_dynres_min_scale_hi = 1.0f;  // valid range upper bound (≤ 1)
};

// ── Pure level→count mappings (single source of truth) ──────────

// Map an ordered quality level to a strictly increasing count spanning the
// inclusive bound range [lo, hi]. Low → lo, High → hi, Medium → the rounded
// midpoint. The three results are strictly increasing whenever hi - lo ≥ 2
// (every configured knob default below satisfies this), which is the
// monotonicity contract  asserts. The level is clamped to the valid
// {0,1,2} range so an out-of-range raw value cannot index past the ends.
[[nodiscard]] inline int level_to_count(QualityLevel level, int lo, int hi) {
    if (hi < lo) std::swap(lo, hi);
    const int l = glm::clamp(static_cast<int>(level), 0, 2);
    const long span = static_cast<long>(hi) - static_cast<long>(lo);
    const long v = static_cast<long>(lo) +
                   std::lround((static_cast<double>(l) / 2.0) * static_cast<double>(span));
    return static_cast<int>(v);
}

// soft-shadow penumbra sample count. Fixed, strictly increasing ladder
// (no configured bounds field): each higher level uses strictly more penumbra
// samples.
[[nodiscard]] inline int soft_shadow_sample_count(QualityLevel level) {
    switch (glm::clamp(static_cast<int>(level), 0, 2)) {
        case 0: return 8;   // Low
        case 1: return 16;  // Medium
        default: return 24; // High
    }
}

// soft-shadow penumbra width fed to filament::SoftShadowOptions::
// penumbraScale (it globally scales the PCSS penumbra; 1.0 is Filament's
// default). The level scales it monotonically via the sample-count ladder
// normalised to the Medium baseline (0.5 / 1.0 / 1.5), multiplied by a base
// softness so the directional shadows show a real soft transition instead of a
// near-hard PCSS edge. Single source of truth shared by LightingSystem and the
// soft-shadow smoke test. Strictly increasing in `level`
// Low 1.5 / Medium 3.0 / High 4.5.
[[nodiscard]] inline float soft_shadow_penumbra_scale(QualityLevel level) {
    constexpr float k_base_penumbra = 3.0f;
    const float level_scale =
        static_cast<float>(soft_shadow_sample_count(level)) /
        static_cast<float>(soft_shadow_sample_count(QualityLevel::Medium));
    return k_base_penumbra * level_scale;
}

// contact-shadow ray-march step count, strictly increasing across
// levels and bounded by [contact_shadow_step_min, contact_shadow_step_max]

[[nodiscard]] inline int contact_shadow_steps(QualityLevel level, const QualityKnobs& k) {
    return level_to_count(level, k.contact_shadow_step_min, k.contact_shadow_step_max);
}

// volumetric-cloud raymarch step count, strictly increasing across
// levels and bounded by [cloud_step_min, cloud_step_max] with the minimum
// forced to at least 1 step.
[[nodiscard]] inline int cloud_raymarch_steps(QualityLevel level, const QualityKnobs& k) {
    const int lo = std::max(1, k.cloud_step_min);
    const int hi = std::max(lo, k.cloud_step_max);
    return level_to_count(level, lo, hi);
}

// SSGI sample/ray count, strictly increasing across levels and bounded
// by ssgi_sample_max. There is no configured minimum
// field, so the low end is derived as a quarter of the maximum (kept ≥ 1).
[[nodiscard]] inline int ssgi_samples(QualityLevel level, const QualityKnobs& k) {
    const int hi = std::max(2, k.ssgi_sample_max);
    const int lo = std::max(1, hi / 4);
    return level_to_count(level, lo, hi);
}

// Source-template grass blade budget for a foliage density multiplier:
// round(baseline * density), capped at the configured maximum so the count
// never exceeds foliage_blade_max. Monotonic
// non-decreasing in density.
[[nodiscard]] inline int foliage_blade_count(float density, const QualityKnobs& k) {
    const long scaled =
        std::lround(static_cast<double>(k.foliage_blade_baseline) * static_cast<double>(density));
    const long capped = std::min<long>(std::max<long>(scaled, 0), k.foliage_blade_max);
    return static_cast<int>(capped);
}

// soft-shadow penumbra-width estimator. PCSS widens the penumbra with
// the blocker-to-receiver distance; this pins that relationship as a monotonic
// non-decreasing function of distance (a larger distance never produces a
// narrower penumbra). Negative distances clamp to zero. This
// is the host-testable estimator the soft-shadow knob feeds and the property
// test in  asserts against.
[[nodiscard]] inline float penumbra_width(float blocker_to_receiver_distance) {
    const float d = std::max(0.0f, blocker_to_receiver_distance);
    constexpr float kLightSize = 0.05f; // effective light angular size factor
    return kLightSize * d;
}

// ── Quality-knob clamp / reject helpers (single source of truth) ─

// The advanced-rendering knobs split into two distinct out-of-range
// behaviours, each captured by its own helper so the CLI parser
// (apply_quality_args) and the property tests share exactly one
// mechanism:

//   • CLAMP-and-report  — the ordered-level knobs (soft-shadow, contact-shadow,
//     cloud, SSGI) and the foliage/budget scalars: an out-of-range value is
//     pulled to the nearest valid bound and a flag reports that a clamp
//     occurred. This mirrors clamp_dry_down_seconds.
//   • REJECT-and-flag   — MSAA: an out-of-set value is REJECTED (not clamped),
//     the previously configured count is retained, and the rejected value is
//     flagged so the CLI can name it in a diagnostic  —
//     deliberately distinct semantics from the clamping knobs.

// Every helper is pure (no I/O, no printing), mirroring the RainIntensityParse
// precedent, so the CLI layer owns all warning/diagnostic messages and the
// helpers stay host-testable without argv.

// Result of clamping a requested quality level to the valid ordered range.
// `clamped` is true when the supplied raw level fell outside {Low, Medium,
// High} and was pulled to the nearest valid level.
struct QualityLevelClamp {
    QualityLevel value   = QualityLevel::Medium;
    bool         clamped = false;
};

// Clamp a raw integer quality level to the valid ordered range
// {Low, Medium, High} == {0, 1, 2}. A value below 0 or above 2 is pulled to
// the nearest valid level and flagged. Shared by every ordered-level knob
// (soft-shadow, contact-shadow, cloud, SSGI): they all use the same
// QualityLevel enum, so one clamp covers them all.
[[nodiscard]] inline QualityLevelClamp clamp_quality_level(int raw) {
    QualityLevelClamp r;
    const int c = glm::clamp(raw, 0, 2);
    r.clamped = (c != raw);
    r.value   = static_cast<QualityLevel>(c);
    return r;
}

// Result of clamping an integer scalar knob to its valid range. `clamped` is
// true when the supplied value was out of range and pulled to the nearest
// bound.
struct IntKnobClamp {
    int  value   = 0;
    bool clamped = false;
};

// Clamp the shadow-casting light budget to [0, shadow_light_budget_max]
// Reports whether the supplied value was out of
// range. The upper bound is read from the knob struct so the bound stays the
// single source of truth shared with the CLI and the property tests.
[[nodiscard]] inline IntKnobClamp clamp_quality_shadow_light_budget(int raw, const QualityKnobs& k) {
    IntKnobClamp r;
    const int hi = std::max(0, k.shadow_light_budget_max);
    const int c  = glm::clamp(raw, 0, hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// Result of clamping a float scalar knob to its valid range. `clamped` is true
// when the supplied value was out of range (or NaN) and replaced with a valid
// value.
struct FloatKnobClamp {
    float value   = 1.0f;
    bool  clamped = false;
};

// The requested bloom strength scales the existing day/night response. Keep
// malformed programmatic/UI inputs finite and use the same bound everywhere.
[[nodiscard]] inline FloatKnobClamp clamp_bloom_strength(float raw) {
    FloatKnobClamp result;
    result.value = std::isfinite(raw)
        ? std::clamp(raw, 0.0f, 1.0f)
        : render_tuning::lighting.bloom_reference_strength;
    result.clamped = !std::isfinite(raw) || result.value != raw;
    return result;
}

// Clamp the foliage density multiplier to [1.0, foliage_density_max]
// A NaN request collapses to the 1.0 Shipping_Look
// baseline (mirroring clamp_dry_down_seconds' NaN handling) and is reported as
// clamped rather than propagating through the comparison.
[[nodiscard]] inline FloatKnobClamp clamp_quality_foliage_density(float raw, const QualityKnobs& k) {
    FloatKnobClamp r;
    if (std::isnan(raw)) {
        r.value   = 1.0f;
        r.clamped = true;
        return r;
    }
    const float hi = std::max(1.0f, k.foliage_density_max);
    const float c  = glm::clamp(raw, 1.0f, hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// the MSAA sample count accepts EXACTLY {1, 2, 4} — no other value is
// valid. This is the predicate the reject-and-flag apply helper below is built
// on, and the contract  asserts.
[[nodiscard]] inline bool msaa_accepts(int n) {
    return n == 1 || n == 2 || n == 4;
}

// Result of applying a requested MSAA sample count. Distinct from the clamping
// knobs above: an out-of-set value is REJECTED (not clamped) — the previously
// configured count is retained and the rejected value is flagged so the CLI
// can emit a diagnostic naming it.
//   value          — the count to use: the request when accepted, otherwise
//                    the previously configured count (retained).
//   rejected       — true when the request was outside {1, 2, 4}.
//   rejected_value — the rejected request (meaningful only when rejected).
struct MsaaApplyResult {
    int  value          = 1;
    bool rejected       = false;
    int  rejected_value = 0;
};

// Apply a requested MSAA sample count against the previously configured count.
// An accepted request ({1,2,4}) becomes the new value with rejected=false; an
// out-of-set request retains `previous` and is flagged with the rejected value
// so the CLI can name it. Pure — no clamping, no I/O.
[[nodiscard]] inline MsaaApplyResult apply_msaa_samples(int requested, int previous) {
    MsaaApplyResult r;
    if (msaa_accepts(requested)) {
        r.value = requested;
    } else {
        r.value          = previous;
        r.rejected       = true;
        r.rejected_value = requested;
    }
    return r;
}

// GPU capability fallback for an MSAA sample count. Given a requested
// sample count and the set of sample counts the GPU actually supports, pick the
// highest supported count that is ≤ the request; when nothing supported is at
// or below the request, fall back to 1 sample (MSAA effectively off). The
// result is never greater than the request and is always either 1 or a member
// of `supported` — the contract  pins down. Pure — no Filament query,
// no I/O; the init-time capability query (which DOES touch Filament) feeds the
// queried set in and emits the single requested-vs-applied diagnostic.
[[nodiscard]] inline int msaa_fallback(int requested, std::span<const int> supported) {
    int applied = 1;
    for (int count : supported) {
        if (count <= requested && count > applied) {
            applied = count;
        }
    }
    return applied;
}

// ── Weather system (rain intensity → coordinated responses) ─────

// Upper bounds on the build-once precipitation geometry. The rain
// feature allocates this many camera-facing streak quads and impact
// quads once and scales the *active* count via uniforms; the weather
// math reports a [0,1] fraction and the feature multiplies by these
// (caps streaks at 60000,  caps concurrent splashes at 4096).

// kMaxStreaks was raised from 20000 to 60000 alongside the larger
// camera-centered precipitation volume (weather_ref::kVolumeExtent grew
// from 20 to 70): the same streak count spread across a ~43x larger
// volume read as sparse dashes, so the pool is enlarged to keep medium
// rain (intensity 0.5 ⇒ 30000 active streaks) reading as steady rain
// across the whole frame.
inline constexpr int kMaxStreaks =
    render_tuning::shader_limits.rain_streaks;
inline constexpr int kMaxSplashes =
    render_tuning::shader_limits.rain_splashes;

// Tunable endpoints for the weather responses. Every "@1.0" field is
// the value the corresponding response reaches at full storm
// (Rain_Intensity == 1.0); each response interpolates from its
// Shipping_Look value at intensity 0 to this endpoint at intensity 1.
// These are the single source of the overcast/wetness "look" and are
// deliberately data (not magic numbers buried in the mapping) so the
// renderer and the property tests share them.
struct WeatherConfig {
    // Initial commanded Rain_Intensity in [0,1]. By product decision
    // the default is now medium rain (0.5) rather than clear, so a flagless
    // run renders the storm; pass `--rain-intensity 0` (or `--no-rain`) to
    // restore the clear Shipping_Look. The [0,1] clamp semantics are unchanged.
    float rain_intensity   = 0.5f;
    // Wetness dry-down duration in seconds (default; clamped to [0.5,30]
    // when consumed so the dry-down transition can never divide by zero).
    float dry_down_seconds = 5.0f;

    // Progressive surface-wetness time constants. These govern the SEPARATE
    // accumulated wetness level (surface_wetness_) that LAGS the precipitation,
    // distinct from effective_intensity(): surfaces soak up water over a few
    // seconds when it starts raining and dry out more slowly when it stops, so
    // the wet look fades in/out instead of snapping on at full strength.
    //   wet_soak_seconds — exponential time constant (seconds) the surface
    //     wetness approaches its rain target with while raining (target > cur).
    //     8 s reads as "the ground darkens and glosses over the first several
    //     seconds of the storm" without being so slow it never arrives.
    //   wet_dry_seconds  — exponential time constant (seconds) the surface
    //     wetness decays toward 0 with after the rain stops (target < cur).
    //     Longer than the soak (12 s) because wet stone/pavement dries more
    //     slowly than it wets — puddles linger after the rain passes.
    // Both are time constants for a frame-rate-independent exponential approach
    // (see WeatherState::advance); they are NOT per-frame lerp factors.
    float wet_soak_seconds = 8.0f;
    float wet_dry_seconds  = 12.0f;

    // Overcast atmospheric response endpoints.
    float min_sun_scale_at_1   = 0.25f; // sun-intensity multiplier at full storm
    float overcast_desat_at_1  = 0.70f; // sky/ambient saturation reduction at 1.0
    float overcast_lum_at_1    = 0.60f; // sky/ambient luminance reduction at 1.0
    float max_fog_density_at_1 = 0.003f; // added overcast fog density at full storm; tuned to
                                         // subtle aerial haze, not a white wall — full storm 1.0 → 0.003
                                         // (between the shipping-good 0.0008 and the 0.005 "haze wall"),
                                         // medium 0.5 → 0.0015. Paired with the bounded grey fog in
                                         // post_process.cpp (cutOffDistance/maxOpacity/grey color).

    // Surface-wetness response endpoints.
    // LOOK PASS: raised so medium intensity (0.5) clearly reads as wet.
    //   max_spec_boost_at_1     0.60 → 0.90 — stronger reflectance/spec rise so
    //     wet ground gains a glossy sheen (paired with the harder roughness drop
    //     in heaven_lit*.mat). Still dielectric — reflectance only, not metallic.
    //   max_diffuse_darken_at_1 0.35 → 0.45 — wet stone/pavement reads darker,
    //     the classic rain-darkened look. Both feed wetness_global() linearly,
    //     so the response stays monotonic non-decreasing and exactly 0 at i=0
    //     (Property "wetness monotonic" holds; only the i=1 endpoints grow).
    float max_spec_boost_at_1     = 0.90f; // specular/reflectance boost at full storm
    float max_diffuse_darken_at_1 = 0.45f; // diffuse darkening at full storm
};

// Clamp a requested wetness dry-down duration into the supported range
// [0.5, 30.0] seconds. This is the single place that mechanism lives,
// so both the CLI (`--dry-down-seconds`) at configuration time and the
// dry-down transition at consumption time (WeatherState) agree on the bounds:
// the lower bound keeps the decay denominator strictly positive, the upper
// bound caps how long a transition may take. A NaN request collapses to the
// 5.0 s default rather than propagating through the clamp.
[[nodiscard]] inline float clamp_dry_down_seconds(float seconds) {
    if (std::isnan(seconds)) return 5.0f;
    return glm::clamp(seconds, 0.5f, 30.0f);
}

// The single source of truth for rain intensity and its dry-down
// transition, plus the pure mapping from intensity to every weather
// response. Header-only, GLM-only, no GPU dependencies — the whole
// type is unit/property testable on the host. The renderer owns one
// instance (in main.cpp, next to LightingParams) and threads it to the
// features through FrameContext, exactly as LightingParams already
// flows; no per-effect state lives on the backend.

// `target_intensity` is the commanded Rain_Intensity. When the command
// drops to 0 while surfaces are still wet, a frame-rate-independent
// dry-down begins: `effective_intensity()` decays linearly from the
// wetness present when drying started (`drying_from`) to 0 over
// `dry_down_seconds`, and every visual response reads
// `effective_intensity()` so precipitation, splashes, wetness, and the
// overcast atmosphere all fade together. At effective intensity 0 with
// no dry-down in progress, every response collapses to its
// Shipping_Look value.
struct WeatherState {
    WeatherConfig cfg;
    float target_intensity = 0.0f; // commanded Rain_Intensity, clamped to [0,1]
    float drying_from       = 0.0f; // effective intensity at the moment dry-down began
    float dry_elapsed       = 0.0f; // seconds advanced into the current dry-down
    bool  drying            = false;

    // Progressive surface-wetness level in [0,1] (0 = bone dry, 1 = fully wet).
    // This LAGS the precipitation: it is a separate accumulator that soaks
    // toward effective_intensity() while raining and decays toward 0 when the
    // rain stops, advanced frame-rate-independently in advance(dt). It is
    // distinct from effective_intensity() (the instantaneous commanded/drying
    // rain level) and is what the renderer pushes to the wetness material
    // global so the wet look builds up and dries out over time. A fresh dry
    // state starts at exactly 0 (Shipping_Look parity).
    float surface_wetness_  = 0.0f;

    WeatherState() = default;
    explicit WeatherState(const WeatherConfig& c)
        : cfg(c), target_intensity(glm::clamp(c.rain_intensity, 0.0f, 1.0f)) {}

    // Command a new Rain_Intensity. The value is always clamped to [0,1]
    // Any positive command cancels an in-progress
    // dry-down and snaps the effective intensity to the commanded value
    // A zero command issued while surfaces are still
    // wet starts a fresh dry-down from the current wetness; a zero command
    // while already dry (or already drying) leaves the state alone.
    void set_intensity(float v) {
        v = glm::clamp(v, 0.0f, 1.0f);
        if (v > 0.0f) {
            drying      = false;
            drying_from = 0.0f;
            dry_elapsed = 0.0f;
            target_intensity = v;
        } else {
            const float current = effective_intensity();
            if (!drying && current > 0.0f) {
                drying      = true;
                drying_from = current;
                dry_elapsed = 0.0f;
            }
            target_intensity = 0.0f;
        }
    }

    // Advance the weather transitions by `dt` real-time seconds. Two things
    // move here, both purely time-driven so they are frame-rate independent
    // (any partition of the same elapsed wall-clock time reaches the same
    // state, within float tolerance —):

    //   1. The dry-down of effective_intensity(): time is simply accumulated
    //      (dry_elapsed += dt); once the configured duration elapses the state
    //      snaps to fully dry (effective intensity exactly 0). A no-op when not
    //      drying.

    //   2. The progressive surface-wetness accumulator (surface_wetness_): it
    //      approaches a TARGET equal to the current effective_intensity() with
    //      an exponential time constant — wet_soak_seconds while soaking
    //      (target > current, i.e. it is raining and the ground is getting
    //      wetter) and wet_dry_seconds while drying out (target < current,
    //      i.e. the rain eased and the ground is drying). The update is
    //          cur += (target - cur) * (1 - exp(-dt / tau))
    //      which composes exactly across substeps (exp(-a)·exp(-b) ==
    //      exp(-(a+b))), so reaching the same elapsed time as one big step or
    //      many small steps yields the same wetness up to float accumulation —
    //      it is NOT a per-frame lerp factor that depends on frame count. The
    //      result is clamped to [0,1]. When target == current (e.g. a fresh dry
    //      state with no rain: both 0) the level does not move, so a never-wet
    //      dry state stays exactly 0 (Shipping_Look parity).
    void advance(float dt) {
        if (drying) {
            if (dt > 0.0f) dry_elapsed += dt;
            if (dry_elapsed >= effective_dry_down_seconds()) {
                drying      = false;
                drying_from = 0.0f;
                dry_elapsed = 0.0f;
            }
        }

        if (dt > 0.0f) {
            const float target = effective_intensity();
            const float tau = (target > surface_wetness_) ? effective_wet_soak_seconds()
                                                           : effective_wet_dry_seconds();
            // tau is strictly positive (the accessors clamp away zero), so the
            // exponential approach is always well defined.
            const float k = 1.0f - std::exp(-dt / tau);
            surface_wetness_ += (target - surface_wetness_) * k;
            surface_wetness_ = glm::clamp(surface_wetness_, 0.0f, 1.0f);
        }
    }

    // The intensity actually driving visuals this frame: the commanded
    // value while raining, or the linearly decaying value during a
    // dry-down. Exactly 0 once a dry-down completes.
    [[nodiscard]] float effective_intensity() const {
        if (!drying) return target_intensity;
        const float d = effective_dry_down_seconds();
        const float t = glm::clamp(dry_elapsed / d, 0.0f, 1.0f);
        return drying_from * (1.0f - t);
    }

    // ── Pure intensity → response mappings ──────────────────────

    // Fraction of the streak pool that is active. Linear in
    // effective intensity, so the active streak count
    // round(streak_fraction() * kMaxStreaks) is monotonic non-decreasing
    // and bounded by [0, kMaxStreaks].
    [[nodiscard]] float streak_fraction() const { return effective_intensity(); }

    // Fraction of the splash pool that is active. Linear in
    // effective intensity; the active count round(splash_fraction() *
    // kMaxSplashes) is monotonic non-decreasing and capped at kMaxSplashes
    // (4096) by construction.
    [[nodiscard]] float splash_fraction() const { return effective_intensity(); }

    // Added overcast fog density. The Shipping_Look uses no distance
    // fog (base density 0), so this rises linearly from 0 at clear weather
    // to `max_fog_density_at_1` at full storm — monotonic non-decreasing
    // The renderer passes this as the fog-density override,
    // substituting the shipping config when it is 0.
    [[nodiscard]] float fog_density() const {
        return effective_intensity() * cfg.max_fog_density_at_1;
    }

    // Multiplier applied to the Shipping_Look sun intensity. 1.0 at
    // clear weather, `min_sun_scale_at_1` at full storm — monotonic
    // non-increasing.
    [[nodiscard]] float sun_scale() const {
        return glm::mix(1.0f, cfg.min_sun_scale_at_1, effective_intensity());
    }

    // Per-view wetness global packed for setMaterialGlobal(0,...), read by
    // the lit materials as getMaterialGlobal0():
    //   (specular boost, diffuse darken, wet amount, 1).
    // Both the specular-boost and diffuse-darken terms rise linearly with
    // effective intensity. At intensity 0 this is
    // exactly (0,0,0,1) — Filament's documented default global — so the lit
    // shaders take their no-op path and the output is bit-identical to the
    // Shipping_Look.
    [[nodiscard]] glm::vec4 wetness_global() const {
        const float i = effective_intensity();
        if (i <= 0.0f) return {0.0f, 0.0f, 0.0f, 1.0f};
        return {cfg.max_spec_boost_at_1 * i,
                         cfg.max_diffuse_darken_at_1 * i,
                         i,
                         1.0f};
    }

    // The accumulated surface-wetness level in [0,1] (0 = bone dry, 1 = fully
    // wet). This LAGS the precipitation (soaks in / dries out over time in
    // advance(dt)), unlike effective_intensity() which is the instantaneous
    // rain level. The renderer reads this to drive the wet material look and
    // the SSR retune so reflections strengthen as the ground soaks.
    [[nodiscard]] float surface_wetness() const { return surface_wetness_; }

    // Per-view wetness global packed for setMaterialGlobal(0,...) using the
    // ACCUMULATED surface wetness (surface_wetness()) instead of the
    // instantaneous effective intensity. Same layout and endpoints as
    // wetness_global() — (specular boost, diffuse darken, wet amount, 1) — and
    // the SAME exact (0,0,0,1) guarantee at w <= 0, so a never-wet dry state
    // packs Filament's default global and the lit shaders take their no-op
    // (bit-identical Shipping_Look) path. This is what the renderer
    // pushes to the material so the wet look builds up and dries out over time;
    // wetness_global() above is left untouched (pure, intensity-driven) so the
    // wetness-monotonic property is unaffected.
    [[nodiscard]] glm::vec4 surface_wetness_global() const {
        const float w = surface_wetness_;
        if (w <= 0.0f) return {0.0f, 0.0f, 0.0f, 1.0f};
        return {cfg.max_spec_boost_at_1 * w,
                         cfg.max_diffuse_darken_at_1 * w,
                         w,
                         1.0f};
    }

    // Derive overcast LightingParams from the Shipping_Look base. Templated
    // on the lighting type so this header stays free of the rhi.h dependency
    // (LightingParams lives in rhi.h, which already includes this header);
    // it works for any type exposing `sun_intensity`, `sky_color`, and
    // `ambient`. The sun intensity is scaled by sun_scale(), and the
    // sky and ambient colours are pushed toward an overcast tint — reduced
    // saturation and reduced luminance — monotonically with intensity
    // At effective intensity 0 the base is
    // returned unchanged, giving exact Shipping_Look parity (Property
    // 15).
    template <typename Lighting>
    [[nodiscard]] Lighting overcast_lighting(const Lighting& base) const {
        const float i = effective_intensity();
        if (i <= 0.0f) return base;

        Lighting out = base;
        out.sun_intensity = base.sun_intensity * sun_scale();

        const float desat_frac = cfg.overcast_desat_at_1 * i; // → toward grey
        const float lum_scale  = 1.0f - cfg.overcast_lum_at_1 * i; // dim luminance
        out.sky_color = overcast_tint(base.sky_color, desat_frac, lum_scale);
        out.ambient   = overcast_tint(base.ambient,   desat_frac, lum_scale);
        return out;
    }

    // Equality over the DERIVED weather-dependent outputs that the renderer
    // actually pushes to Filament when the weather changes — the effective
    // (instantaneous) intensity, the distance-fog density override, the
    // accumulated surface wetness, and the overcast sun-intensity scale — NOT
    // the raw transition timers (drying_from / dry_elapsed / drying). Two
    // states that produce the same applied look compare equal even if their
    // bookkeeping differs, so the backend's per-frame change check (Requirement
    // 11) re-applies state if and only if the APPLIED result would actually
    // differ. While the weather sits at its Shipping_Look default these are all
    // their clear-weather values, so the change check stays false after the
    // first frame and SDR output is byte-for-byte unchanged.
    [[nodiscard]] bool operator==(const WeatherState& o) const {
        return effective_intensity() == o.effective_intensity()
            && fog_density()         == o.fog_density()
            && surface_wetness()     == o.surface_wetness()
            && sun_scale()           == o.sun_scale();
    }
    [[nodiscard]] bool operator!=(const WeatherState& o) const {
        return !(*this == o);
    }

private:
    // Dry-down duration clamped to the supported [0.5, 30.0] s range,
    // guaranteeing a strictly positive denominator for the decay. Delegates
    // to the shared clamp_dry_down_seconds() so the consumption-time bound
    // here and the CLI configuration-time bound stay in lockstep.
    [[nodiscard]] float effective_dry_down_seconds() const {
        return clamp_dry_down_seconds(cfg.dry_down_seconds);
    }

    // Surface-wetness soak / dry time constants, each clamped to a strictly
    // positive lower bound so the exponential approach in advance() can never
    // divide by zero or stall, and an upper bound that caps how slow the
    // transition may be. A NaN config collapses to the documented default.
    // These keep surface_wetness_ accumulation well defined for any config.
    [[nodiscard]] float effective_wet_soak_seconds() const {
        if (std::isnan(cfg.wet_soak_seconds)) return 8.0f;
        return glm::clamp(cfg.wet_soak_seconds, 0.05f, 120.0f);
    }
    [[nodiscard]] float effective_wet_dry_seconds() const {
        if (std::isnan(cfg.wet_dry_seconds)) return 12.0f;
        return glm::clamp(cfg.wet_dry_seconds, 0.05f, 120.0f);
    }

    // Push a colour toward an overcast look: first mix toward its own
    // luminance-grey by `desat_frac` (lowers saturation while preserving
    // luminance), then scale by `lum_scale` (lowers luminance without
    // changing saturation). Both steps are monotonic in their arguments, so
    // saturation and luminance each fall monotonically as intensity rises
    // Rec. 709 luminance weights.
    [[nodiscard]] static glm::vec3 overcast_tint(const glm::vec3& base_color,
                                                 float desat_frac,
                                                 float lum_scale) {
        const float lum = glm::dot(base_color, glm::vec3(0.2126f, 0.7152f, 0.0722f));
        const glm::vec3 grey(lum);
        const glm::vec3 desaturated = glm::mix(base_color, grey, desat_frac);
        return desaturated * lum_scale;
    }
};

// Result of parsing a `--rain-intensity` command-line argument. The CLI layer
// applies the value and decides what to print; this struct carries enough to
// satisfy  without the parser doing any I/O:
//   value   — the Rain_Intensity to use, always in [0,1].
//   clamped — true when a numeric argument fell outside [0,1] and was pulled
//             to the nearest bound (0.0 below, 1.0 above) — the CLI warns
//             that the value was clamped.
//   ignored — true when the argument was not a number (junk / empty / null)
//             and the parser fell back to 0.0 — the CLI warns that the
//             argument was ignored.
// A well-formed in-range argument yields {value, false, false}.
struct RainIntensityParse {
    float value   = 0.0f;
    bool  clamped = false;
    bool  ignored = false;
};

// Parse a `--rain-intensity` argument string into a Rain_Intensity in [0,1].
// Pure (no I/O, no printing) so it is unit/property testable without argv and
// so the CLI owns all warning messages. Behaviour:
//   • numeric in [0,1]            → accepted as-is        (clamped=false, ignored=false)
//   • numeric outside [0,1]       → clamped to 0.0 / 1.0  (clamped=true)
//   • non-numeric / junk / empty  → falls back to 0.0     (ignored=true)
//   • null pointer                → falls back to 0.0     (ignored=true)
// Parsing uses the shared strict numeric path. A trailing-garbage token
// like "0.5abc", NaN, or infinity is ignored rather than silently accepted.
[[nodiscard]] inline RainIntensityParse parse_rain_intensity(const char* arg) {
    RainIntensityParse result;
    if (arg == nullptr) {
        result.ignored = true;
        return result;
    }

    const auto parsed = parse_number_strict<double>(arg);
    if (!parsed) {
        result.ignored = true;
        return result;
    }
    const double v = *parsed;

    if (v < 0.0) {
        result.value   = 0.0f;
        result.clamped = true;
    } else if (v > 1.0) {
        result.value   = 1.0f;
        result.clamped = true;
    } else {
        result.value = static_cast<float>(v);
    }
    return result;
}

// ── Background music configuration ──────────────────────────────

// Small POD on AppConfig (same tier as WeatherConfig / TessellationConfig) so
// it is constructible without Filament/SDL and is host-testable. The
// Music_Player reads this to decide whether to play and how loud.
//   enabled — default on; cleared by --no-music.
//   volume  — default 1.0; always stored post-clamp in [0,1], set by
//             --music-volume via parse_music_volume below.
struct MusicConfig {
    bool  enabled = true;
    float volume  = 1.0f;
};

// Relative path (under the Data_Root) of the background Music_Track. This is
// the single source of truth shared by PathResolver::music_path() and the
// used-asset packer's discovery so the renderer's resolved path and the
// packer's recorded Relative_Asset_Path can never drift.
inline constexpr std::string_view k_music_rel_path = "heaven/heaven.oga";

// Result of parsing a `--music-volume` command-line token. The CLI layer
// applies the value and owns every diagnostic; this struct carries enough to
// satisfy  without the parser doing any I/O:
//   value   — the Music_Volume to use, always in [0.0, 1.0].
//   clamped — true when a numeric token fell outside [0,1] and was pulled to
//             the nearest bound (0.0 below, 1.0 above) — the CLI warns that
//             the value was clamped.
//   ignored — true when the token was not a number (junk / empty / null /
//             trailing-garbage / NaN) and the parser retained the default
//             1.0 — the CLI warns that the token was ignored.
// A well-formed in-range token yields {value, false, false}. The
// default 1.0 is the retained value for the ignored case.
struct MusicVolumeParse {
    float value   = 1.0f;
    bool  clamped = false;
    bool  ignored = false;
};

// Parse a `--music-volume` token into a Music_Volume in [0.0, 1.0]. Pure (no
// I/O, no printing) so it is unit/property testable without argv and so the
// CLI owns all warning messages. Behaviour mirrors
// parse_rain_intensity exactly, only the default differs (1.0, not 0.0):
//   • numeric in [0,1]            → accepted as-is        (clamped=false, ignored=false)
//   • numeric outside [0,1]       → clamped to 0.0 / 1.0  (clamped=true)
//   • non-numeric / junk / empty  → retains default 1.0   (ignored=true)
//   • null pointer                → retains default 1.0   (ignored=true)
// Parsing uses the same strict numeric path as rain intensity. Trailing
// garbage, NaN, and infinity are ignored.
[[nodiscard]] inline MusicVolumeParse parse_music_volume(const char* token) {
    MusicVolumeParse result;
    if (token == nullptr) {
        result.ignored = true;
        return result;
    }

    const auto parsed = parse_number_strict<double>(token);
    if (!parsed) {
        result.ignored = true;
        return result;
    }
    const double v = *parsed;

    if (v < 0.0) {
        result.value   = 0.0f;
        result.clamped = true;
    } else if (v > 1.0) {
        result.value   = 1.0f;
        result.clamped = true;
    } else {
        result.value = static_cast<float>(v);
    }
    return result;
}

// ── Music callback core: fill_block ─────────────────────────────

// The pure heart of the SDL audio callback, extracted so the looping +
// volume-scaling logic is host-testable with no audio device, decoder, or
// Filament dependency (Properties 2 and 4). The real MusicPlayer callback is a
// thin shell that loads the atomic volume/cursor and calls this.

// Contract:
//   • `pcm`    — the immutable decoded track as interleaved 16-bit samples.
//                Channel count is NOT a parameter: fill_block operates purely
//                at the sample level and wraps on `pcm.size()`, so it is
//                channel-agnostic. (The MusicPlayer keeps the decoded buffer a
//                whole number of frames, so a sample-level wrap is also a
//                frame-aligned wrap.)
//   • `cursor` — the read position as an index into `pcm` (in samples). On
//                entry it is taken modulo `pcm.size()` defensively; on exit it
//                is left in the half-open range [0, pcm.size()).
//   • `volume` — amplitude scalar; each emitted sample is the input sample
//                scaled by `volume`, rounded to the nearest int16 and clamped
//                to the int16 range. `volume == 1.0` reproduces the input
//                sample unchanged; `volume == 0.0` emits silence.
//   • `out`    — the destination block; exactly `out.size()` samples are
//                written.

// Looping is gap-free by construction: when the read position reaches the end
// of `pcm` it wraps back to 0 within the same call, so the sample following
// the last is the first with no zero-sample inserted at the wrap point.

// An empty `pcm` (no track) zero-fills `out` and leaves `cursor` unchanged.
inline void fill_block(std::span<const int16_t> pcm, std::size_t& cursor,
                       float volume, std::span<int16_t> out) {
    if (pcm.empty()) {
        std::fill(out.begin(), out.end(), static_cast<int16_t>(0));
        return;
    }

    const std::size_t n = pcm.size();
    std::size_t pos = cursor % n;
    for (short & i : out) {
        const long scaled = std::lround(static_cast<float>(pcm[pos]) * volume);
        const long bounded = std::clamp<long>(scaled, -32768, 32767);
        i = static_cast<int16_t>(bounded);
        if (++pos == n) {
            pos = 0;
        }
    }
    cursor = pos;
}

// ── Backend API enum ────────────────────────────────────────────

// The OpenGL backend was removed in v0.4 — we used to ship a hand-
// rolled GL 4.5 reference path alongside the Filament backend, but
// it was unfinished and hadn't been maintained for months. The
// enum is kept (rather than collapsed) so configs and CLI flags
// don't break; new GPU backends would fit here naturally. It lives
// in this GPU-free header (rather than rhi/rhi.h) so the host-
// testable CLI parser can produce a BenchConfig without pulling in
// any Filament/SDL dependency.
enum class GraphicsApi {
    Filament
};

[[nodiscard]] constexpr std::string_view to_string(GraphicsApi api) {
    switch (api) {
        case GraphicsApi::Filament: return "Filament";
    }
    return "Unknown";
}

// ──  — tone-mapper selection, color grading & physical camera ──

// SDR tone-mapping operator selection. ACES is the Shipping_Look
// default so a flagless run is bit-identical; the other operators are
// selectable via `--tone-mapper <name>`. The `--hdr` branch always uses GT7
// regardless of this selection. Parsed from a name by
// parse_tone_mapper, which rejects unknown names back to ACES.
enum class ToneMapperSelection : int {
    ACES       = 0,
    AgX        = 1,
    PBRNeutral = 2,
    GT7        = 3,
    Generic    = 4,
};

[[nodiscard]] constexpr std::string_view to_string(ToneMapperSelection tm) {
    switch (tm) {
        case ToneMapperSelection::ACES:       return "ACES";
        case ToneMapperSelection::AgX:        return "AgX";
        case ToneMapperSelection::PBRNeutral: return "PBRNeutral";
        case ToneMapperSelection::GT7:        return "GT7";
        case ToneMapperSelection::Generic:    return "Generic";
    }
    return "Unknown";
}

// Group C color-grading controls. Every field defaults to its neutral
// value so a disabled / untouched grading config reproduces the Shipping_Look
// to ≤1/255. When enabled, configure_post_processing chains the
// ColorGrading::Builder calls whiteBalance → channelMixer →
// shadowsMidtonesHighlights → slopeOffsetPower (ASC CDL) → contrast →
// saturation from these values. All bounds are constexpr members —
// the single source of truth shared by the CLI clamp helpers and the property
// tests.
struct ColorGradingConfig {
    // White balance. temperature/tint are normalized controls in [-1, 1];
    // 0 == neutral (no shift).
    float temperature = 0.0f;
    float tint        = 0.0f;

    // Channel mixer. Each output channel is a linear combination of the input
    // RGB; the identity (red = {1,0,0}, green = {0,1,0}, blue = {0,0,1}) is the
    // neutral pass-through.
    glm::vec3 mixer_red   = glm::vec3(1.0f, 0.0f, 0.0f);
    glm::vec3 mixer_green = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 mixer_blue  = glm::vec3(0.0f, 0.0f, 1.0f);

    // Shadows / midtones / highlights (tonal range color balance). The.rgb of
    // each is a per-range color offset (0 == neutral); the.w is a per-range
    // weight (0 == neutral). Defaults leave the tonal balance untouched.
    glm::vec4 shadows   = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    glm::vec4 midtones  = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    glm::vec4 highlights= glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);

    // ASC CDL slope / offset / power. Identity is slope 1, offset 0, power 1.
    glm::vec3 cdl_slope  = glm::vec3(1.0f);
    glm::vec3 cdl_offset = glm::vec3(0.0f);
    glm::vec3 cdl_power  = glm::vec3(1.0f);

    // Contrast and saturation. 1.0 == neutral.
    float contrast   = 1.0f;
    float saturation = 1.0f;

    // Night-adaptation preset: when set, configure_post_processing
    // applies a blue-shifted white balance + reduced saturation on top of the
    // controls above. Off == no preset.
    bool  night_adaptation = false;

    // Bounds — single source of truth shared by the CLI clamp helpers (task
    // and the property tests.
    static constexpr float k_white_balance_lo = -1.0f;
    static constexpr float k_white_balance_hi =  1.0f;
    static constexpr float k_contrast_lo       = 0.0f;
    static constexpr float k_contrast_hi       = 2.0f;
    static constexpr float k_saturation_lo     = 0.0f;
    static constexpr float k_saturation_hi     = 2.0f;
};

// physically-based camera. The defaults reproduce the Shipping_Look
// exposure when the feature is disabled. When enabled, FilamentBackend's
// update_camera step calls Camera::setExposure(aperture, shutter_speed,
// sensitivity) and feeds the same aperture into the depth-of-field options so
// exposure and DOF stay coherent. Bounds are constexpr members —
// the single source of truth shared by the CLI clamp helpers and the property
// tests. Ranges follow photographic / Filament conventions.
struct PhysicalCameraConfig {
    float aperture      = 16.0f;        // f-number
    float shutter_speed = 1.0f / 125.0f;// seconds
    float sensitivity   = 100.0f;       // ISO

    static constexpr float k_aperture_lo = 0.5f;
    static constexpr float k_aperture_hi = 64.0f;
    static constexpr float k_shutter_lo  = 1.0f / 8000.0f;
    static constexpr float k_shutter_hi  = 30.0f;
    static constexpr float k_iso_lo      = 25.0f;
    static constexpr float k_iso_hi      = 204800.0f;
};

// ──  — grading / camera / knob clamp + parse helpers ──────

// Pure (no I/O, no printing), mirroring the existing clamp_quality_* /
// apply_msaa_samples / parse_rain_intensity precedents, so the CLI parser
// owns every diagnostic and the helpers stay host-testable without
// argv. Each clamp helper uses the constexpr bounds already authored on the
// owning struct as the single source of truth  — no new magic
// numbers — and follows the clamp-and-report contract of
// clamp_quality_foliage_density: an out-of-range value is pulled to the nearest
// bound and reported `clamped`, and a NaN collapses to the neutral /
// Shipping_Look default (also reported clamped) rather than propagating through
// the comparison.

// Grading contrast → [k_contrast_lo, k_contrast_hi] (== [0, 2]); neutral
// default 1.0.
[[nodiscard]] inline FloatKnobClamp clamp_grading_contrast(float raw) {
    FloatKnobClamp r;
    if (std::isnan(raw)) {
        r.value   = ColorGradingConfig{}.contrast; // neutral 1.0
        r.clamped = true;
        return r;
    }
    const float c = glm::clamp(raw, ColorGradingConfig::k_contrast_lo,
                                    ColorGradingConfig::k_contrast_hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// Grading saturation → [k_saturation_lo, k_saturation_hi] (== [0, 2]); neutral
// default 1.0.
[[nodiscard]] inline FloatKnobClamp clamp_grading_saturation(float raw) {
    FloatKnobClamp r;
    if (std::isnan(raw)) {
        r.value   = ColorGradingConfig{}.saturation; // neutral 1.0
        r.clamped = true;
        return r;
    }
    const float c = glm::clamp(raw, ColorGradingConfig::k_saturation_lo,
                                    ColorGradingConfig::k_saturation_hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// Grading white-balance control (temperature or tint) → [k_white_balance_lo,
// k_white_balance_hi] (== [-1, 1]); neutral default 0.0.
[[nodiscard]] inline FloatKnobClamp clamp_grading_white_balance(float raw) {
    FloatKnobClamp r;
    if (std::isnan(raw)) {
        r.value   = ColorGradingConfig{}.temperature; // neutral 0.0
        r.clamped = true;
        return r;
    }
    const float c = glm::clamp(raw, ColorGradingConfig::k_white_balance_lo,
                                    ColorGradingConfig::k_white_balance_hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// Physical-camera aperture (f-number) → [k_aperture_lo, k_aperture_hi]; default
// 16.0.
[[nodiscard]] inline FloatKnobClamp clamp_camera_aperture(float raw) {
    FloatKnobClamp r;
    if (std::isnan(raw)) {
        r.value   = PhysicalCameraConfig{}.aperture; // default 16.0
        r.clamped = true;
        return r;
    }
    const float c = glm::clamp(raw, PhysicalCameraConfig::k_aperture_lo,
                                    PhysicalCameraConfig::k_aperture_hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// Physical-camera shutter speed (seconds) → [k_shutter_lo, k_shutter_hi];
// default 1/125.
[[nodiscard]] inline FloatKnobClamp clamp_camera_shutter(float raw) {
    FloatKnobClamp r;
    if (std::isnan(raw)) {
        r.value   = PhysicalCameraConfig{}.shutter_speed; // default 1/125
        r.clamped = true;
        return r;
    }
    const float c = glm::clamp(raw, PhysicalCameraConfig::k_shutter_lo,
                                    PhysicalCameraConfig::k_shutter_hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// Physical-camera sensitivity (ISO) → [k_iso_lo, k_iso_hi]; default 100

[[nodiscard]] inline FloatKnobClamp clamp_camera_iso(float raw) {
    FloatKnobClamp r;
    if (std::isnan(raw)) {
        r.value   = PhysicalCameraConfig{}.sensitivity; // default 100
        r.clamped = true;
        return r;
    }
    const float c = glm::clamp(raw, PhysicalCameraConfig::k_iso_lo,
                                    PhysicalCameraConfig::k_iso_hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// Dynamic-resolution minimum render scale → [k_dynres_min_scale_lo,
// k_dynres_min_scale_hi] (a sub-range of (0, 1]); NaN collapses to
// the default. The bounds live on QualityKnobs as
// the single source of truth shared with the CLI and the property tests.
[[nodiscard]] inline FloatKnobClamp clamp_dynres_min_scale(float raw, const QualityKnobs&  /*k*/) {
    FloatKnobClamp r;
    if (std::isnan(raw)) {
        r.value   = QualityKnobs{}.dynres_min_scale; // default 0.5
        r.clamped = true;
        return r;
    }
    const float c = glm::clamp(raw, heaven::QualityKnobs::k_dynres_min_scale_lo, heaven::QualityKnobs::k_dynres_min_scale_hi);
    r.clamped = (c != raw);
    r.value   = c;
    return r;
}

// Result of parsing a `--tone-mapper <name>` argument. Mirrors the reject
// semantics of apply_msaa_samples rather than the clamp helpers: an
// unrecognized name is REJECTED (not coerced to a neighbour) — the value falls
// back to the ACES Shipping_Look default, `rejected` is flagged, and the
// offending name is retained so the CLI can name it in a diagnostic.
// A recognized name yields {value, false, ""}.
struct ToneMapperParse {
    ToneMapperSelection value = ToneMapperSelection::ACES;
    bool                rejected = false;
    std::string         rejected_name;
};

// Parse a tone-mapper name into a ToneMapperSelection. Pure (no I/O) so the CLI
// owns the diagnostic. Recognized names (case-insensitive, ignoring spaces /
// dashes / underscores): ACES, AgX, PBR Neutral, GT7, Generic. Any unrecognized
// string (including empty) returns ACES + rejected with the original name
// retained.
[[nodiscard]] inline ToneMapperParse parse_tone_mapper(std::string_view name) {
    ToneMapperParse result;

    // Normalize: lowercase and drop spaces / dashes / underscores so "PBR
    // Neutral", "pbr-neutral" and "pbrneutral" all match the same operator.
    std::string key;
    key.reserve(name.size());
    for (char ch : name) {
        if (ch == ' ' || ch == '-' || ch == '_') continue;
        key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }

    if (key == "aces") {
        result.value = ToneMapperSelection::ACES;
    } else if (key == "agx") {
        result.value = ToneMapperSelection::AgX;
    } else if (key == "pbrneutral" || key == "neutral") {
        result.value = ToneMapperSelection::PBRNeutral;
    } else if (key == "gt7") {
        result.value = ToneMapperSelection::GT7;
    } else if (key == "generic") {
        result.value = ToneMapperSelection::Generic;
    } else {
        // Unrecognized → ACES + rejected, retaining the original name so the CLI
        // can name it in a diagnostic.
        result.value         = ToneMapperSelection::ACES;
        result.rejected      = true;
        result.rejected_name = std::string(name);
    }
    return result;
}

// Pure mirror of filament::Camera::setExposure's photometric math (via
// Exposure::ev100 + Exposure::exposure), returning the image-brightness
// multiplier the renderer applies to scene luminance:
//   EV100      = log2(aperture^2 / shutter * 100 / iso)
//   brightness = 1 / (1.2 * 2^EV100)   ==   (shutter * iso) / (120 * aperture^2)
// Returning the brightness multiplier (not the raw EV) lets the monotonicity
// contract read directly in image-brightness terms: increasing
// the sensitivity (ISO) or the shutter speed (longer exposure) does not decrease
// brightness, and increasing the aperture f-number does not increase it.
[[nodiscard]] inline float exposure_value(float aperture, float shutter, float iso) {
    const float ev100 = std::log2((aperture * aperture) / shutter * 100.0f / iso);
    return 1.0f / (1.2f * std::exp2(ev100));
}

// Depth-of-field circle-of-confusion / blur proxy as a function of the aperture
// f-number. The blur diameter scales with the physical aperture diameter
// (focal_length / f_number), so the metric is inversely proportional to the
// f-number: a wider aperture (smaller f-number) yields a larger blur (shallower
// DOF) than a narrower one, i.e. the metric is strictly decreasing in the
// f-number over positive inputs. The constant is arbitrary — this is
// a relative metric, only its monotonicity matters. Non-positive / NaN
// f-numbers collapse to 0 (no blur).
[[nodiscard]] inline float dof_metric(float f_number) {
    if (std::isnan(f_number) || f_number <= 0.0f) return 0.0f;
    return 1.0f / f_number;
}

// Specular-AA variance/threshold pair produced by the strength ladder.
struct SpecAaStrength {
    float variance  = 0.0f;
    float threshold = 0.0f;
};

// Specular-AA strength ladder. Interpolates the variance and threshold from the
// QualityKnobs *_min endpoints (Low) to the *_max endpoints (High), with the
// Medium level at the midpoint. Strictly increasing in `level` (both endpoint
// pairs satisfy max > min) and bounded by the configured maximum strength
// The endpoints live on QualityKnobs as the single source of
// truth shared by MaterialInstanceCache and the property tests. The
// level is clamped to {Low, Medium, High} so an out-of-range raw value cannot
// over/under-shoot the ladder.
[[nodiscard]] inline SpecAaStrength spec_aa_strength(QualityLevel level) {
    const int l = glm::clamp(static_cast<int>(level), 0, 2);
    const float t = static_cast<float>(l) / 2.0f; // 0.0 (Low) → 1.0 (High)

    SpecAaStrength s;
    s.variance  = glm::mix(QualityKnobs::k_spec_aa_variance_min,
                           QualityKnobs::k_spec_aa_variance_max, t);
    s.threshold = glm::mix(QualityKnobs::k_spec_aa_threshold_min,
                           QualityKnobs::k_spec_aa_threshold_max, t);
    // Defensively bound by the configured maximum strength (the interpolation
    // never exceeds it for l ∈ {0,1,2}, but this pins  unconditionally).
    s.variance  = std::min(s.variance,  QualityKnobs::k_spec_aa_variance_max);
    s.threshold = std::min(s.threshold, QualityKnobs::k_spec_aa_threshold_max);
    return s;
}

struct AppConfig {
    std::string        data_dir         = "assets";
    // Compatibility default: schema-v1 archives and existing callers load the
    // exact authored DDS files unless a user/config explicitly selects a
    // generated texture tier. The repository config opts into Ultra4K.
    TextureQuality     texture_quality  = TextureQuality::Original;
    std::string        node_file;
    // Small fixtures use the normal renderer. Procedural scenes need no Heaven
    // assets; asset-* scenes retain authored nodes, meshes and material inputs.
    // Empty selects the authored Heaven scene. Rendering presets never own it.
    std::string        render_test_scene;
    int                width            = 1920;
    int                height           = 1080;
    bool               fullscreen       = false;
    // Android preview submission cap only; scored runs and captures remain
    // uncapped. Zero disables it. Desktop retains these config fields but does
    // not apply preview pacing or an Android Surface render scale.
#ifdef __ANDROID__
    int                android_preview_fps = 30;
#else
    int                android_preview_fps = 0;
#endif
    float              android_render_scale = 1.0f;
    // HDR display output (--hdr). When set, the renderer requests an HDR10
    // (PQ / ST.2084, Rec.2020) swap chain and runs a PQ output-encode pass
    // instead of the SDR ACES tonemap. Requires the native Wayland WSI; the
    // window-creation path forces SDL_VIDEODRIVER=wayland and fails fast if a
    // Wayland surface is unavailable. Off by default — SDR output is unchanged.
    bool               hdr              = false;
    // Peak display luminance (nits) the PQ output pass maps scene white to.
    // Tunable via --hdr-nits; 1000 is a common HDR10 mastering target.
    float              hdr_peak_nits    = 1000.0f;
    // HDR "paper white" / reference luminance (nits) that a GT7 display-referred
    // value of 1.0 maps to. This is the dominant brightness control: GT7's own
    // reference is 100 cd/m^2, which renders dimmer than the SDR image shown at
    // a typical (boosted) HDR-mode SDR brightness, so we default higher to make
    // HDR roughly match SDR perceived brightness. Tunable via --hdr-paper-white.
    float              hdr_paper_white_nits = 400.0f;
    std::string        shader_dir       = "benchmark/shaders";
    TessellationConfig tessellation     = {};

    // Time of day in hours [0, 24). Drives sun direction, sun colour, sky
    // colour, and ambient intensity. The original Heaven benchmark exposed
    // this as a slider in its UI; we expose it as `--time-of-day H` on the
    // command line. 12.0 = midday, 18.0 = sunset, 0.0 = midnight.
    float              time_of_day      = 18.0f;

    // Requested bloom strength; the existing day/night curve is normalized to
    // this default. Zero suppresses bloom without changing emission or lighting.
    float              bloom_strength = render_tuning::lighting.bloom_reference_strength;

    // Heat-haze refraction at chimney mouths. Costs an extra
    // RGBA16F scene capture + composite pass per frame, so users
    // running in benchmark mode may want to disable it via
    // `--no-heat-haze`. On by default; enabling/disabling is
    // permanent for the run (no live toggle).

    // Retained as a standalone field for the pre-feature-framework
    // call sites; `--no-heat-haze` keeps it and `features.heat_haze`
    // in sync. New code should read `features.heat_haze`.
    bool               heat_haze_enabled = true;

    // Per-feature on/off switches for the optional render modules
    // (grass, particles, god rays, fog, clouds, sky, lens flares,
    // heat haze). Driven by `--no-<feature>` flags today, by a
    // settings UI later.
    FeatureToggles     features = {};

    // Weather system configuration — initial rain intensity, dry-down
    // duration, and the overcast/wetness response endpoints. Driven by
    // `--rain-intensity` / `--dry-down-seconds` on the command line.
    // Defaults to medium rain (rain_intensity 0.5) by product decision, so
    // the default run starts the storm; `--rain-intensity 0` (or `--no-rain`)
    // restores the clear-weather Shipping_Look.
    WeatherConfig      weather = {};

    // Background music configuration — whether music plays and at what volume.
    // Driven by `--music` / `--no-music` / `--music-volume` on the command
    // line. Defaults to enabled at full volume. The Music_Player
    // is GPU-free infrastructure started in Application::init(); the packer
    // path never constructs it.
    MusicConfig        music = {};

    // Advanced-rendering feature quality/scale knobs (soft-shadow level,
    // contact-shadow steps, point-light shadow budget, MSAA samples, cloud
    // raymarch steps, SSGI samples, foliage density). Bounds live in the
    // struct as the single source of truth shared by the CLI and the property
    // tests. Driven by the `--<feature>-quality` flag family on the command
    // line (see apply_quality_args).
    QualityKnobs       quality = {};

    // ──  — advanced-materials-and-grading config ──
    // SDR tone-mapping operator, full color-grading controls,
    // and the physically-based camera. All default to their neutral /
    // Shipping_Look values so a flagless run is bit-identical (≤1/255). Driven
    // by `--tone-mapper`, the `--grading-*` / `--white-balance` flags, and the
    // `--aperture` / `--shutter` / `--iso` flags respectively.
    ToneMapperSelection  tone_mapper     = ToneMapperSelection::ACES;
    ColorGradingConfig   grading         = {};
    PhysicalCameraConfig physical_camera = {};

    // Preset identity is canonical and machine-readable. The deprecated label
    // below is retained only as a read/CLI compatibility alias.
    RenderPreset       render_preset    = RenderPreset::BalancedSystem;
    MaterialWorkflow  material_workflow =
        MaterialWorkflow::MetallicRoughnessCompat;
    CpuStressConfig   cpu_stress        = {};

    // Cloth wind debug arrows — render a coloured cuboid at every
    // flag's anchor centroid pointing along the world-space wind
    // vector. Off by default; enable with `--wind-debug`.
    bool               wind_debug = false;

    // Cloth simulation rate (Hz). Default 60. Set lower to slow
    // the simulator for visual debugging — e.g. `--cloth-rate 1`
    // runs one PBD step per second so individual force impulses
    // can be inspected frame-by-frame.
    float              cloth_sim_hz = 60.0f;

    // Human-readable quality-preset name shown in the benchmark Result_Report's
    // Settings block (Quality_Preset_Label). No preset-name
    // field existed before; set via `--quality-preset <name>`. The default is a
    // non-empty placeholder ("Custom") so the run-info formatter never emits a
    // blank line; an explicitly empty label maps to the formatter's
    // Unknown_Value_Placeholder.
    std::string        quality_preset_label = "Balanced System";
};

// ── Utility: convert Unigine row-major mat4 to glm::mat4 ───────

[[nodiscard]] inline glm::mat4 unigine_to_glm(const float m[16]) {
    // Unigine row-major with row-vector convention is equivalent to
    // GLM column-major with column-vector convention WITHOUT transposing.
    return glm::make_mat4(m);
}

} // namespace heaven

#endif // HEAVENBENCH_SRC_ENGINE_TYPES_H_
