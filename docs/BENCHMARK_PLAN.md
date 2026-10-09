# Plan: Converting Unigine Heaven to a Modern Standalone Benchmark

## Asset Inventory

### What We Have

| Category | Count | Details |
|----------|-------|---------|
| Static meshes (.mesh) | 63 | mi08 format, 577K vertices, 691K triangles, 1072 surfaces |
| Skinned meshes (.smesh) | 4 | Mill wings, port propeller, weathercock, lab gears |
| Textures (.dds) | 265 | 292 MB total. DXT1, DXT5, ATI1, ATI2 formats. Up to 2048×2048 |
| Materials (.mat) | 8 | Binary xml0 with PBR-like properties: diffuse, normal, specular, height, emission, detail maps, tessellation params, fresnel, fleck (metallic paint) |
| Camera tracks (.track) | 55 | Binary xml0 spline keyframes for camera position/rotation/FOV |
| Node files (.node) | 51 | Scene assembly with transforms |
| World file (.world) | 1 | Full scene hierarchy, render settings, lighting, scripts |
| Audio (.oga) | 1 | Ambient soundtrack (4.5 MB Ogg Vorbis) |
| Shaders (.shader) | 303 | Unigine GLSL/HLSL shaders for the original engine |
| UI files (.ui) | 2 | Benchmark interface definitions |
| Localization (.en, .ru) | 2 | English and Russian strings |
| Benchmark scripts (.h/.cpp) | 3 | UnigineScript: camera system, benchmark logic, UI |

### Scene Complexity

The full Heaven scene, when all instances are placed, contains roughly:
- ~200 unique mesh files instanced ~400+ times via node references
- ~1.5M triangles on screen at peak (with LOD and instancing)
- 4 animated objects (mill wings, propeller, weathercock, gears)
- 1 directional light (sun) with dynamic time-of-day
- 7 spot lights (lamp posts) + 1 blue accent light
- Procedural grass on the main island
- Animated cloud layer (shader-driven UV scrolling)
- Animated floating islands (script-driven sine wave)
- 26 camera path segments forming a continuous flythrough (~25 seconds per loop)

### Material System

Materials use a PBR-like pipeline with these texture channels:
- `_d.dds` — Diffuse/albedo (DXT1 or DXT5)
- `_n.dds` — Normal map (ATI2 / BC5 two-channel)
- `_s.dds` — Specular map (DXT1 or DXT5)
- `_h.dds` — Height/parallax map (ATI1 / BC4 single-channel)
- `_t.dds` — Tessellation displacement map (ATI1)
- `_e.dds` — Emission/glow map
- `_c.dds` — Cube reflection map
- `_irregularity_d.dds` — Detail/variation overlay

Material properties include: parallax scale, specular scale/power, fresnel bias/power, tessellation scale/factor/distance, emission color/scale, detail transform, color scale, and metallic fleck parameters.

---

## Proposed Architecture

### Engine Choice: Custom Vulkan/OpenGL Renderer

A lightweight custom renderer gives full control over the benchmark pipeline and avoids dependency on a large game engine. The goal is a focused GPU benchmark, not a general-purpose engine.

**Alternative**: Use an existing framework like bgfx, wgpu, or The Forge for cross-platform GPU abstraction. This reduces boilerplate while keeping the benchmark self-contained.

### Target Stack

```
┌─────────────────────────────────────────────┐
│  Benchmark Application                      │
│  ┌───────────────────────────────────────┐  │
│  │  Scene Manager                        │  │
│  │  - Load OBJ meshes + DDS textures     │  │
│  │  - Material assignment                │  │
│  │  - Instance transforms from .node     │  │
│  │  - Camera path interpolation          │  │
│  ├───────────────────────────────────────┤  │
│  │  Renderer                             │  │
│  │  - Forward+ or Deferred pipeline      │  │
│  │  - PBR shading (GGX/Cook-Torrance)   │  │
│  │  - Normal mapping                     │  │
│  │  - Shadow mapping (CSM for sun)       │  │
│  │  - HDR + tone mapping + bloom         │  │
│  │  - SSAO                               │  │
│  │  - Optional: tessellation, DOF        │  │
│  ├───────────────────────────────────────┤  │
│  │  GPU Backend (Vulkan / OpenGL 4.5)    │  │
│  ├───────────────────────────────────────┤  │
│  │  Window + Input (SDL2 or GLFW)        │  │
│  └───────────────────────────────────────┘  │
│  Benchmark Harness                          │
│  - FPS counter, frame time histogram        │
│  - Deterministic camera path playback       │
│  - Score calculation                        │
│  - Result export (JSON/CSV)                 │
└─────────────────────────────────────────────┘
```

---

## Implementation Plan

### Phase 1: Asset Pipeline (foundation)

**Goal**: Get all Heaven assets into standard formats that any renderer can consume.

| Task | Status | Notes |
|------|--------|-------|
| UNG archive extraction | ✅ Done | `uniginex` tool |
| mi08 mesh → OBJ conversion | ✅ Done | `mesh2obj` tool |
| Scene assembly (node transforms) | ✅ Done | `tools/extraction/assemble_scene.py` |
| DDS texture loading | To do | DXT1/DXT5 are standard; ATI1/ATI2 = BC4/BC5, widely supported |
| smesh (skinned mesh) → glTF | To do | 4 animated meshes: mill wings, propeller, weathercock, gears |
| Material mapping → PBR JSON | To do | Parse .mat xml0 files, map to metallic-roughness PBR |
| Camera track extraction | To do | Parse .track xml0 files, export as keyframe arrays |
| Full scene → glTF export | To do | Single glTF with all instances, materials, textures |

**Deliverable**: A complete glTF 2.0 scene file (or set of OBJ + material JSON) that can be loaded in Blender, three.js, or any glTF viewer to verify correctness.

### Phase 2: Renderer Core

**Goal**: Minimal renderer that can display the scene correctly.

| Task | Priority | Description |
|------|----------|-------------|
| Window + context creation | High | SDL2/GLFW + Vulkan or OpenGL 4.5 |
| Mesh loading (GPU buffers) | High | VBO/IBO from OBJ or custom binary |
| Texture loading (DDS) | High | Direct GPU upload of DXT/BC compressed textures |
| Basic PBR shader | High | Diffuse + normal + specular + ambient |
| Camera system | High | Interpolate extracted camera keyframes |
| Sun light + basic shadows | High | Single directional light with CSM |
| Instance rendering | Medium | Draw 400+ mesh instances efficiently |
| Sky rendering | Medium | Procedural or skybox from extracted textures |

**Deliverable**: The Heaven scene rendered with correct geometry, textures, and lighting. Camera flies through the scene on the original path.

### Phase 3: Visual Features

**Goal**: Match the visual quality of the original benchmark.

| Task | Priority | Description |
|------|----------|-------------|
| HDR pipeline | High | Render to float16 target, tone map, bloom |
| SSAO | High | Screen-space ambient occlusion |
| Normal mapping | High | Tangent-space normal maps from `_n.dds` |
| Parallax/height mapping | Medium | From `_h.dds` textures |
| Tessellation | Medium | Hardware tessellation with displacement maps |
| Depth of field | Medium | Post-process DOF with bokeh |
| Volumetric light shafts | Low | God rays from sun |
| Animated objects | Low | Mill wings, propeller, weathercock rotation |
| Procedural grass | Low | Instanced grass blades on main island |
| Cloud animation | Low | Scrolling cloud layer |
| Floating island animation | Low | Sine-wave vertical motion |

### Phase 4: Benchmark Harness

**Goal**: Produce a repeatable, comparable benchmark score.

| Task | Description |
|------|-------------|
| Deterministic playback | Authored camera path plus a fixed-step real-scene CPU simulation phase |
| Frame timing | Accepted-frame wall time plus Filament whole-frame GPU timestamps |
| Score calculation | Version 3: overall smoothness, calibrated CPU world-update throughput, and deduplicated GPU throughput |
| Quality presets | Low / Medium / High / Ultra (matching original) |
| Resolution control | Render at arbitrary resolution; adaptive resolution is disabled while scoring |
| Result screen | Display Overall / CPU / GPU scores and the underlying averages, tails, and rates |
| Result export | JSON with per-frame timings, system info, settings |
| CLI mode | `--benchmark --preset ultra --resolution 1920x1080 --output results.json` |

Score version 3 uses an explicit reference for each independently meaningful
workload:

- **Overall** measures the real rendered camera path. Its effective rate is the
  geometric mean of per-scene FPS and the presented-frame 1% low, so every
  authored scene has equal influence and frame-time hitches reduce the result.
  60 effective FPS is 1000 points.
- **CPU** runs a three-second, fixed-60-Hz workload as quickly as possible
  after warm-up. It loads a separate copy of the production scene without a GPU
  backend and exercises camera playback, moving-world collision updates, Jolt
  bridge physics, bone transforms, cloth simulation, and weather. Asset loading
  and tessellation cache construction are outside the timed interval. The
  reference is 10,000 updates/s (0.1 ms/update) for 1000 points; the report also
  exposes raw throughput and the 60 Hz real-time multiplier.
- **GPU** is derived from the average completed whole-frame GPU timestamp.
  Each asynchronous Filament frame ID is consumed once, samples are restricted
  to submissions made during the benchmark, and final in-flight frames are
  drained before scoring. Scoring forces the requested drawable resolution
  rather than allowing adaptive resolution to change the amount of rendered
  work. 60 GPU-equivalent FPS is 1000 points.

### Phase 5: Polish & Distribution

| Task | Description |
|------|-------------|
| Cross-platform build | CMake targets for Linux, Windows, and Android |
| GPU detection | Vendor/model/driver version in results |
| Comparison database | Optional: upload scores to a web service |
| Installer/package | AppImage (Linux), MSI (Windows), APK/AAB packaging (Android; separate from native CMake build) |

---

## Key Technical Decisions

### 1. Mesh Format for Runtime

**OBJ is too slow for runtime loading.** Convert to a custom binary format or glTF binary (.glb):
- Pre-bake vertex buffers: interleaved position + normal + UV + tangent
- Pre-compute tangent vectors (needed for normal mapping, not in mi08)
- Store triangle indices as uint16 or uint32
- Include material assignments per surface

### 2. Texture Format

DDS files can be loaded directly to GPU — no conversion needed. The BC1/BC3/BC4/BC5 formats are natively supported on all modern GPUs. Just need a DDS header parser and `glCompressedTexImage2D` or `vkCreateImage` with the right format enum.

### 3. Camera Path

The `.track` files use Unigine's Tracker system with spline interpolation. We need to:
1. Parse the xml0 track files to extract keyframe times + values
2. Implement Catmull-Rom or Hermite spline interpolation
3. The clip.track has 26 segments (path keys), each ~25.19 seconds total

### 4. Coordinate System

Unigine uses a right-handed Y-up coordinate system (same as OpenGL). The row-vector × matrix convention means transforms are transposed relative to OpenGL's column-major convention. The OBJ exporter already handles this correctly.

### 5. What We Can Skip

Some original features add complexity without significant benchmark value:
- **UnigineScript VM**: Replace with hardcoded C++ logic
- **Physics**: Only used for walk mode (not in benchmark path)
- **GUI system**: Replace with simple imgui overlay
- **Sound**: Optional, not GPU-relevant
- **Editor integration**: Not needed

---

## Estimated Effort

| Phase | Effort | Dependencies |
|-------|--------|-------------|
| Phase 1: Asset Pipeline | 2-3 weeks | Python + existing tools |
| Phase 2: Renderer Core | 4-6 weeks | Vulkan/GL experience |
| Phase 3: Visual Features | 4-6 weeks | Phase 2 |
| Phase 4: Benchmark Harness | 1-2 weeks | Phase 2 |
| Phase 5: Polish | 2-3 weeks | Phase 3-4 |
| **Total** | **13-20 weeks** | |

A minimal viable benchmark (Phase 1-2 + basic Phase 4) could be functional in ~6-8 weeks, showing the scene with basic PBR and producing an FPS score.

---

## Open Questions

1. **Licensing**: The Heaven assets are copyrighted by Unigine Corp. A standalone benchmark using these assets would need either permission or replacement assets. The code and tools we've built are original work.

2. **glTF vs custom format**: glTF 2.0 is the most portable option for the asset pipeline, but a custom binary format would load faster. Could do both — glTF for verification, custom binary for runtime.

3. **Vulkan vs OpenGL**: Vulkan gives better control over GPU scheduling (important for benchmarking) but is significantly more code. OpenGL 4.5+ with DSA is simpler and still allows compute shaders, tessellation, and precise timing queries. Could start with OpenGL and add Vulkan later.

4. **Tessellation**: The original Heaven benchmark was famous for its tessellation showcase. This requires hardware tessellation shaders and displacement maps. The `_h.dds` and `_t.dds` textures provide the displacement data. Worth implementing as it's a key differentiator from other benchmarks.
