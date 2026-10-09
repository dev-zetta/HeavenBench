# Filament rendering features

This is the canonical description of HeavenBench's effective rendering
pipeline. `RENDERING_FEATURES.md` remains a historical implementation plan.

Use the asset-free [rendering test scenes](RENDER_TEST_SCENES.md) for isolated feature debugging and Filament rebase reviews. Their fixed on/off capture matrix and receipt audit separate configured inputs, native activation and visual acceptance; open image findings remain explicit.

## Quality presets

Presets change rendering and optional CPU stress only. They do not change
resolution, fullscreen/HDR output, scene, camera path, weather, music, or
result locations. Explicit JSON values overlay the selected preset; CLI
arguments are evaluated left to right.

| Setting | Balanced System | Visual Ultra | GPU Torture | CPU Torture |
|---|---:|---:|---:|---:|
| ID | `balanced` | `visual-ultra` | `gpu-torture` | `cpu-torture` |
| Tessellation | 1.0 | 1.5 | 1.5 | 1.0 |
| MSAA / TAA | 2x / x16 | 4x custom / x32 | 4x custom / x32 | 2x / x16 |
| GTAO | half High | full Ultra, 8x6 | full Ultra, 8x6 | half High |
| Bloom | 384 / 6 | 512 / 8 | 1024 / 10 | 384 / 6 |
| DOF | half/default | native 5/5/5 | native 9/7/7 | half/default |
| Sun shadow | 2048 | 4096 stable CSM | 4096 stable CSM | 2048 |
| Point shadow / lights | 256 / 2 | 512 / 4 | 1024 / 16 | 256 / 2 |
| Foliage density | 1.0 | 1.5 | 1,000,000 source-blade cap | 1.0 |
| CPU multicore phase | off | off | off | all logical cores, 10 s |

The AO resolution row describes each preset's base setting. Enabling contact shadows requires full-resolution structure depth and promotes AO resolution to full as well, because Filament shares that depth hierarchy. This applies even when AO itself is disabled; turning contact shadows off restores the selected AO resolution. TAA reduces the contact ray marcher's visible spatial dithering.

Select with `--preset NAME`, the Quality panel, or `quality_preset`.
Changing a preset-owned field makes the effective preset `custom`. The old
label input is read for compatibility but is not emitted by new config files.

## Implemented inventory

| Area | Implementation |
|---|---|
| Backend/output | Filament Vulkan, OpenGL fallback, SDR tone mappers, native-Wayland HDR10 PQ/Rec.2020 |
| Environment | Four startup-built RGBA16F probes with GGX specular and diffuse irradiance filtering |
| Materials | Lit, masked/two-sided, clear coat, anisotropy, subsurface, cloth, glass/window, wetness and emission |
| Geometry | Cached CPU tessellation, persistent renderables, GPU instancing and GPU skinning |
| Shadows | Four-cascade PCSS sun, contact shadows, independently sized/budgeted point shadows |
| AA/AO | TAA plus 2x/4x MSAA, HDR custom resolve, cutout A2C, GTAO, bent normals and multi-bounce AO |
| Reflections/GI | Prefiltered IBL, SSR, wet reflections and previous-frame scene-color SSGI |
| Atmosphere | Procedural sky, volumetric clouds, per-light volumetric fog and god rays |
| Post effects | Tiered bloom with extracted lens dirt, authored/physical DOF and color grading |
| Scene effects | Lit, shadow-receiving grass/massive foliage; particles, heat haze, rain/splashes, lens flares and cloth |

### Modular render features

| Feature | Status | Main fallback or limit |
|---|---|---|
| `SkyFeature` | Active by default | Filament cubemap sky remains available when the visible sphere is disabled |
| `CloudFeature` | Optional legacy cloud deck | Missing authored textures disables the deck |
| `VolumetricCloudFeature` | Active by default | Screen-space ray marching, not a global atmosphere model |
| `GodraysFeature` | Active by default | Uses the shared half-resolution scene depth |
| `VolFogFeature` | Active by default | Screen/depth limited; reads active authored scene lights |
| `GrassFeature` | Active by default | Startup-generated blades; vertex-shader wind; receives direct/IBL lighting and scene shadows; full cascade casting is disabled to avoid opaque self-shadowing by the dense card field |
| `MassiveFoliageFeature` | Active by default | Uses the same lit/shadowed grass path; preset density caps source-template blades; every matching island instance is still rendered |
| `ParticleFeature` | Active by default | Owns heat-haze scene capture/composite; missing sprites disable affected emitters |
| `LensFlareFeature` | Active by default | Owns authored flares; Filament stock lens flare stays disabled |
| `RainFeature` | Active by default | Missing drop/splash sprites disables rain rendering |
| `WetReflectionsFeature` | Active by default | Screen-space visibility limits apply |
| `SsgiFeature` | Active by default | Gathers previous-frame scene color and yields to heat-haze capture |
| `AdvancedMaterialsFeature` | Active by configured model toggles | Specialized models retain their calibrated workflow |
| `WindArrowsFeature` | Debug-only | Disabled unless wind debug is requested |

Foliage budgets count blades generated for each source mesh template, not the scene total after those templates are placed on repeated island instances. For example, the audited density of 1.95 produces 908,700 source blades and 1,873,691 scene blades. GPU buffer sharing preserves all of those instances; it reduces duplicate geometry storage rather than density.

### Nine quality improvements

| Improvement | Configuration | Active fallback / platform limit |
|---|---|---|
| Texture color spaces | Authored `format="srgb"` plus `TextureSemantic` role inference | Unsupported/missing DDS data uses the role-appropriate placeholder |
| HDR prefiltered IBL | Four fixed time bands, generated at startup | One diagnostic and flat SH ambient plus the procedural sky probe if filtering fails |
| Legacy specular workflow | `material_workflow`, `--material-workflow`, Quality-panel combo | Specialized models remain on compatibility mapping |
| Ultra AO | `quality.ao_quality`, `--ao-quality` | High uses full-resolution GTAO/Ultra; SSCT deliberately stays off |
| True MSAA/A2C | `quality.msaa_samples`, `--msaa-quality` | Chooses the greatest supported sample count not exceeding the request |
| Bloom/lens dirt | `quality.bloom_quality`, `--bloom-quality`; `bloom_strength`, `--bloom-strength` and the Quality-panel strength slider | Requested strength is bounded to `0..1` (default `0.3`) and scales the established time-of-day curve; missing extracted dirt leaves clean bloom active |
| Authored DOF tiers | `quality.dof_quality`, `--dof-quality` | Filament cannot express the original independent near/far blur powers |
| Independent shadows | sun map, point map, and point-caster budget | Sizes are validated/clamped; Filament exposes no reliable post-allocation size query |
| Runtime accounting | automatic report-v2 snapshot | Counters report actual resource/routing use known to the renderer |

The visible sky and IBL have separate ownership. Disabling the sky sphere does
not remove environment lighting. All four IBL bands are filtered before
measured rendering, so authored time-of-day changes only switch handles.

## Texture and material rules

`TextureSemantic` is part of each load and cache key:

- `ColorSRGB`: bindings authored with `format="srgb"`, role-inferred
  diffuse/detail-diffuse/emission textures from convention auto-discovery, and
  explicitly annotated custom-feature color assets such as grass and lens dirt.
- Authored bindings without format metadata retain their authored/default
  linear interpretation; role inference must not override them.
- `NormalLinear`: normal and detail-normal maps.
- `ScalarLinear`: specular, height, tessellation density, masks and depth.
- `HDRLinear`: floating-point radiance.

The default `metallic-roughness` path retains the calibrated compatibility
mapping. `specular-glossiness` is an experimental runtime A/B branch on the
eligible standard lit, opaque/two-sided, masked, and window families. It uses
specular RGB to modulate metallic base reflection, maps dielectric luminance
into Filament's F0 control, and derives roughness from final gloss luminance.
Clear coat, cloth, anisotropic, subsurface, colored-penumbra, refraction, and
other specialized models stay on their dedicated mapping. Presets retain
compatibility until extracted-shader/reference validation supports changing
the benchmark baseline.

## Configuration, fallbacks, and reporting

Post tiers use `quality.ao_quality`, `quality.bloom_quality`,
`quality.dof_quality`, and `quality.taa_quality` (`0..2`). Shadows use
`quality.sun_shadow_map_size`, `quality.point_shadow_map_size`, and the
independent `quality.shadow_light_budget`.

Bloom, ambient occlusion, TAA and depth of field also have independent default-on switches: `features.bloom`, `features.ambient_occlusion`, `features.taa` and `features.depth_of_field`. Use `--no-bloom`, `--no-ambient-occlusion`, `--no-taa` or `--no-depth-of-field` for true pass-disable comparisons; their positive forms re-enable them. Quality level zero changes sampling quality and does not disable a pass. Weather reconfiguration preserves camera-owned DOF state, and disabling reflections remains effective while the wet-surface feature is enabled.

Bloom strength belongs to the caller. `bloom_strength` is serialized in configuration, editable in the Quality panel and set by `--bloom-strength N`; finite out-of-range requests are clamped to `0..1`. Invalid CLI numeric tokens are diagnosed and ignored, while the shared clamp uses the default `0.3` for nonfinite programmatic inputs. `LightingSystem` scales its existing day/night strength by `requested / 0.3` and clamps the effective result to `0..1`; changes to the request also invalidate its steady-lighting cache. The default request preserves the previous strength curve exactly. Capture metadata separates `runtime_settings.bloom_strength_requested` from the actual View's `renderer_features.bloom_strength` and `renderer_features.bloom_enabled`.

Lens dirt uses a linear gain texture with `RGB = 1 + 0.2 * sRGB_EOTF(source_byte / 255)`, alpha one and Filament `dirtStrength = 1`. Filament multiplies the complete bloom accumulation by this texture, so the unit term retains clean bloom while the authored dirt adds bounded modulation. The former dark sRGB map with strength `0.2` attenuated the entire bloom term. The correction preserves original emission/material data, threshold and anti-firefly settings, and requires no vendor shader change; bloom-on pixels intentionally change even though the default strength curve remains exact.

The gain has a distinct versioned cache entry and linear HDR `RGBA16F` upload; it cannot alias an ordinary sRGB DDS load. It converts every declared mip of the original post-process dirt DDS, independently of optional high-resolution surface texture tiers. The procedural fixture converts its existing byte-quantized dirt pattern through the same color-space rule and generates mips in linear gain space. Float gain is `1..1.2`; half precision can round the maximum to `1.2001953125`. TextureStore accounts for the derived authored upload as eight bytes per texel over its uploaded mips. CPU compressed-texture decoding supplies the new gain pixels, without claiming byte-equivalence to compressed GPU sampling. Views detach borrowed dirt before cache or fixture-resource retirement; missing or unsupported dirt retains the clean-bloom fallback.

Missing lens dirt leaves clean bloom active. Missing textures use white,
flat-normal, or zero-depth placeholders. Unsupported MSAA steps down to the
greatest supported count not exceeding the request. HDR display still requires
native Wayland and an advertised HDR10 surface.

HDR startup and explicit resize probe the actual Vulkan surface formats; an unsupported request fails before PQ output can be sent to an SDR surface. The vendor swap-chain guard also rejects incompatible automatic recreation. The scene renders to linear Rec.2020, while the UI and loading splash retain their sRGB shader output in a separate transparent layer. A final fullscreen pass decodes that layer, converts its primaries, composites in linear light and applies PQ once. Both layers use the configured paper-white and highlight peak. Numeric GPU readback checks the encoder; physical display calibration and complete scene-image parity remain separate acceptance steps.

SDR keeps temporal output dithering. HDR disables that 8-bit-oriented stage on its linear floating-point intermediate; injecting its noise before PQ encoding creates visible grain in dark scenes. Full-scene capture can retain raw floating-point PQ values plus a diagnostic SDR preview; that preview is not a measurement of the physical display.

Both outputs apply a -0.25 EV display-transform calibration for the prefiltered
environment. SDR applies a further -0.5 EV before the selected tone mapper
(-0.75 EV total). HDR retains its independent GT7, paper-white,
peak-luminance, and PQ calibration. These display compensations do not alter
scene lighting, physical-camera exposure, or texture data.

Report schema v2 records the effective preset/configuration, CPU stress,
requested/active MSAA, custom resolve, A2C, AO material options, IBL status,
shadow sizes, requested/active material workflow, unique eligible surface
counts and actual scene-use-weighted workflow counts, enabled render-feature
count, semantic texture counts, and fallbacks. Score version remains 3.

## Benchmark methodology

- FPS measures intervals between accepted, presented frames.
- Resolution means the live Vulkan drawable extent in physical pixels. Windows
  request high-DPI drawables in SDR and HDR; a scored run is refused when the
  compositor cannot provide the configured pixel dimensions. Native Wayland is
  preferred for SDR on Wayland desktops so SDR and HDR share compositor-scaling
  behavior; `--x11` remains an explicit compatibility path.
- GPU timing consumes each asynchronous Filament history sample once and drains
  the final submissions.
- The canonical CPU score remains the deterministic single-thread fixed-60 Hz
  real-scene workload.
- CPU Torture creates independent CPU-only worlds per worker and reports
  aggregate/per-worker throughput and checksums under `cpu_stress`; it does not
  alter CPU, GPU, or overall scores.

## Limits relative to current AAA engines

This deliberately expensive pipeline is not a complete contemporary game
engine. It has no hardware tessellation, mesh/task shaders, hardware ray
tracing, path-traced GI, virtualized geometry, or current-generation temporal
super-resolution. SSR and SSGI retain normal screen-space visibility and
disocclusion limits. The experimental legacy specular path also needs reference
calibration before it can replace the stable baseline.

The static displaced-surface preparation path uses a generic bounded geometry budget whenever tessellation is enabled, independent of the preset label: three million triangles per eligible surface, 128 times its source triangle count and 27 million additional triangles across all authored instances. It preserves native height sampling, source-parent displacement and runtime fade while limiting physical smoothing to 20 mm. Failed orientation components keep their original geometry, and the approximation can remain budget limited. The user accepted a measured 35.68% mean-frame-time increase for the cleaner geometry in the matched 1920×1080 high-quality scene; this is a configuration-specific Vulkan measurement. [The visual audit](VISUAL_AUDIT.md#bounded-displaced-surface-continuation) records the exact source, budgets and validation.
