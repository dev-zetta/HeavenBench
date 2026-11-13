# Intel GPU workloads

The optional GPU lab measures matrix compute separately from the Heaven scene. It provides FP16-input/FP32-output shader and cooperative-matrix workloads, plus signed INT8-input/INT32-output shader, packed integer-dot/DP4a and cooperative-matrix workloads. On compatible Intel GPUs, cooperative matrices expose the XMX matrix path through a standard Vulkan API. This is a hardware diagnostic, not XeSS, a neural rendering effect, a scene FPS improvement or part of the benchmark score. The GPU lab itself does not change the ordinary renderer, Filament, material packages or default configuration; the separate optional scene VRS integration is described below.

## Build and run

Native Linux, the normal Clang/libc++ build environment, Vulkan 1.3 headers/loader, `glslangValidator` and `spirv-val` are required. All shaders, including the deliberate wrong-store correctness control, are compiled and validated during the optional build; no shader compiler runs during measurement. Existing release packages do not include this new executable. The target is disabled by default with `HEAVEN_BUILD_GPU_LAB=OFF` and uses the project's shared build-directory helper.

```sh
./scripts/run_gpu_lab.sh --build --list
mkdir -p build/gpu-lab
./scripts/run_gpu_lab.sh --gpu 8086:e223 --quick-check --output build/gpu-lab/check.json
./scripts/run_gpu_lab.sh --gpu 8086:e223 --output build/gpu-lab/intel.json
python3 scripts/analyze_gpu_lab.py build/gpu-lab/intel.json
# Signed INT8 correctness check, then a clean 60-second window per kernel:
./scripts/run_gpu_lab.sh --gpu 8086:e223 --workload int8 --kernel all --quick-check --validation --output build/gpu-lab/int8-check.json
./scripts/run_gpu_lab.sh --gpu 8086:e223 --workload int8 --kernel all --output build/gpu-lab/int8.json
python3 scripts/analyze_gpu_lab.py build/gpu-lab/int8.json
```

Use the vendor/device ID reported by `--list`, not the example's ID on another machine. Selection must identify exactly one adapter; missing or ambiguous devices fail without substituting another GPU. Capability enumeration works without scene assets or a display. An unsupported matrix kernel is explicitly unavailable; merely advertising another graphics extension does not establish its logical-device enablement or execution.

The backward-compatible default is `--workload fp16 --kernel both`, selecting reference and cooperative kernels. `--workload int8 --kernel all` additionally selects DP4a; `--kernel dp4a` requires INT8. Individual `reference`, `dp4a` and `cooperative` selections support balanced repeated runs: reference → DP4a → cooperative → cooperative → DP4a → reference, retaining a separate file for each run. `both` always means reference and cooperative, including for INT8. `--seconds N` accepts 60–600 seconds per kernel; normal runs exclude at least two warmup seconds. `--quick-check` is a correctness/timing sanity check and is refused by the performance analyzer. Ctrl+C cancels without exporting an incomplete performance result. `--validation` explicitly requires Khronos validation and is for correctness runs; the performance analyzer refuses validation-enabled captures.

## Measurement and correctness

Each dispatch multiplies four independent 256×256 matrices. For the default FP16 workload, both kernels read identical FP16 values, accumulate and store FP32, and use device-local working buffers with separate coherent staging. An independent integer CPU oracle checks all 262,144 output values before and after each timing window. Inputs are exact fractions of 1/16, making the products and bounded FP32 sums exactly representable. The destination is poisoned before each check so stale results from another kernel cannot pass. Dispatches in a timed batch have explicit write dependencies; the report records that dispatch/barrier overhead is included.

Two bounded Vulkan timestamps measure each completed batch. Upload, final readback and CPU verification are outside the GPU interval. Timestamp precision, valid bits and queue family are retained; intervals that might hide an entire counter wrap are rejected. These headless diagnostics deliberately wait for their own small batches and never add waits to ordinary renderer frames. GPU intervals and host elapsed time remain separate clocks. Reports preserve raw samples, adapter/driver UUIDs, enabled validation, build/compiler identity, a source-and-SPIR-V implementation fingerprint, workload dimensions and coverage. The analyzer recomputes statistics from raw intervals and refuses incompatible captures or incomplete coverage. Output publication is atomic and does not replace an existing capture; a failed export may leave a `.partial` file for investigation.

Throughput counts a multiply-add as two operations: FP16/FP32 results use FLOP/s, while INT8/INT32 results use integer GOP/s and never FLOP/s. Observed ratios compare kernel implementations, whose thread organization, memory access and synchronization differ; they are not an isolated instruction speedup or a claim of theoretical XMX peak. The small working set can be cache-resident. Keep the same hardware, driver, implementation, build, workload and competing GPU activity when comparing runs. Desktop composition and other GPU applications can affect the measurements.

## Native Linux feature inventory, November 10, 2026

The tested system reports Intel Battlemage G31, PCI `8086:e223`, the `xe` kernel driver and Mesa 26.0.8. Its cooperative matrix inventory includes subgroup 8×16×16 FP16-input/FP32-accumulator operations. The Vulkan capability dump also advertises FP16, integer dot products, fragment shading rates, ray queries and mesh shaders. The matrix workload above retains its independent measured evidence below; the separate VRS integration now implements an optional real-scene shading-rate setting, with its own native and image/performance acceptance record.

| Feature | Current scope |
| --- | --- |
| XMX through cooperative matrices | Optional GPU-lab implementation with an independent shader reference and CPU oracle. |
| FP16 storage and arithmetic | Queried and enabled explicitly for the matrix workload; shader reference accumulation remains FP32. |
| Signed integer dot product / DP4a | Optional INT8/INT32 workload, with signed packed acceleration and logical-device enablement checked independently; see the November 11 controls below. |
| Variable-rate shading | Implemented as a default-Off optional Vulkan main-color quality setting, with explicit logical-device/sample-count support and fallback reasons; see [VRS controls, scope and acceptance](VARIABLE_RATE_SHADING.md). Actual-scene measurement and image acceptance are recorded separately. |
| Ray queries and mesh shaders | Advertised; no scene implementation or performance claim. |
| XeSS super resolution | Intel's published SDK requires Windows; its Vulkan API is an upscaling path. Native Linux runtime integration is not supplied by this addition. |
| XeSS frame generation and XeLL | Intel's published support matrix lists DirectX 12, which this Vulkan/Filament renderer does not use. |

Intel's official SDK tree was inspected on November 10: its runtime files are Windows DLLs, with no Linux `.so` runtime. This statement concerns that published SDK, not experimental Proton/community work. Relevant primary references are [Intel's API support matrix](https://www.intel.com/content/www/us/en/developer/topic-technology/gamedev/xess.html), [XeSS SDK requirements](https://github.com/intel/xess/blob/main/doc/xess_sr_developer_guide_english.md), [Vulkan cooperative matrices](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_cooperative_matrix.html) and [Khronos' hardware mapping overview](https://github.khronos.org/Vulkan-Site/tutorial/latest/ML_Inference/Advanced_Topics/03_vendor_optimizations.html).

Builds, capability dumps, the deliberate missing-output shader control, validation logs and repeated measurements are retained under ignored `build/intel-features/20261110/`. Native numerical correctness and validation do not establish rendering quality, Windows compatibility, physical latency or a scored Heaven improvement.

## Earlier measured FP16/FP32 workload, November 10

Four Release runs on this Intel/Mesa device used reference → cooperative → cooperative → reference order, with two seconds of warmup and at least sixty measured wall seconds per run. Validation was disabled during timing. All four processes exited zero, retained identical implementation/binary inputs, and passed complete numerical checks before and after measurement. Separate final synchronization-validation checks reported zero errors and zero warnings. A deliberate shader with all output stores removed failed the CPU oracle; restoration reproduced the working executable byte for byte.

| Kernel | Mean GPU time per dispatch, averaged over two runs | Per-run range | Arithmetic throughput range |
| --- | --- | --- | --- |
| Shared-memory tiled shader | 0.04401518 ms | 0.04361775–0.04441261 ms | 3.02–3.08 TFLOP/s |
| Cooperative matrix / XMX path | 0.00584788 ms | 0.00583445–0.00586130 ms | 22.90–23.00 TFLOP/s |

The observed implementation time ratio is **7.53×** for this fixed compute workload. This is not a scene FPS gain, an isolated instruction comparison or hardware-peak qualification. Raw captures and independently recomputed statistics are `measurements/*.json` and `analysis.json` under the ignored evidence directory above. The compiled implementation fingerprint is `d9d2fe3ff1a79219e64794026bf4bb8bb4c836e3f1ab151d9129c10044215b65`.

## Signed INT8 controls, November 11, 2026

All three INT8 implementations multiply the same four 256×256 signed matrices and store exact INT32 outputs. A is row-major; B is column-major. Bytes are packed four per UINT32 in little-endian order. The shared reference unpacks and multiplies INT32 values, while DP4a performs signed packed four-byte dot products. These paths require ordinary 32-bit storage only. The cooperative path views the identical input bytes as signed INT8 and explicitly enables `shaderInt8`, `storageBuffer8BitAccess`, `cooperativeMatrix`, `vulkanMemoryModel` and `maintenance4`. Its actual subgroup shape must use signed INT8 for both inputs, signed INT32 for accumulation/output, nonsaturating accumulation and dimensions dividing 256. Packed-dot execution additionally requires `shaderIntegerDotProduct` and `integerDotProduct4x8BitPackedSignedAccelerated`; unsigned-only acceleration does not qualify. Unsupported kernels retain a specific unavailable reason.

Deterministic inputs span −128 through 127 and deliberately include signed extremes, zero and alternating signs. An independent INT64 CPU matrix sum checks every one of the 262,144 INT32 output elements before and after timing. The worst-case accumulator magnitude is 4,194,304, below the INT32 limit. The entire output is poisoned with INT32_MIN before each check. The explicit `--workload int8 --kernel reference --quick-check --negative-control zero-store` invocation selects a deliberately incorrect shader and must fail without publishing a capture; it cannot produce performance evidence. The CPU contract test independently checks production-sized logical matrix multiplication, byte packing, signed boundaries, stale poison, zero-output, unsigned-result and incomplete-output controls.

Native Intel BMG G31, PCI `8086:e223`, Mesa 26.0.8 passes the shared reference, packed-dot and cooperative quick checks with the retained Khronos 1.4.341 layer: all outputs match exactly before and after each check, with zero validation errors and warnings. The enabled signed cooperative shape is 8×16×32. The real zero-store GPU control fails at element zero and produces no result file. Default FP16/FP32 quick checks also pass. AMD Radeon 610M passes the signed reference and DP4a controls; cooperative matrices remain explicitly unavailable. These quick checks establish numerical/feature correctness on those devices, with performance windows and other platforms separate.

The additive schema-1 reports retain FP16 compatibility and include INT8 precision, signedness, layout, input bounds, operation counts, enabled logical-device features and the actual cooperative shape. The analyzer recomputes integer GOP/s from raw GPU intervals, compares only matching precision, hardware, driver, build and source/SPIR-V identities, checks signed acceleration/feature/shape evidence, and rejects quick checks, validation-enabled windows, incomplete coverage and integer results labelled as FLOPs. Native logs, the initial sandbox-only llvmpipe enumeration, unavailable global-layer attempt, successful actual-adapter enumeration and local-layer controls are retained separately under ignored `build/intel-int8/20261111/`. The focused host selection passes all three GPU-lab CTest cases; the analysis suite contains eight Python cases. Full rendering-suite and full-source analysis acceptance remain separate.

## Measured signed INT8 workload, November 11

Six Release windows on the same Intel/Mesa adapter used reference → DP4a → cooperative → cooperative → DP4a → reference order. Every window excludes at least two warmup seconds and retains at least sixty measured wall seconds, with validation disabled and complete exact pre/post output checks. All six processes exit zero and retain the identical executable, shader/input implementation and device/driver identity. The implementation fingerprint is `1af1c3bbdf5298a8655d7c816bd691332e5d49fe92a6a2da8830e68ab8cdecfd`; the executable SHA-256 is `ffa17cbd5b99ac7efb9b19b16fc8f190ccafeae69b71bdbeb3dd1f74f118447c`.

| Signed INT8 → INT32 implementation | Mean GPU time per dispatch, averaged over two runs | Per-run range | Integer throughput range |
| --- | --- | --- | --- |
| Shared-memory tiled shader | 0.05502717 ms | 0.05495843–0.05509592 ms | 2.436–2.442 TOP/s |
| Packed-dot / DP4a shader | 0.01933398 ms | 0.01933170–0.01933626 ms | 6.941–6.943 TOP/s |
| Cooperative matrix / Intel XMX path | 0.00461962 ms | 0.00459051–0.00464873 ms | 28.872–29.238 TOP/s |

The observed implementation time ratios are **2.85×** reference/DP4a, **11.91×** reference/cooperative and **4.19×** DP4a/cooperative. They include different tiling, memory access, synchronization and automatic GPU clock/power behavior. These are integer operation rates for the fixed cache-resident compute workload, not FLOP/s, isolated instruction throughput, a hardware-peak result or a Heaven scene FPS improvement.

The strict controller refused an initial start because cancelled static-analysis children were still running; that failed start produced no measurement. The exact owned process group was stopped and verified drained before retry. The successful six windows detect no known competing benchmark/compiler/Tidy process in one-second checks. Renderer source editing continued without builds or tests; desktop composition and unrelated GPU activity were not isolated. Read-only telemetry retains unchanged frequency-limit/power-profile values, package temperatures of 46–71°C, and median active render clocks around 2,650 MHz for the reference versus 2,800 MHz for DP4a/cooperative. Clocks were not locked or normalized, and sampled telemetry does not prove the absence of throttling. No clocks, power settings or driver settings were changed.

Reproduction commands, the frozen binary identity, all six raw captures, per-process receipts, clock/thermal JSONL, `telemetry-summary.json` and independently recomputed `analysis.json` are under ignored `build/intel-int8/20261111/measurements/`; `run_balanced.py` is plan-only unless `--execute` is supplied. Final-build Intel INT8, zero-store, FP16 compatibility and AMD INT8 correctness controls precede/follow these measurements separately. The final AMD reference/DP4a checks pass with zero validation errors/warnings and cooperative matrices explicitly unavailable. These controls remain separate from renderer diagnostic-capture coverage and full static-analysis acceptance.

The final canonical LLVM 21.1.8 gate passes all 442 owned renderer/lab and two extraction translation units, including both optional GPU-lab sources. All 634 frozen owned/header/build-database/gate inputs remain unchanged; the only retained diagnostics are the four exact reviewed Filament bitmask baselines. The complete receipt is `build/intel-diagnostics/20261111/static-gate-05-receipt.json`. Earlier interrupted scans and the two corrected source-style findings remain separately recorded; process-group drainage was verified before performance work. This source-analysis evidence does not extend the native hardware or platform qualification above.

## Earlier validation scope, November 10

The enabled FSR development build passes all 41 selected host/CLI CTest cases with the native rendering suite excluded; the report-analysis test covers raw-statistic recomputation, mismatched captures and incomplete coverage. Eighteen existing analysis-gate tests, nine compilation-coverage tests and eleven repository-hygiene tests pass. The new owned C++ translation unit passes the existing LLVM 21.1.8 analysis gate without new diagnostic suppressions. Initial host/gate attempts failed because `/tmp` is non-executable; retained reruns use an executable `TMPDIR` below the evidence directory. Run these tests with an executable temporary directory on similarly configured systems.

The ordinary renderer also exports the actual `material-models` fixture after 30 presented frames on verified Intel Vulkan; the requested 640×360 Wayland window has an actual 639×360 drawable, retained in its capture evidence. This limited fixture is not a full-Heaven tour or a complete native regression suite. On the AMD Radeon 610M, the quick reference result passes and the cooperative result is explicitly unavailable. A nonexistent device is rejected without fallback. SIGINT stops a real Intel workload with exit 130 and no completed/partial report; an invalid export path returns failure while preserving the existing parent-file sentinel. Raw logs and terminal receipts retain each case separately.
