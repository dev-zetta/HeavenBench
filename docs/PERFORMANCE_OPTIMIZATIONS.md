# HeavenBench Performance Optimizations

This document describes the performance work implemented after the analytic-sky and bundled-sky-assets changes. The work targets per-frame CPU overhead, repeated GPU state uploads, duplicated geometry, physics cost, expensive screen-space effects, and repeatable profiling.

Correctness takes priority over raw throughput. The frustum-culling work exposed a bounding-box construction bug that initially caused visible objects to disappear under small camera rotations. Optimization 4 documents the root cause and the corrected culling implementation.

## November 4 review remediation

[Rain workload gating](CODE_REVIEW_2026-11-04.md#final-rain-workload-and-image-controls) removes ordinary and temporal precipitation membership at effective intensity zero and resolves depth demand before target arbitration. The isolated corrected native fixture submits 1,017 main-pass triangles/five commands with no rain-only depth pass, matching rain disabled on Vulkan and OpenGL; the initial implementation submitted 120,002 additional main-pass triangles plus a 1,017-triangle depth pass. Positive rain keeps its original workload, dry-down follows effective intensity, independent wetness continues, and buffers remain persistent. Zero/off pixels match exactly; positive captures retain a bounded two-RGB8-level noise-phase difference caused by corrected first-wet depth scheduling. Short draw-count traces do not establish an FPS gain. The CR-06 continuation refreshes retained depth-proxy transforms, culling and instanced bounds once after an inactive interval, so a past one-off dirty event cannot leave stale depth on reactivation. Idle frames retain their skipped synchronization and pass; steady active frames keep ordinary dirty-index updates.

The production binary reader now owns a lazy 64 KiB heap read-ahead buffer, drains cached bytes before direct large-array reads, and retains checked logical seek/tell and EOF/CRC error propagation. The initial scratch `fort.mesh` trial reduced mean load time from 24.41 to 10.41 ms; that historical prototype result is separate from the production acceptance below. Host, malformed-input, corruption and scoped ASan/UBSan regressions cover the integrated reader. Complete-scene startup and rendering FPS effects remain unmeasured.

The [final production-reader acceptance](CODE_REVIEW_2026-11-04.md#production-zip-reader-performance-acceptance) measures 26.101/10.811/26.179 ms per warm ZIP mesh load in baseline/candidate/baseline order on fixed CPU 15, each with at least two seconds of warmup and 60 measured seconds. The integrated reader takes 58.58–58.71% less time with 0.30% baseline drift and identical complete geometry hashes. Task build/test/GPU work is stopped during the measurement. This qualifies the one mesh-loader workload, not whole-scene startup or rendering FPS.

Concrete Vulkan GPU selectors now validate actual identity before scene loading; default/environment behavior and heuristic policies remain explicit. Controlled comparisons must continue recording the backend's actual GPU. Earlier measurements below retain their own recorded hardware/source identities.

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

## November 10 Intel full-scene diagnosis

The Intel BMG G31 (`8086:e223`) workload uses Mesa `26.0.8-1ubuntu0.3`, Vulkan/Wayland, the [Intel compatibility fix](INTEL_VULKAN_COMPATIBILITY.md), the fixed full-Heaven village noon view, actual 1920×1080, Visual Ultra, enhanced 4K textures and 4× MSAA. VRS, frame generation, HDR and dynamic resolution remain off. The frozen renderer is `4aff61b11df20e51c35704917368a7f224d8d8df57e87be9bde6db8b173d62c3`.

The 12-second microprofiler capture has 22 retained native-frame groups with all expected GPU categories after excluding the oldest boundary group. Mean observed interval unions are **262.33 ms opaque color**, 59.56 ms shadows, 20.32 ms SSR, 15.70 ms structure depth and 15.46 ms auxiliary depth. Each opaque invocation submits 54,523,679 primitives in 1,717 commands. Rejected-frame retry waits occupy 96.00% of retained complete same-thread Main-loop intervals, while accepted CPU submission intervals average 2.65 ms. This indicates GPU backpressure. The 138,206 overwritten CPU events prevent a complete CPU utilization or exclusive-cost claim. GPU categories remain separate; these retained intervals are neither complete frame times nor additive with CPU time.

A quarter-pixel diagnostic at actual 960×539 reduces opaque time to 116.36 ms with identical opaque work counts. More directly, turning only contact shadows off at 1080p reduces the opaque interval from 262.33 to **25.72 ms**, while opaque, structure, auxiliary depth and SSR submissions remain matched. Other GPU passes vary by roughly 5–8% across the recordings. The 236.61 ms difference is the observed toggle effect rather than an exclusive shader timer; the full quality setting can evaluate 16 Sun directions with up to 32 depth steps per direction. These two quality changes are mechanism probes and are excluded from the equal-quality implementation comparison.

None of the tested full-quality implementation changes earned retention. Compact foliage buffers remove 611 MB of upload without changing the expanded triangle stream, but their one 2.964 FPS run is within the earlier Off variation. The one-footprint cache, paired-ray cache and nearest-depth prefilter increase observed opaque time to 282.05, 276.92 and 305.12 ms respectively. Shared ray setup alone looks promising in one instrumented trace at 241.37 ms and passes both native suites (Vulkan 56 passed/five skipped, OpenGL 55 passed/six skipped). Its matched fixed-time image has RGB8 mean absolute difference 0.47335, close to the historical unchanged-repeat value 0.47333. The clean profile cohort instead measures **2.9083 FPS baseline versus 2.6415 FPS shared setup** (−9.17%), with both candidate runs slower than both bracketing baselines. The reason for the instrumented ranking reversal remains unisolated. Retain raw traces for diagnosis and use clean frame intervals for performance decisions.

All experimental renderer changes are restored. The original source, compiled materials and Filament patch/tree pins retain their bytes; the canonical helper reproduces the original renderer and native smoke binaries byte for byte. No default or saved user setting changes. The [November 11 conservative depth-tile trial](#november-11-intel-conservative-depth-tile-trial) tests the next proposed approach; it also fails the clean performance gate. The per-step cache and prefilter trials provide no reason to keep those shortcuts.

### Tested opt-in setting

**Quality → Contact-shadow quality → Low** keeps contact shadows enabled and uses eight depth steps instead of 32. With every other effective setting matched, one clean 61.87-second Low window measures 4.7036 FPS, versus 2.9083 FPS pooled across two High repeats. FPS increases **61.73%**, and mean interval falls 38.17%. This is a measured quality trade-off at one camera, not a shader speedup, benchmark score or general FPS prediction. Both modes use the restored executable, the same geometry, textures and MSAA, and the same Intel policy without driver debug flags.

| Contact-shadow quality | Clean runs | Pooled FPS | Mean frame time | p95 frame time |
| --- | ---: | ---: | ---: | ---: |
| High (32 steps) | 2 | 2.9083 | 343.84 ms | 364.44 ms |
| Low (8 steps) | 1 | 4.7036 | 212.60 ms | 219.40 ms |

The paired 90-frame fixed-time images retain the same geometry, textures, camera and effective settings apart from contact quality. The Low image changes local roof and surface contact shading; RGB8 mean absolute difference is 0.71913, pixel-maximum p99 is nine and 21,504 of 2,073,600 pixels differ by more than eight levels. This is one still-view quality comparison. The normal Visual Ultra preset retains High. The opt-in file `build/intel-performance/20261110/intel-1080p-contact-low.config.json` holds the tested settings with the ordinary writable cache location; pass it to `scripts/run.sh --config` or use the setting above.

The ignored `build/intel-performance/20261110/` workspace retains the initial trace's JSON/HTML, both mechanism probes, failed and passing build/native receipts, source identities, reproducible commands and clean raw intervals. See `optimization-ledger.md`, `analysis/baseline-01/diagnosis.md`, `analysis/contact-setup-profile-comparison/comparison.md` and `final-restoration.json`. Timing runs use the existing ≥2 s warmup and ≥60 s measured gates, private warm geometry and initially empty private shader caches. One-second competing-work checks and kernel intervals pass; short unclassified desktop work can escape the process checks. Native Windows, macOS, Android and other cameras remain separate scopes.

## November 11 Intel conservative depth-tile trial

**Both conservative tile implementations regress High contact-shadow performance and are rejected.** The actual full-Heaven village uses the same Intel BMG G31 (`8086:e223`, PCI `0000:03:00.0`), Mesa `26.0.8-1ubuntu0.3`, kernel `7.2.6-070206-generic`, Vulkan/native Wayland, fixed noon camera, 1920×1080, Visual Ultra, enhanced 4K assets and 4× MSAA as the earlier work. Contact shadows remain High with 16 finite-Sun directions and up to 32 original depth steps. VRS, frame generation, HDR and dynamic resolution stay off. Geometry, material textures, features and effective configuration are matched; the existing exact-driver Intel image compatibility fix remains active without driver debug overrides.

The first candidate constructs a separate RG32F min/max depth image, one bound per 8×8 native structure tile. It leaves the AO/SSR structure image and its original mip chain intact. A bounds test covers eight original steps, including clamped texture edges, and rejects a segment only when its nearest-depth interval cannot satisfy the existing dual-depth hit predicate. Uncertain cases execute the original fine samples. The second candidate stores overlapping 8×8 windows every four texels, replacing up to four conditional bounds fetches with one. Both preserve the original repeated `t += dt`, first hit, geometric t=0 contact and terminal edge fade. Neither reduces the High sample budget.

Clean profiling is separate from microprofiling. Each row excludes more than two warmup seconds and retains at least sixty measured seconds, using private warm neutral/prepared geometry and initially empty private shader caches. Independent analysis reconstructs the statistics from raw float32 frame intervals and checks binary/output identities, effective settings, hardware, assets and controller receipts.

| Trial, in execution order | Measured seconds | Frames | FPS | Mean frame time | p95 frame time |
| --- | ---: | ---: | ---: | ---: | ---: |
| Original High, opening reference | 61.8922 | 182 | 2.94059 | 340.067 ms | 346.304 ms |
| Separate 8×8 depth tiles | 60.9859 | 125 | 2.04965 | 487.887 ms | 504.712 ms |
| Overlapping 8×8 windows, stride 4 | 61.1707 | 132 | 2.15789 | 463.415 ms | 479.178 ms |
| Original High, closing reference | 61.8875 | 182 | 2.94082 | 340.041 ms | 352.289 ms |

The pooled bracketing reference is **2.94071 FPS / 340.054 ms**. Closing-reference FPS differs by only +0.0077% from opening. Separate tiles reduce FPS **30.30%** and increase mean frame time 43.47%; overlapping windows reduce FPS **26.62%** and increase mean frame time 36.28%. Each candidate has one clean trial, sufficient to reject these substantial regressions against stable references; this is not a repeated winning-candidate or statistical-significance claim. The earlier Low result is a separate quality trade-off and is excluded from this High-only comparison.

The diagnostic microprofiler makes the location of the added cost clear. Across 26 qualified reference groups, 16 separate-tile groups and 17 overlapping-window groups, mean opaque GPU interval is **229.753 → 381.085 → 359.143 ms**. The new bounds pass itself costs only **0.0394 / 0.1080 ms**. Shadow intervals remain about 53.4 ms, structure about 14.4–14.6 ms, SSR about 17.6–17.9 ms and auxiliary depth about 14.4–14.5 ms. Opaque submissions remain exactly 54,523,679 primitives, 1,717 commands, 991 visible renderables and 1,778 instance invocations; structure, SSR and auxiliary workload counters also match. This points to the changed material-fragment path rather than the reduction pass or additional scene geometry. Register pressure, instruction scheduling and the actual fraction of skipped samples have not been isolated. Retained GPU intervals are not certified complete-frame times; CPU history loss and unavailable/boundary coverage remain explicit, and CPU/GPU clocks are never aligned or added.

Actual-source host oracles execute the GLSL producer, consumer helpers and full original/candidate marchers through both gather and fallback sampling. Separate tiles pass 2,261,653 checks per path and four deliberate bad-source controls; overlapping tiles pass 2,781,547 checks per path and six controls covering lost overlap coverage, extra fetches and changed sample timing. A separate interval-containment oracle checks 1,450,251 intervals including partial edge tiles. The temporary sampler-accounting correction is tested against actual descriptor layouts, 69 method cases and seven negative controls, including preservation of the existing compute-material limit. All 48 host CTest entries pass for the initial candidate. The packaged desktop reduction shader uses float32 fetch/min/max/output without RelaxedPrecision or Float16. These checks establish bounded source/package properties, not final Intel ISA behavior or native pixel parity. Because both candidates fail the clean timing gate, native visual/API acceptance and other-platform acceptance are deliberately not completed, and neither candidate is promoted.

The first sandboxed reference cannot open Wayland and two candidate startup attempts stop on a workload-detector false positive; none contributes performance data. A read-only monitor identifies the latter as `/usr/bin/git` invoked by the Codex desktop app for a diff with hooks/fsmonitor disabled and explicit `--no-ext-diff --no-textconv`. All four qualified timing rows use the same narrowly reviewed detector: only that exact verified executable/parent/cwd/argument grammar is allowed and recorded; unknown, mutating, unreadable or actual compiling processes still block. The three observed real commands and sixteen negative classifier controls are preserved. Qualified runs have no input drift, application errors or checked kernel GPU errors. Validation layers are disabled for clean timing; no native validation qualification is inferred. The short desktop inspection is disclosed rather than represented as a completely idle host.

All experimental vendor files, material packets, generated embeds, test registrations and fixture edits are restored to their pre-trial bytes. The preceding dirty worktree and existing distributed Filament patch/tree pins are preserved. No preset or saved user configuration changes. The canonical helper reproduces the original renderer, native smoke and host-test executables byte for byte, and all 46 restored host CTest entries pass. Exact readback is in `final-restoration.json`, separate from rejected-candidate receipts. The [shared Sun rejection experiment](#november-11-shared-sun-rejection-experiment) tests the follow-up idea of moving rejection outside the repeated ray work; it also does not earn retention.

Reproduction commands, failed attempts, exact sources and binaries, package hashes, CPU controls, runtime/startup JSON and offline HTML traces, raw clean profiles and restoration proof remain under ignored `build/intel-depth-tiles/20261111/`. Start with `analysis/final-rejected-cohort.{json,md}`, `analysis/trace-comparison-01.json`, `analysis/overlap-trace-comparison-01.json`, `oracle/`, `producer-packet-audit/receipt.json`, `tile-v1-rejected-sources/`, `tile-v2-rejected-sources/`, `source-restoration.json` and `final-restoration.json`. Frozen renderer identities are original `4aff61b11df20e51c35704917368a7f224d8d8df57e87be9bde6db8b173d62c3`, separate tiles `503cffa5ff5bca0ed224be4bea0ef3e30c3a93b26d48692d810222331021adc6` and overlapping windows `b13411416f7de21e4c9d715f8bcb319985906a71e7f8bb1baa1aab4fd229438c`. No source release, benchmark score or cross-platform performance claim follows from these experiments.

## November 11 shared Sun rejection experiment

**Sharing conservative rejection across the sixteen Sun rays does not demonstrate a speedup, and both implementations are restored.** The full actual Heaven village retains contact-shadow quality High with up to 32 original depth steps per ray, unchanged sample positions and the sixteen original Sun directions, the fixed noon camera, 1920×1080 Visual Ultra, enhanced 4K assets and 4× MSAA. Hardware remains Intel BMG G31 `8086:e223` on Mesa `26.0.8-1ubuntu0.3`, kernel `7.2.6-070206-generic`, Vulkan/native Wayland, with the existing exact-driver compatibility fix. VRS, frame generation, HDR and dynamic resolution remain off; no geometry, texture, feature or saved user setting is reduced.

The first variant computes one conservative whole-cone depth test before the sixteen-ray loop. Only when every original ray/sample is proven empty does it return zero; otherwise it executes the byte-identical original fine marcher. The second variant bounds only the latter half of the original sample positions. A successful shared proof shortens all sixteen Sun marches to the same prefix, preserving the original `dt`, dither, retained positions, t=0 checks and first hit. It adds no rejection branch inside each fine step. Partially lit caps, non-Sun lights and uncertain inputs retain the full configured march. `TARGET_MOBILE` source controls retain the original full path, and the desktop shader-model allocation gate excludes mobile auxiliary images; this is not device/API acceptance.

Both variants use a separate float32 8×8 min/max depth image and leave the existing structure image, AO/SSR mips and rendering features intact. Shared projected intervals use positive-W checks, cancellation-aware absolute matrix-product reserves and the exact repeated sample-time additions, including samples beyond t=1. Native texel bounds cover clamped edges and up to two-by-two tiles. Unsafe projections or unsupported inputs fail open. A strict front-facing cone test preserves geometric self-contact. Thirteen lit packets are intentionally regenerated for each trial; the other 36 packets retain their bytes. The material compiler's validation behavior is unchanged; its obsolete froxel-sampler comment is temporarily corrected to describe the new sampler occupying that reserved budget.

The first clean pair appears promising, but the complete original/candidate/candidate/original sequence does not confirm it:

| Whole-cone cohort, in order | FPS | Mean frame time | p95 frame time |
| --- | ---: | ---: | ---: |
| Original, opening | 2.95810 | 338.055 ms | 353.173 ms |
| Shared whole cone, first trial | 3.08645 | 323.997 ms | 329.933 ms |
| Shared whole cone, repeat | 3.08540 | 324.107 ms | 330.028 ms |
| Original, closing | 3.14746 | 317.716 ms | 323.919 ms |

The initial +4.34% FPS comparison disappears against a closing original that is **6.40% faster** than opening. The candidate repeats closely, but is 1.97% slower than the adjacent closing reference. Pooled candidate FPS is only 1.08% above pooled reference, smaller than the reference drift, so no repeatable gain is established. Independent ten-second bins show about 1.5–1.8% within-run warming in all four runs; that alone does not explain the persistent cross-run reference shift. Assets and logical cache seeds match. This cohort has no during-run clock or consumed-power observations, so a hardware/cache cause is not claimed.

A fresh cohort adds identical read-only, timestamped Intel GT-frequency sampling at approximately 1 Hz. It measures the shared-tail candidate against two stable original runs:

| Shared-tail cohort, in order | Measured seconds | FPS | Mean frame time | p95 frame time |
| --- | ---: | ---: | ---: | ---: |
| Original, opening | 61.9144 | 3.14951 | 317.510 ms | 320.951 ms |
| Shared latter-half rejection | 60.8979 | 3.07071 | 325.657 ms | 333.477 ms |
| Original, closing | 61.6485 | 3.14687 | 317.776 ms | 325.389 ms |

The two references differ by only −0.0838%. Against their pooled **3.14819 FPS / 317.642 ms**, shared-tail rejection loses **2.46% FPS**, increases mean frame time **2.52%**, and increases p95 **3.21%**. It is rejected after one clean trial; no winning-candidate repetition or statistical-significance claim is made. All timing rows exclude more than two warmup seconds and retain at least sixty measured seconds, with private warm geometry and initially empty private shader caches. Independent analysis reconstructs raw float32 frame intervals and verifies exact hardware, effective settings, asset/cache identities, controller policy and output hashes. Both policies retain the narrow verified desktop-app read-only Git exception; unknown or actual competing build/capture processes still block.

All 59 / 59 / 58 clock snapshots in the final sixty process seconds of the new opening/candidate/closing runs report GT0 actual and requested frequency at **2800 MHz**, with unchanged 1200/2800 MHz configured limits. These are approximately 1 Hz observations, not a frame-level clock lock or proof of causation. Sampling costs about one millisecond per poll, identically applied to all three runs; it adds no GPU wait, driver override or frequency/power write. Consumed-power inputs are unavailable. The observed power-cap setting is configuration and is not reported as measured consumption. The earlier and later controller policies are analyzed as separate cohorts.

Microprofiler observations remain separate from clean timing. Initial original opaque time is 229.194 ms; whole-cone and tail captures show 218.859 and 220.091 ms, but their unchanged shadow, depth and SSR passes also become faster. A fresh post-drift original trace measures 225.318 ms opaque, 53.321 ms shadows, 14.357 ms structure, 17.828 ms SSR and 14.485 ms auxiliary depth. Tail opaque appears 2.32% lower than that trace while the other large passes are about 2.8–3.4% lower, yet its clean mean frame interval is worse against stable references. Neither stage comparison establishes an algorithmic speedup. The reduction itself costs about 0.039 ms. Opaque/structure/SSR/auxiliary submitted workloads match, including 54,523,679 main opaque primitives in 1,717 commands. GPU pass observations are not complete-frame timing or additive CPU/GPU totals. The final trace overwrites 180,201 CPU events and reports 1,453 stale-epoch events; its retained retry-wait union is 95.32% of retained Main-loop wall, not a complete CPU utilization measurement.

Actual-source host contracts compare the complete original and candidate Sun calculations, directions, first hits and sample sequences. Whole-cone desktop gather/fallback each pass 66,839,778 checks, including 17,598,000 original projected samples inside conservative bounds; mobile controls each pass 54,009,972 checks with zero shared early returns. Six deliberate invalid-source controls are rejected. Shared-tail desktop paths each pass 62,982,711 checks, including 8,852,800 original tail positions; mobile controls each pass 56,303,617 checks with full budgets and zero trimming. Seven negative controls cover invalid cone extent, unsafe W, changed time, missing adjacent depth, forced trimming, the strict front-facing/partial-cap full-budget guard and omitted cancellation reserves. Geometric t=0 has a separate positive zero-prefix control. Independent blockers whose first hits occur exactly at High sample indices 16 and 31 (zero-based) remain detected. These are bounded source/math checks, not Intel ISA, native pixel or other-platform acceptance. Since neither candidate passes the performance gate, native image/API qualification and distribution bootstrap/pin changes are not performed.

Both candidate source closures, packet/embed bytes, frozen binaries, failed/superseded builds, host controls, CPU/GPU traces and raw timing data remain under ignored `build/intel-shared-rejection/20261111/`. Start with `analysis/whole-cone-abba/comparison.{json,md}`, `analysis/half-tail-cohort/comparison.{json,md}`, `analysis/half-tail-postdrift-trace-comparison.json`, `oracle/final-contract-receipt.json`, `tail-oracle/final-contract-receipt.json`, `whole-cone-unqualified-sources/` and `half-tail-rejected-sources/`. The original renderer is SHA-256 `4aff61b11df20e51c35704917368a7f224d8d8df57e87be9bde6db8b173d62c3`; whole-cone `c77cbe87758f3537313925e7ea33b9554e999a0ae3946ef9a46e1544ad19da02`; shared-tail `04bcc75c69f0e17d4aac6c0b0135e396930c8c8c4a520580567c3054bd4e318f`. Build receipts bind executable identities; contemporaneous source observations are retained separately.

All nineteen vendor edits, forty-nine production packets and generated embeds are restored, preserving the preceding dirty worktree, original presets and existing Filament patch/tree pins. Source restoration readback covers all 1,193 recorded files before this documentation update. The canonical helper rebuilds the original renderer, smoke and host-test binaries byte for byte; all 46 restored host CTest entries pass. Proof is recorded in `source-restoration.json` and `final-restoration.json`. No benchmark score, release or claimed FPS improvement results from these trials. Further optimization needs measurements of shader execution and actual successful rejection coverage.

## Intel GPU diagnostics checkpoint, November 11, 2026

The next investigation adds optional native invocation counters, compiled-shader properties and bounded shader text to the microprofiler, with read-only Intel clock sampling. These diagnostics preserve the preceding contact-shadow source and all production material bytes. Their retained Filament tree is `91a9e95e4e4f6ef6c7956be4b956a72573ad5ba2`; the final renderer is SHA-256 `87cbc1cb466a684ec231f98dc1c63628aa93c1ddde7c6fa21362e4c5a33bddc6`. The distributed patch, source pins, fresh/repeated bootstraps and native counter/pixel controls pass. See [GPU diagnostics](GPU_DIAGNOSTICS.md) for commands, exact counter semantics, evidence and validation limits.

On Intel BMG G31 at 1920×1080 Visual Ultra/MSAA4/High contact shadows, the main opaque pass remains the largest measured GPU interval at a 213.106-ms median. The common colored-penumbra lit fragment variant reports 265 maximum live registers and compiler spill/fill operations. Removing grass and dense foliage greatly reduces vertex invocations but leaves that pass around 216 ms in a short diagnostic probe. These observations support investigating lit/contact-shadow register lifetimes next; they do not assign a per-material GPU duration or establish an optimization gain. Full/away/grass-disabled captures retain every observed mandatory compiler identity, while optional text loss and overwritten CPU history remain explicit.

Eight clean full-scene windows keep compiler/invocation diagnostics off. Paired original/current/current/original means are 317.793674/317.832114 ms, a +0.0121% disabled-path difference within observed repeat variation. Hidden/visible recording means are 317.416882/320.895258 ms, but surrounding Off controls drift by 1.77%, so precise recording overhead is not isolated. Actual accepted intervals, incomplete retained task histories, observer telemetry and unchanged-input receipts remain under `build/intel-diagnostics/20261111/performance-final-01/`. The separate [signed INT8 compute workloads](INTEL_GPU_FEATURES.md) measure DP4a and cooperative-matrix implementation ratios with exact CPU verification; they do not change Heaven rendering or its score.

## Intel shader register-pressure trials, November 12, 2026

No shader optimization is retained from these trials. Three candidates preserved the High contact-shadow algorithm but failed to establish a safe performance improvement. The final controlled candidate is consistently slower, so all shader/compiler edits, material packets and embedded arrays are restored. Register-pressure reduction remains unresolved.

The workload is the complete Heaven scene at the pinned village camera, 1920×1080, Visual Ultra with enhanced 4K textures, MSAA4 and High contact shadows. All sixteen Sun directions, thirty-two steps per ray, distance, displacement, actual-triangle self-hit, dual-depth reconstruction, tolerances, screen-edge fade and lighting accumulation are preserved. VRS, frame generation, HDR, dynamic resolution, music, video/readback, profiler recording, opt-in compiler/invocation diagnostics and validation layers are off during clean timing. Hardware is Intel BMG G31, `8086:e223` at `0000:03:00.0`, `xe`, Mesa `26.0.8-1ubuntu0.3`, Linux `7.2.6-070206-generic`. Effective configurations, animation policy and private geometry/shader-cache inputs are verified per window; this remains one camera/workload on one driver.

| Candidate | Hypothesis | Measured compiler result | Outcome |
| --- | --- | --- | --- |
| C1: earlier directional visibility | Avoid retaining derived BRDF/IBL values through contact rays | Dominant SIMD16 shader remains at 265 maximum-live registers; spills/fills change 4/24 → 5/22 | No demonstrated gain; restored |
| C2: shared pixel dither | Compute the identical gradient noise once for the sixteen Sun rays | All thirteen observed program groups retain their maximum-live, spill/fill, GEN and SEND counts | No native pressure reduction; restored |
| C3: looped cap inversion | Request `DontUnroll` for the existing twelve-step inverse CDF in eligible desktop Vulkan fragments | Dominant GEN instructions 4,422 → 3,855; code 64,656 → 57,856 bytes; maximum-live 265 and spills/fills 4/24 unchanged | Clean frame time increases 1.77%; restored |

C1's initial sixty-second screen is inconclusive by itself: opening/closing reference means drift from 340.739 to 360.012 ms, with candidate 351.390 ms inside that range. The reference's later bins return near the opening reference, while sparse core-clock observations do not explain the early transient. Its pooled +0.36% candidate difference is not a causal regression or gain. That failure motivates a prospectively declared longer warmup rather than selecting a favorable short tail afterward.

The source-hash-matched baseline assembly identifies scratch slots for the actual contact-normal X/Y and fragment-coordinate X/Y values. The Sun marcher reloads coordinates to evaluate gradient noise. This motivates C2, whose host arithmetic is exact but whose native maximum-live, spill/fill, GEN and SEND compiler statistics remain unchanged. The complete private driver dump uses [Mesa's documented `fs,ann` and hash-filter controls](https://docs.mesa3d.org/envvars.html#intel-debug), separately from all clean timing. A parent reader bounds retained logs to sixteen MiB without changing the production sixty-four-KiB executable-IR retrieval limit. The initial sandbox-display failure and a process-wide file-limit startup failure remain archived; no clock, power, CCS or global driver setting is changed.

C3's final packaged SPIR-V proves induction from zero through twelve iterations with `DontUnroll`, and its matched native NIR/assembly proves the actual twelve-bound backedge inside the sixteen-ray loop. All thirteen rebuilt production models retain that loop. The colored-penumbra packet's other hundred excluded shader texts, including OpenGL, mobile, vertex, depth and SSR routes, match the baseline bytes. Static code size decreases across the eleven lit program groups, but this does not measure dynamic instruction work, occupancy or runtime memory traffic. Anisotropy is less favorable: its spills/fills increase from 16/38 to 24/48. Driver cycle estimates are not performance measurements.

The final reference → candidate → candidate → reference cohort requests 125 runtime seconds per launch. Before execution, its analysis specifies the shortest contiguous suffix of complete accepted intervals totaling at least sixty seconds and requires at least sixty seconds of preceding accepted intervals. Actual warmups are 64.90–65.00 seconds. Original native statistics are preserved separately; earlier sixty-five-second captures are never relabelled with this protocol.

| Window, in order | Accepted FPS | Mean frame time | p95 frame time | Retained seconds |
| --- | ---: | ---: | ---: | ---: |
| Baseline A3 | 3.15176 | 317.283 ms | 321.540 ms | 60.284 |
| Candidate C3B1 | 3.09729 | 322.862 ms | 328.728 ms | 60.052 |
| Candidate C3B2 | 3.09824 | 322.764 ms | 329.015 ms | 60.034 |
| Baseline A4 | 3.15335 | 317.123 ms | 322.152 ms | 60.253 |

Pooled retained means are **317.202893 → 322.813431 ms (+1.76875%)**, or **3.152556 → 3.097765 FPS (−1.73801%)**. Reference repeat spread is 0.0503% and candidate repeat spread 0.0304%. The separate native full-window comparison also slows by 1.75537%. Both C3 repeats are consistently slower in this cohort. These are accepted presentation intervals, not physical scanout; asynchronous last-completed GPU samples are never added to CPU intervals. Nonzero late GT0 clock observations are 2,800 MHz; a final zero sample does not establish throttling. The 275-W power cap is configuration, not measured consumption.

C1's actual-source guard matrix passes 9,984 comparisons. C2/C3 arithmetic oracles each compare more than eight million exact ray samples across desktop/mobile and gather/fetch paths, with deliberate invalid controls rejected. C3's actual C++ prolog scope matrix checks 24,576 states. Its native Vulkan suite passes 56 cases with six disclosed skips, including all eighteen stability configurations and independent finite-Sun blocker tests at unchanged thresholds, with zero validation or kernel GPU errors. These correctness results do not establish every-view pixel parity; extended paired image and other-platform qualification is not reached after the performance rejection. One diagnostic capture is interrupted by an unknown competing process match and is retained separately; its retry passes. C3 compiler metadata coverage is complete, while its 61 pending trailing-frame query samples remain explicitly pending.

All 1,206 original source-file hashes match the opening baseline before this documentation update, and the separately preserved compiler source/header also match. All forty-nine production packets and both embedded-array files are restored. The canonical helper rebuilds renderer `87cbc1cb466a684ec231f98dc1c63628aa93c1ddde7c6fa21362e4c5a33bddc6`, smoke harness `c4314d29c3080abffc9c593d14de7006ab55090dd0ef9d08286ac17d0922220d` and matc `ac38d12484eaa7070f46024f2800c8c7090278b9df1807001e3ffc5dba6b5bc0` byte for byte. All forty-eight restored host CTest cases pass. The four external test-material packets match the pre-task November 11 fixture/full-smoke receipts. Vendor HEAD/index/status semantics and the existing distributed patch/hash/tree pins are unchanged; no new source bootstrap is claimed for these rejected candidates.

Exact commands, immutable candidate binaries/source closures, failed controls, native shader dumps, checksums and raw profiles remain under ignored `build/intel-register-pressure/20261112/`. Start with `candidate-3-final-summary.md`, `long-cohort-c3-01/comparison.json`, the `contracts/` receipts, `driver-dump/` analysis and `final-source-restoration.json`. The cohort checks 267 explicit artifact paths and 1,207 candidate source hashes; the additional compiler path leaves that changed-file set again after restoration. Current-source observations and the older executable's preserved embedded-source closure remain separately labelled. Do not reintroduce any of these candidates as an established optimization or claim that smaller compiler output implies lower register pressure or faster rendering.

## Filament 1.80.1 follow-up, November 13, 2026

The November 12 register-pressure trials above retain their Filament 1.77.2/package77 source and exact binaries. The current 1.2.0 candidate uses Filament 1.80.1/package80, retains the original High contact-shadow policy and excludes all three rejected shader rewrites. Register-pressure optimization remains open; old compiler/register estimates do not automatically describe the new base. [Release readiness](RELEASE_READINESS.md#november-13-filament-1801-and-120-candidate) records the separate source/native/static/image checks and pending CI/package/publication gates.

Fresh cohort04 compares the preserved development renderer `87cbc1cb466a684ec231f98dc1c63628aa93c1ddde7c6fa21362e4c5a33bddc6` (application stamp 1.1.1, Filament 1.77.2) with final local 1.2.0 renderer `3cf93279ce4b295e3f54c5af5f30ae27e43d9bdeceb89086ccdf71923f4bfcbb` (Filament 1.80.1). The baseline is **not the published 1.1.1 release**. The full Heaven fixed-noon/fixed-camera scene uses Intel BMG G31 / Mesa 26.0.8, Vulkan/X11, 1920×1080, Visual Ultra, enhanced 4K textures, MSAA4 and unchanged High contact shadows at 32 steps / 16 Sun rays. Frame generation, VRS, dynamic resolution, HDR, capture, recording, invocation/IR diagnostics, validation and music are off. Ordinary wall-time animation remains active.

A1→B1→B2→A2 provides two windows per treatment, each requesting 125 runtime seconds. The complete native profile preserves its ordinary warmup and full statistics. Before launch, a separate tail policy selects the shortest final contiguous suffix of whole accepted intervals reaching 60 seconds, requiring at least 60 preceding accepted seconds; no interval is trimmed or boundary-interpolated, and failed earlier 65-second cohorts are not relabelled. Prefix durations are 64.799–65.118 seconds and retained tails 60.034–60.220 seconds.

| Interval scope | Development baseline mean | 1.2.0 candidate mean | Mean frame-time change |
| --- | ---: | ---: | ---: |
| Complete native measured windows | 327.928 ms | 329.834 ms | +0.581% |
| Prospectively declared final tails | 325.886 ms | 334.546 ms | +2.657% |

Complete-window pooled accepted FPS is 3.04945→3.03183 (−0.578%), with baseline endpoint mean drift +2.636% and candidate repeat drift −0.840%. Adjacent full-window mean changes have opposite signs, +2.319% and −1.146%. Declared-tail accepted FPS is 3.06855→2.98913 (−2.588%); both adjacent tail comparisons are slower, +3.112% and +2.207%, with baseline/candidate tail repeat drift −0.234%/−1.110%. These are descriptive statistics from autocorrelated frames in one view with two repeats, not confidence bounds or a universal causal estimate. The cohort associates modestly slower declared tails with this candidate and demonstrates no speedup.

The whole-controller background log retains 15 accepted observations of three unique verified read-only GUI Git processes in A1 and none in B1/B2/A2, with 15 successful same-process additive rechecks and no blockers or failed rechecks. Those counts include startup and are not frame/tail aligned. Accepted desktop Git activity is not CPU/I/O isolation. Read-only one-Hz GT0 current-frequency medians are 2800 MHz across all four controller traces, including startup; this is not tail alignment, thermal/power normalization or proof of isolation.

Accepted presentation intervals include rejected attempts and are not physical scanout/display FPS. Asynchronous last-completed whole-frame GPU values may lag or repeat; their column means are not independent GPU distributions, CPU/GPU clocks are not aligned and their durations are not added. All four windows retain unchanged source/effective quality and no recorded validation finding or kernel GPU error, and independent post-analysis checks verify every owned controller/renderer/analysis process drained. Source closures, plans, original profiles, separate full/tail calculations and GUI observations are retained under ignored `build/filament-1.80.1/20261112/native-plan/performance-final-04/{final-results-handoff.json,comparison-summary.md}` and `performance-x11-final-04/`. Failed cohorts01/02/03 remain preserved, unqualified and unused. This local development comparison establishes neither published-release performance nor other-platform acceptance; fresh exact-source CI, all eight package/source audits, downloaded-package controls and publication remain pending.

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
