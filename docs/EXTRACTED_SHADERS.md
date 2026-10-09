# Extracted Heaven Benchmark Shaders

This document analyses the **shipped** GPU shaders extracted from the original
Unigine Heaven Benchmark (Engine 1.x). They live entirely under the **core**
asset archive and there are **no shaders inside the heaven scene archive** — the
scene only references material *families* whose definitions are in `heaven/.../*.mat`,
and those materials in turn reference the core shader programs documented here.

> Source: `extracted/core/core/shaders/default/`
> File counts: 303 `.shader` programs and 189 `.h` includes (492 files total),
> covering 13 subsystems.

This is reverse-engineering documentation. None of these shaders are used by
our Filament backend — they are kept as a reference for what the original
benchmark draws and how, while we re-implement the visual features against
Filament's PBR pipeline. See `docs/RENDERING_FEATURES.md` for the renderer-side
status of each effect.

---

## 1. Shader language and dispatch

Every shader is a single source file that compiles to **four** graphics APIs
through preprocessor branching. Every `.shader` file is structured as:

```
#include <core/shaders/default/common/...base.h>

#ifdef OPENGL
    /* GLSL 1.50 (OpenGL 3.2+ core) source */
#elif DIRECT3D11
    /* HLSL Shader Model 5 source */
#elif DIRECT3D9
    /* HLSL Shader Model 3 source */
#elif PLAYSTATION3
    /* Cg / RSX source */
#endif
```

The four blocks are functionally equivalent reformulations of the same
algorithm — they exist because the asset pipeline picks one based on the
target API and feeds it (after macro expansion and `#include` resolution) to
the API's compiler. Heaven shipped working code paths for all four; the
PS3 path is the most diverged because of RSX-specific intrinsics
(`h4tex2D`, `texCUBE`, `WPOS`).

### Type aliases

`common/common_base.h` aliases types so the same body works in GLSL and HLSL:

```glsl
#define half     float
#define half2    vec2     // (HLSL keeps half/half2 native)
#define half4x4  mat4
#define float3x3 mat3
#define lerp(X,Y,V) mix(X,Y,V)
#define rsqrt(V)    inversesqrt(V)
#define saturate(V) clamp(V,0.0f,1.0f)
#define frac(V)     fract(V)
#define fmod(X,Y)   mod(X,Y)
```

Function-call versions of `mul3()` and `mul4()` wrap GLSL row-vs-column-major
differences so the rest of the code can be written identically.

### Pre-processor specialisation

Material variants are not separate files; they are compiled with different
`#define`s that the engine sets per draw. The heaviest fan-out you'll see is
the mesh subsystem, where `fragment_base_ambient.shader` alone branches on:

- Pass class: `OPACITY` / `TRANSPARENT` / `LIGHTMAP`
- Optional features: `EMISSION`, `USE_ENVIRONMENT`, `USE_OCCLUSION`,
  `USE_DEFERRED`, `USE_TESSELLATION` × `TESSELLATION`, `USE_ALPHA_FADE` ×
  `ALPHA_FADE`, `USE_PARALLAX` × `PARALLAX`
- Quality: `QUALITY_MEDIUM`, `QUALITY_HIGH`
- MSAA: `MULTISAMPLE_0/2/4/8/16` (chooses `sampler2D` vs `sampler2DMS`)
- API extensions: `USE_ARB_SAMPLE_SHADING`, `HAS_ARB_GPU_SHADER5`,
  `USE_ARB_UNIFORM_BUFFER_OBJECT`, `USE_ARB_SHADER_BIT_ENCODING`

Each combination becomes a separate compiled program in the engine's shader
cache. This is why Heaven's main rendering loop owns hundreds of GPU
programs — the `.shader` file count is small but each file is a *family*.

### Shared uniform schema

`common/common_base.h` declares the canonical uniform set every program can
reach. Names that start with `s_` are engine-driven, the rest are material
parameters. The interesting ones for the benchmark renderer are:

| Group | Uniforms | Notes |
|-------|----------|-------|
| Camera | `s_projection`, `s_imodelview`, `s_camera_position`, `s_viewport`, `s_depth_range` | `s_imodelview` is camera-space → world-space |
| Transforms | `s_transform[3]` | Per-instance row-vector transform; with `USE_INSTANCING` the array becomes `s_instances[96]` (32 instances) |
| Lighting | `s_light_position/direction/color/iradius`, `s_light_environment[9]` | SH-9 lookup `s_environment[9]` is also used for ambient |
| Material | `s_material_textures[16]`, `s_material_shading`, `s_material_fresnel`, `s_material_phong_rim`, `s_material_anisotropy` | The 16-entry texture array has each slot's `(width, height, 1/width, 1/height)` |
| Tessellation | `s_material_tessellation_factor`, `s_material_tessellation_distance` | Drives both control and evaluate stages (see §6) |
| Animation | `s_material_animation_stem`, `s_material_animation_leaf`, `s_material_animation_wind` | Drives wind/sway in vertex shaders for grass and leaves |
| Spacers | `s_spacer_num_spacers`, `s_spacer_parameters[8]`, `s_spacer_transforms[24]` | Up to 8 box/sphere keep-out volumes that fade out grass/water |

D3D11 wraps these in `cbuffer`s (`shader_common_parameters`,
`shader_camera_parameters`, `shader_material_parameters`, …); GL/D3D9/PS3
declare them as flat `uniform`s at file scope. The names are identical, only
the wrapping differs.

### Texture-slot convention

Slots 0–8 are material textures; 9–15 are framebuffer/shadow/light textures
the engine binds for the current pass. From the comment headers in the mesh
shaders:

| Slot | Role |
|------|------|
| 0 | Diffuse |
| 1 | Normal |
| 2 | Specular |
| 3 | Detail diffuse |
| 4 | Detail normal |
| 5 | Detail specular |
| 9 | Alpha-fade noise |
| 10 | Deferred parallax |
| 11 | Emission |
| 12 | Ambient cube / deferred ambient (depends on pass) |
| 13 | Deferred ambient / shadow color |
| 14 | Lightmap / shadow depth |
| 15 | Deferred light / shadow depth |
| 16 | (vertex-side) Tessellation height texture |

The heightmap texture being on slot 16 *for the vertex stage only* is why
`vertex_base.shader` declares `uniform sampler2D s_texture_16;` and samples
it via `textureLod(s_texture_16, texcoord.xy, 0.0f)`.

---

## 2. Subsystems at a glance

| Directory | Files | What it draws |
|-----------|------:|---------------|
| `mesh/`        | 147 | Static and skinned meshes (the workhorse): airship, dragon, fort, props |
| `render/`      |  83 | Full-screen passes: HDR, godrays, DoF, SSAO, glow, occlusion, deferred |
| `grass/`       |  39 | Grass blade and impostor rendering with wind animation |
| `post/`        |  36 | Post-processing filters: AA, scattering, Sobel, stereo, blur, wet-screen |
| `water/`       |  30 | Water surface and volume (waves, foam, caps, refraction, reflection) |
| `common/`      |  26 | Shared headers (`common_base.h`, `fragment_base.h`, shadow sampling) |
| `particles/`   |  22 | Particle quads (smoke, sparks, volumes), animation flipbooks |
| `volume/`      |  18 | VolumeBox/Sphere/Omni/Proj fog and god-ray volumes |
| `billboards/`  |  15 | Tree-card and screen-aligned billboards |
| `terrain/`     |  14 | Terrain heightfield (patch / surface / coarse LODs) |
| `gui/`         |  12 | UI text and panels |
| `decal/`       |  11 | Projected decals onto deferred normals |
| `sky/`         |   6 | Sky sphere, volume sky, world-shadow sky |

**492 files total.**

The legacy benchmark renderer in `benchmark/shaders/` is a much smaller
hand-rolled subset (mesh, grass, sky, clouds, godrays, ssao, dof, bloom,
tonemap) that recreates the *visible* subset of these shaders — Filament
materials in `benchmark/materials/` now own that responsibility.

---

## 3. Material model

Heaven uses a classic **multi-pass forward + deferred-ambient hybrid**, not a
modern single-pass PBR pipeline:

1. **Ambient pass** — `fragment_base_ambient.shader`: emission, hemisphere
   ambient, optional environment cube, optional baked lightmap, plus a
   contribution from a pre-rendered SSAO buffer.
2. **One light pass per dynamic light**, blended additively:
   - `fragment_base_light_world.shader` — directional sun (with PCF cascades).
   - `fragment_base_light_omni.shader` — point lights (with cube shadows).
   - `fragment_base_light_spot.shader` — spotlights (with 2D shadows).
   - `fragment_base_light_proj.shader` — projected texture / "video" lights.
   - `fragment_base_light_prob.shader` — light probes (uses SH-9).
3. **Deferred buffers** are filled in parallel by `*_deferred.shader`,
   `*_auxiliary.shader`, and `*_velocity.shader` programs; they feed the
   later post-processing passes.

The shading library that all light passes call into lives in
`common/fragment_base_shading.h`. It is essentially the BRDF kernel:

```hlsl
half3 getDiffuse(...);
half3 getSpecular(...);
half3 getDiffuseSpecularNormalize(...);
half3 getDiffuseSpecularFresnelNormalize(...);
half3 getDiffuseRimSpecularFresnelNormalize(...);  // Phong-rim term
half3 getDiffuseSpecularNormalizeAttenuate(...);   // for omni/spot
half  getFresnel(half3 fresnel, half3 normal, half3 camera_direction);
half  getAttenuation(half radius);
half3 getEnvironment(half3 direction);             // SH-9 evaluation
half3 getDiffuseSpecularDeferredAmbient(...);
```

This is **not** the GGX/Disney BRDF Filament uses. It's classic Blinn–Phong
with a Fresnel-modulated specular and an order-2 spherical-harmonic
environment lookup. When porting visuals to Filament, those `s_environment[9]`
SH coefficients map onto Filament's IBL diffuse irradiance and the rim/Fresnel
parameters map onto Filament's `clearCoat` and `sheen`.

### Mesh material families

The mesh subsystem ships **eight** texturing variants. Each variant has a
matching set of `vertex_*` / `fragment_*_ambient` / `fragment_*_light_*` /
`fragment_*_shadow` / `fragment_*_deferred` files, plus `_sample.h` /
`_light.h` includes that the family's `main()` pulls in. Heaven's `.mat`
files reference these by name through their `parent="..."` chain (e.g.
Heaven's `heaven_base` has parent `mesh_base`, `heaven_leaves_base` has
parent `mesh_leaf_base`, `heaven_metal_base` has parent
`mesh_paint_reflection_cube_base`).

| Family | What it adds on top of `base` | Used in Heaven for |
|--------|------------------------------|---------------------|
| `base`        | Plain diffuse + normal + spec | Most static props |
| `leaf`        | Two-sided foliage with billboard pivot, leaf wiggle, alpha-test | Trees, branches |
| `noise`       | Detail noise modulation       | Stone/rock variation |
| `lut`         | LUT-based shading             | Stylised surfaces |
| `paint`       | Cube-mapped reflection layer  | Metal / car-paint |
| `layer`       | Multi-texture blending        | Layered ground |
| `terrain`     | Height-blended channels       | Terrain surfaces |
| `triplanar`   | World-axis triplanar mapping  | Cliff / rock |
| `overlap`     | Stamp-style overlay           | Detail wear / dirt |
| `refraction`  | Screen-space refraction       | Glass, ice |

The "ambient + reflection" sub-variants (`fragment_base_ambient_reflection_2d`,
`_cube`, `_fresnel`, `_indirect`) are alternate ambient passes that fold a
local reflection probe into the ambient term.

### Skinning (`vertex_base.shader`, `MESH_SKINNED` branch)

Two skinning paths sit behind the same input layout:

- **Linear blend skinning** (default): four bone weights, each bone is
  encoded as a quaternion + translation. Vertex positions, normals, and
  tangents are transformed by `Σ wᵢ * Tᵢ`.
- **Dual-quaternion skinning** (`USE_DUAL_QUATERNIONS`): blends the
  rotation/translation pair with sign correction (`sign(dot(q0, q2))`).
  This avoids candy-wrapper artefacts on bent joints and is what Heaven
  uses for the dragon.

Both paths share the same `SKINNING(SWIZZLE,OP)` macro for unrolled bone
contributions, and `QUALITY_MEDIUM` selects between 2-bone and 4-bone
blending.

In the renderer we don't run this on the GPU at all — `engine/skinned_mesh.cpp`
does the LBS on the CPU and pushes vertices through
`IRhiBackend::update_mesh_vertices()` each frame.

---

## 4. Tessellation pipeline

The mesh subsystem includes **all four** tessellation stages so that displacement
maps can add geometric detail at runtime. Filament cannot run these (no
tessellation-stage support), which is why `engine/cpu_tessellation.cpp`
exists.

```
vertex_base.shader      — emits per-vertex tess factor:
                            f = pow(saturate(1 - (dist - tess_dist.x) * tess_dist.y), tess.w)
                                 * texLod(heightmap, uv, 0).x * tess.z
                          stored in s_texcoord_9.w (clamped 1..15)

control_base.shader     — TCS / Hull. Per-edge:
                            f_edge = (factor[i] + factor[j]) * saturate(edge_len * tess_dist.z)
                          early-cull: if all 3 verts project to outside [-1.2, 1.2]² in
                          NDC, set all factors to 0 (skip patch entirely).
                          Inside factor = mean of three edges.
                          maxtessfactor = 15.0; partitioning = fractional_odd; ccw triangles.

evaluate_base.shader    — TES / Domain. Barycentric blend, then:
                            lod    = clamp((dist - tess_dist.x) * tess_dist.y, 0, 8)
                            offset = pow(saturate(1 - …), tess.y) *
                                     (heightmap.SampleLevel(uv, lod).x - 127/255) * tess.x
                            vertex.xyz += normal * offset
                          Note the bias of 127/255 — the height texture is signed.

fragment_*.shader       — uses the displaced position + normal as usual.
```

The evaluate-stage displacement is what we replicate offline in
`cpu_tessellation.cpp` (subdivision level 3 for `_h` height maps with
`displacement_scale = 0.3`, level 4 for `_t` tessellation maps with
scale 1.0). Foliage is excluded for the same reason Heaven distance-fades
its tessellation factor — pushing flat billboards along their normal looks
ridiculous.

---

## 5. Lighting and shadowing

The light passes are classic forward shading; the interesting part is
**how shadows are sampled**. The PCF helpers live in `common/fragment_base.h`
and look like:

```
SAMPLER_SHADOW_WORLD_PCF        // 4-cascade directional shadow setup
SHADOW_WORLD_PCF(color)         // applies PCF cascade compare to `color`
shadow2DLinear(...)             // fall-back manual 2×2 PCF
shadow2DArrayLinear(...)        // for cascade arrays on D3D11/GL
```

`fragment_base_shadow_world.h`, `fragment_base_shadow_omni.h`, and
`fragment_base_shadow_proj.h` each define a `SHADOW_*` macro the light pass
calls right before final output.

Shadow noise (jittered offset to break PCF banding) comes from `s_texture_14`
in the omni/proj passes — that's the grayscale noise texture Heaven ships in
`extracted/core/core/textures/`.

Heaven uses a 9-coefficient SH expansion for its environment lighting (both
ambient and per-light, with a separate `s_light_environment[9]`). The
mathematical layout in `getEnvironment()` is:

```
L(d) = max(  E0
           - E1·d.y                + E2·d.z + E3·d.x
           + E4·(d.x·d.y) + E5·(d.y·d.z) + E6·(d.z·d.x)
           + E7·(3·d.z² - 1) + E8·(d.x² - d.y²),
           0)
```

— that's the standard order-2 SH basis (constant, three linear, five
quadratic). When porting to Filament, this maps cleanly onto a 3×3 SH
representation in the IBL `IndirectLight`.

---

## 6. Deferred-buffer encoding

`common/deferred_base.h` encodes/decodes the G-buffer slots Heaven uses.
This is *not* a wide-channel modern G-buffer — it's a tight 8-bit RGBA
packing scheme:

### Depth

Encoded across **3 bytes** of `r,g,b` and 1 bit of `r` reserved for the
volumetric flag (when MSAA > 0):

```
distance' = sqrt(distance) * 8388608        // 24-bit fixed-point
deferred.x = floor(distance' / 65536)
deferred.y = floor((distance' - x*65536) / 256)
deferred.z = floor(distance' - x*65536 - y*256)
deferred = (127, 255, 255) - deferred       // store as one's-complement
deferred.w = volumetric                      // (or top bit of x if MSAA)
```

The reverse uses `(16711680, 65280, 255) / 8388608` as the dot-product factor.
Storing distance squared ahead of `sqrt()` gains precision near the camera.

### Color, normal, parallax

```
setDeferredColor(color, glow)     => (color.rgb, glow / 16)
setDeferredNormal(normal, power)  => (normal*0.5 + 0.5, power / 64)
setDeferredParallax(offset.xy)    => ((offset*0.25+0.5).xy split into hi/lo bytes per channel)
```

The fact that `glow` is stored at `w / 16` and `specular_power` at
`w / 64` means both fit in 4-bit precision — fine for what they do, but it's
why Heaven's bloom and specular look quantized at low quality settings.

---

## 7. Post-processing chain

`render/` and `post/` together implement Heaven's full post chain. The
canonical full-quality sequence (reconstructed from filename order and the
shaders' `s_texture_*` slot inputs) is:

1. **Geometry pass** — fills HDR color + deferred buffers (`fragment_*_ambient` + `*_deferred`).
2. **Light passes** — additive blend of `fragment_*_light_*` per light.
3. **Volumetric shadows / God rays** — `volume/fragment_box_shaft_light_world.shader`
   raymarches the world-shadow map (4–256 samples, choose via `SAMPLES_*`).
4. **Occlusion (SSAO)** — `render/fragment_occlusion_ambient.shader` +
   `_blur` (separable Gaussian) → fed into the ambient pass.
5. **HDR luminance** — `fragment_hdr_luminance` → `fragment_hdr_average`
   (1×1 reduction to `s_texture_x`) → `fragment_hdr_normalize` (auto-exposure).
6. **HDR bloom** — `fragment_hdr_bright` (threshold) → `fragment_hdr_blur`
   (separable) → `fragment_hdr_composite` (mix back).
7. **Lens flare** — `fragment_hdr_lens` + `fragment_hdr_cross`.
8. **DoF bokeh** — `fragment_hdr_bokeh` + a **geometry shader**
   `geometry_hdr_bokeh.shader` that turns each bright pixel into a sprite
   quad scaled by `circle-of-confusion radius`.
9. **HDR shaft** — `fragment_hdr_shaft.shader`: 32-tap radial march from the
   sun, attenuated by `attenuation^i` per step. This is the radial sun-rays
   you see through clouds.
10. **DoF blur** — `fragment_dof_blur` / `_extrude` / `_sample`.
11. **Scattering filter** — `post/fragment_filter_scattering.shader`:
    12-tap depth-aware blur in the auxiliary mask region, blends `direct_color`
    and `scattering_color`. This is Heaven's "fog/haze" pass.
12. **Antialiasing** — `post/fragment_filter_antialiasing.shader` (FXAA-style).
13. **Wet-screen, Sobel, stereo** — optional final filters.

The most distinctive Heaven shaders are the **HDR shaft** (god-ray) sampler
and the **scattering filter**, which is why they were the first effects
re-implemented in `benchmark/materials/heaven_godrays.mat` and
`benchmark/shaders/godrays.frag`.

### A worked example: HDR shaft (radial god rays)

```hlsl
// fragment_hdr_shaft.shader (D3D11 path, simplified)
[unroll] for(int i = 1; i < 32; i++) {
    value = s_texture_0.Sample(s_sampler_0, texcoord);
    shaft += value.xyz * (saturate(value.w - threshold) * w);
    texcoord += direction;        // step toward sun in screen space
    weight += w;
    w *= attenuation;             // exponential falloff per step
}
return half4(color.xyz * shaft * (1.0f / weight), 1.0f);
```

`direction` is a screen-space unit vector toward the sun, perturbed per pixel
by a noise sample (`s_texture_1`) to break the regularity of the 32-tap
march. `attenuation` is a tunable per-frame scalar (`< 1.0`) that controls
how far the rays reach.

---

## 8. Animation: grass, leaves, water

Everything that moves does so in the **vertex shader** by reading
`s_material_animation_*` uniforms. There is **no compute pass** for this
(pre-DX11 design).

### Grass (`grass/vertex_base.shader`, `ANIMATION` branch)

```hlsl
stem_scale = animation.stem.y * fade * max(blade_height, 0)
stem_angle = dot(world_xy, animation.stem.x.xx) + animation.stem.w
stem_offset = getAnimationOffset(stem_angle) * (stem_scale²)
vertex.xyz += s_material_animation_wind * (stem_scale²)   // bulk wind sway
vertex.xyz += transform_basis(stem_offset)                 // tip wobble
```

`getAnimationOffset(angle)` is a cheap deterministic 2-tap sine — the
`stem_scale²` scaling is the bend curve (bottom of the blade barely
moves, top moves a lot).

`fade` is a distance-falloff across `s_surface_distances.{xy}` and
`s_spacer_*` carve-outs (so footsteps and rope-bridges flatten the grass
locally). The renderer's `engine/grass_cpu.cpp` produces blade geometry once
and lets `heaven_grass.mat` drive this same animation per frame from a
single `time` uniform.

### Leaves (`mesh/vertex_leaf.shader`)

Two animation modes overlap:
- **Stem sway** — the same `s_material_animation_stem` formula as grass,
  applied to the whole branch.
- **Leaf wiggle** — a 2D rotation of the per-leaf tangent/binormal frame:
  ```
  leaf_time  = dot(local_pos, animation.leaf.x.xxx) + animation.leaf.z
  leaf_angle = sin(leaf_time) * animation.leaf.y
  tangent  =  T·cos(leaf_angle) − B·sin(leaf_angle)
  binormal =  T·sin(leaf_angle) + B·cos(leaf_angle)
  ```

`LEAFS_BILLBOARD` swaps the leaf normal to face the camera (always
`(0,0,1)` in eye-space), which is what makes fir/leaf cards feel volumetric.

### Water (`water/vertex_surface.shader`, `WAVES` branch)

Four Gerstner-style sine waves sum into a height + xy-normal offset:

```
phase = (dot(wave.xy, texcoord) + s_water_time) * wave.z
height += sin(phase) * wave.w * scale
normal_xy -= wave.xy * cos(phase) * wave.w * wave.z * scale
```

`scale` is a distance-based amplitude window: it ramps up between
`16 * near_clip` and a configurable `waves_radius`, so the horizon water is
flat and the foreground is animated.

`SPACER` further deforms the surface to avoid clipping into hulls and
piers — the same 8-volume system grass uses.

---

## 9. Sky and clouds

`sky/fragment_sphere_ambient.shader` is *the* Heaven sky shader. The cloud
layer is two cascaded 2D textures (`clouds_01`, `clouds_23`) modulated by two
cloud masks, blended over an optional cubemap background:

```hlsl
clouds_01 = max((clouds_0 + clouds_1 - mask_0 * threshold_01) * scale_01, 0)
clouds_23 = max((clouds_2 + clouds_3 - mask_1 * threshold_23) * scale_23, 0)

color  = clouds_01.rgb * min(clouds_01.a, 1)
color  = lerp(color, clouds_23.rgb, min(clouds_23.a, 1))
// then blended over cubemap background_0/1, scaled, in-scattering applied
```

This is the procedural cloud layer the renderer's `heaven_clouds.mat`
re-implements as a Filament unlit material.

`sky/fragment_volume_ambient.shader` is the lower-tier "volume" sky
(box-projected gradient — used inside enclosed scenes, not relevant for the
Heaven outdoor benchmark).

`sky/fragment_shadow_light_world.shader` injects the world-shadow term
into the sky so distant terrain casts onto cloud bottoms.

---

## 10. Mapping shipped shaders to our Filament backend

| Heaven shader (or family) | Our equivalent (Filament backend) |
|--------------------------|-----------------------------------|
| `mesh/fragment_base_*.shader` (lit family) | `benchmark/materials/heaven_lit.mat` |
| `mesh/fragment_leaf_*.shader` + `vertex_leaf.shader` | `benchmark/materials/heaven_lit_twosided.mat` (with surface-name foliage detection in `engine/material_rhi.cpp`) |
| `grass/fragment_base_*.shader` + `vertex_base.shader` | `benchmark/materials/heaven_grass.mat` (vertex anim in `materialVertex`) |
| `sky/fragment_sphere_ambient.shader` | `benchmark/materials/heaven_sky.mat` + `heaven_clouds.mat` |
| `volume/fragment_box_shaft_light_world.shader` + `render/fragment_volumetric_shadow.shader` + `render/fragment_hdr_shaft.shader` | `benchmark/materials/heaven_godrays.mat` (separate depth `View`, see `rhi/filament_backend.cpp`) |
| `mesh/control_base.shader` + `mesh/evaluate_base.shader` | `engine/cpu_tessellation.cpp` (offline subdivision) |
| Various `*_unlit*` paths | `benchmark/materials/heaven_unlit.mat` |
| `common/fragment_base_shading.h` (Blinn-Phong) | Filament's stock GGX BRDF — rendered values won't match exactly |

### Things we have **not** re-implemented

The following Heaven shader subsystems do not currently have Filament
equivalents because the scene Heaven loads doesn't use them visibly, or
because Filament covers their function natively:

- Decals (`decal/`) — Heaven uses a few footprint decals; the benchmark
  scene we render doesn't.
- Particles (`particles/`) — Heaven's smoke / sparks. Filament has its own
  stylised particle path that we don't drive.
- Water (`water/`) — Heaven has surface and volumetric water materials, but
  the floating-island benchmark scene has no visible water bodies.
- Terrain (`terrain/`) — the original outdoor levels use this, but Heaven's
  scenes are floating-island geometry, not heightfield.
- Stereo / wet-screen / Sobel filters in `post/` — debug or showcase
  features that the benchmark mode doesn't enable.

---

## 11. Cross-references

- High-level migration plan: `docs/FILAMENT_MIGRATION_PLAN.md`
- Renderer feature checklist: `docs/RENDERING_FEATURES.md`
- Binary file formats (UNG / xml0 / mi08): `docs/FILE_FORMATS.md`
- Known visual / behavioural issues: `docs/KNOWN_BUGS.md`

For the *complete* shader inventory grouped by family, run:

```bash
find extracted/core/core/shaders/default -type f \( -name '*.shader' -o -name '*.h' \) | sort
```

For just the headline mesh fragment programs:

```bash
ls extracted/core/core/shaders/default/mesh/fragment_*.shader | sort
```
