# Faithful Hi-Res / 4K Texture Pipeline

The Python baker and its tests live under `tools/hires_textures/`; the setup and generation scripts below remain stable entry points. Model inference and compression are offline tools and are not linked into HeavenBench. The separate C++ ZIP packer uses the renderer's CPU loaders through a shared library; see [`tools/README.md`](../tools/README.md).

HeavenBench keeps texture enhancement offline. The renderer never downloads a
model and normal builds do not link PyTorch, OpenCV, HAT, or Compressonator.
A base archive contains the exact authored textures; a separate hi-res
extension contains validated 2K and 4K views of generated full-resolution mip
chains. The runtime mounts both without duplicating originals.

## Reproducible setup

Install the host-only tools below `build/`:

```bash
./scripts/setup_hires_texture_tools.sh --torch cpu
./scripts/setup_hires_texture_tools.sh --torch rocm  # AMD GPU
# Or use --torch cuda for NVIDIA.
```

The helper downloads SHA-256-pinned source archives for HAT, BasicSR, and
Compressonator, verifies the `Real_HAT_GAN_SRx4` model, creates a backend-specific
environment with seeded `pip` (Python 3.11 for CPU/CUDA and Python 3.12 for
ROCm), runs the GPU-free texture-policy golden tests, and installs AMD's
SHA-256-pinned official Linux CompressonatorCLI archive. No tool or model is installed system-wide. Exact revisions,
checksums, runtime/device details, and third-party licenses are recorded in every
generated texture set. CPU, CUDA, and ROCm installations can coexist under
`build/hires-texture-tools/venv-<backend>`.

The ROCm setup uses AMD's official PyTorch 2.9.1 / ROCm 7.2.1 wheels from its
[native Linux installation guide](https://rocm.docs.amd.com/projects/radeon-ryzen/en/latest/docs/install/installrad/native_linux/install-pytorch.html).
During a bake it validates that PyTorch is a HIP build and automatically selects the GPU
with the most VRAM, avoiding an integrated GPU when a larger discrete GPU is
present. Set `HEAVEN_HIRES_GPU_INDEX=N` to override that choice. PyTorch retains
the `torch.cuda` API name for both CUDA and ROCm devices; this does not mean the
ROCm path uses NVIDIA CUDA.

## Bake

Use the current original-only `assets/assets.zip` as the canonical inventory. That
archive contains exactly the scene and feature textures discovered by the same
packer that will validate the generated set:

```bash
./scripts/generate_hires_textures.sh \
  --data ./assets/assets.zip \
  --output ./build/hires-textures \
  --device rocm
```

The operation is content-addressed and resumable. `--force` invalidates the
local reuse decision; `--limit N` is a developer smoke and deliberately emits
an incomplete manifest. A full bake writes:

- `texture-set.json`, containing source/output fingerprints and all variant
  metadata;
- mirrored BC7, BC5, and BC4 DDS variants with complete semantic mip chains;
- JSON and HTML validation reports;
- source/2K/4K contact sheets for representative scene texture groups;
- model, dependency, license, and generator provenance.

Missing, stale, corrupt, or invariant-violating textures leave `complete`
false. Comparative quality metrics (PSNR, SSIM, normal angle, scalar MAE, and
cutout coverage) remain prominent review warnings: learned super-resolution is
not expected to reproduce the authored low-resolution pixels exactly, and a
threshold miss does not make an otherwise complete DDS payload unusable. The
asset packer still rejects incomplete sets instead of silently producing a
mixed-quality release.

If a completed bake from generator 1.2.0 ended as `INCOMPLETE` only because of
those quality thresholds, revalidate the content-addressed outputs without
initializing PyTorch or a GPU:

```bash
./scripts/generate_hires_textures.sh \
  --data ./assets/assets.zip \
  --output ./build/hires-textures \
  --device rocm \
  --revalidate
```

Every source and generated hash is checked. A missing, changed, or operationally
failed entry remains incomplete and must be resumed normally; `--revalidate`
never generates or recompresses a texture.

## Pack the hi-res extension

Run the normal deterministic packer over the loose extracted Data_Root and add
the validated bake:

```bash
./build/renderer/heaven_benchmark \
  --data ./extracted/heaven \
  --pack-assets ./assets/assets-hires.zip \
  --pack-texture-set ./build/hires-textures/texture-set.json \
  --pack-overwrite
unzip -tq ./assets/assets-hires.zip
```

The packer recalculates every original and generated SHA-256, requires complete
coverage of the actual packaged texture inventory, rejects unsafe or duplicate
paths, and emits a schema-v2 manifest. The extension contains variants and
texture-set metadata/provenance only; authored assets and the packer-owned
ground-height derivative stay solely in `assets.zip`. DDS and PNG entries remain
stored
because they are already compressed; ZIP64 is retained for the multi-GB
archive. Run with `--data ./assets` to mount both packs.

## Select at runtime

The stable quality values are `original`, `2k`, and `4k`:

```bash
./scripts/run.sh -- --texture-quality original
./scripts/run.sh -- --texture-quality 2k
./scripts/run.sh -- --texture-quality 4k
```

They are also accepted by `texture_quality` in `config.json` and exposed by the
Graphics settings UI under “Textures - high VRAM impact.” Original loads the
authored DDS/PNG. High 2K opens the generated chain at the first mip whose
longest edge is no more than 2048; skipped 4K bytes are not buffered. Ultra 4K
uses the top generated mip, capped at 4096. Schema-v1 archives, missing
variants, corrupt entries, and unsupported BC7 devices consolidate warnings
and fall back to the exact authored texture while reporting explicit counters.

## Semantic policy and acceptance

- Diffuse/emissive color uses deterministic FP32 `Real_HAT_GAN_SRx4` inference
  with 256-pixel tiles and 32-pixel wrap/clamp-aware context. Alpha never enters
  the model; foreground RGB is dilated and alpha is resampled independently.
- Normal maps are decoded as vectors, filtered, reconstructed, and normalized.
- Height and tessellation-density maps use bounded linear Lanczos filtering.
- Specular and other multi-channel data use non-generative per-channel
  filtering.
- Color/data is BC7, normals BC5, and scalar data BC4. Every mip is generated
  before each level is independently compressed at maximum encoder quality.

The manifest is complete only when color round-trip consistency reaches 30 dB
PSNR and 0.95 SSIM, alpha drift is at most 0.5 percentage points at measurable
mip sizes, mean normal error is at most 2 degrees, scalar MAE is at most 1/255
without overshoot, and repeat-edge discontinuity does not regress by more than
2/255.
