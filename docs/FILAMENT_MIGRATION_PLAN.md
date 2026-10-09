# Filament Migration Plan

Migrate the HeavenBench renderer from our hand-rolled OpenGL 4.5 backend to
[Google Filament](https://github.com/google/filament) (already cloned at `lib/filament`).
Only the OpenGL path is in scope — the incomplete Vulkan backend (`vk_backend.cpp`,
`vk_backend_render.cpp`, `dds_vulkan.cpp`) will be removed.

---

## 1  Current Architecture Summary

| Layer | Files | Responsibility |
|-------|-------|----------------|
| **RHI** | `rhi.h`, `rhi.cpp`, `gl_backend.h/cpp`, `backend_factory.cpp` | Fat interface: each backend owns the full multi-pass pipeline (shadow → scene → bloom → tonemap). Opaque `uint64_t` handles for meshes/textures. |
| **Engine** | `mesh.h/cpp`, `shader.h/cpp`, `framebuffer.h/cpp`, `texture.h/cpp`, `material_rhi.h/cpp`, `scene_rhi.h/cpp`, `skinned_mesh.h/cpp`, `camera.h/cpp`, `window.h/cpp`, `rope_sim.h/cpp` | CPU-side data types (`Vertex`, `Mesh`, `Surface`, `MaterialDef`), mi08/ms08 loaders, DDS texture loading, Jolt physics bridge sim, camera path playback. |
| **Renderer** | `render_pipeline.h/cpp`, `scene_pass.h/cpp`, `shadow_pass.h/cpp`, `bloom_pass.h/cpp`, `tonemap_pass.h/cpp` | Thin wrappers around GL calls (used by the older non-RHI path; the RHI `GlBackend` inlines all passes). |
| **Shaders** | `benchmark/shaders/*.vert/frag/comp/tcs/tes` | 15+ GLSL 4.50 shaders: mesh, shadow, sky, clouds, grass (compute + render), particles, SSAO, bloom, god rays, tonemap, DOF, tessellation. |

### Render pipeline (GlBackend::render_frame)

1. **Shadow pass** — 4096² depth-only FBO, front-face culling
2. **HDR scene pass** — dual-attachment FBO (scene + bright), PBR forward shading
3. **Sky pass** — procedural Rayleigh-like gradient, fullscreen triangle
4. **Cloud layer** — dual-texture animated clouds, alpha blended
5. **Tessellation** — TCS/TES displacement for materials with `_h`/`_t` maps
6. **Grass pass** — compute shader generates blade geometry → SSBO → draw
7. **Particle pass** — instanced billboard quads (smoke + fire)
8. **SSAO pass** — half-res, 24-sample hemisphere kernel, 4×4 blur
9. **Bloom pass** — 5-iteration ping-pong Gaussian blur on bright pixels
10. **God rays pass** — half-res radial blur from projected sun position
11. **Tonemap pass** — ACES + bloom composite + SSAO + god rays + lens dirt
12. **DOF pass** — Poisson disc blur, CoC from linearized depth

---

## 2  What Filament Provides Out of the Box

Filament is a PBR-first engine with a well-defined material model and built-in
post-processing. The following features map directly to our current pipeline:

| Our feature | Filament equivalent | Notes |
|-------------|-------------------|-------|
| Shadow mapping (PCF) | `LightManager` + `View::setShadowType(PCF/PCSS/VSM)` | Cascaded shadow maps, configurable per-light |
| PBR forward shading | `Material` / `MaterialInstance` | Standard `lit` material model with baseColor, metallic, roughness, normal, AO, emissive |
| SSAO | `View::setAmbientOcclusionOptions` | SAO or GTAO, configurable quality/radius/intensity |
| Bloom | `View::setBloomOptions` | Multi-level, lens dirt texture, lens flare, configurable strength |
| Tone mapping | `ColorGrading` | ACES, filmic, linear, and custom operators |
| DOF | `View::setDepthOfFieldOptions` | Physically-based CoC, configurable ring count |
| Fog / atmosphere | `View::setFogOptions` | Height fog with sun in-scattering, IBL-colored |
| Skybox | `Skybox` class | Cubemap or procedural color |
| Normal mapping | Material `normal` property | Tangent-space, same as ours |
| Alpha masking | Material `maskThreshold` | Equivalent to our `alpha_ref` |
| Emission | Material `emissive` property | HDR emissive contribution |
| Environment reflections | `IndirectLight` + material `reflectance` | IBL-based, replaces our per-material cubemaps |
| Anti-aliasing | FXAA, TAA, MSAA | More options than our current MSAA-only |
| Screen-space reflections | `View::setScreenSpaceReflectionsOptions` | Not in our current pipeline — bonus |

### What Filament Does NOT Provide

| Our feature | Gap | Mitigation |
|-------------|-----|------------|
| **Hardware tessellation** (TCS/TES displacement) | Filament has no tessellation shader support | Pre-tessellate meshes offline, or use Filament custom materials with parallax occlusion mapping |
| **Compute shader grass** | Filament has limited compute support; no raw SSBO rendering | Port to CPU-side grass mesh generation, upload as `VertexBuffer`/`IndexBuffer` each frame, or use instanced quads |
| **Billboard particles** (smoke/fire) | No built-in particle system | Implement as instanced renderables with transparent `unlit` materials |
| **God rays** (radial blur) | No built-in volumetric/god-ray pass | Implement as a custom post-process pass using Filament's `RenderTarget` + custom material, or accept the loss |
| **Cloud layer** | No built-in cloud rendering | Render as a transparent fullscreen quad with a custom `unlit` material |
| **DDS texture loading** | Filament uses KTX/KTX2 natively | Convert DDS assets to KTX2 offline, or write a thin DDS→Filament `Texture` loader |
| **mi08/ms08 mesh format** | Filament uses filamesh or glTF | Keep our CPU-side loaders, upload via Filament `VertexBuffer`/`IndexBuffer` API |
| **Lens dirt overlay** | Filament `BloomOptions` has a `dirt` texture slot | Direct mapping — pass our `lens_dirt_d.dds` as the dirt texture |

---

## 3  Migration Phases

### Phase 0 — Build Integration & Scaffolding

**Goal:** Filament compiles as part of our build; a blank Filament window opens.

**Status: ✅ COMPLETE**

**Tasks:**

- [x] **0.1** Add Filament as a CMake subdirectory in `benchmark/CMakeLists.txt`:
  Filament is built with `FILAMENT_SKIP_SAMPLES`, `FILAMENT_SKIP_SDL2`,
  `FILAMENT_BUILD_TESTING OFF`, `FILAMENT_SUPPORTS_VULKAN OFF`, OpenGL only.
  C++ standard is set per-target (not globally) to avoid leaking C++23 into
  Filament's C++17 build. Linked against: `filament`, `backend`, `utils`, `math`,
  `geometry`, `filabridge`, `filaflat`.
  **Requires:** Clang 17+ with libc++ on Linux.

- [x] **0.2** Created `benchmark/src/rhi/filament_backend.h/cpp` — a new `IRhiBackend`
  implementation wrapping Filament's `Engine`, `Renderer`, `SwapChain`, `View`,
  `Scene`, and `Camera`. Includes:
  - Mesh upload via `VertexBuffer`/`IndexBuffer` with quaternion tangent conversion
    using Filament's `SurfaceOrientation` helper.
  - DDS texture loading via a new CPU-side loader (`dds_loader_cpu.h`) that decodes
    DDS to raw buffers, then uploads to Filament `Texture` objects (BC1/BC3/BC4/BC5
    compressed + RGBA8 uncompressed, with cubemap support).
  - Post-processing setup: bloom, SSAO, ACES tone mapping, fog.
  - Directional sun light via `LightManager`.
  - Solid-color skybox placeholder.

- [x] **0.3** Updated `backend_factory.cpp` to instantiate `FilamentBackend` when
  `GraphicsApi::Filament` is selected. Added `Filament` to the `GraphicsApi` enum.

- [x] **0.4** Updated `main.cpp` to accept `--api filament` and handle Filament's
  internal presentation (no `swap_buffers()` call needed). Updated `rhi.cpp` window
  creation to create an SDL window without a GL context for Filament (Filament
  creates its own).

**Files created:** `benchmark/src/rhi/filament_backend.h`,
`benchmark/src/rhi/filament_backend.cpp`, `benchmark/src/dds_loader_cpu.h`.

**Files modified:** `benchmark/CMakeLists.txt`, `benchmark/src/rhi/rhi.h`,
`benchmark/src/rhi/rhi.cpp`, `benchmark/src/rhi/backend_factory.cpp`,
`benchmark/src/main.cpp`, `benchmark/src/engine/scene_rhi.cpp`.

---

### Phase 1 — Static Geometry & Materials

**Goal:** Scene meshes render with correct PBR materials through Filament.

**Status: ✅ COMPLETE**

**Tasks:**

- [x] **1.1** Implement `FilamentBackend::upload_mesh()`:
  Maps our `Vertex` struct to Filament `VertexBuffer` (position+UV in buffer 0,
  quaternion tangents in buffer 1 via `SurfaceOrientation`). Creates `IndexBuffer`
  from uint32 index data. Each surface gets its own VB/IB pair.

- [x] **1.2** Create Filament material templates:
  Created `benchmark/materials/heaven_lit.mat` (opaque, single-sided) and
  `heaven_lit_twosided.mat` (masked blending, double-sided). Both expose:
  `baseColorMap`, `normalMap`, `specularMap`, `emissiveMap`, `detailMap`,
  `heightMap` with boolean enable flags. Specular maps are converted to
  roughness/metallic approximation. Normal maps handle ATI2/BC5 two-channel
  format. Detail diffuse uses overlay blending. Height maps contribute normal
  perturbation. Materials compiled with `matc` and embedded as C++ byte arrays
  in `benchmark/src/rhi/materials_data.h`.

- [x] **1.3** Implement `FilamentBackend::load_texture()`:
  CPU-side DDS loader (`dds_loader_cpu.h`) reads DDS to `std::vector<uint8_t>`
  per mip level. Supports BC1/BC3/BC4/BC5 compressed and RGBA8 uncompressed,
  with cubemap support. Uploads to Filament `Texture` objects.

- [x] **1.4** Implement material resolution in `FilamentBackend::render_frame()`:
  `get_or_create_material_instance()` creates and caches a `MaterialInstance`
  per surface name. Sets all texture parameters (diffuse, normal, specular,
  emission, detail, height) with proper sampler configuration (linear mipmap,
  repeat wrap, 8x anisotropy). Chooses between opaque and masked/two-sided
  material based on `GpuMaterial` flags. Placeholder 1×1 textures used for
  missing slots.

- [x] **1.5** Set up the Filament `Scene` and `Camera`:
  Camera entity created, directional sun light via `LightManager::Builder`,
  solid-color skybox, 3-band SH indirect light for ambient.

- [x] **1.6** Wire up `render_frame()`:
  Creates renderable entities per instance with material instances assigned,
  sets transforms via `TransformManager`, calls `beginFrame`/`render`/`endFrame`.
  Lighting and post-processing initialized once (not per-frame).

**Files created:** `benchmark/materials/heaven_lit.mat`,
`benchmark/materials/heaven_lit_twosided.mat`,
`benchmark/materials/heaven_lit.filamat`,
`benchmark/materials/heaven_lit_twosided.filamat`,
`benchmark/src/rhi/materials_data.h`.

**Files modified:** `benchmark/src/rhi/filament_backend.h`,
`benchmark/src/rhi/filament_backend.cpp`, `benchmark/CMakeLists.txt`.

**Validation:** Scene geometry renders with diffuse + normal + specular + emission
textures. Compare screenshots against the GL backend.

**Files touched:** `filament_backend.h/cpp`, new `materials/lit.mat`, `materials/lit_twosided.mat`.

---

### Phase 2 — Post-Processing Pipeline

**Goal:** Bloom, SSAO, tone mapping, DOF, and fog match the original look.

**Status: ✅ COMPLETE** (initial configuration done, tuning may be needed)

**Tasks:**

- [x] **2.1** Enable and configure SSAO:
  `AmbientOcclusionOptions` with radius=1.5, intensity=1.0, MEDIUM quality.

- [x] **2.2** Enable and configure bloom:
  `BloomOptions` with strength matching our `bloom_strength` param, 6 levels,
  threshold enabled, MEDIUM quality. Lens dirt texture support available via
  `BloomOptions::dirt` (not yet wired to our `lens_dirt_d.dds`).

- [x] **2.3** Set up tone mapping via `ColorGrading`:
  Uses `ACESLegacyToneMapper` with configurable exposure from `TonemapParams`.

- [x] **2.4** Enable and configure DOF:
  `DepthOfFieldOptions` is enabled during benchmark camera playback and follows
  the nested `dof_control2.track` / `dof_control16.track` focus and preset-blend
  channels. The complete original asymmetric near/far parameter set is parsed
  and retained; the current Filament path maps it to physical focus distance
  and effective aperture.

- [x] **2.5** Enable and configure fog:
  `FogOptions` with distance=100, density=0.005, height falloff, sky-tinted color.

**Validation:** Post-processing effects are visually comparable. Side-by-side
screenshot comparison with the GL backend.

**Files touched:** `filament_backend.cpp` (render_frame configuration).

---

### Phase 3 — Sky, Clouds & Environment Lighting

**Goal:** Procedural sky and cloud layer render correctly.

**Status: ✅ COMPLETE**

**Tasks:**

- [x] **3.1** Skybox: Solid-color skybox created via `Skybox::Builder`.

- [x] **3.2** `IndirectLight` for environment reflections: 3-band spherical harmonics
  configured with sky-like ambient color, intensity 30000 lux.

- [x] **3.3** Cloud layer: Full port of the original `clouds.vert`/`clouds.frag`.
  Created `heaven_clouds.mat` — a transparent, unlit material that:
  - Renders a fullscreen quad positioned at the camera's far plane
  - Passes the interpolated world position to the fragment shader via a `variable`
  - Reconstructs the view ray direction from the world position and camera position
  - Ray-plane intersects at cloud height (200 world units)
  - Samples two scrolling cloud textures (`clouds1_d.dds`, `clouds2_d.dds`)
    multiplied with a mask (`clouds_mask0_d.dds`) for density
  - Applies `smoothstep` density thresholding, horizon fade, sun-tinted
    highlights (`pow(sunAngle, 3)` blend), and underside darkening
  - Alpha-blended output at 70% max opacity
  - Updated per-frame with camera position, sun direction/color, and time
  - Quad repositioned each frame to track the camera

**Files created:** `benchmark/materials/heaven_clouds.mat`,
`benchmark/materials/heaven_clouds.filamat`.

---

### Phase 4 — Grass System

**Goal:** Procedural grass renders with wind animation.

**Status: ✅ COMPLETE**

**Tasks:**

- [x] **4.1** Port grass generation from compute shader to CPU:
  Created `benchmark/src/engine/grass_cpu.h/cpp` — a complete C++ port of
  `grass_generate.comp`. Includes simplex noise, area-weighted root sampling
  from mesh surfaces, blade geometry generation with curvature/bend, wind
  animation, and per-vertex color computation (tip/top/bottom/AO gradient
  with sun lighting and translucency baked in).

- [x] **4.2** Upload grass geometry to Filament:
  `VertexBuffer` with position+UV+color in buffer 0, dummy tangent quaternions
  in buffer 1. Sequential `IndexBuffer`. Buffer 0 is updated every frame via
  `setBufferAt()` with new vertex data from `generate_grass_blades()`.
  Grass entity is persistent (not recreated per frame).

- [x] **4.3** Grass material: `heaven_grass.mat` — unlit, masked blending,
  double-sided. Uses vertex color for the pre-computed grass color gradient.

**Files created:** `benchmark/src/engine/grass_cpu.h`,
`benchmark/src/engine/grass_cpu.cpp`, `benchmark/materials/heaven_grass.mat`,
`benchmark/materials/heaven_grass.filamat`.

---

### Phase 5 — Particles & Special Effects

**Goal:** Smoke, fire particles, and god rays render.

**Status: ✅ COMPLETE**

**Tasks:**

- [x] **5.1** Implement billboard particles:
  Created `heaven_unlit.mat` — transparent, double-sided, unlit material with
  optional texture. Particle emitter positions defined for chimneys (smoke) and
  torches (fire). Smoke and fire textures loaded from DDS. Material instances
  created with appropriate textures.

- [x] **5.2** Volumetric god rays:
  Implemented as screen-space depth-buffer raymarching — a significant upgrade
  over the original GPU Gems 3 radial blur technique. The `heaven_godrays.mat`
  material renders a fullscreen quad with additive blending that:
  - Projects the sun position to screen space each frame
  - Marches 64 samples from each pixel toward the sun in screen space
  - Samples the depth buffer at each step; sky pixels (depth ≈ 1.0) contribute
    scattered light, occluded pixels block it
  - Uses `smoothstep` for soft occlusion transitions (no hard threshold)
  - Applies exponential decay for physically-motivated Mie scattering
  - Fades at screen edges and with distance from sun for natural falloff
  - Sun visibility check prevents rays when sun is behind camera
  The quad is positioned at the camera's far plane each frame and rendered
  with priority 7 (last among transparent objects).

  Note: The depth pre-pass renders the scene to a half-resolution `RenderTarget`
  using `renderStandaloneView()`, then passes the depth texture to the god rays
  material for the main render pass. The god rays quad is temporarily removed
  from the scene during the depth pass to avoid self-referencing.

**Files created:** `benchmark/materials/heaven_unlit.mat`,
`benchmark/materials/heaven_unlit.filamat`,
`benchmark/materials/heaven_godrays.mat`,
`benchmark/materials/heaven_godrays.filamat`.

---

### Phase 6 — Skinned Meshes & Animation

**Goal:** Animated objects (mill wings, propeller, weathercock, gears) work.

**Status: ✅ COMPLETE**

**Tasks:**

- [x] **6.1** Added `update_mesh_vertices()` to the `IRhiBackend` interface:
  New virtual method that re-uploads vertex data for an existing mesh surface.
  Implemented in all three backends:
  - **Filament**: Re-extracts positions/normals/tangents/UVs from the new `Vertex`
    data, recomputes quaternion tangent frames via `SurfaceOrientation`, and
    uploads both buffer 0 (position+UV) and buffer 1 (tangent quaternions).
  - **OpenGL**: `glBufferSubData` on the existing VBO.
  - **Vulkan**: Stub (backend is incomplete).

- [x] **6.2** Added per-frame animation loop in `main.cpp`:
  For each `AnimatedObject`, advances the animation frame counter using
  `render_time_ * anim_speed` (30 fps), wraps with `fmod`, calls
  `skin_interpolated()` for each surface (which does lerp/slerp bone
  interpolation + linear blend skinning on the CPU), and pushes the
  resulting vertices to the backend via `update_mesh_vertices()`.

**Files modified:** `benchmark/src/rhi/rhi.h` (new interface method),
`benchmark/src/rhi/gl_backend.h/cpp`, `benchmark/src/rhi/vk_backend.h`,
`benchmark/src/rhi/filament_backend.h/cpp`, `benchmark/src/main.cpp`.

---

### Phase 7 — Tessellation Replacement

**Goal:** Materials with height/tessellation maps still show displacement.

**Status: ✅ COMPLETE**

**Tasks:**

- [x] **7.1** Parallax Occlusion Mapping in both lit materials:
  Rewrote `heaven_lit.mat` and `heaven_lit_twosided.mat` with a full POM
  implementation that replaces the original hardware tessellation (TCS/TES).
  The POM algorithm:
  - Computes view direction in tangent space using `getWorldTangentFrame()`
  - Adaptive layer count: 8 layers at head-on, 32 at grazing angles
  - Linear search through the height field to find the surface crossing
  - 4-step binary search refinement for sub-texel precision
  - Offsets all subsequent texture lookups (diffuse, normal, specular, etc.)
    with the POM-displaced UV coordinates

- [x] **7.2** Separate handling for `_h` (height) and `_t` (tessellation) maps:
  Added `tessMap` and `hasTess` parameters alongside the existing `heightMap`.
  The `displacementScale` parameter controls POM depth:
  - Tessellation maps (`_t`): 0.08 scale — visible displacement for cobblestones,
    brick walls, stone surfaces
  - Height maps (`_h`): 0.03 scale — subtle parallax for general surfaces
  - When a tessellation map is present, it takes priority over the height map
    for POM; the height map still contributes normal perturbation

- [x] **7.3** Backend wiring:
  `get_or_create_material_instance()` now loads tessellation textures from
  `GpuMaterial::tessellation`, sets `hasTess`/`tessMap`/`displacementScale`
  on the material instance. The 15 materials with `_t` maps and 40 with `_h`
  maps all get proper POM displacement.

**Files modified:** `benchmark/materials/heaven_lit.mat`,
`benchmark/materials/heaven_lit_twosided.mat`,
`benchmark/src/rhi/filament_backend.cpp`.

---

### Phase 8 — Cleanup & Vulkan Removal

**Goal:** Remove dead code, finalize the Filament-only path.

**Tasks:**

- [ ] **8.1** Remove the old GL backend:
  - Delete `gl_backend.h/cpp`.
  - Delete `vk_backend.h/cpp`, `vk_backend_render.cpp`, `dds_vulkan.h/cpp`.
  - Delete `benchmark/shaders/vk/` directory.
  - Delete the old renderer pass files if no longer used:
    `scene_pass.h/cpp`, `shadow_pass.h/cpp`, `bloom_pass.h/cpp`, `tonemap_pass.h/cpp`.

- [ ] **8.2** Remove old engine wrappers that are now redundant:
  - `shader.h/cpp` (Filament compiles its own shaders via `matc`).
  - `framebuffer.h/cpp` (Filament manages its own render targets).
  - `window.h/cpp` (if SDL2 window creation is handled by Filament's `SwapChain`).

- [ ] **8.3** Simplify `rhi.h`:
  - Remove `GraphicsApi` enum (only Filament remains).
  - Flatten the `IRhiBackend` interface or remove it entirely if there's only
    one implementation.

- [ ] **8.4** Remove build dependencies that are no longer needed:
  - `GLEW` (Filament manages its own GL loading).
  - `Vulkan` SDK.
  - `shaderc` (was used for Vulkan SPIR-V compilation).

- [ ] **8.5** Update `benchmark/CMakeLists.txt` to reflect the simplified dependency list.

- [ ] **8.6** Archive the old GLSL shaders in `benchmark/shaders/` (or delete them).
  Filament materials replace all of them.

**Files deleted:** `gl_backend.h/cpp`, `vk_backend.h/cpp`, `vk_backend_render.cpp`,
`dds_vulkan.h/cpp`, `shader.h/cpp`, `framebuffer.h/cpp`, `benchmark/shaders/vk/*`.

---

### Phase 9 — Backend Decomposition (maintainability)

**Goal:** Break the monolithic `FilamentBackend` into focused, single-
responsibility collaborator subsystems and per-effect render features, leaving
the backend as a thin coordinator.

**Status: ✅ COMPLETE**

This was a behavior-preserving structural refactor in two waves:

1. **Visual effects → render features** (earlier work): the seven optional
   effects became `IRenderFeature`s under `rhi/features/` driven by the framework
   in `rhi/render_feature.h` — clouds, god rays, volumetric fog, grass, sky,
   particles + heat haze, lens flares. Toggleable via `--no-<feature>` /
   `--clouds`.

2. **Infrastructure → backend-owned collaborator subsystems** (this phase). The
   remaining non-feature responsibilities were extracted into concrete
   (non-polymorphic) classes the backend owns by `std::unique_ptr` and constructs
   in dependency order:

   | Subsystem | File | Responsibility |
   |-----------|------|----------------|
   | `TextureStore` | `rhi/texture_store.{h,cpp}` | DDS load + path/handle cache + 1×1 placeholders |
   | `MeshStore` | `rhi/mesh_store.{h,cpp}` | mesh upload (tangents, VB/IB) + CPU-skinning re-upload, keyed by surface handle |
   | `MaterialInstanceCache` | `rhi/material_instance_cache.{h,cpp}` | base materials + per-surface `MaterialInstance` build |
   | `SceneBuilder` | `rhi/scene_builder.{h,cpp}` | persistent renderable entities + per-frame transform sync |
   | `SceneLightSpawner` | `rhi/scene_light_spawner.{h,cpp}` | emission + authored point/spot lights, floating-island bob, the `FrameContext` light spans |
   | `LightingSystem` | `rhi/lighting_system.{h,cpp}` | sun + moon + IBL cubemap/Skybox + bloom retune (camera exposure is owned solely by `update_camera`; the IBL probe is lighting infra, not the visible-sky feature) |
   | `DepthPrePass` | `rhi/depth_prepass.{h,cpp}` | half-res depth-only view/target shared by god rays, volfog, heat haze |
   | `configure_post_processing` | `rhi/post_process.{h,cpp}` | free function (no persistent state): bloom/SSAO/DOF/fog/SSR/TAA/z-range |
   | `BackendCore` | `rhi/backend_core.h` | non-owning bundle (engine/scene/view/renderer/camera) handed to the subsystems |

   The wind-debug arrows — a toggleable, visible scene-graph effect — were
   promoted to `rhi/features/wind_arrows_feature.{h,cpp}` rather than a
   collaborator.

   `FilamentBackend` now keeps only genuine core: the engine/swap-chain/renderer/
   scene/view/camera lifecycle, `get_native_window` (dual-WSI), the feature
   orchestration, the `render_frame` sequencer, and the `IResourceProvider`
   façade (one-line delegations to `TextureStore`). The implementation dropped
   from ~4600 lines (pre-feature) → ~865 lines.

**Verification:** each extraction was built and run with parity checks — identical
startup counts (321 entities, 194 emission lights, 11 authored, 466k grass,
28 emitters + 14 haze, 88 lens flares), ~13–14k FPS, no validation errors, all
toggles functional.

---

## 4  Dependency Changes

| Dependency | Before | After |
|-----------|--------|-------|
| OpenGL (direct) | Required | Managed by Filament internally |
| GLEW | Required | **Remove** |
| Vulkan SDK | Required | **Remove** |
| shaderc | Required | **Remove** |
| SDL2 | Required | Keep (window + input) |
| GLM | Required | Keep for CPU math (Filament uses its own `filament::math`) |
| Jolt Physics | Required | Keep (bridge simulation) |
| **Filament** | — | **Add** (`lib/filament` as CMake subdirectory) |
| **Clang + libc++** | Optional | **Required** (Filament mandates Clang on Linux; installed clang-21 + libc++-21-dev) |

---

## 5  Asset Pipeline Changes

| Asset | Current format | Filament format | Action |
|-------|---------------|-----------------|--------|
| Textures | DDS (BC1/BC3/BC4/BC5) | KTX2 preferred, but raw upload works | Keep DDS loader, upload via `Texture::setImage()` with compressed format |
| Meshes | mi08 binary | `VertexBuffer` + `IndexBuffer` | Keep mi08 loader, upload programmatically |
| Skinned meshes | ms08 binary | `SkinningBuffer` or CPU skinning | Keep ms08 loader |
| Materials | XML0 definitions | Filament `.mat` files compiled with `matc` | Create `.mat` templates, map XML0 properties at runtime via `MaterialInstance` |
| Shaders | GLSL 4.50 source | Filament material definitions | Rewrite as `.mat` files; custom effects use Filament's material language |
| Camera paths | Custom binary | No change | Keep as-is |

---

## 6  Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Filament's PBR model produces different look than our simplified PBR | High | Medium | Tune material parameters; accept minor visual differences as an improvement |
| Compute shader grass can't be ported efficiently | Medium | Medium | CPU fallback (Phase 4.1) is viable for our blade count (~50k) |
| God rays have no clean Filament equivalent | Medium | Low | Filament's fog in-scattering partially compensates; accept regression or implement custom post-process |
| Tessellation displacement is lost | Medium | Low | POM in custom material handles most cases; pre-tessellation for the 15 `_t` materials |
| Build complexity increases (Clang requirement) | Low | Medium | Document in README; provide a Docker build environment |
| Filament's OpenGL backend doesn't support GL 4.5 features we need | Low | High | Filament targets GL 4.1+ on desktop; our vertex format and textures are compatible |
| Performance regression from CPU grass generation | Medium | Medium | Profile; if too slow, batch updates or reduce blade count |

---

## 7  Estimated Effort

| Phase | Effort | Status |
|-------|--------|--------|
| Phase 0 — Build integration | 2–3 days | ✅ Complete |
| Phase 1 — Static geometry & materials | 5–7 days | ✅ Complete |
| Phase 2 — Post-processing | 2–3 days | ✅ Complete |
| Phase 3 — Sky, clouds, IBL | 3–4 days | ✅ Complete (clouds deferred) |
| Phase 4 — Grass system | 3–5 days | ✅ Complete |
| Phase 5 — Particles & god rays | 3–4 days | ✅ Complete |
| Phase 6 — Skinned meshes | 2–3 days | ✅ Complete |
| Phase 7 — Tessellation replacement | 2–4 days | ✅ Complete (POM) |
| Phase 8 — Cleanup | 1–2 days | Pending |

---

## 8  Success Criteria

1. The benchmark runs end-to-end using Filament as the sole rendering backend.
2. All 45 assembled scenes render with correct geometry and materials.
3. Post-processing (bloom, SSAO, DOF, fog, tone mapping) is visually comparable.
4. Animated objects (skinned meshes, floating islands, bridge physics) work.
5. Grass and particles render (even if the implementation differs).
6. Frame rate is within 20% of the original GL backend on the same hardware.
7. The old GL and Vulkan backends are fully removed.
8. Build succeeds on Linux with Clang 17+ and libc++, and has CMake entry
   points for Windows and Android toolchains.

---

## 9  Build Instructions

Use the repository helper rather than invoking CMake directly; it preserves the
compiler/stdlib flags, platform selection, and incremental material workflow.

Linux requires Clang with libc++. Install dependencies:

```bash
sudo apt install clang libc++-dev libc++abi-dev libglu1-mesa-dev \
    libxi-dev libxcomposite-dev libxxf86vm-dev
```

Build by platform:

```bash
./scripts/build.sh --platform linux
./scripts/build.sh --platform android --android-abi arm64-v8a
HEAVEN_WINDOWS_TOOLCHAIN=/path/to/toolchain.cmake ./scripts/build.sh --platform windows
```

Android builds require an Android NDK discoverable through `ANDROID_NDK_HOME`,
`ANDROID_NDK_ROOT`, or an SDK root (`ANDROID_HOME` / `ANDROID_SDK_ROOT`) with an
installed `ndk/` directory. The Android target is a native CMake/NDK build; APK /
Activity packaging is not produced here. Windows builds require a CMake
toolchain file through `HEAVEN_WINDOWS_TOOLCHAIN`, `CMAKE_TOOLCHAIN_FILE`, or
`./scripts/build.sh --cmake-arg`.

Run the Linux desktop benchmark:

```bash
./scripts/run.sh
```

The `--api` CLI flag is retained for compatibility but every renderer path lands
in the Filament backend.
