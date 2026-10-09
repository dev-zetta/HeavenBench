#!/usr/bin/env bash
# Build a relocatable Linux x86-64 AppImage. Original Heaven assets stay external.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/build_paths.sh"
BUILD_DIR="${HEAVEN_BUILD_DIR:-$(heaven_renderer_build_dir linux Release)}"
OUTPUT_DIR="$HEAVEN_BUILD_ROOT/release"
CPU_ARCH=portable
JOBS=2
SKIP_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir|--output-dir|--cpu-arch|--jobs)
            [[ $# -ge 2 && -n "$2" ]] || { echo "$1 requires a value" >&2; exit 1; } ;;
    esac
    case "$1" in
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
        --cpu-arch) CPU_ARCH="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --help|-h)
            cat <<'EOF'
Usage: scripts/package_appimage.sh [--build-dir DIR] [--output-dir DIR]
       [--cpu-arch portable|zen1|zen2|zen3|zen4|zen5] [--jobs N] [--skip-build]

Builds the renderer through build.sh, then creates an AppImage and SHA-256 file.
--skip-build packages an existing Release tree and verifies its architecture.
Requires Linux x86-64, curl, file, Python 3, patchelf and desktop-file-validate.
Packaging tools are downloaded to build/packaging-tools and checksum-verified.
Build on Ubuntu 24.04 for the CI baseline; newer glibc hosts raise that baseline.
EOF
            exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo "AppImage packaging requires Linux x86-64" >&2; exit 1; }
[[ "$CPU_ARCH" =~ ^(portable|zen[1-5])$ ]] || { echo "Choose an explicit distributable CPU architecture" >&2; exit 1; }
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be positive" >&2; exit 1; }
for tool in curl file python3 patchelf desktop-file-validate sha256sum; do
    command -v "$tool" >/dev/null || { echo "Required tool not found: $tool" >&2; exit 1; }
done
BUILD_DIR=$(realpath -m "$BUILD_DIR")
OUTPUT_DIR=$(realpath -m "$OUTPUT_DIR")
if [[ "$SKIP_BUILD" == 0 ]]; then
    "$SCRIPT_DIR/build.sh" --release --build-dir "$BUILD_DIR" --cpu-arch "$CPU_ARCH" --jobs "$JOBS" \
        --target heaven_benchmark --target heaven_asset_packer
fi
for executable in heaven_benchmark heaven_asset_packer; do
    [[ -x "$BUILD_DIR/$executable" ]] || { echo "Executable missing: $BUILD_DIR/$executable" >&2; exit 1; }
done
grep -qx "HEAVEN_CPU_ARCH:STRING=$CPU_ARCH" "$BUILD_DIR/CMakeCache.txt" || { echo "Build CPU architecture does not match $CPU_ARCH" >&2; exit 1; }
grep -qx 'CMAKE_BUILD_TYPE:STRING=Release' "$BUILD_DIR/CMakeCache.txt" || { echo "Packaging requires a Release build" >&2; exit 1; }
VERSION=$(sed -n 's/^project(HeavenBench VERSION \([^ ]*\) LANGUAGES.*$/\1/p' "$REPO_ROOT/benchmark/CMakeLists.txt")
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Cannot determine project version" >&2; exit 1; }
TOOLS_DIR=$(realpath -m "$HEAVEN_BUILD_ROOT/packaging-tools")
mkdir -p "$TOOLS_DIR" "$OUTPUT_DIR"
fetch_tool() {
    local name=$1 digest=$2 url=$3 path="$TOOLS_DIR/$1"
    if [[ ! -f "$path" ]]; then
        curl --fail --location --retry 3 --output "$path.part" "$url"
        mv "$path.part" "$path"
    fi
    printf '%s  %s\n' "$digest" "$path" | sha256sum --check --status || {
        echo "Checksum mismatch for $path; remove it and retry after verifying upstream" >&2; exit 1;
    }
    chmod +x "$path"
}
fetch_tool linuxdeploy.AppImage c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d \
    https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
fetch_tool appimagetool.AppImage ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 \
    https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
# Use the fixed runtime release as well as a digest; continuous assets change.
fetch_tool runtime-20251108-x86_64 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d \
    https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64

STAGE=$(mktemp -d "$OUTPUT_DIR/.appimage-stage.XXXXXX")
trap 'rm -rf -- "$STAGE"' EXIT
APPDIR="$STAGE/HeavenBench.AppDir"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/heavenbench/resources"
install -m755 "$BUILD_DIR/heaven_benchmark" "$BUILD_DIR/heaven_asset_packer" "$APPDIR/usr/bin/"
cp -a "$REPO_ROOT/benchmark/assets/." "$APPDIR/usr/share/heavenbench/resources/"
python3 "$SCRIPT_DIR/collect_licenses.py" --output "$APPDIR/usr/share/licenses/HeavenBench"
cp -a "$REPO_ROOT/packaging/linux/licenses/." "$APPDIR/usr/share/licenses/HeavenBench/"
install -m644 "$REPO_ROOT/packaging/linux/heavenbench.desktop" "$APPDIR/"
install -m644 "$REPO_ROOT/packaging/linux/heavenbench.svg" "$APPDIR/"
install -m755 "$REPO_ROOT/packaging/linux/AppRun" "$APPDIR/AppRun"
desktop-file-validate "$APPDIR/heavenbench.desktop"

# Extract our verified tools explicitly, avoiding any FUSE requirement in CI.
mkdir -p "$STAGE/linuxdeploy" "$STAGE/appimagetool"
(cd "$STAGE/linuxdeploy" && "$TOOLS_DIR/linuxdeploy.AppImage" --appimage-extract >/dev/null)
(cd "$STAGE/appimagetool" && "$TOOLS_DIR/appimagetool.AppImage" --appimage-extract >/dev/null)
# Vulkan/GL loaders, ICDs, GBM and DRM must come from the user's graphics stack.
# Bundling this workstation's /opt/amdgpu libraries would break other drivers.
EXCLUDES=(libvulkan.so.1 libGL.so.1 libGLX.so.0 libGLdispatch.so.0 libEGL.so.1 libOpenGL.so.0 libgbm.so.1 'libdrm*' libwayland-client.so.0)
DEPLOY_ARGS=()
for lib in "${EXCLUDES[@]}"; do DEPLOY_ARGS+=(--exclude-library "$lib"); done
"$STAGE/linuxdeploy/squashfs-root/AppRun" --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/heaven_benchmark" \
    --executable "$APPDIR/usr/bin/heaven_asset_packer" \
    --desktop-file "$APPDIR/heavenbench.desktop" --icon-file "$APPDIR/heavenbench.svg" \
    --custom-apprun "$REPO_ROOT/packaging/linux/AppRun" "${DEPLOY_ARGS[@]}"

# Retain distribution copyright files for libraries in the bundled closure.
python3 "$SCRIPT_DIR/collect_linux_licenses.py" "$APPDIR"
ARTIFACT="$OUTPUT_DIR/HeavenBench-$VERSION-linux-x86_64-$CPU_ARCH.AppImage"
ARCH=x86_64 VERSION="$VERSION" "$STAGE/appimagetool/squashfs-root/AppRun" \
    --runtime-file "$TOOLS_DIR/runtime-20251108-x86_64" "$APPDIR" "$ARTIFACT"
(cd "$OUTPUT_DIR" && sha256sum "$(basename "$ARTIFACT")" > "$(basename "$ARTIFACT").sha256")
echo "AppImage: $ARTIFACT"
