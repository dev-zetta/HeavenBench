# HeavenBench Performance Optimizations

This document describes the performance work implemented after the analytic-sky
and bundled-sky-assets changes. The work targets per-frame CPU overhead, repeated
GPU state uploads, duplicated geometry, physics cost, expensive screen-space
effects, and repeatable profiling.

Correctness takes priority over raw throughput. The frustum-culling work exposed
a bounding-box construction bug that initially caused visible objects to
disappear under small camera rotations. Optimization 4 documents the root cause
and the corrected culling implementation.

## 1. Frame telemetry and benchmark accounting

The RHI now returns a `FrameResult` from `render_frame()`. It contains:

- whether Filament accepted the frame and reached `endFrame()`;
- CPU time for camera setup;
- CPU time for scene and light synchronization;
- CPU time for render-feature updates;
- CPU time spent submitting render views;
- Filament's most recent completed whole-frame GPU duration, when available.

The GPU value comes from Filament's public frame-history API. It is deliberately
reported as a whole-frame duration rather than a per-pass measurement because
the public renderer API does not expose per-pass GPU timestamps.

Benchmark statistics now count only frames accepted by Filament. Elapsed time
from rejected `beginFrame()` attempts is accumulated and attached to the next
accepted frame, preventing rejected frames from inflating the reported FPS.
The benchmark completes only after the terminal camera-path frame is actually
accepted.

At the end of a run, HeavenBench prints average CPU stage costs and the available
whole-frame GPU average. Setting `HEAVEN_BENCHMARK_EXIT=1` makes the application
exit after one completed benchmark run, which supports automated profiling.

Primary files:

- `benchmark/src/rhi/rhi.h`
- `benchmark/src/rhi/filament_backend.cpp`
- `benchmark/src/benchmark/benchmark_system.cpp`
- `benchmark/src/main.cpp`

## 2. Dirty-instance transform propagation

Previously, the renderer uploaded transforms for every persistent scene entity
and rebuilt every instanced transform array each frame, even when only floating
islands, rocks, or bridge pieces had moved.

The main loop now constructs a sorted, unique list of dirty instance indices.
Floating-island and floating-rock transforms are marked dirty only when their
matrix actually changes. Bridge instances append their indices only after a
physics step produces new transforms.

The dirty span is passed through the RHI to:

- `SceneBuilder`;
- `DepthProxyBuilder`;
- `SceneLightSpawner`;
- render-feature frame context.

Singleton transform-manager writes, emissive-light position updates, and depth
proxy writes are skipped for clean instances. Authored floating-light updates
are also skipped when the floating-offset vector is unchanged.

Primary files:

- `benchmark/src/main.cpp`
- `benchmark/src/rhi/scene_builder.cpp`
- `benchmark/src/rhi/depth_proxy_builder.cpp`
- `benchmark/src/rhi/scene_light_spawner.cpp`

## 3. Persistent instancing transform staging

Each visible and depth-only instanced chunk now owns a persistent
`local_transforms` staging vector. Per-frame synchronization changes only entries
whose instance indices are dirty.

An `InstanceBuffer::setLocalTransforms()` upload occurs only when at least one
member of that chunk changed. This removes a vector allocation, a full transform
rebuild, and a GPU buffer update from clean chunks.

Primary files:

- `benchmark/src/rhi/scene_builder.h`
- `benchmark/src/rhi/scene_builder.cpp`
- `benchmark/src/rhi/depth_proxy_builder.h`
- `benchmark/src/rhi/depth_proxy_builder.cpp`

## 4. Correct frustum-culling bounds

Frustum culling is enabled for eligible singleton scene renderables, GPU-instanced chunks, depth proxies and generated grass patches. Singleton bounds come from actual uploaded vertex positions. Instanced chunks group nearby placements and refresh their aggregate world AABB when members move; non-displaced groups use the singleton local margin, while ordinary displaced groups retain the existing 16-metre world allowance. See [the October 9 audit](CULLING_AUDIT.md) for granularity, envelope limits and measured results.

The initial implementation passed `{minimum, maximum}` directly to
`filament::Box`. That type is not a min/max pair: its aggregate fields are
`{center, halfExtent}`. As a result, every scene culling volume was shifted to
the minimum corner and assigned the maximum corner as its half-size. Grass
volumes intended to cover `[-200, 200]` were therefore centered at `-200`, and
ordinary object bounds were malformed in the same way. Filament correctly
culled against those incorrect boxes, causing rotation-dependent pop-in.

All enabled-culling paths now construct boxes with
`box_from_min_max()`, which calls `filament::Box::set(min, max)` to derive the
correct center and half-extent. This covers:

- persistent scene renderables;
- instanced scene chunks;
- scene depth proxies;
- instanced depth-proxy chunks;
- generated grass and massive-foliage patches;
- grass depth proxies.

Dormant renderables with culling disabled (fullscreen effects, loading/HDR
composites, particles, rain, sky, lens flares, and debug arrows) use the same
explicit conversion. Although their bounds do not currently participate in
culling, removing the misleading `{min, max}` aggregate syntax prevents a
future culling toggle from silently reviving the bug.

`prop_filament_bounds.cpp` is a host-side RapidCheck regression test against the
real `filament::Box` type. For randomized boxes it verifies the recovered
minimum, maximum, center, and half-extent over at least 100 generated cases.

Large scene meshes and GPU-skinned meshes remain excluded from culling through
the existing safety policy. Explicit grass distance LOD also remains active and
is synchronized with the grass depth proxies.

Primary files:

- `benchmark/src/rhi/filament_bounds.h`
- `benchmark/tests/prop_filament_bounds.cpp`
- `benchmark/src/rhi/scene_builder.cpp`
- `benchmark/src/rhi/depth_proxy_builder.cpp`
- `benchmark/src/rhi/features/grass_feature.cpp`
- `benchmark/src/rhi/features/massive_foliage_feature.cpp`
- `benchmark/src/rhi/grass_depth_proxy.cpp`

## 5. Grass population deduplication and distance LOD

Massive foliage already contains the baseline grass population. Running it
together with the legacy grass feature rendered two complete meadow populations.
When massive foliage is enabled, the regular grass feature now starts disabled.
Runtime toggles keep the two alternatives mutually exclusive.

Both grass systems retain the 650-metre island-origin distance LOD. Within each island/category, spatial index ranges have individual wind-expanded bounds, letting camera and depth passes exclude distant blades without reducing the population. Each visible range and its matching depth proxy are toggled together. Geometry remains persistent and buffers are shared across ranges; distance LOD changes scene membership rather than regenerating blades.

Primary files:

- `benchmark/src/rhi/filament_backend.cpp`
- `benchmark/src/rhi/features/grass_feature.cpp`
- `benchmark/src/rhi/features/massive_foliage_feature.cpp`
- `benchmark/src/rhi/grass_depth_proxy.cpp`

## 6. Fixed-rate bridge physics

Bridge simulation now uses a fixed 120 Hz step with an accumulator and at most
four catch-up steps per rendered frame. This makes physics cost bounded during a
slow frame and avoids making simulation behavior directly dependent on render
FPS.

The bridge endpoints use separate invisible kinematic bodies at the authored
`start_bridge*` / `end_bridge*` dummy positions. Every visible plank remains
dynamic, including the first and last links. Previously those visible endpoint
planks were themselves kinematic: when a floating island moved vertically, the
last plank retained its authored rotation while the neighboring dynamic plank
was pulled toward it, producing an intermittent hard kink at the final joint.
Two lateral point constraints attach each dynamic endpoint plank to its authored
anchor, matching the internal two-point joints while allowing a continuous
rotation through the span.

On render frames where the accumulator has not reached the next fixed step, the
renderer skips bridge transform readback entirely. When physics does step, only
bridge instances are appended to the dirty-instance list.

Primary files:

- `benchmark/src/engine/rope_sim.h`
- `benchmark/src/engine/rope_sim.cpp`
- `benchmark/src/main.cpp`
- `benchmark/tests/unit_bridge_endpoint_dynamics.cpp`

## 7. Explicit dynamic resolution

Dynamic resolution is opt-in for interactive rendering. Measured benchmark runs disable it so the configured output resolution describes a fixed pixel workload; expensive clouds, fog, and SSGI do not silently enable a reduced render scale. TAA and the shaders' temporal sample phases remain independent of this policy. Earlier documentation describing automatic adaptive scaling was stale.

Primary file: `benchmark/src/rhi/filament_backend.cpp`.

## 8. Shadow-work policy

The current public Filament API does not expose a static-shadow cache or the per-pass controls needed to implement one cleanly. The existing application shadow policy uses public controls; the microprofiler's vendor instrumentation observes their resulting pass work.

The retained supported path minimizes shadow work through:

- applying shadow-caster selection only when its enabled state, budget, or a
  fingerprint of candidate entity IDs and intensities changes;
- limiting the shadow-casting point/spot-light budget;
- using a 256-pixel point-light shadow map;
- limiting point-light shadow falloff to 12 metres;
- preserving the command-line option to disable point-light shadows for
  profiling.

Light positions are intentionally excluded from the policy fingerprint:
floating lights can move without reissuing `setShadowCaster` or
`setShadowOptions`. Filament still updates the selected light's shadow map, but
the application no longer invalidates the shadow-caster policy/atlas merely
because the per-frame configuration entry point ran.

A true static shadow-map cache remains future work and would require an upstream Filament API or a documented vendor implementation.

Primary file:

- `benchmark/src/rhi/lighting_system.cpp`

## 9. Foliage geometry ownership and wind-aware bounds

Massive foliage now uploads each source mesh/category once and shares those immutable vertex/index buffers across authored island placements. The visible renderables and depth proxies keep their independent transforms and visibility. Source seeds, generated vertex bytes, blade and flower counts, materials, wind animation, and draw placement are preserved. Teardown destroys depth proxies and visible renderables before their shared buffer owners. The CPU engine library also compiles the shared loaders once for the renderer, packer, and tests instead of compiling those sources separately into each executable.

Both grass systems replace the fixed local `[-200,200]` cube with bounds from their actual generated vertices. The expansion covers the shader's complete wind amplitude, transformed back into local coordinates for rotation, scale, reflection, and shear. Invalid or singular transforms disable culling. Floating-island translation leaves the local bounds unchanged; changes to the linear transform update visible and depth bounds together. This removes off-screen work without changing the visible population. `HEAVEN_PROFILE_BOUNDS=1` logs the computed bounds once per patch.

The host fixtures check wind extrema, transforms, degenerate inputs, exact generated vertex parity, and translation caching. A separate renderer-linked NOOP fixture checks six buffer owners shared by twelve grass/flower placements, twelve depth proxies with no extra geometry allocations, independent transforms, and repeated resource teardown. Real GPU timings and remaining visual acceptance are recorded in the release audit.

## Repeatable profiling

Run the feature-ablation suite using the saved configuration at 1920×1080, or choose one diagnostic variant and pass explicit renderer/run options:

```bash
./scripts/profile.sh --x11
./scripts/profile.sh no_grass --config config.json --x11
./scripts/profile.sh half_resolution --config config.json --x11
./scripts/profile.sh all_features --config config.json --width 1920 --height 1080 --x11
```

Each invocation freezes the effective base configuration and writes commands, environment metadata, logs, terminal status, JSON/HTML reports, and compact timing summaries into a fresh `build/profiles/run-*` directory. `HEAVEN_PROFILE_DIR` selects the parent directory; `HEAVEN_PROFILE_TIMEOUT` bounds each complete camera-path run (600 seconds by default). `HEAVEN_BIN` can select a frozen packaged binary. The default variants are the unchanged base, no foliage, no volumetric clouds, no volumetric fog, no SSGI, no point-light shadows, selected screen-space depth effects disabled, and no GPU instancing. `half_resolution`, `no_tessellation`, `no_contact_shadows`, `no_ambient_occlusion`, `no_msaa` and `sun_shadow_2048` are additional individually selectable diagnostics. The screen-space-effects variant leaves AO, SSR and contact shadows enabled, and remaining consumers can still request a depth pass. Quality level zero means Low; use the feature's disable flag for an off control.

Feature removal and resolution changes isolate workloads; their faster results are not improvements at unchanged quality. Use the same API, GPU, driver, assets, effective settings, and camera path for implementation comparisons, repeat the baseline, and keep compilation and other controlled GPU workloads outside the measurement interval. On Linux, `--x11` can avoid a Wayland compositor's FIFO presentation; confirm the actual logged present mode. GPU timing is a completed whole-frame sample, not an isolated per-pass timestamp. Render submission includes driver waiting and is not a pure CPU-work measurement.

The performance window excludes its first two seconds and requires at least sixty measured seconds. FPS is informational: the former universal 100/3,500 FPS thresholds did not account for hardware, resolution, or most feature settings and have been removed. Score version 3 and result schema 2 are unchanged. Regression decisions need a calibrated comparison at a recorded workload. Headless tests can validate accounting and implementation invariants, but cannot establish rendering throughput or visual acceptance.

## Settings performance guidance

Settings shows an estimated impact beneath each rendering feature and its quality controls. Minimal, Low, Medium, High and Very high describe the potential enabled workload; CPU, GPU, VRAM, memory and loading labels distinguish where that work occurs. Hover an estimate for its conditions and reason. These are qualitative guidance, not measured percentages or FPS predictions, and effects can share passes or change cost with visible coverage, resolution, quality and hardware. Disabled features retain their guidance so the cost is visible before enabling them. The two CLI-only features, color grading and dynamic resolution, have a read-only guide in Graphics.

GPU instancing and GPU skinning usually save CPU work; they are labelled accordingly. Texture tiers primarily increase memory, upload time and bandwidth, while physical exposure and HDR brightness are inexpensive parameters. Tessellation factor controls topology; displacement scale and distance do not provide proportional triangle-count reductions. The diagnostics now label unique stored mesh counts explicitly: repeated instances, generated foliage and depth/shadow passes add work beyond those numbers.

## October 9 fixed-view diagnosis

Thirteen native runs used the saved configuration, the same Vulkan/Wayland API, Radeon AI PRO R9700, RADV 26.0.8-1ubuntu0.3, 1920×1080 output, render scale 1, 4K textures and 4× MSAA. Each run excluded its first two seconds and measured at least 62.88 seconds at camera pose `-9,11,28,113,-8`, FOV 50. The screenshot's 22,323,082 stored mesh triangles are reproduced exactly: 623,503 base triangles become 19,355,485 neutral tessellated triangles plus 2,967,597 prepared triangles. Those counts exclude the 1,873,691 instantiated grass blades and 3,747 flower cards. The prepared addition alone represents 26,708,373 triangles across all sharing placements before culling or repeated passes.

| Diagnostic workload | FPS | Frame time | Time saved against first baseline |
|---|---:|---:|---:|
| Saved configuration | 6.53 | 153.17 ms | — |
| Generated foliage off | 12.09 | 82.73 ms | 70.44 ms |
| Tessellation/displacement off | 11.51 | 86.91 ms | 66.26 ms |
| Point/spot-light shadows off | 10.39 | 96.26 ms | 56.91 ms |
| Contact shadows off | 7.32 | 136.58 ms | 16.58 ms |
| MSAA off | 6.97 | 143.55 ms | 9.62 ms |
| Volumetric clouds off | 6.62 | 151.08 ms | 2.09 ms |
| Ambient occlusion off | 6.57 | 152.19 ms | 0.98 ms |
| Volumetric fog off | 6.57 | 152.27 ms | 0.90 ms |
| Ground relief off | 6.55 | 152.57 ms | 0.60 ms |
| SSGI off | 6.52 | 153.33 ms | −0.16 ms |
| Foliage and tessellation off | 66.05 | 15.14 ms | 138.03 ms |
| Repeated saved configuration | 6.54 | 153.02 ms | 0.15 ms |

The baseline is GPU-bound: valid delayed whole-frame GPU samples average 150.68 ms against approximately 2.37 ms of CPU render submission. Foliage, tessellation and local shadow rendering are the largest measured costs in this view. The repeated baseline differs by only 0.10%, while SSGI and the small atmosphere/AO/ground-relief deltas do not establish meaningful general performance gains. The individual savings overlap; removing tessellation also changes shadow and depth workloads. These diagnostic feature removals change visual quality and leave the saved configuration unchanged.

This is one fixed camera with a live animated world, rather than the user's exact screenshot camera or an authored tour. The 100 ms simulation-delta clamp means fast and slow runs can sample different motion phases. GPU samples omit unavailable values and the complete warmup prefix, but remain delayed whole-frame observations rather than per-pass costs. The original 4K archives retain known missing-texture fallback events; these measurements do not establish texture-complete visual acceptance. Wayland logged FIFO presentation consistently. No compilation or other controlled GPU workload ran during the measurement series.

Evidence is under `build/performance-impact/20261009/`: every case retains its command, effective configuration, raw samples, log and terminal status. `heaven_benchmark.profile` preserves the exact measured ELF (`fd179172f2dc94100bd5cd01c1589a0a85fadedd4ec239ff54e98acd7f4dffa0`); the later UI-only rebuild is separate. `independent-review/completion.json` binds 234 successful checks, input hashes, actual feature activation, geometry/foliage count parity and eight verifier negative controls. Use the reviewer's `summary.md` and `final-thirteen-verification.json` for stage means: the initial root harness includes one extra warmup row and averages unavailable `-1` GPU samples, while its FPS and measurement windows are correct.

## Microprofiler pass diagnosis

The October 9 microprofiler captures actual hardware timestamps and submitted draw workloads on Vulkan and OpenGL. In the recorded full scene, shadow-pass interval unions dominate at approximately 83 ms per retained native frame, followed by opaque color at approximately 23 ms and reflections at approximately 7 ms. The mesh-only and camera-facing-away traces independently reduce shadow work. These are observed unions with partial-history and unavailable-result controls, not additive CPU/GPU frame costs. [MICROPROFILER.md](MICROPROFILER.md) records commands, effective settings, exact executable identities, recorder loss and overhead measurements.

The trace also exposes a source-backed explanation for grass shadow cost: the existing upstream PCSS/VSM preparation policy promotes shadow receivers to casters. Both grass paths receive shadows, so their public `castShadows(false)` flag does not prevent this prepared work. A full Shadow Pass submits 13,130,825 more triangles than the mesh-only control, exactly matching the instantiated massive-foliage vertex count divided by three. The later cascade checkpoint below proves this policy with a tiny caster/receiver fixture and preserves it while removing out-of-cascade work. Shadow caster LOD and shadow bounds/budgets remain possible later optimizations. Off-camera visibility alone is insufficient for removing casters.

Repeated Apply Weather / color-grading tasks exposed redundant CPU work, corrected in the checkpoint below. The 32³ LUT's 32 slice jobs execute inline on the main thread at this size. Dense rejected-attempt loops overwrite older CPU events in the bounded recorder, so these captures do not certify exclusive CPU percentages. Inline grass, parallax and material effects remain part of their owning passes; the profiler does not assign invented separate shader costs to them.

## October 9 CPU weather and submission optimization

On Linux visual-ultra at 1920×1080, the saved full-scene trace measures approximately 128 ms between presentations and 126 ms of hardware GPU pass interval union. High main-thread activity did not establish CPU-limited throughput: changing surface wetness repeatedly rebuilt an unchanged color-grading LUT, including on rejected submissions. The retained baseline weather applications average 6.87 ms. Filament builds the tone-mapping LUT even when optional color grading is disabled.

`ColorGradingCache` now reuses the backend-owned LUT when its exact active exposure, HDR/SDR, mapper and grading inputs match. Fog, bloom, AO, reflections, dynamic resolution and camera-owned DOF continue updating; inactive SDR controls do not invalidate HDR, and inactive grading controls do not invalidate disabled grading. Ownership changes and backend initialization reset the non-owning cache. The isolated 64-call CPU fixture measures approximately 7 ms with one LUT build versus 441–444 ms with 64 forced builds; those are CPU configuration timings, not GPU passes or FPS predictions.

The cache alone revealed another problem: rejected attempts became cheap enough to flood the command stream. Recorder-off B-A-B controls measure 7.820 → 7.252 → 7.808 FPS, and the recorded candidate contains a 125.5 ms command-buffer-space wait and 41.7 million overwritten events. A fixed one-millisecond retry reduces CPU activity but still loses throughput. Both negative candidates remain saved. The retained implementation services input-derived controls and exports first, then waits only after a rejected submission. Its separate owner-thread retry helper retains the last finite positive asynchronous GPU duration, requests one sixteenth of it capped at eight milliseconds, and falls back to one millisecond before a usable sample. Missing samples remain unavailable in reports. Resize, reload, settings changes and lifecycle/benchmark transitions reset the estimate. Accepted submissions remain uncapped; scheduler rounding and oversleep remain platform behavior.

The frozen Linux candidate is `67a209792136840a01499b2ef26cd0e534d12888c74206c540f225ae3f8181aa`. Its repeated recorder-off runs reach 7.846–7.848 FPS and hidden recording reaches 7.843 FPS at unchanged quality, compared with the 7.808–7.820 FPS recorder-off controls. Each excludes the two-second warmup and retains more than 62.9 measured seconds. In matched approximately 50-second recorder-off observations, main-thread CPU falls from approximately one full core to 5.0–6.5% of one core, and whole-process CPU falls from approximately 1.17 cores to 0.21–0.26 cores. Averaging normalized CPU rates across each pair gives 94.2% less main-thread CPU and 79.9% less process CPU. These are Linux `/proc` CPU-tick observations; they are separate from wall-clock task spans and GPU timing. Hidden recording adds 0.047% mean presentation interval relative to the two off controls, meeting the 5% target in this workload. The small FPS differences do not establish a general speedup; visible-panel overhead is not remeasured for this CPU checkpoint.

The retained hidden trace measures a 126.019 ms GPU pass interval union over 291 qualified native frames, with unchanged selected pass workload counters. Its 532 retained weather applications average 2.88 microseconds and contain no LUT rebuild. Of 500 retained retry spans, 499 join rejected frames, none joins an accepted frame, and one falls just before retained frame history. Its 2,742,065 overwritten CPU events and zero drops still prevent complete-window exclusive CPU percentages. The remaining dominant FPS cost is GPU shadow work, approximately 83 ms per native frame; source-backed caster-policy and LOD work above remain the next measured optimization targets.

Final fixed-camera captures use the real authored airship, AO on, explicit rain intensity 0.5 and deterministic 60 Hz simulation steps after 45 warmup frames. The final effective rain setting is independently checked because fixture defaults override config-file weather unless explicitly overridden by CLI. Vulkan candidate images are byte-identical to the first baseline. OpenGL differs by at most two RGB8 levels and is byte-identical to its repeated baseline. Retained weather changes do not rebuild the candidate LUT. Both final native rasters are inspected, and all 14 startup/runtime JSON and embedded HTML pairs agree. This is a bounded fixture comparison, not complete scene appearance acceptance.

The canonical build, seven host CTest checks and focused Clang 21 gate pass; the headless GPU CTest is an explicit skip. Complete native Vulkan and OpenGL smoke suites each pass 46 checks with four documented skips, including exact LUT invalidation, resource ownership, View refresh and camera DOF checks. The adaptive-delay host fixture covers missing/delayed arrivals, nonfinite/nonpositive samples, reset, sub-millisecond scaling and the cap. Full-scene commands, effective settings, profile rows, CPU ticks, binary/source freezes, negative candidates and independent source reviews are retained under `build/cpu-bottleneck/20261009/`. This checkpoint changes project-owned CPU code only; the existing Filament patch and all material sources/packages are retained. Android and Windows are not rebuilt by this Linux optimization checkpoint.

## October 9 conservative directional cascade culling

The next unchanged-quality Linux candidate reaches **8.280 FPS versus 7.843 and 7.842 FPS** in the two controls: **5.57% more FPS** and 5.28% less mean presentation interval. Each hidden-recording run uses the saved 1920×1080 visual-ultra configuration, 4K textures, 4× MSAA, PCSS, 4096-pixel Sun maps and the fixed ledge camera on the same R9700/RADV Vulkan/Wayland path. Each excludes the first two seconds and measures more than 62.98 seconds. Baseline-repeat drift is −0.012%; no compilation or other controlled native GPU work overlaps these windows. This is one animated camera/hardware workload, not a general speedup or a new profiler-overhead qualification.

Filament previously sent the global directional-caster range to every Sun cascade. `ShadowMapManager.cpp` now independently refines its directional visibility bit against each map's actual raster-camera frustum before command generation, preserving the original global range for eligibility and UBO preparation. Six conservative AABB-plane tests use double intermediates and a scale-relative 64-FLOAT-epsilon reserve; invalid inputs and explicit `culling(false)` remain visible. Depth-clamped passes retain the original global mask because their rasterization can extend past near/far planes. The directional bit is restored afterward. No mesh, material, texture, displacement, shadow quality, receiver policy or punctual-light workload is reduced.

Source/history review confirms that receiver promotion is deliberate VSM behavior, introduced upstream in 2021; current public PCSS uses EVSSM moments. Removing that promotion would change overlapping-card self-shadowing. The new twelve-cell fixture observes actual PCF/VSM/PCSS pass commands, primitives and instance counters for flat/overlapping production-lit and production-grass receivers. Explicit-caster, nonreceiver, self-shadow and external off-camera blocker controls pass on both APIs. An isolated VSM/PCSS warmup initializes Filament's engine-owned Gaussian material before the strict eight-resource teardown baseline. The PCF external-blocker control uses a conservative off-camera bound with an analytic receiver-light-depth support point to accommodate upstream caster-only far fitting; it does not claim to fix that separate fitting behavior.

The cascade fixture exercises PCF/VSM/PCSS with one and four cascades. Its four moment-map counts fall from `[504,504,504,504]` to `[96,196,336,504]`, while `.culling(false)` reference pixels remain byte-identical on Vulkan and OpenGL. The far cascade may legitimately retain the entire near-to-far caster population. Independent off-camera blockers still darken 784 receiver pixels. Complete native suites each pass 47 cases, skip four documented cases and fail zero. Seven host CTest suites and the focused Clang 21 fixture gate pass, with the opt-in GPU entry separately skipped. The original vendor policy fails the new cascade count oracle while its receiver oracle passes; restoring the retained source reproduces the final renderer and smoke executable byte-exactly. Two initial fixture constant-width warnings are corrected, with failed analysis logs preserved.

Across 291 qualified retained native-frame groups per run, observed shadow GPU interval union falls **83.07→76.38 ms** (8.1%). The first four shadow invocations fall 47.14→40.41 ms; the remaining twelve remain approximately 35.95 ms. Baselines submit 65,305,016 triangles to each Sun cascade; the candidate submits approximately 43.71M, 54.72–54.76M, 65.15M and 65.31M. Source/configuration review identifies the first four as directional maps in this no-atlas path; labels alone do not encode light identity. GPU unions omit unavailable and oldest boundary groups, and CPU ring overwrites remain disclosed. They are bounded observations, not complete-window exclusive CPU costs or additive CPU/GPU totals. Historical archive hashes come from the preceding post-series receipt; no independent pre/post asset hash exists for this three-run cohort.

Real full-scene ledge and close morning-roof captures on both APIs retain their original settings and 45 deterministic warmup steps. Roof differences stay within two RGB8 levels, with a signed distribution matching Filament's temporal RGB dithering. Ledge differences are similarly dominated by that noise with a few small highlight outliers. Unchanged-baseline repeats also show sparse highlight instability, but do not explain every candidate outlier: one round-window highlight differs consistently by up to 64 RGB8 levels over a handful of pixels. That residual remains unexplained; byte-exact full-scene parity is not established. No coherent lost-shadow boundary is observed, and separate no-culling fixture controls are exact. The native candidate rasters are visually inspected. These views retain known 4K texture-fallback limitations and do not establish complete appearance acceptance.

The rebuilt ARM64 development APK also exercises a bounded five-second tablet shadow fixture with basic shadows and then soft shadows at 512-pixel maps. Both runtime/startup JSON and embedded HTML pairs agree. The soft-shadow trace includes actual `Cull directional shadow cascade` CPU scopes and three visible shadow maps plus their EVSM mip passes. Maximum sampled CPU temperature is 53.2°C with thermal status 0. Saved low settings are restored byte-exactly and rendering is stopped. The first-runtime raster shows the fixture and ordinary controls; the later screenshot shows the launcher after SDL's timed main returns. These tests qualify short fixture execution, not full-Heaven FPS/endurance, Windows runtime or release readiness.

The frozen Linux candidate is `a6a215f6af85b399a8e9b70dee1e5d9838d81123e19e8655b20e606c371cd173`; APK `71dbc6dcc4edf8778ccc19f46a70f125f17d06a2c45ec1491313d5d6cbcab363`. The distributed Filament patch is `9c8a335526ef129ad41c3bbea11a4c7b99427d5d28539e18c2136383f88baa38`, resulting tree `9650b6bd65498b746291c6e56f6a897f578b93b4`, verified in two fresh indices without changing the actual vendor index. All 34 material packages retain their bytes. Commands, frozen binaries, failed fixture controls, source reviews, GPU traces, image comparisons and device receipts remain under `build/shadow-culling/20261009/`. The remaining dominant costs are approximately 40 ms of directional shadows, 36 ms of local shadows and 23 ms of opaque work in this view. A next optimization should measure shadow LOD or local-map bounds/budgets with receiver-preservation controls before retaining a change.

## Validation

The renderer is validated with:

```bash
./scripts/build.sh
ctest --test-dir build/renderer -R heaven_benchmark_smoke --output-on-failure
ctest --test-dir build/renderer --output-on-failure
```

At the time of this work, the renderer build and smoke test pass. The startup
loading screen is a renderer-owned bundled asset and does not depend on the
extracted Heaven data tree.
