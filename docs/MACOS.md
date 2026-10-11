# Apple Silicon builds

The [macOS workflow](../.github/workflows/macos.yml) builds a native ARM64 candidate for Apple Silicon Macs (M1 and newer). The renderer targets macOS 14 or newer; the package inspects all bundled libraries and declares their highest required macOS version in its README and app metadata. It runs on pushes, pull requests and manual dispatch. Download the `HeavenBench-macos-arm64` artifact from a successful run; it contains the app ZIP, SHA-256 checksum, build environment and package verification report. macOS packages are separate from the published Linux, Windows and Android 1.0.0 release assets.

## Build locally

Install Xcode Command Line Tools and [Homebrew](https://brew.sh), then run these commands from the repository root on an Apple Silicon Mac:

```bash
brew install cmake ninja pkgconf llvm@21 sdl2 glm vulkan-headers vulkan-loader molten-vk
./scripts/fetch_dependencies.sh
./scripts/ci_macos.sh --jobs 2 --output-dir build/release/macos
```

The shared pipeline builds the renderer, standalone asset packer and extraction tools, runs host tests, packages the app and verifies its relocated command-line tools. The app ZIP and checksum appear under the selected output directory. Two build jobs limit memory use on the hosted runner; local machines with more memory can raise the value.

For incremental renderer development or packaging an existing build:

```bash
./scripts/build.sh --platform macos --jobs 2
./scripts/package_macos.sh --skip-build --output-dir build/release/macos
```

The default build tree is `build/renderer-macos-arm64/`. `--build-dir` selects another tree; use the same value when packaging. `HEAVEN_LLVM_ROOT` can select an LLVM installation compatible with the required C++23 library features. The helper uses LLVM 21 and its libc++ headers and runtime together because the system Apple Clang/library combination may lack required features.

## Run the app

Extract the ZIP and keep `HeavenBench.app` intact. Original Heaven assets remain external: place `assets.zip` or an `assets/` directory beside the app, or pass an explicit path:

```bash
./HeavenBench.app/Contents/MacOS/HeavenBench --data /path/to/assets.zip
```

The launcher stores settings, caches and reports under `~/Library/Application Support/HeavenBench`; `HEAVEN_USER_DIR` overrides that directory. The bundle includes its Vulkan loader, MoltenVK implementation and required non-system libraries, so Homebrew is needed to build rather than to run the packaged application. MoltenVK translates the existing Vulkan renderer to Metal while retaining the committed Vulkan material packages. HDR display output remains specific to compatible native-Wayland Linux displays.

The bundle is ad hoc signed for local integrity checks. It is not Developer ID signed or notarized; macOS may require using **Open Anyway** in **System Settings → Privacy & Security** after the first launch attempt. Only bypass this warning for an artifact you trust and whose checksum you have verified. Apple signing and notarization are separate distribution work.

## Validation scope

CI checks ARM64 architecture, host behavior, dependent-library relocation, bundled licenses, code signatures, checksums and CLI execution after moving the app to a path containing spaces. Its report records the checks and build environment. A successful host build does not establish full-scene rendering, performance, Retina resizing or interactive acceptance on a physical Mac. Those checks are required before treating this candidate as a qualified macOS release.

The runner label follows [GitHub's hosted runner documentation](https://docs.github.com/en/actions/reference/runners/github-hosted-runners). The workflow explicitly verifies `uname -m` so an unexpected runner change fails before compilation.
