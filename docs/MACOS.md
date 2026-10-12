# HeavenBench on Apple Silicon

[Download the latest HeavenBench release for macOS](https://github.com/dev-zetta/HeavenBench/releases/latest). Choose the Apple Silicon app ZIP and its matching checksum, source archive and provenance files. The published 1.0.1 package requires **Apple Silicon (M1 or newer) and macOS 15.0 or newer**; each package's verification record declares its actual minimum OS version. Original Heaven assets are supplied separately.

The 1.0.1 app uses the same release commit as the other platforms. Its matching source is `HeavenBench-1.0.1-source.tar.gz`, and `SHA256SUMS` covers the app ZIP and supporting files. Historically, the 1.0.0 macOS package was added separately from source commit [`6e959ae`](https://github.com/dev-zetta/HeavenBench/commit/6e959ae59338258743b8226776d8a2b265fe20bc), with `HeavenBench-1.0.0-macos-source.tar.gz` and `SHA256SUMS-macos`; that older release retains its original identities.

The [macOS workflow](../.github/workflows/macos.yml) runs on branch pushes, pull requests and manual dispatch. Its `HeavenBench-macos-arm64` artifact contains the app ZIP, SHA-256 checksum, build environment and package verification report. The renderer's development deployment target is macOS 14; packaging inspects all bundled libraries and declares their highest minimum OS version. The 1.0.1 package declares macOS 15.0.

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
./scripts/package_macos.sh --jobs 2 --output-dir build/release/macos
```

The package helper also builds the standalone asset packer and extraction executables that a renderer-only build omits. Use `--skip-build` only after the complete `ci_macos.sh` pipeline has populated the selected build tree.

The default build tree is `build/renderer-macos-arm64/`. `--build-dir` selects another tree; use the same value when packaging. For development, `HEAVEN_LLVM_ROOT` can select an LLVM installation compatible with the required C++23 library features. App packaging requires LLVM from a Homebrew keg with its installation receipt and redistributable notices. The helper uses LLVM 21 and its libc++ headers and runtime together because the system Apple Clang/library combination may lack required features.

## Run the app

Extract the ZIP and keep `HeavenBench.app` intact. Original Heaven assets remain external: place `assets.zip` or an `assets/` directory beside the app, or pass an explicit path:

```bash
./HeavenBench.app/Contents/MacOS/HeavenBench --data /path/to/assets.zip
```

The launcher stores settings, caches and reports under `~/Library/Application Support/HeavenBench`; `HEAVEN_USER_DIR` overrides that directory. The bundle includes its Vulkan loader, MoltenVK implementation and required non-system libraries, so Homebrew is needed to build rather than to run the packaged application. MoltenVK translates the existing Vulkan renderer to Metal while retaining the committed Vulkan material packages. HDR display output remains specific to compatible native-Wayland Linux displays.

The bundle is ad hoc signed for local integrity checks. It is not Developer ID signed or notarized; macOS may require using **Open Anyway** in **System Settings → Privacy & Security** after the first launch attempt. Only bypass this warning for an artifact you trust and whose checksum you have verified. Apple signing and notarization are separate distribution work.

## Validation scope

CI checks ARM64 architecture, host behavior, dependent-library relocation, bundled licenses, code signatures, checksums, headless SDL initialization and CLI execution after moving the app to a path containing spaces. Runtime-loaded SDL3 is included when Homebrew supplies the SDL2 compatibility layer; the probe rejects loading an SDL library from outside the bundle. Its report records the checks and build environment. The published 1.0.1 package also passed an independent package inventory, checksum and Mach-O dependency audit. Full-scene rendering, performance, Retina resizing and interactive acceptance on a physical Mac remain unverified.

The runner label follows [GitHub's hosted runner documentation](https://docs.github.com/en/actions/reference/runners/github-hosted-runners). The workflow explicitly verifies `uname -m` so an unexpected runner change fails before compilation.
