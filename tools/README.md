# Asset tools

## Build assets from the original installer

`asset_builder/build_assets.py` orchestrates installer acquisition, UNG extraction, derived ground-texture generation, and ZIP validation. On Linux, the helper builds the extractor and CPU-only packer automatically:

```sh
./scripts/build_assets.sh --jobs 2
# Reuse a particular installer without downloading it:
./scripts/build_assets.sh --installer /path/to/Unigine_Heaven-4.0.run --no-download
# Replace a previously built pack explicitly:
./scripts/build_assets.sh --overwrite
```

Automatic builds require Python 3.10+, CMake 3.25+, Ninja, a C++23-capable Clang compiler with libc++, GLM and zlib development headers, and Git. Missing pinned dependency sources require network access. The asset build does not configure Filament, SDL, or Vulkan, and needs no display or GPU.

The default output is `assets/assets.zip`. Installer lookup checks the current directory, the repository root, then `data/Unigine_Heaven-4.0.run`. If no installer exists, the builder downloads it from the [official Heaven download service](https://benchmark.unigine.com/heaven) into `data/`. An explicit `--installer PATH` is also the download destination when that file is absent. Both local and downloaded files must match the pinned 286,332,610-byte Heaven 4.0 installer and SHA-256 `52db2e5dedd5621d7c2bfa936b482c0a7cc69b30616278ef199513d494d8e397`; an invalid local file fails rather than being silently replaced.

The builder reads the installer's compressed archive directly and extracts only `data/core.ung` and `data/heaven.ung`; it never runs the installer. Temporary original data and build products stay under `build/asset-builder/` by default (`--work-dir` overrides this). Scene extraction preserves the archive's internal `heaven/` and `core/` prefixes. The pack includes optional cloud resources and generated ground relief. ZIP checks cover CRCs, required resources, and the manifest before publication, and failures preserve any previous output. A provenance receipt records input and output fingerprints under the work directory. `--output PATH.zip` selects another destination; base packs still contain original assets and must remain outside tracked source paths.

For a prepared offline build, supply the already-built executables and `--skip-build --no-download`. `--no-download` prevents installer acquisition; building missing dependencies still requires their sources locally. The Python entry point also works with native Windows executables from `scripts/build_windows.ps1` or a Windows package:

```sh
python3 tools/asset_builder/build_assets.py --skip-build --no-download \
  --installer data/Unigine_Heaven-4.0.run \
  --extractor build/tools/uniginex \
  --packer build/asset-tools/heaven_asset_packer
```

On Windows, use `python` and native `uniginex.exe` / `heaven_asset_packer.exe` paths. Automatic compiler setup is currently provided by the Linux shell helper. The generated base pack can be used by Linux, Windows, or Android builds. High-resolution neural texture baking remains optional and separate from the base pack.

## Extraction, packing, and texture generation

`extraction/` contains the UNG extractor, mesh converter, and Python scene assembler. The root CMake project builds the two extraction executables without renderer dependencies.

`asset_packer/` contains used-asset discovery and ZIP writing. Its `heaven_asset_packer` executable links the same CPU loaders as the renderer through `heaven_cpu_engine`, plus Jolt and libzip; it does not link Filament, SDL, or GPU libraries. The independent build helper selects the required C++23 toolchain and configures only CPU dependencies:

```sh
./scripts/build_asset_tools.sh --jobs 2
./build/asset-tools/heaven_asset_packer --data extracted/heaven --pack-assets assets/assets.zip
```

The renderer build also exposes `heaven_asset_packer` via `scripts/build.sh --target heaven_asset_packer`, sharing the same CMake source/dependency definitions; native Windows builds include it through `scripts/build_windows.ps1`. The renderer's existing `--pack-assets` option remains compatible and calls the same packer library before initializing graphics. The standalone tool accepts the same scene/feature flags and explicit `--config FILE`, with CLI settings overriding the file. Invalid CLI or explicit configuration diagnostics stop the standalone tool before packing. It does not implicitly load saved interactive settings. Both entry points discover bundled sky/loading/attribution inputs beside the executable in Windows ZIPs and under `usr/share/heavenbench/resources` in Linux AppDirs, independent of the current directory. `HEAVEN_RESOURCE_DIR` remains an explicit override.

`hires_textures/` contains the optional Python texture baker, its policy tests, and its pinned upstream compatibility patch. PyTorch, BasicSR/HAT inference, and Compressonator run only in this offline tool; they are never embedded in the benchmark or C++ packer. Existing setup and generation commands remain:

```sh
./scripts/setup_hires_texture_tools.sh --torch cpu
./scripts/generate_hires_textures.sh --data assets/assets.zip --output build/hires --device cpu
./build/asset-tools/heaven_asset_packer --data extracted/heaven --pack-assets assets/hires.zip \
    --pack-texture-set build/hires/texture-set.json
```

The legacy Python entry points under `scripts/` delegate to the implementation under `tools/hires_textures/`. GPU-free policy tests require the offline tool's NumPy/SciPy/scikit-image environment; the ordinary extraction and C++ packer builds do not require those Python packages.

The small procedural ground-relief fallback remains in the shared CPU engine so loose and older asset sets still load correctly. It runs during asset loading, not as neural inference or per-frame texture baking. Filament material compilation is a separate build-time operation handled by `scripts/build.sh --materials`; ordinary builds use the committed material packages.
