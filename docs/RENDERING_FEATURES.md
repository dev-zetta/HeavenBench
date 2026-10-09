# Rendering Features Implementation Plan (historical)

The canonical current inventory, presets, fallbacks, and benchmark accounting
are documented in
[FILAMENT_RENDERING_FEATURES.md](FILAMENT_RENDERING_FEATURES.md).

Current environment lighting no longer uses the early single-mip/flat-probe
design described below: it prefilters four HDR time-of-day probes before
measurement. Lens dirt is implemented through Filament bloom and falls back
nonfatally when the extracted asset is absent.

Gap analysis between our current renderer and the original Unigine Heaven Benchmark.

## Current State

- Forward PBR: diffuse + normal + specular textures
- Single directional light (sun) with 4096² shadow map
- HDR rendering with 5-iteration Gaussian bloom
- ACES tonemapping + gamma correction
- Detail diffuse overlay blending
- Alpha test for cutout materials
- OpenGL 4.5 + Vulkan 1.3 backends via RHI abstraction

## Asset Inventory (extracted data)

| Type | Count | Description |
|------|-------|-------------|
| Diffuse (_d) | 86 | Base color/albedo |
| Normal (_n) | 65 | Tangent-space normal maps |
| Specular (_s) | 40 | Specular intensity |
| Height (_h) | 40 | Parallax/displacement depth |
| Tessellation (_t) | 15 | Hardware tessellation displacement |
| Emission (_e) | 5 | Self-illumination (lamps, windows, balloon) |
| Cubemap (_c) | 3 | Environment reflections (dragon, airship, environment) |
| Cloud | 4 | Cloud layer textures + masks |
| Grass | 4 | Procedural grass textures |
| SFX | 11 | Fire, smoke, sparks, lens dirt |
| Camera tracks | 55 | Benchmark camera paths |
| Skinned meshes | 4 | Mill wings, propeller, weathercock, gears |

## Implementation Phases

### Phase 1 — Quick Wins (highest visual impact per effort)

These features use existing texture data and require only shader changes.

- [x] **1.1 Emission maps** — Load `_e` textures, add emission channel to fragment shader. Additive contribution to final color. Affects: lamps, windows, doors, mill elements, airship balloon.
- [x] **1.2 Fresnel rim lighting** — Schlick approximation `F0 + (1-F0) * pow(1-NdotV, 5)`. Adds edge highlights to all surfaces, especially metallic ones.
- [x] **1.3 Cubemap reflections** — Load `_c` cubemap textures, sample with reflected view vector. Fresnel-weighted blend. Affects: dragon statue, airship metal, general environment.
- [x] **1.4 Sky rendering** — Procedural sky gradient (Rayleigh-like blue→white→horizon) instead of flat clear color. Sun disc. Renders as background behind all geometry.
- [x] **1.5 Improved bloom** — Superseded by preset-tiered Filament bloom
  (384/6, 512/8, or 1024/10) with high-quality upsampling and extracted
  `lens_dirt_d.dds`.

### Phase 2 — Major Visual Features

- [x] **2.1 SSAO** — Screen-space ambient occlusion. Separate post-process pass. Darkens crevices, corners, contact shadows. 24 samples with noise dithering + 4x4 blur.
- [x] **2.2 Parallax occlusion mapping** — Use `_h` height maps for per-pixel depth displacement. 40 materials benefit. Steep parallax with interpolation.
- [x] **2.3 Multiple light sources** — Two layers, both honouring the original benchmark's lighting model:
   1. *Authored scene lights*: parses `LightOmni` / `LightSpot` nodes from `heaven.world` and any `.node` files referenced from it. The world's only enabled authored light is `lstreet_light` — Heaven's other authored lamps (`lamp_post_X_light`, `blue_light`) ship disabled or were stripped before release. Lights tied to floating-island groups bob with the island animation.
   2. *Emission-derived lights*: 197 `POINT` lights spawned at every emissive surface (lamp glass, window, door, torch). All are point lights so light always spreads in every direction from the bulb — earlier spot-cone heuristics produced visible "cone stamp" artefacts on walls because the cone axis derived from `surface_center − mesh_center` is unreliable on gable walls. Lanterns and torches sit at the surface centre; windows and doors are pushed slightly outward laterally so they don't bury inside their own wall. The `lamp_glass` and `torch` surfaces also get a synthesised emissive material override (warm amber tint, scale 3.5) since our material database doesn't perform Heaven's day→night `building_glass_on` variant swap. The `lamp` metal frame intentionally does NOT emit — only the bulb glass does. All 4 mill animated parts and the floating islands track their parent transform per frame so lights don't drift.
- [x] **2.4 Atmospheric scattering** — Distance fog with height falloff and sun-tinted inscattering. Blends geometry toward sky color at distance.

### Phase 3 — Signature Features

- [x] **3.1 Hardware tessellation** — Adaptive tessellation with `_h` and `_t` displacement maps. Distance-based LOD up to 16x. TCS+TES shaders. 40 materials with height maps, 15 with dedicated tessellation maps.
- [x] **3.2 Depth of field** — Filament physical DOF driven dynamically by the original benchmark's nested `dof_control2.track` / `dof_control16.track` bindings. Camera playback evaluates the authored focus pulls and normalized f/2:f/16 preset blend per frame; the effective f-number uses the harmonic blend required by optical aperture strength, rather than linearly averaging f-numbers and weakening the blur. The complete original near/far focal ranges, blur ranges/radii, near power, and bokeh size are retained in `AuthoredDofState`; Filament currently approximates them with focus distance plus effective aperture because its built-in DOF does not expose the original asymmetric controls.
- [x] **3.3 Volumetric light shafts** — God rays from directional sun light. 64-sample radial blur from sun screen position. Depth-based occlusion. Half-resolution for performance. Composited additively in tonemap.
- [x] **3.4 Camera path playback** — Parse `.track` xml0 files for benchmark camera paths. Linear interpolation with quaternion slerp. 48 track segments, 48s loop. `--benchmark` flag or B key toggle.

### Phase 4 — Polish

- [x] **4.1 Procedural grass** — 8000 instanced grass blades on main island. Wind animation with dual-frequency sine waves. Distance fade (30-60 units). Per-blade random rotation and scale.
- [x] **4.2 Animated objects** — Full ms08 skinned mesh parser implemented. Loads bone hierarchy, per-frame bone transforms (position+scale+rotation quaternion), weighted vertices. Linear blend skinning (LBS) with bind-inverse matrices. All 4 animated objects loaded: mill wings (1 bone, 41 frames), weathercock (2 bones, 101 frames), lab gears (6 bones, 81 frames), port propeller (1 bone, 41 frames). Currently rendered at bind pose; animation playback infrastructure ready (`skin_interpolated()`).
- [x] **4.3 Particle effects** — 37 billboard particle emitters (25 smoke from chimneys, 12 fire from torches). Animated rising smoke with fade. Flickering fire with HDR bloom contribution. Instanced rendering with per-particle rotation and size.
- [x] **4.4 Lens effects** — Lens dirt overlay on bloom using `lens_dirt_d.dds`. Bloom-modulated dirt pattern.
- [x] **4.5 Cloud layer** — Animated cloud plane at height 200 using extracted cloud textures (clouds1_d, clouds2_d, clouds_mask0_d). Dual-layer scrolling with mask. Sun-tinted highlights. Alpha-blended above scene.
- [x] **4.6 Bridge physics (Jolt Physics)** — 3 rope bridge chains (6+18+11 segments) simulated with Jolt Physics rigid body engine. Each bridge segment is a dynamic rigid body connected to neighbors via dual PointConstraints (left/right railing attachment points). Endpoint segments are kinematic, tracking floating island positions. Gravity creates natural catenary sag. 170+ floating island instances animated with sine wave bobbing, bridge endpoints track per-group Z offsets.

### Phase 5 — 2026 Modernisation

Lifts the rendering quality from "2009 benchmark" to "current AA game" by adding the post-processing effects expected of a modern renderer.

- [x] **5.1 Temporal anti-aliasing (TAA)** — Replaces FXAA with Filament's TAA using a 16-sample Halton(2,3) jitter sequence and AABB-variance neighbourhood box clipping. Crushes the per-frame shimmer on grass blades and foliage billboards that single-frame edge filters cannot see, and the −1.0 LOD bias roughly doubles effective texturing resolution. `preventFlickering=true` keeps the airship's spinning propeller and grass-tip motion stable. Mild post-TAA sharpen (0.20) counters the slight softening.
- [x] **5.2 Screen-space reflections (SSR)** — Filament's built-in SSR with `maxDistance=25` and `stride=1.5` so reflections reach the dragon statue from the airship and the cobblestone road catches adjacent lamp pools. SSR samples the post-tonemap-pre-bloom HDR colour, so lamp glow contributes naturally. Cost is dwarfed by the existing render pipeline at the scene's scale.
- [x] **5.3 Volumetric fog** — Screen-space ray-march per pixel: 32 taps along the view ray from camera to scene depth, with a blue-noise jitter on the start offset (TAA averages out the dither across frames). At each tap, accumulates Henyey–Greenstein-phased in-scattering from the sun and up to 16 CPU-culled nearby lights, weighted by the running transmittance, with Beer–Lambert extinction along the ray. Lamp contributions are soft-clipped by `1 / (1 + 0.5·Σ att)` so dense clusters of lamps don't saturate to white. Properly handles Filament's reversed-Z depth and the Vulkan render-target Y-flip when sampling depth. Composites as transparent (mix sceneColor and fogColor by opacity), under god rays, so the haze appears beneath but bloom still produces lamp halos through it. Shares the depth pre-pass with god rays. Material: `benchmark/materials/heaven_volumetric_fog.mat`.

### Phase 6 — Weather System

A rain/weather system driven by a single `--rain-intensity` parameter (0.0–1.0, default 0.5 = medium rain, so the default run renders the storm; pass `--rain-intensity 0` to restore the clear Shipping_Look). Each effect is a toggleable `IRenderFeature`.

- [x] **6.1 Rain (`rain`)** — Renders falling precipitation streaks plus ground-impact splashes, driven by `--rain-intensity`; on by default at medium intensity (0.5) and renders nothing at intensity 0. Controlling flag: `--no-rain`.
- [x] **6.2 Wet reflections (`wet-reflections`)** — Renders wet-surface shading via the per-view wetness global plus retuned screen-space reflections while raining. Controlling flag: `--no-wet-reflections`.

### Phase 7 — Advanced Rendering Features

A coordinated set of advanced rendering features that spend the renderer's large
GPU headroom on modern realism plus deliberate, scalable load. Delivered in two
tiers (Tier 1 quick wins, Requirements 1–5; Tier 2 new techniques, Requirements
6–8). Each feature is an individually toggleable `IRenderFeature` or
View-option group, preserves the Shipping_Look when disabled, and — where its
GPU cost is otherwise unbounded — exposes a quality/scale knob. The entries below
record, for each feature, its name, what it renders, its controlling
command-line flag, and its quality knob (refraction has none).

- [x] **7.1 Soft directional shadows** — Renders the sun's directional shadows with a contact-hardening soft-shadow mode (PCSS/DPCF) whose penumbra widens with blocker-to-receiver distance, replacing the basic PCF mode. Controlling flag: `--no-soft-shadows` (on by default). Quality knob: `--soft-shadow-quality` (penumbra sample-count level).
- [x] **7.2 Screen-space contact shadows** — Renders tight screen-space ray-marched micro-shadows where small/near geometry (grass, props) meets receiving surfaces, grounding them. Controlling flag: `--no-contact-shadows` (on by default). Quality knob: `--contact-shadow-quality` (ray-march step-count level).
- [x] **7.3 Shadow-casting point/spot lights** — Renders local shadows from the brightest subset of the scene's authored and emission-derived point/spot lights at night, selected highest-intensity-first up to a budget. Controlling flag: `--no-point-light-shadows` (on by default). Quality knob: `--point-light-quality` (shadow-casting light budget).
- [x] **7.4 MSAA on top of TAA** — Renders multi-sample anti-aliasing layered over the existing TAA so hard geometric edges stay clean, adding scalable resolution-dependent load. Controlling flag: `--no-msaa` (on by default). Quality knob: `--msaa-quality` (sample count in {1,2,4}).
- [x] **7.5 Screen-space refraction for glass** — Renders windows, airship glass, and lab equipment as refractive surfaces that transmit and displace the scene behind them, replacing the opaque glass look. Controlling flag: `--no-refraction` (on by default). Quality knob: none.
- [x] **7.6 Raymarched volumetric clouds** — Renders a raymarched 3D cloud layer (worley/perlin noise + light scattering) with ground shadows, replacing the scrolling textured cloud quads. Controlling flag: `--no-volumetric-clouds` (on by default). Quality knob: `--cloud-quality` (raymarch step-count level).
- [x] **7.7 Screen-space global illumination (SSGI)** — Renders one-bounce indirect light reconstructed from the depth and color buffers, adding color bleeding and contact lighting to the ambient-dominated scene. The gather source is the **previous frame's lit color**, captured by the backend's `SceneColorCapture` (ping-pong RTs + blit) since SSGI composites inline in the main scene and cannot read the current frame. Controlling flag: `--no-ssgi` (on by default; when off the main view renders straight to the swap chain — no capture/blit, Shipping_Look parity). Quality knob: `--ssgi-quality` (sample/ray-count level).
- [x] **7.8 Massive foliage / GPU-driven geometry** — Renders grass scaled from the ~466k-blade baseline toward millions of blades plus added ground clutter and higher terrain tessellation for heavy, scalable geometry throughput. Controlling flag: `--no-massive-foliage` (on by default). Quality knob: `--foliage-quality` (foliage density multiplier).

### Village ground relief

The two confirmed village-gravel surfaces use a deterministic height derived
from their authored tangent-space normal map. CPU topology is bounded to five
levels, geometric relief is limited to 3 cm, and open edges fade to neutral
within 0.30 m so the ground continues to meet adjacent walls. Controlling flag:
`--no-ground-relief` (on by default; `--ground-relief` is the enable alias).

### Phase 8 — Advanced Materials, Shadows & Color Grading

A coordinated set of advanced material models, shadow refinements, and
post-processing controls that spend the renderer's GPU headroom on additional
realism and artistic control. Delivered in four groups (A material models, B
shadows & specular AA, C tone-mapping/grading/camera, D dynamic resolution).
By product decision the recommended subset (clear coat, anisotropic, subsurface,
cloth, transparent foliage shadows, colored penumbra, specular AA) now defaults
**ON** for the best visual look; each is disabled with `--no-<feature>` (and
keeps a bare `--<feature>` compat enable). The remaining capabilities (color
grading, physical camera, dynamic resolution) stay **OFF** by default and are
opted into with the bare `--<feature>` flag; ACES remains the default
tone-mapper. The ≤1/255 Shipping_Look parity now holds in the all-disabled
configuration (every `--no-<feature>` passed), not the default run. The entries
below record, for each capability, its name, what it renders, its controlling
command-line flag, and its quality knob where applicable.

#### Group A — Material models

- [x] **8.1 Clear coat** — Renders a second specular clear-coat lobe over coated surfaces (lacquered wood, varnished metal, painted props), adding a sharp secondary highlight on top of the base layer. Controlling flag: `--clear-coat` (on by default; disable with `--no-clear-coat`). Quality knob: none. Material: `heaven_lit_clearcoat`.
- [x] **8.2 Anisotropic** — Renders direction-dependent (brushed-metal) specular so highlights stretch along the surface grain on metals like the airship hull and lab equipment. Controlling flag: `--anisotropic` (on by default; disable with `--no-anisotropic`). Quality knob: none. Material: `heaven_lit_aniso`.
- [x] **8.3 Subsurface / translucency** — Renders light transmission through thin organic surfaces (leaves, cloth, wax), letting backlit geometry glow instead of reading as opaque. Controlling flag: `--subsurface` (on by default; disable with `--no-subsurface`). Quality knob: none. Material: `heaven_subsurface`.
- [x] **8.4 Cloth / sheen** — Renders the soft grazing-angle sheen of fabric (flags, banners, awnings) using Filament's cloth shading model. Controlling flag: `--cloth` (on by default; disable with `--no-cloth`). Quality knob: none. Material: `heaven_cloth`.

#### Group B — Shadows & specular AA

- [x] **8.5 Transparent foliage shadows** — Renders coverage-modulated, tinted shadows from alpha-tested foliage so leaf and branch shadows show their cutout silhouette instead of a solid blob. Controlling flag: `--transparent-shadows` (on by default; disable with `--no-transparent-shadows`). Quality knob: none. Material: `heaven_foliage_shadow`.
- [x] **8.6 Colored penumbra** — Renders a tinted soft-shadow penumbra (the colored-penumbra shading variant) so shadow edges pick up the diffuse colour of the occluder. Controlling flag: `--colored-penumbra` (on by default; disable with `--no-colored-penumbra`). Quality knob: none. Material: `heaven_lit_colored_penumbra`. The vendored standard/cloth shading blocks clamp the tint divisor with `max(1.0 - diffuseColor, vec3(1e-3))` to prevent Inf/NaN fireflies on near-white surfaces. Shader generation emits `HAS_COLORED_PENUMBRA` only when enabled, matching the shader's `#if defined()` guard.
- [x] **8.7 Specular AA** — Renders geometric specular anti-aliasing that narrows highlight variance on high-frequency normals, reducing the shimmer/fireflies on sharp metallic edges. Controlling flag: `--specular-aa` (on by default; disable with `--no-specular-aa`). Quality knob: `--specular-aa-quality` (strength level 0..2).

#### Group C — Tone-mapping, grading & camera

- [x] **8.8 Tone-mapper selection** — Renders the scene through a selectable SDR tone-mapping operator (`aces` default, `agx`, `pbr-neutral`, `gt7`, `generic`) instead of the fixed ACES curve. Controlling flag: `--tone-mapper` (name argument; ACES by default). Quality knob: none.
- [x] **8.9 Full color grading** — Renders the full color-grading chain (white balance, ASC CDL slope/offset/power, contrast, saturation) on the final image for artistic look control. Controlling flag: `--color-grading` (off by default). Quality knob: none. Parameters: `--grading-contrast`, `--grading-saturation`, `--white-balance`, `--grading-slope`, `--grading-offset`, `--grading-power`.
- [x] **8.10 Physical camera** — Renders with a physically-based camera, deriving exposure and standalone free-flight depth-of-field from aperture, shutter, and ISO. During benchmark camera playback, the authored track controls focus and effective DOF aperture while these settings continue to control exposure. Controlling flag: `--physical-camera` (off by default). Quality knob: none. Parameters: `--aperture`, `--shutter`, `--iso`.

#### Group D — Dynamic resolution

- [x] **8.11 Dynamic resolution + FSR** — Renders at a dynamically scaled internal resolution and upscales with FSR to hold frame-time under load, trading internal resolution for performance. Controlling flag: `--dynamic-resolution` (off by default). Quality knob: `--dynamic-resolution-quality` (minimum render scale in (0,1]).

### Phase 9 — GPU Instancing & Skinning

Moves two per-frame CPU workloads onto the GPU using Filament's native paths.
Both default **ON**; passing the `--no-...` flag restores the exact prior
CPU/per-instance path so the all-disabled configuration holds the `--benchmark`
parity bar (≤1/255). Each entry records the feature name, what it does, and its
controlling command-line flag.

- [x] **9.1 GPU instancing** — Draws repeated static meshes as Filament instanced renderables instead of one renderable per copy. Instances sharing a `mesh_index` are grouped (singletons and skinned meshes excluded) and chunked to Filament's automatic-instance limit (≈64); each chunk becomes one renderable per shadow-split subset, backed by a shared `InstanceBuffer` whose per-member local transforms are synced each frame. The depth-proxy builder instances the same groups in lockstep so the linear-depth silhouette matches the visible scene. The ~170 floating islands therefore collapse to a few instanced renderables per mesh. Controlling flag: `--no-gpu-instancing` (on by default; when off, falls back to one renderable per instance). Per-group fallback: if `getMaxAutomaticInstances() < 2` or an `InstanceBuffer` fails to build, that group renders via the per-instance path with a one-time diagnostic.
- [x] **9.2 GPU skinning** — Skins the four Animated_Objects (mill wings, weathercock, lab gears, port propeller) on the GPU via Filament's native skinning. Each is uploaded once as a static rest-pose buffer with `BONE_INDICES`/`BONE_WEIGHTS` attributes and rest-pose tangents; the renderable is built with `.skinning(boneCount)`, and per frame only the bone matrices (`SkinnedMesh::bone_matrices_interpolated`) are pushed via `update_skinned_bones`. This eliminates the per-frame CPU linear-blend skin, vertex re-upload, and tangent recompute. Controlling flag: `--no-gpu-skinning` (on by default; when off, restores the CPU `update_mesh_vertices` skinning path). Per-object fallback: on skinned-upload/enable failure or a >255-bone overflow, that object falls back to the CPU path with a one-time diagnostic.

## Priority Rationale

Phase 1 items were chosen because:
- They use textures already loaded but ignored (emission, cubemaps)
- They require only fragment shader changes (no new render passes)
- They have outsized visual impact (sky alone transforms the scene)
- The original screenshots show these features prominently

Phase 2 adds depth and realism that's immediately noticeable in side-by-side comparison. Phase 3 adds the features Heaven was famous for. Phase 4 is polish that makes the scene feel alive.
