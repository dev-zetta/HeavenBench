# GPU diagnostics

GPU diagnostics extend the microprofiler with actual shader-invocation queries and driver compiler information. They are optional Vulkan diagnostics, including on Intel GPUs; ordinary launches retain their existing device and pipeline configuration. Invocation counts, compiler statistics and GPU durations answer different questions and remain separate in the reports.

## Capture

```sh
./scripts/run.sh --gpu 8086:e223 --gpu-diagnostics --microprofile build/profiles/intel --microprofile-seconds 10
./scripts/run.sh --gpu 8086:e223 --gpu-shader-ir --microprofile build/profiles/intel-ir --microprofile-seconds 10
python3 scripts/analyze_microprofile.py build/profiles/intel.json
```

Use the adapter identity reported on your own system. `--gpu-diagnostics` requests pipeline statistics and compiled-executable information before Vulkan creates the logical device. `--gpu-shader-ir` also requests internal shader representations and implies diagnostics. Both flags are transient: they do not become saved settings, start recording or open the profiler panel. Without `--microprofile`, use Diagnostics → Microprofiler to record and save later. Compilation information retained before recording is replayed into a new capture on the backend thread. See [microprofiler controls and exports](MICROPROFILER.md) for frame selection, freezing, JSON/HTML files and capture limits.

Diagnostic pipeline flags and hardware queries can change compiler behavior and performance even while the panel and recorder are closed. Every benchmark tour requesting these options is unscored, including unsupported devices and tours that stop recording before completion. For ordinary performance measurements, restart without these options. Compare captures only with matching hardware, driver, API, resolution, effective settings and diagnostic options.

## What the counters mean

Each eligible native pass has an independent query status and reason alongside its timestamp status. Available queries report input-assembly vertices/primitives, vertex-shader invocations, clipping invocations/primitives and fragment-shader invocations. These are actual Vulkan query results, rather than estimates from index counts. Submitted primitives and draw-instance invocations remain separate workload fields. Fragment invocations are shader work, not a unique visible-pixel count; multisampling, helper work, overdraw and shading rates affect interpretation. Do not add nested pass ranges together.

Khronos documents these counters as relative observations whose counting details can differ across architectures. See the [pipeline-statistic definitions](https://docs.vulkan.org/refpages/latest/refpages/source/VkQueryPipelineStatisticFlagBits.html) and [executable-properties extension](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_pipeline_executable_properties.html).

View identity matters: a main scene, auxiliary depth view, UI and composite can all contain a pass named `Color Pass`. Hardware-counter distributions group by renderer/view/name, and `gpu_by_view` / `workload_by_view` retain source and view IDs. Compatibility summaries named `gpu` / `gpu_workloads` explicitly aggregate across views. IDs are process local and must not be joined between independent captures. The HTML viewer labels views separately.

The native storage is bounded to sixteen unresolved frames and 256 pass invocations per frame. Results are polled asynchronously without query waits in ordinary frames. Unsupported features, allocation failures, overflow, incomplete scopes, native-command boundaries and unresolved results retain explicit statuses; an unavailable query is never a measured zero. Timestamp errors do not automatically invalidate completed invocation counters, and counter errors do not fabricate GPU timings. GPU intervals remain frame relative and are not calibrated to the CPU clock.

## Compiled shaders

`VK_KHR_pipeline_executable_properties` supplies driver-named executable properties and typed compiler statistics. Reports preserve those names, descriptions, scalar types, shader-stage masks and subgroup sizes. Depending on the driver, statistics may describe register use, spills or instruction counts. They are compile-time information, not measured runtime cache misses, EU utilization or occupancy. This integration does not implement Vulkan performance-query counters. The tested Intel Mesa capability inventory does not advertise `VK_KHR_performance_query` or `VK_INTEL_performance_query`.

Pipeline-use events link a pass to the pipelines and program names actually bound in it. A pass can use many pipelines. Pipeline and program IDs are opaque process-local correlation keys, not native pointer values or identities reproducible across builds. Executable metadata and text are separate from runtime pass intervals. Missing metadata or truncated names remain a coverage limit when interpreting links.

Shader text is optional and requires driver support. The diagnostic cache retains at most 4,096 actual pipeline creations within sixteen MiB, reserving twelve MiB for compiler metadata and four MiB for optional representations. Pipeline IDs retain each actual compilation identity even when equivalent pipelines are recreated. Text retrieval is limited to 64 KiB per representation; exports retain a prefix of at most 4,096 bytes, its original size and a truncation reason. Binary representations are unavailable as text. The recorder shares repeated metadata strings and protects pipeline headers within its existing desktop/mobile recording budget; optional text has separate bounded storage, and stale text references become explicitly unavailable. Cache loss, header/statistic loss, optional-text loss and overwritten history remain visible. Metadata replay and shader enumeration run outside hot draw callbacks; they can still increase diagnostic frame time.

Compiler-cache limits apply to the engine's diagnostic lifetime, including pipeline recreation. Long sessions can exhaust compiler metadata before a later recording starts; the report discloses that loss while invocation queries continue independently. Restart the diagnostic launch to begin a fresh compiler catalog. A new recorder epoch alone does not reset the engine cache.

## Intel clocks

The optional sampler also checks standard-library support for `std::jthread` and stop tokens. Toolchains without that capability, including the pinned Android NDK, compile without the worker and report an explicit unavailable reason if clock sources are discovered. This does not fabricate clock readings or disable the independent GPU query/compiler diagnostics.

On Linux, an explicitly requested diagnostic session can sample read-only `xe` frequency files once per second on a worker. Discovery verifies the renderer's complete PCI address and vendor/device identity before looking for tile/GT sources. Missing files, malformed values and other drivers remain unavailable; an idle value of zero is retained as a valid observation. The capture records actual/requested/minimum/maximum MHz separately for each source.

These samples use host observation timestamps and do not align GPU and CPU clocks. Frequency limits are configuration, not measured frequency or power consumption. The sampler makes no clock, power, driver or thermal-setting changes and performs no main-loop file reads. Normal launches do not create this worker.

## Validation record

Implementation and validation evidence for the November 11 addition is retained under ignored `build/intel-diagnostics/20261111/`; INT8 compute correctness and measurements are separate under `build/intel-int8/20261111/`. The final build08 renderer has SHA-256 `87cbc1cb466a684ec231f98dc1c63628aa93c1ddde7c6fa21362e4c5a33bddc6`. Native checks use Intel BMG G31, PCI `8086:e223` at `0000:03:00.0`, the `xe` driver and Mesa `26.0.8-1ubuntu0.3` on Linux.

All 48 selected host CTest cases pass. Actual-source vendor tests exercise query lifetime, independent timing/statistic availability and bounded compiler enumeration. The final triangle fixture measures three input vertices, one input primitive, three vertex invocations, one clipping input/output primitive and 5,408 fragment invocations per controlled color pass, independently of its single draw command. It checks visible pixels, recording-off/on pixel equality and metadata replay across captures. The final Vulkan suite passes 56 cases with six disclosed skips; the GLX fallback correctly exports unavailable invocation counters and no compiler data. Native receipts report zero Vulkan validation findings and zero kernel GPU-error lines. The final suite's initial music sanity fixture failed while host tests ran concurrently; its idle-CPU repeat passed with music-on/off timing at 98.2%. Missing music assets prevent an audio-playback acceptance claim, and the timing difference is not proven to have only one cause.

The final canonical LLVM 21.1.8 analysis gate passes all 442 owned renderer/lab and two extraction sources, with 18 recognition and nine coverage tests, unchanged fingerprints for 634 inputs and four explicitly reviewed bitmask baselines. Corrected intermediate findings, interrupted scans and verified process-group cleanup remain in the evidence directory. The retained Filament patch also passes two independent tree reconstructions and complete fresh/repeated offline bootstraps. All 49 production material packages and both embedded-array files retain their preceding hashes.

The final full-Heaven capture retains 732 available native query samples and 100% observed mandatory compiler coverage: opaque 78/78 pipeline IDs including fragment statistics, shadows 84/84 and auxiliary depth 42/42. Earlier final-vendor full/away/grass-disabled captures also retain every observed mandatory header/statistic. The initial fixed-size metadata implementation lost most real-scene shader identities; its failed coverage evidence remains under `scene-01-full/`. Compact shared strings, protected headers and separate optional text storage correct that failure without increasing the recorder's 64/16-MiB budgets. The clock worker registers after the native GPU producer so one-Hz telemetry does not consume its larger CPU-history ring. CPU history still overwrites under this workload, so complete CPU-exclusive costs or CPU-utilization certification are unavailable.

Three actual-Heaven image controls use identical camera/quality, 90 accepted frames, fixed 60-Hz simulation, 1.5-second source time and frozen floating islands: recording Off, diagnostics/IR/recording On, then an Off repeat. They pass native validation and preserve all runtime inputs. Images are not byte-exact: Off/Off mean absolute channel delta is 0.472811 and RMS 0.717016, while Off/On is 0.473204 and 0.717277 on an 8-bit scale. Maximum differences are 26–29. The recording comparison stays at the observed repeatability floor, and inspected images show no broad geometry/material change. Temporal/dither variation is a supported inference rather than proven causality. Raw PPMs, metadata, region statistics and PNG differences remain in `image-variation-summary.*` and comparison folders; these bounded views do not establish complete scene-image parity.

## Scene observations and next optimizations

The final fixed village view uses Visual Ultra, enhanced 4K textures, 1920×1080, MSAA 4×, High contact shadows and the original authored workload. Compiler capture, hardware queries and validation are enabled. These are short diagnostic observations, rather than matched clean FPS or overhead measurements. GPU ranges are inclusive and can overlap; the table is not a frame-time sum.

| GPU pass | Samples | Median GPU interval |
| --- | ---: | ---: |
| Main opaque | 12 | 213.106 ms |
| SSR | 12 | 17.347 ms |
| Shadow invocation | 48 | 16.037 ms |
| Structure | 12 | 13.933 ms |

The opaque workload invokes approximately 62.86 million vertex shaders and 14.47 million fragment shaders per pass. In separate diagnostic probes, disabling grass and dense foliage reduces vertex work to 38.67 million but leaves the opaque interval around 216 ms; facing away reduces it to about 24 ms. These probes deliberately change the workload and have few samples, so they are evidence for investigation rather than measured optimization gains. The away view still submits about 22 million main-pass primitives to produce about two million clipping-output primitives, while shadow vertex work remains large. Clipping can create primitives, so these are not an exact rejected-triangle percentage.

Mesa reports 4,422 GEN instructions, 88 SEND instructions, four spill operations, 24 fill operations and 265 maximum live registers for the frequently used SIMD16 colored-penumbra lit fragment variant. Clear-coat and anisotropic variants report further spill/fill work. The driver's separate scratch/GRF fields are zero despite these reported operations; preserve the raw values without inferring measured memory traffic or occupancy. No per-material GPU time is assigned from a shared pass. The evidence supports investigating contact-shadow/lit-shader register lifetimes first, followed by finer per-pass geometry/meshlet culling and repeated structure/SSR/auxiliary-depth geometry work. Main-camera culling must preserve off-camera shadow casters. See `scene-diagnostic-summary.md` and the per-variant compiler JSON files in the evidence directory.

Native query resize/reload, Android/Windows/NVIDIA execution and physical latency remain untested by these controls. The browser security policy rejected local `file://` navigation, so interactive offline-browser inspection is unverified; native exports do establish matching JSON/HTML data, and JavaScript syntax and view-separation regressions pass. Optional IR truncation and independent vendor/app IR exhaustion remain explicit. Source/build checks and these bounded native controls do not establish every hardware path or a scene FPS improvement.

The November 12 continuation tests three quality-preserving shader rewrites using these diagnostics and a separate bounded native assembly dump. None lowers the dominant maximum-live count or qualifies for retention. Keeping the twelve-step inverse-CDF loop reduces compiled code size but slows a prospectively warmed full-scene comparison from 317.20 to 322.81 ms (+1.77%); the original source, packets and binaries are restored. See [the register-pressure trials](PERFORMANCE_OPTIMIZATIONS.md#intel-shader-register-pressure-trials-november-12-2026) for all candidates, native correctness, stable timing controls, limitations and exact evidence. Register-pressure optimization remains unresolved.

## Disabled-path and recording controls

Eight clean native windows use the same Intel driver, full village view, 1920×1080 Visual Ultra/4K/MSAA4/High-contact workload and private warm geometry caches. Each requests 65 seconds, excludes at least two warmup seconds and retains at least sixty measured seconds. Compiler/invocation diagnostics stay off throughout. The first four alternate original → current → current → original; four more measure current Off → hidden recorder → visible recorder → Off. All eight strict receipts pass, with frozen source/assets/binaries, matching effective workloads, no detected competing benchmark/compiler/Tidy jobs, no kernel GPU errors and reaped process groups. Desktop activity is not completely isolated.

| Condition | Windows | Mean accepted frame interval |
| --- | ---: | ---: |
| Original, recorder off | 2 | 317.793674 ms |
| Current, recorder off, paired cohort | 2 | 317.832114 ms |
| Current, local off controls | 2 | 320.523659 ms |
| Current, recording hidden | 1 | 317.416882 ms |
| Current, recording visible | 1 | 320.895258 ms |

The disabled-path paired difference is +0.0121%, within the observed repeat spread for this fixed configuration. Hidden/visible windows are −0.969%/+0.116% relative to the pooled local Off controls, whose individual intervals drift from 317.739420 to 323.351402 ms (+1.77%). This does not establish a negative recording cost or isolate a precise overhead; the recording windows remain within the control variation. One window per recording mode and one hardware/configuration do not certify a universal five-percent overhead bound. These are accepted presentation intervals rather than physical scanout measurements. Opt-in compiler/statistic diagnostics can have additional costs and remain unscored.

Both recording windows contain genuine CPU spans, GPU samples and frame markers, with explicit ordinary-pipeline metadata. Their bounded histories are incomplete: hidden recording drops zero events and overwrites 1,097,844, while visible recording drops 6,163 and overwrites 787,162; stale-epoch counts are 1,504 and 132. Do not infer complete sixty-five-second task coverage from these files. Visible p95 is 353.662 ms versus 324.998/344.827 ms for the surrounding Off windows. Read-only one-Hz observations show median actual/requested GT0 clocks of 2,800 MHz in all eight windows, with unchanged limits; sampling does not explain the closing-Off drift or establish thermal/power isolation.

Raw intervals, actual hidden/visible CPU/GPU microprofile files, startup sidecars, strict process/kernel/input receipts, clock observations and the recomputed summary are retained under `build/intel-diagnostics/20261111/performance-final-01/`. The original executable is pinned to SHA-256 `4aff61b11df20e51c35704917368a7f224d8d8df57e87be9bde6db8b173d62c3`; current-source observations do not assign that source to the frozen original binary. No clock/power setting, rendering quality, geometry, material package or benchmark scoring formula was changed to obtain these comparisons.
